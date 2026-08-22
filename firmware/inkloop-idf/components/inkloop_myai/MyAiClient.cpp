#include "MyAiClient.h"
#include "EndpointPolicy.h"
#include "GatewayProbeContract.h"

#include <algorithm>
#include <sstream>

namespace inkloop {
namespace myai {
namespace {

bool successfulHttp(int status) { return status >= 200 && status < 300; }

std::string urlEncode(const std::string& value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string output;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('%');
      output.push_back(hex[(ch >> 4) & 0x0f]);
      output.push_back(hex[ch & 0x0f]);
    }
  }
  return output;
}

std::string lowercase(std::string value) {
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] >= 'A' && value[index] <= 'Z') value[index] += ('a' - 'A');
  }
  return value;
}

bool terminalImageFailure(const std::string& state) {
  const std::string value = lowercase(state);
  return value.find("fail") != std::string::npos ||
         value.find("error") != std::string::npos ||
         value.find("cancel") != std::string::npos ||
         value.find("reject") != std::string::npos ||
         value.find("block") != std::string::npos ||
         value.find("moderat") != std::string::npos;
}

std::string normalizedBaseUrl(std::string value) {
  while (!value.empty() && value[value.size() - 1] == '/') value.erase(value.size() - 1);
  return value;
}

bool nonEmptyOpaqueValue(const std::string& value, size_t maximumBytes) {
  if (value.empty() || value.size() > maximumBytes) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x20 || byte == 0x7f) return false;
  }
  return true;
}

std::string httpFailureDetail(const char* summary, int httpStatus,
                              const std::string& error) {
  std::ostringstream detail;
  detail << summary << " (HTTP " << httpStatus;
  // parseErrorCode() already rejects control characters and caps this value
  // at 128 bytes. Preserve the Center contract error for field diagnostics
  // without ever logging a token-bearing response body.
  if (!error.empty()) detail << ", error=" << error;
  detail << ")";
  return detail.str();
}

bool canonicalMacAddress(const std::string& value) {
  if (value.size() != 17) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    if (index % 3 == 2) {
      if (value[index] != ':') return false;
      continue;
    }
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
  }
  return true;
}

bool myAiBindingUrl(const std::string& value) {
  static const std::string base = "https://myai.mess.host";
  if (value.size() <= base.size() || value.compare(0, base.size(), base) != 0 ||
      value.size() > 1024) {
    return false;
  }
  const char boundary = value[base.size()];
  return boundary == '/' || boundary == '?' || boundary == '#';
}

bool validPendingPairing(const PendingPairing& pending) {
  return pending.valid() &&
      nonEmptyOpaqueValue(pending.pairingToken, 1024) &&
      myAiBindingUrl(pending.bindingUrl) &&
      nonEmptyOpaqueValue(pending.expiresAt, 128);
}

bool validCredentialSnapshot(const CredentialSnapshot& snapshot) {
  const bool hasFingerprint = !snapshot.installationFingerprint.empty();
  const bool hasPending = !snapshot.pending.empty();
  const bool hasToken = !snapshot.deviceToken.empty();

  // The all-empty snapshot is the only state allowed before the installation
  // fingerprint has been initialized atomically.
  if (!hasFingerprint) {
    return snapshot.deviceId.empty() && !hasPending && !hasToken &&
        !snapshot.active;
  }
  if (!nonEmptyOpaqueValue(snapshot.installationFingerprint, 256)) return false;

  // Pending and bound credentials are mutually exclusive. The six-digit
  // MyAI device ID must identify the same pending/bound credential and may not
  // survive after the corresponding runtime credential has been cleared.
  if (hasPending) {
    return validPendingPairing(snapshot.pending) && !hasToken &&
        !snapshot.active && snapshot.deviceId == snapshot.pending.deviceId;
  }
  if (!snapshot.pending.empty()) return false;
  if (hasToken) {
    return isSixDigitCode(snapshot.deviceId) &&
        nonEmptyOpaqueValue(snapshot.deviceToken, 2048) &&
        snapshot.deviceToken != snapshot.installationFingerprint;
  }
  return snapshot.deviceId.empty() && !snapshot.active;
}

}  // namespace

MyAiClient::MyAiClient(
    const ClientConfig& config, IHttpTransport& http,
    IGatewayProbeSet& gatewayProbes,
    IWebSocketTransport& webSocket, IAigcOutputTransport& output,
    IEndpointSecurity& endpointSecurity, ICredentialStore& credentials,
    IWireCodec& codec, IClock& clock, IAudioSink& audio,
    ILocalTranscriptInterceptor& localCommands, IMyAiEvents& events)
    : config_(config), http_(http), gatewayProbes_(gatewayProbes),
      webSocket_(webSocket), output_(output),
      endpointSecurity_(endpointSecurity), credentialStore_(credentials),
      codec_(codec), clock_(clock), audio_(audio), localCommands_(localCommands),
      events_(events),
      activationState_(ActivationState::Unconfigured),
      voiceState_(VoiceState::Idle), aigcState_(AigcState::Idle), voiceLastSeq_(0),
      voiceReconnectAttempts_(0), voiceSocketOpen_(false), voiceClosing_(false),
      credentialHealthy_(false), initialized_(false) {}

MyAiClient::~MyAiClient() {
  if (voiceSocketOpen_) {
    voiceClosing_ = true;
    webSocket_.close(1000, "client_shutdown");
  }
  voiceLease_.clearSensitive();
  imageLease_.clearSensitive();
  credentials_.redact();
}

Status MyAiClient::initialize() {
  initialized_ = false;
  credentialHealthy_ = false;
  if (config_.installationFingerprint.empty())
    return Status(ErrorCode::InvalidArgument, 0, "missing installation fingerprint");
  const std::string& mac = config_.macAddress.empty()
      ? config_.installationFingerprint : config_.macAddress;
  if (!canonicalMacAddress(mac))
    return Status(ErrorCode::InvalidArgument, 0,
                  "missing canonical MyAI device MAC");
  Status status = loadCredentials();
  if (!status.ok()) return status;
  bool initializedFingerprint = false;
  if (credentials_.installationFingerprint.empty()) {
    status = credentialStore_.initializeFingerprintAtomically(
        config_.installationFingerprint);
    if (!status.ok()) return storageFailure("initialize_fingerprint", status);
    initializedFingerprint = true;
    status = loadCredentials();
    if (!status.ok()) return status;
  }
  if (credentials_.installationFingerprint != config_.installationFingerprint) {
    if (initializedFingerprint)
      return storageFailure("initialize_fingerprint_postcondition", Status());
    return Status(ErrorCode::Conflict, 0,
                  "installation fingerprint does not match credential store");
  }
  initialized_ = true;
  if (credentials_.hasDeviceToken()) {
    if (credentials_.active) {
      setActivation(ActivationState::Bound, Status::success());
    } else {
      setActivation(
          ActivationState::PaymentRequired,
          Status(ErrorCode::PaymentRequired, 402,
                 "bound MyAI device is inactive"));
    }
  } else if (credentials_.pending.valid()) {
    setActivation(ActivationState::Pairing, Status::success());
  } else {
    setActivation(ActivationState::Unconfigured, Status::success());
  }
  return Status::success();
}

Status MyAiClient::loadCredentials() {
  CredentialSnapshot next;
  Status status = credentialStore_.load(next);
  if (!status.ok()) return storageFailure("credential_load", status);
  if (!validCredentialSnapshot(next)) {
    next.redact();
    credentials_.redact();
    return storageFailure("credential_load_invariant", Status());
  }
  credentials_.redact();
  credentials_ = next;
  credentialHealthy_ = true;
  return Status::success();
}

Status MyAiClient::requireCredentialHealth() const {
  return credentialHealthy_
             ? Status::success()
             : Status(ErrorCode::Storage, 0,
                      "MyAI credential storage requires recovery");
}

Status MyAiClient::storageFailure(const std::string& operation,
                                  const Status&) {
  credentialHealthy_ = false;
  Status status(ErrorCode::Storage, 0,
                "MyAI credential storage failure: " + operation);
  setActivation(ActivationState::Error, status);
  events_.onError(status);
  return status;
}

Status MyAiClient::clearPendingFailClosed() {
  if (!credentialHealthy_)
    return storageFailure("clear_pending_unhealthy", Status());
  const std::string fingerprint = credentials_.installationFingerprint;
  const std::string deviceId = credentials_.deviceId;
  const std::string deviceToken = credentials_.deviceToken;
  const bool active = credentials_.active;
  Status status = credentialStore_.clearPendingAtomically();
  if (!status.ok()) return storageFailure("clear_pending", status);
  status = loadCredentials();
  if (!status.ok()) return status;
  const std::string expectedDeviceId = deviceToken.empty()
      ? std::string() : deviceId;
  if (!credentials_.pending.empty() ||
      credentials_.installationFingerprint != fingerprint ||
      credentials_.deviceId != expectedDeviceId ||
      credentials_.deviceToken != deviceToken ||
      credentials_.active != (deviceToken.empty() ? false : active)) {
    return storageFailure("clear_pending_postcondition", Status());
  }
  return Status::success();
}

Status MyAiClient::clearRuntimeCredentialFailClosed() {
  if (!credentialHealthy_)
    return storageFailure("clear_runtime_unhealthy", Status());
  const std::string fingerprint = credentials_.installationFingerprint;
  Status status = credentialStore_.clearRuntimeCredentialAtomically();
  if (!status.ok()) return storageFailure("clear_runtime", status);
  status = loadCredentials();
  if (!status.ok()) return status;
  if (credentials_.hasDeviceToken() || !credentials_.deviceId.empty() ||
      !credentials_.pending.empty() || credentials_.active ||
      credentials_.installationFingerprint != fingerprint) {
    return storageFailure("clear_runtime_postcondition", Status());
  }
  return Status::success();
}

Status MyAiClient::verifyRecoveryReadable() {
  if (!credentialHealthy_)
    return storageFailure("recovery_read_unhealthy", Status());
  const std::string fingerprint = credentials_.installationFingerprint;
  const std::string deviceId = credentials_.deviceId;
  const std::string deviceToken = credentials_.deviceToken;
  const PendingPairing pending = credentials_.pending;
  Status status = loadCredentials();
  if (!status.ok()) return status;
  if (credentials_.installationFingerprint != fingerprint ||
      credentials_.deviceId != deviceId || credentials_.deviceToken != deviceToken ||
      credentials_.pending.deviceId != pending.deviceId ||
      credentials_.pending.pairingToken != pending.pairingToken ||
      credentials_.pending.bindingUrl != pending.bindingUrl ||
      credentials_.pending.expiresAt != pending.expiresAt) {
    return storageFailure("recovery_read_postcondition", Status());
  }
  return Status::success();
}

Status MyAiClient::health() {
  HttpResponse response;
  return perform(centerRequest("GET", "/healthz", "", false), response,
                 RequestKind::Health);
}

Status MyAiClient::setSystemPrompt(const std::string& prompt) {
  if (prompt.size() > 2048)
    return Status(ErrorCode::TooLarge, 0, "system prompt exceeds 2048 bytes");
  if (voiceLease_.valid() || voiceSocketOpen_)
    return Status(ErrorCode::InvalidState, 0,
                  "disconnect voice before changing system prompt");
  config_.systemPrompt = prompt;
  return Status::success();
}

Status MyAiClient::startPairing(const std::string& candidateSixDigits,
                                PairingView& onboarding) {
  if (!initialized_) return Status(ErrorCode::InvalidState, 0, "client not initialized");
  Status storage = requireCredentialHealth();
  if (!storage.ok()) return storage;
  if (credentials_.hasDeviceToken())
    return Status(ErrorCode::Conflict, 0,
                  "reset the bound MyAI credential before starting pairing");
  if (!isSixDigitCode(candidateSixDigits))
    return Status(ErrorCode::InvalidArgument, 0, "MyAI device code must be six digits");
  HttpRequest request = centerRequest(
      "POST", "/api/v1/devices/pairing/start",
      codec_.pairingStartBody(candidateSixDigits,
                              wireMacAddress(),
                              config_.deviceLabel),
      false);
  HttpResponse response;
  Status status = perform(request, response, RequestKind::PairingStart);
  if (!status.ok()) return status;
  PairingStartResponse parsed;
  status = codec_.parsePairingStart(response.body, parsed);
  if (!status.ok()) return status;
  if (parsed.appId != kAppId || !isSixDigitCode(parsed.deviceId) ||
      parsed.deviceId != candidateSixDigits ||
      !nonEmptyOpaqueValue(parsed.pairingToken, 1024) ||
      !myAiBindingUrl(parsed.bindingUrl) ||
      !nonEmptyOpaqueValue(parsed.expiresAt, 128) ||
      parsed.pairingToken == credentials_.installationFingerprint) {
    return Status(ErrorCode::Protocol, 0, "pairing response identity mismatch");
  }
  PendingPairing pending;
  pending.deviceId = parsed.deviceId;
  pending.pairingToken = parsed.pairingToken;
  pending.bindingUrl = parsed.bindingUrl;
  pending.expiresAt = parsed.expiresAt;
  status = credentialStore_.savePendingAtomically(pending);
  if (!status.ok()) return storageFailure("save_pending", status);
  status = loadCredentials();
  if (!status.ok()) return status;
  if (credentials_.installationFingerprint != config_.installationFingerprint ||
      credentials_.hasDeviceToken() ||
      credentials_.pending.deviceId != pending.deviceId ||
      credentials_.pending.pairingToken != pending.pairingToken ||
      credentials_.pending.bindingUrl != pending.bindingUrl ||
      credentials_.pending.expiresAt != pending.expiresAt) {
    return storageFailure("save_pending_postcondition", Status());
  }
  onboarding.onboardingCode = parsed.deviceId;
  onboarding.bindingUrl = parsed.bindingUrl;
  onboarding.expiresAt = parsed.expiresAt;
  setActivation(ActivationState::Pairing, Status::success());
  events_.onPairingReady(onboarding);
  return Status::success();
}

Status MyAiClient::pendingPairing(PairingView& onboarding) const {
  onboarding = PairingView();
  if (!initialized_)
    return Status(ErrorCode::InvalidState, 0, "client not initialized");
  if (!credentialHealthy_)
    return Status(ErrorCode::Storage, 0,
                  "MyAI credential storage requires recovery");
  if (!credentials_.pending.valid())
    return Status(ErrorCode::InvalidState, 0, "no pending MyAI pairing");
  if (!isSixDigitCode(credentials_.pending.deviceId) ||
      !myAiBindingUrl(credentials_.pending.bindingUrl) ||
      !nonEmptyOpaqueValue(credentials_.pending.expiresAt, 128)) {
    return Status(ErrorCode::Storage, 0,
                  "pending MyAI pairing failed public-view validation");
  }
  onboarding.onboardingCode = credentials_.pending.deviceId;
  onboarding.bindingUrl = credentials_.pending.bindingUrl;
  onboarding.expiresAt = credentials_.pending.expiresAt;
  return Status::success();
}

Status MyAiClient::pollPairing(bool& bound) {
  bound = false;
  Status storage = requireCredentialHealth();
  if (!storage.ok()) return storage;
  // A token-bearing success atomically clears pending. Refuse any stale
  // scheduler call locally so it cannot rotate a Center credential again.
  if (credentials_.hasDeviceToken() ||
      activationState_ != ActivationState::Pairing) {
    return Status(ErrorCode::InvalidState, 0,
                  "MyAI pairing polling is no longer active");
  }
  if (!credentials_.pending.valid())
    return Status(ErrorCode::InvalidState, 0, "no pending MyAI pairing");
  HttpRequest request = centerRequest(
      "POST", "/api/v1/devices/pairing/status",
      codec_.pairingStatusBody(credentials_.pending.deviceId,
                               credentials_.pending.pairingToken),
      false);
  HttpResponse response;
  Status status = perform(request, response, RequestKind::PairingStatus);
  if (!status.ok()) return status;
  PairingStatusResponse parsed;
  status = codec_.parsePairingStatus(response.body, parsed);
  if (!status.ok()) return status;
  if (parsed.deviceId != credentials_.pending.deviceId || parsed.appId != kAppId)
    return Status(ErrorCode::Protocol, 0, "pairing status identity mismatch");
  if (parsed.recoveryRequired) {
    status = verifyRecoveryReadable();
    if (!status.ok()) return status;
    status = Status(ErrorCode::RecoveryRequired, 409,
                    "device credential recovery required");
    setActivation(ActivationState::RecoveryRequired, status);
    return status;
  }
  if (!parsed.bound) {
    if (parsed.paymentRequired) {
      status = Status(ErrorCode::PaymentRequired, 402,
                      "MyAI activation/payment required");
      setActivation(ActivationState::PaymentRequired, status);
      return status;
    }
    return Status::success();
  }
  if (parsed.deviceToken.empty())
    return Status(ErrorCode::Protocol, 0, "bound pairing omitted device token");
  if (parsed.deviceToken == credentials_.installationFingerprint ||
      parsed.deviceToken == credentials_.pending.pairingToken)
    return Status(ErrorCode::Protocol, 0, "MyAI credential identity collision");
  // Stop polling immediately after this token-bearing success. The store must
  // atomically replace the runtime token and clear the pending token.
  status = credentialStore_.promoteBoundAtomically(
      credentials_.pending.pairingToken, parsed.deviceId, parsed.deviceToken,
      parsed.active);
  if (!status.ok()) return storageFailure("promote_bound", status);
  status = loadCredentials();
  if (!status.ok()) return status;
  if (credentials_.installationFingerprint != config_.installationFingerprint ||
      credentials_.deviceId != parsed.deviceId ||
      credentials_.deviceToken != parsed.deviceToken ||
      !credentials_.pending.empty() || credentials_.active != parsed.active) {
    bound = false;
    return storageFailure("promote_bound_postcondition", Status());
  }
  bound = true;
  setActivation(parsed.active ? ActivationState::Bound
                              : ActivationState::PaymentRequired,
                parsed.active ? Status::success()
                              : Status(ErrorCode::PaymentRequired, 402,
                                       "bound MyAI device is inactive"));
  return Status::success();
}

Status MyAiClient::checkAuthorization(bool& authorized) {
  authorized = false;
  Status storage = requireCredentialHealth();
  if (!storage.ok()) return storage;
  if (!credentials_.hasDeviceToken())
    return Status(ErrorCode::Unauthorized, 401, "MyAI device is not bound");
  HttpResponse response;
  Status status = perform(
      centerRequest("POST", "/api/v1/devices/check",
                    codec_.deviceCheckBody(credentials_.deviceId,
                                           wireMacAddress()),
                    true),
      response, RequestKind::DeviceCheck);
  if (!status.ok()) return status;
  bool active = false;
  status = codec_.parseDeviceCheck(response.body, authorized, active);
  if (!status.ok()) return status;
  if (!authorized) {
    status = Status(ErrorCode::Unauthorized, 401, "MyAI device authorization rejected");
    Status cleared = clearRuntimeCredentialFailClosed();
    if (!cleared.ok()) return cleared;
    setActivation(ActivationState::Unconfigured, status);
    return status;
  }
  if (!active) {
    status = Status(ErrorCode::PaymentRequired, 402, "MyAI device is inactive");
    setActivation(ActivationState::PaymentRequired, status);
    return status;
  }
  setActivation(ActivationState::Bound, Status::success());
  return Status::success();
}

Status MyAiClient::cancelPairing() {
  Status storage = requireCredentialHealth();
  if (!storage.ok()) return storage;
  Status status = clearPendingFailClosed();
  if (!status.ok()) return status;
  setActivation(credentials_.hasDeviceToken() ? ActivationState::Bound
                                              : ActivationState::Unconfigured,
                Status::success());
  return Status::success();
}

Status MyAiClient::resetCredentialForRebind() {
  if (!initialized_)
    return Status(ErrorCode::InvalidState, 0, "client not initialized");
  if (voiceLease_.valid() || voiceSocketOpen_ ||
      voiceState_ != VoiceState::Idle || aigcState_ != AigcState::Idle) {
    return Status(ErrorCode::InvalidState, 0,
                  "finish MyAI voice or image work before re-binding");
  }
  Status status = clearRuntimeCredentialFailClosed();
  if (!status.ok()) return status;
  setActivation(ActivationState::Unconfigured, Status::success());
  return Status::success();
}

Status MyAiClient::openGatewaySession(Capability capability,
                                      GatewayLease& lease) {
  bool authorized = false;
  Status status = checkAuthorization(authorized);
  if (!status.ok()) return status;

  std::string providerProfileId;
  if (capability == Capability::Voice) {
    HttpResponse preferenceResponse;
    status = perform(centerRequest(
                         "GET",
                         std::string("/api/v1/model-preferences?app_id=") +
                             urlEncode(kAppId),
                         "", true),
                     preferenceResponse, RequestKind::CenterAuthenticated);
    if (!status.ok()) return status;
    status = codec_.parseModelPreference(preferenceResponse.body,
                                         providerProfileId);
    if (!status.ok()) return status;
    if (!nonEmptyOpaqueValue(providerProfileId, 512)) {
      return Status(ErrorCode::Protocol, 0,
                    "invalid MyAI provider profile id");
    }
  }

  HttpResponse requestedResponse;
  status = perform(
      centerRequest("POST", "/api/v1/client/sessions",
                    codec_.sessionRequestBody(
                        capability, credentials_.deviceId,
                        wireMacAddress(), config_.clientRegion),
                    true),
      requestedResponse, RequestKind::CenterAuthenticated);
  if (!status.ok()) return status;
  SessionRequestResponse requested;
  status = codec_.parseSessionRequest(requestedResponse.body, requested);
  if (!status.ok()) return status;
  if (!nonEmptyOpaqueValue(requested.sessionId, 512))
    return Status(ErrorCode::Protocol, 0, "MyAI session omitted session id");

  status = GatewayProbeContract::validateCandidates(requested.gateways);
  if (!status.ok()) return status;
  for (const GatewayCandidate& candidate : requested.gateways) {
    if (!nonEmptyOpaqueValue(candidate.id, 256) ||
        !isPublicGatewayUrl(candidate.pingUrl) ||
        !isPublicGatewayUrl(candidate.baseUrl)) {
      return Status(ErrorCode::Security, 0,
                    "non-public MyAI gateway candidate rejected");
    }
  }

  std::vector<GatewayProbe> probes;
  status = gatewayProbes_.probeConcurrent(
      requested.gateways, deviceHeaders(false),
      GatewayProbeContract::kTotalDeadlineMs, probes);
  if (!status.ok()) return status;
  GatewayCandidate selectedCandidate;
  status = GatewayProbeContract::selectFastest(
      requested.gateways, probes, selectedCandidate);
  if (!status.ok()) return status;
  HttpResponse selectResponse;
  status = perform(
      centerRequest("POST", "/api/v1/client/sessions/select",
                    codec_.sessionSelectBody(requested.sessionId,
                                             selectedCandidate.id, probes),
                    true),
      selectResponse, RequestKind::CenterAuthenticated);
  if (!status.ok()) return status;
  SessionSelectResponse selected;
  status = codec_.parseSessionSelect(selectResponse.body, selected);
  if (!status.ok()) return status;
  if (!nonEmptyOpaqueValue(selected.gatewayToken, 2048) ||
      !nonEmptyOpaqueValue(selected.gateway.id, 256) ||
      selected.gateway.id != selectedCandidate.id ||
      !isPublicGatewayUrl(selected.gateway.baseUrl) ||
      normalizedBaseUrl(selected.gateway.baseUrl) !=
          normalizedBaseUrl(selectedCandidate.baseUrl)) {
    selected.gatewayToken.assign(selected.gatewayToken.size(), '\0');
    return Status(ErrorCode::Protocol, 0, "selected gateway identity/URL rejected");
  }

  lease.clearSensitive();
  lease = GatewayLease();
  lease.capability = capability;
  lease.sessionId = requested.sessionId;
  lease.gatewayId = selected.gateway.id;
  lease.gatewayBaseUrl = normalizedBaseUrl(selected.gateway.baseUrl);
  lease.gatewayToken = selected.gatewayToken;
  lease.providerProfileId = providerProfileId;
  lease.startedAtMs = clock_.monotonicMs();

  if (!lease.valid() ||
      (capability == Capability::Voice && lease.providerProfileId.empty())) {
    lease.clearSensitive();
    lease = GatewayLease();
    return Status(ErrorCode::Protocol, 0, "incomplete selected gateway lease");
  }

  HttpRequest start;
  start.method = "POST";
  start.url = lease.gatewayBaseUrl + "/api/v1/gateway/sessions/start";
  start.body = codec_.gatewayStartBody(lease);
  start.headers["Content-Type"] = "application/json";
  start.headers["X-Gateway-Session-Token"] = lease.gatewayToken;
  HttpResponse startResponse;
  status = perform(start, startResponse, RequestKind::GatewaySessionStart);
  if (!status.ok()) {
    lease.clearSensitive();
    lease = GatewayLease();
    return status;
  }
  return Status::success();
}

Status MyAiClient::connectVoice() {
  if (voiceSocketOpen_) return Status::success();
  voiceClosing_ = false;
  setVoice(VoiceState::Connecting);
  Status status = openGatewaySession(Capability::Voice, voiceLease_);
  if (!status.ok()) {
    ++voiceReconnectAttempts_;
    setVoice(VoiceState::Error);
    return status;
  }
  if (!voiceLease_.valid() || credentials_.deviceToken.empty()) {
    disconnectLease(voiceLease_, "invalid_voice_lease");
    setVoice(VoiceState::Error);
    return Status(ErrorCode::Protocol, 0, "invalid MyAI voice lease");
  }
  status = endpointSecurity_.validatePublicTlsEndpoint(voiceLease_.gatewayBaseUrl);
  if (!status.ok()) {
    status = Status(ErrorCode::Security, 0,
                    "MyAI WebSocket endpoint failed DNS/TLS validation");
    ++voiceReconnectAttempts_;
    disconnectLease(voiceLease_, "websocket_security_rejected");
    setVoice(VoiceState::Error);
    events_.onError(status);
    return status;
  }
  std::map<std::string, std::string> headers;
  headers["Authorization"] = "Bearer " + credentials_.deviceToken;
  status = webSocket_.connect(voiceWebSocketUrl(voiceLease_), headers, *this);
  if (!status.ok()) {
    ++voiceReconnectAttempts_;
    disconnectLease(voiceLease_, "websocket_connect_failed");
    setVoice(VoiceState::Error);
  }
  return status;
}

Status MyAiClient::beginVoiceTurn(const std::string& streamId) {
  if (!voiceSocketOpen_ || voiceState_ != VoiceState::Ready)
    return Status(ErrorCode::InvalidState, 0, "voice session is not ready");
  if (streamId.empty()) return Status(ErrorCode::InvalidArgument, 0, "missing stream id");
  Status status = webSocket_.sendText(codec_.audioStartMessage(streamId));
  if (!status.ok()) return status;
  voiceStreamId_ = streamId;
  voiceLastSeq_ = 0;
  setVoice(VoiceState::Listening);
  return Status::success();
}

Status MyAiClient::sendPcm16(const uint8_t* bytes, size_t length) {
  if (voiceState_ != VoiceState::Listening)
    return Status(ErrorCode::InvalidState, 0, "voice is not listening");
  if (!bytes || length == 0 || (length & 1U) != 0)
    return Status(ErrorCode::InvalidArgument, 0, "PCM16 frame must be non-empty/even");
  Status status = webSocket_.sendBinary(bytes, length);
  if (status.ok()) ++voiceLastSeq_;
  return status;
}

Status MyAiClient::endVoiceTurn() {
  if (voiceState_ != VoiceState::Listening || voiceStreamId_.empty())
    return Status(ErrorCode::InvalidState, 0, "no active voice turn");
  Status status = webSocket_.sendText(
      codec_.audioStopMessage(voiceStreamId_, voiceLastSeq_));
  if (status.ok()) {
    voiceLease_.requestCount++;
    setVoice(VoiceState::Thinking);
  }
  return status;
}

Status MyAiClient::requestResponse(const std::string& transcript) {
  if (!voiceSocketOpen_ || transcript.empty())
    return Status(ErrorCode::InvalidArgument, 0, "missing transcript/voice socket");
  return webSocket_.sendText(codec_.responseCreateMessage(transcript));
}

Status MyAiClient::heartbeatVoice() {
  if (!voiceLease_.valid()) return Status(ErrorCode::InvalidState, 0, "no voice lease");
  const uint32_t active = static_cast<uint32_t>(
      (clock_.monotonicMs() - voiceLease_.startedAtMs) / 1000U);
  HttpResponse response;
  return perform(centerRequest("POST", "/api/v1/client/sessions/heartbeat",
                               codec_.heartbeatBody(voiceLease_, active), true),
                 response, RequestKind::CenterAuthenticated);
}

Status MyAiClient::disconnectVoice(const std::string& reason) {
  if (voiceSocketOpen_) {
    voiceClosing_ = true;
    webSocket_.close(1000, reason);
  }
  voiceSocketOpen_ = false;
  voiceStreamId_.clear();
  audio_.abort();
  Status status = disconnectLease(voiceLease_, reason);
  setVoice(VoiceState::Idle);
  return status;
}

Status MyAiClient::completeVoiceResponseAfterTtsStop() {
  if (!voiceSocketOpen_)
    return Status(ErrorCode::InvalidState, 0, "voice socket is not open");
  if (voiceState_ == VoiceState::Ready) return Status::success();
  if (voiceState_ != VoiceState::Speaking)
    return Status(ErrorCode::InvalidState, 0,
                  "voice response is not awaiting completion");
  setVoice(VoiceState::Ready);
  return Status::success();
}

Status MyAiClient::comboVoice(const std::string& pcm16Base64,
                              size_t maxResponseBase64Bytes,
                              std::string& transcript, std::string& reply,
                              std::string& responseAudioBase64) {
  if (pcm16Base64.empty())
    return Status(ErrorCode::InvalidArgument, 0, "empty combo audio");
  GatewayLease lease;
  Status status = openGatewaySession(Capability::Voice, lease);
  if (!status.ok()) return status;
  if (!lease.valid())
    return Status(ErrorCode::Protocol, 0, "invalid MyAI combo lease");
  HttpResponse response;
  status = perform(gatewayRequest(
                       lease, "/gateway/v1/combo/voice",
                       codec_.comboVoiceBody(credentials_.deviceId,
                                             wireMacAddress(),
                                             lease.sessionId, pcm16Base64)),
                   response, RequestKind::GatewayBusiness);
  if (status.ok())
    status = codec_.parseComboVoice(response.body, transcript, reply,
                                    responseAudioBase64);
  if (status.ok() && responseAudioBase64.size() > maxResponseBase64Bytes) {
    responseAudioBase64.clear();
    status = Status(ErrorCode::TooLarge, 0, "combo voice response exceeds cap");
  }
  disconnectLease(lease, "combo_complete");
  return status;
}

Status MyAiClient::startImage(const ImageRequest& request,
                              AigcGenerateResponse& generated) {
  if (request.prompt.empty())
    return Status(ErrorCode::InvalidArgument, 0, "missing image prompt");
  setAigc(AigcState::Generating);
  Status status = openGatewaySession(Capability::Image, imageLease_);
  if (!status.ok()) {
    setAigc(AigcState::Error, status.detail);
    return status;
  }
  if (!imageLease_.valid()) {
    setAigc(AigcState::Error, "invalid MyAI image lease");
    return Status(ErrorCode::Protocol, 0, "invalid MyAI image lease");
  }
  HttpResponse response;
  status = perform(gatewayRequest(
                       imageLease_, "/gateway/v1/aigc/generate",
                       codec_.aigcGenerateBody(credentials_.deviceId,
                                               wireMacAddress(),
                                               request)),
                   response, RequestKind::GatewayBusiness);
  if (status.ok()) status = codec_.parseAigcGenerate(response.body, generated);
  if (!status.ok()) setAigc(AigcState::Error, status.detail);
  else {
    imageLease_.requestCount++;
    setAigc(AigcState::Polling, generated.promptId);
  }
  return status;
}

Status MyAiClient::pollImage(const std::string& promptId,
                             AigcStatusResponse& statusResponse) {
  if (!imageLease_.valid() || promptId.empty())
    return Status(ErrorCode::InvalidState, 0, "no active image job");
  HttpResponse response;
  Status status = perform(gatewayRequest(
                               imageLease_, "/gateway/v1/aigc/status",
                               codec_.aigcStatusBody(
                                   credentials_.deviceId,
                                   wireMacAddress(), promptId)),
                           response, RequestKind::GatewayBusiness);
  if (status.ok()) status = codec_.parseAigcStatus(response.body, statusResponse);
  if (!status.ok() || terminalImageFailure(statusResponse.status)) {
    if (status.ok())
      status = Status(ErrorCode::Protocol, 422,
                      statusResponse.message.empty() ? statusResponse.status
                                                     : statusResponse.message);
    setAigc(AigcState::Error, status.detail);
  }
  return status;
}

Status MyAiClient::downloadImage(const std::string& promptId,
                                 const AigcOutputRef& outputRef,
                                 const ImageRequest& limits, IImageSink& sink,
                                 AigcOutputMetadata& metadata) {
  if (!imageLease_.valid() || promptId.empty() || outputRef.filename.empty())
    return Status(ErrorCode::InvalidState, 0, "invalid image output request");
  setAigc(AigcState::Downloading, outputRef.filename);
  HttpRequest request = gatewayRequest(
      imageLease_, "/gateway/v1/aigc/output",
      codec_.aigcOutputBody(credentials_.deviceId,
                            wireMacAddress(), promptId,
                            outputRef));
  request.timeoutMs = 60000;
  request.maxResponseBytes = 0;
  Status status;
  if (!request.tlsPeerVerificationRequired ||
      !request.rejectPrivateResolvedAddresses || request.redirectsAllowed ||
      !isPublicGatewayUrl(request.url)) {
    status = Status(ErrorCode::Security, 0,
                    "MyAI output endpoint security policy rejected the request");
  } else {
    status = endpointSecurity_.validatePublicTlsEndpoint(request.url);
  }
  if (!status.ok()) {
    status = Status(ErrorCode::Security, 0,
                    "MyAI output endpoint failed DNS/TLS validation");
  } else {
    status = output_.postAndDecodeBase64(
        request, limits.maxEncodedBytes, limits.maxDecodedBytes, sink, metadata);
  }
  if (!status.ok()) {
    sink.abort();
    setAigc(AigcState::Error, status.detail);
    return status;
  }
  imageLease_.requestCount++;
  setAigc(AigcState::Complete, metadata.filename);
  return Status::success();
}

Status MyAiClient::disconnectImage(const std::string& reason) {
  Status status = disconnectLease(imageLease_, reason);
  setAigc(AigcState::Idle);
  return status;
}

Status MyAiClient::disconnectLease(GatewayLease& lease,
                                   const std::string& reason) {
  if (!lease.valid()) return Status::success();
  HttpResponse response;
  Status status = perform(
      centerRequest("POST", "/api/v1/client/sessions/disconnect",
                    codec_.disconnectBody(lease, reason), true),
      response, RequestKind::CenterAuthenticated);
  lease.clearSensitive();
  lease = GatewayLease();
  return status;
}

Status MyAiClient::perform(const HttpRequest& request, HttpResponse& response,
                           RequestKind kind) {
  if (kind == RequestKind::GatewaySessionStart) {
    const std::map<std::string, std::string>::const_iterator token =
        request.headers.find("X-Gateway-Session-Token");
    if (token == request.headers.end() || token->second.empty()) {
      return Status(ErrorCode::Protocol, 0, "missing gateway session token");
    }
  } else if (kind == RequestKind::GatewayBusiness) {
    const std::map<std::string, std::string>::const_iterator authorization =
        request.headers.find("Authorization");
    if (authorization == request.headers.end() ||
        authorization->second == "Bearer ") {
      return Status(ErrorCode::Protocol, 0, "missing device credential");
    }
  }
  if (!request.tlsPeerVerificationRequired ||
      !request.rejectPrivateResolvedAddresses || request.redirectsAllowed ||
      !isPublicGatewayUrl(request.url)) {
    Status rejected(ErrorCode::Security, 0,
                    "MyAI endpoint security policy rejected the request");
    setActivation(ActivationState::Error, rejected);
    events_.onError(rejected);
    return rejected;
  }
  Status status = endpointSecurity_.validatePublicTlsEndpoint(request.url);
  if (!status.ok()) {
    Status rejected(ErrorCode::Security, 0,
                    "MyAI endpoint failed DNS/TLS validation");
    setActivation(ActivationState::Error, rejected);
    events_.onError(rejected);
    return rejected;
  }
  status = http_.perform(request, response);
  if (!status.ok()) {
    Status mapped(ErrorCode::Transport, 0, "MyAI transport unavailable",
                  reconnectDelayMs(0));
    setActivation(ActivationState::Offline, mapped);
    events_.onError(mapped);
    return mapped;
  }
  if (successfulHttp(response.status)) return Status::success();
  status = classifyHttp(response, kind);
  if (status.code != ErrorCode::Storage) events_.onError(status);
  return status;
}

HttpRequest MyAiClient::centerRequest(const std::string& method,
                                      const std::string& path,
                                      const std::string& body,
                                      bool authenticated) const {
  HttpRequest request;
  request.method = method;
  request.url = std::string(kCenterBaseUrl) + path;
  request.body = body;
  request.headers = authenticated ? deviceHeaders(true)
                                  : std::map<std::string, std::string>();
  if (!body.empty()) request.headers["Content-Type"] = "application/json";
  return request;
}

HttpRequest MyAiClient::gatewayRequest(const GatewayLease& lease,
                                       const std::string& path,
                                       const std::string& body) const {
  HttpRequest request;
  request.method = "POST";
  request.url = lease.gatewayBaseUrl + path;
  request.body = body;
  request.headers = deviceHeaders(true);
  return request;
}

std::map<std::string, std::string> MyAiClient::deviceHeaders(bool json) const {
  std::map<std::string, std::string> headers;
  headers["Authorization"] = "Bearer " + credentials_.deviceToken;
  headers["X-Device-ID"] = credentials_.deviceId;
  headers["X-Device-MAC"] = wireMacAddress();
  if (json) headers["Content-Type"] = "application/json";
  return headers;
}

const std::string& MyAiClient::wireMacAddress() const {
  return config_.macAddress.empty()
      ? credentials_.installationFingerprint : config_.macAddress;
}

Status MyAiClient::classifyHttp(const HttpResponse& response,
                                RequestKind kind) {
  const int httpStatus = response.status;
  const std::string error = codec_.parseErrorCode(response.body);
  Status status;
  if (httpStatus == 401) {
    status = Status(
        ErrorCode::Unauthorized, 401,
        httpFailureDetail("MyAI authorization rejected", 401, error));
    if (kind == RequestKind::PairingStatus) {
      Status cleared = clearPendingFailClosed();
      if (!cleared.ok()) return cleared;
      setActivation(ActivationState::Unconfigured, status);
    } else if (kind == RequestKind::DeviceCheck ||
               kind == RequestKind::CenterAuthenticated ||
               kind == RequestKind::GatewayBusiness) {
      // Center device tokens are stable for the lifetime of the bound device.
      // A 401 therefore means this durable runtime credential is unusable
      // (for example, the owner deleted the device). Preserve only the stable
      // installation fingerprint so the application can start a fresh
      // six-digit pairing flow.
      Status cleared = clearRuntimeCredentialFailClosed();
      if (!cleared.ok()) return cleared;
      setActivation(ActivationState::Unconfigured, status);
    }
  } else if (httpStatus == 402) {
    status = Status(
        ErrorCode::PaymentRequired, 402,
        httpFailureDetail("MyAI activation/payment required", 402, error));
    setActivation(ActivationState::PaymentRequired, status);
  } else if (httpStatus == 409 &&
             (kind == RequestKind::PairingStatus ||
              kind == RequestKind::DeviceCheck) &&
             error == "device_credential_recovery_required") {
    Status readable = verifyRecoveryReadable();
    if (!readable.ok()) return readable;
    status = Status(
        ErrorCode::RecoveryRequired, 409,
        httpFailureDetail("MyAI device credential recovery required", 409,
                          error));
    setActivation(ActivationState::RecoveryRequired, status);
  } else if (httpStatus == 410 && kind == RequestKind::PairingStatus &&
             error == "pairing_expired") {
    Status cleared = clearPendingFailClosed();
    if (!cleared.ok()) return cleared;
    status = Status(ErrorCode::PairingExpired, 410,
                    httpFailureDetail("MyAI pairing expired", 410, error));
    setActivation(ActivationState::Unconfigured, status);
  } else if (httpStatus == 400 && kind == RequestKind::PairingStart &&
             error == "invalid_input") {
    status = Status(ErrorCode::Conflict, 400,
                    "MyAI rejected the six-digit candidate; rotate it before Inkloop binding");
  } else if (httpStatus == 404 && kind == RequestKind::PairingStart &&
             (error == "not found" || error == "not_found" ||
              error == "app_not_found")) {
    status = Status(ErrorCode::AppNotRegistered, 404,
                    "app_not_registered");
  } else if (httpStatus == 429 || httpStatus >= 500) {
    status = Status(ErrorCode::Transport, httpStatus,
                    "MyAI service temporarily unavailable", reconnectDelayMs(0));
    setActivation(ActivationState::Offline, status);
  } else {
    status = Status(ErrorCode::Protocol, httpStatus,
                    "MyAI response did not match the request contract");
  }
  return status;
}

void MyAiClient::setActivation(ActivationState state, const Status& status) {
  activationState_ = state;
  events_.onActivationState(state, status);
}

void MyAiClient::setVoice(VoiceState state) {
  voiceState_ = state;
  events_.onVoiceState(state);
}

void MyAiClient::setAigc(AigcState state, const std::string& detail) {
  aigcState_ = state;
  events_.onAigcState(state, detail);
}

std::string MyAiClient::voiceWebSocketUrl(const GatewayLease& lease) const {
  std::string url = lease.gatewayBaseUrl;
  if (url.compare(0, 8, "https://") == 0) url.replace(0, 8, "wss://");
  return url + "/gateway/v1/voice/ws?device_id=" +
         urlEncode(credentials_.deviceId) + "&mac_address=" +
         urlEncode(wireMacAddress()) + "&app_id=" + kAppId;
}

void MyAiClient::onWebSocketOpen() {
  voiceSocketOpen_ = true;
  Status status = webSocket_.sendText(codec_.sessionUpdateMessage(
      voiceLease_, credentials_.deviceId, config_.systemPrompt));
  if (!status.ok()) {
    setVoice(VoiceState::Error);
    events_.onError(status);
  }
}

void MyAiClient::onWebSocketText(const std::string& message) {
  VoiceEvent event;
  Status status = codec_.parseVoiceEvent(message, event);
  if (!status.ok()) {
    events_.onError(status);
    return;
  }
  if (event.type == "session.ready") {
    voiceReconnectAttempts_ = 0;
    setVoice(VoiceState::Ready);
  } else if (event.type == "vad.state") {
    // VAD is advisory; button ownership remains local and half duplex.
  } else if (event.type == "asr.partial") {
    events_.onTranscript(event.text, false);
  } else if (event.type == "asr.final") {
    events_.onTranscript(event.text, true);
    const LocalTranscriptDecision decision = localCommands_.inspect(event.text);
    if (decision.handled) {
      events_.onLocalCommand(decision.commandName, event.text);
      // A higher-level runtime may already have emitted response.create while
      // still claiming local interception to prevent this client from sending
      // a duplicate. Preserve Thinking until the real TTS/response.done event.
      setVoice(decision.responseAlreadyRequested
                   ? VoiceState::Thinking
                   : VoiceState::Ready);
    } else {
      status = requestResponse(event.text);
      if (!status.ok()) events_.onError(status);
      else setVoice(VoiceState::Thinking);
    }
  } else if (event.type == "llm.delta") {
    events_.onAssistantText(event.text, false);
  } else if (event.type == "llm.done") {
    events_.onAssistantText(event.text, true);
  } else if (event.type == "tts.start") {
    status = audio_.begin(event.sampleRateHz ? event.sampleRateHz
                                            : kVoiceSampleRateHz,
                          event.channels ? event.channels : kVoiceChannels);
    if (!status.ok()) events_.onError(status);
    else setVoice(VoiceState::Speaking);
  } else if (event.type == "tts.stop") {
    status = audio_.end();
    if (!status.ok()) events_.onError(status);
  } else if (event.type == "response.done") {
    // Some compatible gateways omit llm.done but include the final text on
    // response.done. The application owns de-duplication when both arrive.
    if (!event.text.empty()) events_.onAssistantText(event.text, true);
    setVoice(VoiceState::Ready);
  } else if (event.type == "action.execute") {
    events_.onVoiceAction(event);
  } else if (event.type == "error") {
    status = Status(ErrorCode::Protocol, 0,
                    event.code.empty() ? event.message : event.code);
    setVoice(VoiceState::Error);
    events_.onError(status);
  }
}

void MyAiClient::onWebSocketBinary(const uint8_t* bytes, size_t length) {
  if (voiceState_ != VoiceState::Speaking || !bytes || length == 0) return;
  Status status = audio_.write(bytes, length);
  if (!status.ok()) {
    audio_.abort();
    setVoice(VoiceState::Error);
    events_.onError(status);
  }
}

void MyAiClient::onWebSocketClosed(int, const std::string&) {
  voiceSocketOpen_ = false;
  audio_.abort();
  voiceStreamId_.clear();
  if (voiceClosing_) {
    voiceClosing_ = false;
    return;
  }
  ++voiceReconnectAttempts_;
  setVoice(VoiceState::Error);
  Status status(ErrorCode::Transport, 0, "MyAI voice socket closed",
                suggestedVoiceReconnectDelayMs());
  events_.onError(status);
}

uint32_t MyAiClient::suggestedVoiceReconnectDelayMs() const {
  return reconnectDelayMs(voiceReconnectAttempts_);
}

uint32_t MyAiClient::reconnectDelayMs(uint8_t attempt) {
  const uint8_t bounded = std::min<uint8_t>(attempt, 6);
  const uint32_t delay = 1000U << bounded;
  return std::min<uint32_t>(delay, 60000U);
}

bool MyAiClient::isPublicGatewayUrl(const std::string& url) {
  HttpsEndpoint endpoint;
  return EndpointPolicy::parseHttpsUrl(url, endpoint).ok();
}

}  // namespace myai
}  // namespace inkloop
