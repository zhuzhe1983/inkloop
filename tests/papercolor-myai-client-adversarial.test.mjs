import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const source = new URL(
  "../firmware/m5-papercolor/lib/InkloopMyAi/src/",
  import.meta.url,
);

test("MyAiClient fails closed across credentials, route classification, endpoint security, and output caps", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-myai-adversarial-"));
  const harnessPath = join(directory, "adversarial.cpp");
  const executablePath = join(directory, "adversarial");
  const harness = String.raw`
#include <cassert>
#include <map>
#include <string>
#include <vector>

#include "CanonicalJsonCodec.h"
#include "MyAiClient.h"

using namespace inkloop::myai;

static Status storageError() {
  return Status(ErrorCode::Storage, 0, "injected storage failure");
}

struct Clock final : IClock {
  mutable uint64_t now;
  Clock() : now(1000) {}
  uint64_t monotonicMs() const override { return now; }
  std::string utcIso8601() const override { return "2026-08-21T02:00:00Z"; }
};

struct Store final : ICredentialStore {
  CredentialSnapshot value;
  bool failLoad;
  bool failNextLoad;
  bool failInitialize;
  bool failSavePending;
  bool failPromote;
  bool failClearPending;
  bool failClearRuntime;
  bool corruptInitialize;
  bool corruptSavePending;
  bool corruptPromote;
  bool corruptClearPending;
  bool partialClearPending;
  bool corruptClearRuntime;
  bool corruptNextLoad;
  int pendingClears;
  int runtimeClears;

  Store()
      : failLoad(false), failNextLoad(false), failInitialize(false),
        failSavePending(false), failPromote(false), failClearPending(false),
        failClearRuntime(false), corruptInitialize(false),
        corruptSavePending(false), corruptPromote(false),
        corruptClearPending(false), partialClearPending(false),
        corruptClearRuntime(false),
        corruptNextLoad(false), pendingClears(0), runtimeClears(0) {}

  Status load(CredentialSnapshot& output) override {
    if (failLoad || failNextLoad) {
      failNextLoad = false;
      return storageError();
    }
    output = value;
    if (corruptNextLoad) {
      output.deviceToken += "-corrupt";
      corruptNextLoad = false;
    }
    return Status::success();
  }
  Status initializeFingerprintAtomically(const std::string& fingerprint) override {
    if (failInitialize) return storageError();
    if (!corruptInitialize) value.installationFingerprint = fingerprint;
    ++value.generation;
    return Status::success();
  }
  Status savePendingAtomically(const PendingPairing& pending) override {
    if (failSavePending) return storageError();
    if (!corruptSavePending) {
      value.pending = pending;
      value.deviceId = pending.deviceId;
    }
    ++value.generation;
    return Status::success();
  }
  Status promoteBoundAtomically(const std::string& expected,
                                const std::string& deviceId,
                                const std::string& token, bool active) override {
    if (failPromote) return storageError();
    assert(expected == value.pending.pairingToken);
    value.deviceId = deviceId;
    value.deviceToken = corruptPromote ? "wrong-token" : token;
    value.active = active;
    if (!corruptPromote) value.pending = PendingPairing();
    ++value.generation;
    return Status::success();
  }
  Status clearPendingAtomically() override {
    if (failClearPending) return storageError();
    ++pendingClears;
    if (partialClearPending) {
      value.pending.deviceId.clear();
      value.pending.bindingUrl.clear();
      value.pending.expiresAt.clear();
    } else if (!corruptClearPending) {
      value.pending = PendingPairing();
      if (value.deviceToken.empty()) value.deviceId.clear();
    }
    ++value.generation;
    return Status::success();
  }
  Status clearRuntimeCredentialAtomically() override {
    if (failClearRuntime) return storageError();
    ++runtimeClears;
    if (!corruptClearRuntime) {
      value.deviceId.clear();
      value.deviceToken.clear();
      value.active = false;
    }
    ++value.generation;
    return Status::success();
  }
};

struct Security final : IEndpointSecurity {
  std::string rejectContains;
  std::string reason;
  int calls;
  Security() : calls(0) {}
  Status validatePublicTlsEndpoint(const std::string& url) override {
    ++calls;
    if (!rejectContains.empty() && url.find(rejectContains) != std::string::npos)
      return Status(ErrorCode::Security, 0, reason);
    return Status::success();
  }
};

struct Http final : IHttpTransport {
  Clock& clock;
  std::vector<HttpRequest> requests;
  std::string forcedPath;
  std::string forcedError;
  std::string pairingDeviceId;
  std::string pairingToken;
  std::string pairingBindingUrl;
  std::string pairingExpiresAt;
  std::string gatewayToken;
  std::string sessionId;
  std::string selectedGatewayId;
  int forcedStatus;
  bool authorized;

  explicit Http(Clock& next)
      : clock(next), pairingDeviceId("654321"), pairingToken("pair-token"),
        pairingBindingUrl("https://myai.mess.host/?device_code=654321#devices"),
        pairingExpiresAt("2026-08-22T02:00:00Z"),
        gatewayToken("gateway-token"), sessionId("session-1"),
        selectedGatewayId("fast"), forcedStatus(0), authorized(true) {}

  Status perform(const HttpRequest& request, HttpResponse& response) override {
    assert(request.tlsPeerVerificationRequired);
    assert(request.rejectPrivateResolvedAddresses);
    assert(!request.redirectsAllowed);
    requests.push_back(request);
    if (!forcedPath.empty() && request.url.find(forcedPath) != std::string::npos) {
      response.status = forcedStatus;
      response.body = std::string("{\"error\":\"") + forcedError + "\"}";
      forcedPath.clear();
      return Status::success();
    }
    if (request.method == "HEAD") {
      response.status = 204;
      clock.now += 8;
      return Status::success();
    }
    response.status = 200;
    if (request.url.find("/healthz") != std::string::npos) {
      response.body = "{\"ok\":true}";
    } else if (request.url.find("/devices/pairing/start") != std::string::npos) {
      response.body = std::string("{\"device_id\":\"") + pairingDeviceId +
          "\",\"app_id\":\"inkloop\",\"status\":\"pending\",\"pairing_token\":\"" +
          pairingToken + "\",\"binding_url\":\"" + pairingBindingUrl +
          "\",\"expires_at\":\"" + pairingExpiresAt + "\"}";
    } else if (request.url.find("/devices/pairing/status") != std::string::npos) {
      response.body = "{\"device_id\":\"654321\",\"app_id\":\"inkloop\",\"status\":\"claimed\",\"bound\":true,\"device_token\":\"device-token\",\"device\":{\"active\":true},\"expires_at\":\"2026-08-22T02:00:00Z\"}";
    } else if (request.url.find("/devices/check") != std::string::npos) {
      response.body = authorized
          ? "{\"authorized\":true,\"device\":{\"active\":true}}"
          : "{\"authorized\":false,\"device\":{\"active\":false}}";
    } else if (request.url.find("/model-preferences") != std::string::npos) {
      response.body = "{\"provider_profile_id\":\"profile\"}";
    } else if (request.url.find("/client/sessions/select") != std::string::npos) {
      response.body = std::string("{\"gateway_token\":\"") + gatewayToken +
          "\",\"gateway\":{\"id\":\"" + selectedGatewayId +
          "\",\"base_url\":\"https://fast.example\",\"ping_url\":\"https://fast.example/ping\",\"status\":\"available\"}}";
    } else if (request.url.find("/client/sessions/heartbeat") != std::string::npos ||
               request.url.find("/client/sessions/disconnect") != std::string::npos ||
               request.url.find("/gateway/sessions/start") != std::string::npos) {
      response.body = "{\"session\":{\"status\":\"active\"}}";
    } else if (request.url.find("/client/sessions") != std::string::npos) {
      response.status = 201;
      response.body = std::string("{\"session\":{\"id\":\"") + sessionId +
          "\"},\"probe_token\":\"probe\",\"gateways\":[{\"id\":\"fast\",\"base_url\":\"https://fast.example\",\"ping_url\":\"https://fast.example/ping\",\"status\":\"available\"}]}";
    } else if (request.url.find("/aigc/generate") != std::string::npos) {
      response.body = "{\"provider\":\"gateway\",\"model\":\"t2i\",\"prompt_id\":\"prompt-1\",\"status\":\"queued\"}";
    } else {
      assert(false && "unexpected HTTP route");
    }
    return Status::success();
  }
};

struct WebSocket final : IWebSocketTransport {
  int connects;
  WebSocket() : connects(0) {}
  Status connect(const std::string&, const std::map<std::string, std::string>&,
                 IWebSocketListener& listener) override {
    ++connects;
    listener.onWebSocketOpen();
    return Status::success();
  }
  Status sendText(const std::string&) override { return Status::success(); }
  Status sendBinary(const uint8_t*, size_t) override { return Status::success(); }
  void close(uint16_t, const std::string&) override {}
};

struct Audio final : IAudioSink {
  Status begin(uint32_t, uint8_t) override { return Status::success(); }
  Status write(const uint8_t*, size_t) override { return Status::success(); }
  Status end() override { return Status::success(); }
  void abort() override {}
};

struct Local final : ILocalTranscriptInterceptor {
  LocalTranscriptDecision inspect(const std::string&) override {
    return LocalTranscriptDecision();
  }
};

struct Events final : IMyAiEvents {
  int errors;
  int pairingReady;
  ActivationState lastActivation;
  Status lastActivationStatus;
  Events() : errors(0), pairingReady(0),
             lastActivation(ActivationState::Unconfigured) {}
  void onActivationState(ActivationState state, const Status& status) override {
    lastActivation = state;
    lastActivationStatus = status;
  }
  void onPairingReady(const PairingView&) override { ++pairingReady; }
  void onVoiceState(VoiceState) override {}
  void onTranscript(const std::string&, bool) override {}
  void onLocalCommand(const std::string&, const std::string&) override {}
  void onVoiceAction(const VoiceEvent&) override {}
  void onAigcState(AigcState, const std::string&) override {}
  void onError(const Status&) override { ++errors; }
};

struct Sink final : IImageSink {
  bool aborted;
  bool committed;
  Sink() : aborted(false), committed(false) {}
  Status begin(const AigcOutputMetadata&) override { return Status::success(); }
  Status write(const uint8_t*, size_t) override { return Status::success(); }
  Status commit(AigcOutputMetadata&) override { committed = true; return Status::success(); }
  void abort() override { aborted = true; }
};

struct Output final : IAigcOutputTransport {
  size_t encodedBytes;
  size_t decodedBytes;
  bool payloadComplete;
  int calls;
  Output() : encodedBytes(4), decodedBytes(1), payloadComplete(true), calls(0) {}
  Status postAndDecodeBase64(const HttpRequest& request, size_t encodedCap,
                             size_t decodedCap, IImageSink& sink,
                             AigcOutputMetadata& metadata) override {
    ++calls;
    assert(request.tlsPeerVerificationRequired);
    assert(request.rejectPrivateResolvedAddresses);
    assert(!request.redirectsAllowed);
    assert(encodedCap == 32 && decodedCap == 16);
    metadata.promptId = "prompt-1";
    metadata.filename = "paper.png";
    metadata.contentType = "image/png";
    assert(sink.begin(metadata).ok());
    if (encodedBytes > encodedCap) {
      sink.abort();
      return Status(ErrorCode::TooLarge, 0, "encoded output exceeds cap");
    }
    if (!payloadComplete) {
      sink.abort();
      return Status(ErrorCode::Protocol, 0, "truncated base64 output");
    }
    if (decodedBytes > decodedCap) {
      sink.abort();
      return Status(ErrorCode::TooLarge, 0, "decoded output exceeds cap");
    }
    std::vector<uint8_t> bytes(decodedBytes, 1);
    if (!bytes.empty()) assert(sink.write(&bytes[0], bytes.size()).ok());
    return sink.commit(metadata);
  }
};

struct Env {
  Clock clock;
  Store store;
  Security security;
  Http http;
  WebSocket ws;
  Output output;
  CanonicalJsonCodec codec;
  Audio audio;
  Local local;
  Events events;
  ClientConfig config;
  MyAiClient* client;

  Env() : http(clock), client(NULL) {
    config.installationFingerprint = "02:00:00:AA:BB:CC";
  }
  void construct() {
    client = new MyAiClient(config, http, ws, output, security, store, codec,
                            clock, audio, local, events);
  }
  ~Env() { delete client; }
  void seedFingerprint() {
    store.value.installationFingerprint = config.installationFingerprint;
  }
  void seedPending() {
    seedFingerprint();
    store.value.deviceId = "654321";
    store.value.pending.deviceId = "654321";
    store.value.pending.pairingToken = "pair-token";
    store.value.pending.bindingUrl = "https://myai.mess.host/bind";
    store.value.pending.expiresAt = "2026-08-22T02:00:00Z";
  }
  void seedBound() {
    seedFingerprint();
    store.value.deviceId = "654321";
    store.value.deviceToken = "device-token";
    store.value.active = true;
  }
};

static Status start(Env& env) {
  PairingView pairing;
  return env.client->startPairing("654321", pairing);
}

int main() {
  {
    CanonicalJsonCodec codec;
    assert(codec.parseErrorCode("{\"error\":\"not found\"}") == "not found");
    assert(codec.parseErrorCode(
        "{\"error\":{\"code\":\"not_found\",\"message\":\"missing\"}}") ==
        "not_found");
    assert(codec.parseErrorCode("{\"code\":\"app_not_found\"}") ==
        "app_not_found");
    assert(codec.parseErrorCode(
        std::string("{\"error\":\"") + std::string(129, 'x') + "\"}").empty());
    assert(codec.parseErrorCode(std::string(4097, 'x')).empty());
    assert(codec.parseErrorCode("{\"error\":\"not/found\"}").empty());
  }
  {
    Env env; env.config.macAddress = "papercolor-c151-0cda43858428";
    env.construct();
    assert(env.client->initialize().code == ErrorCode::InvalidArgument);
    assert(env.http.requests.empty());
  }
  {
    Env env; env.seedFingerprint(); env.construct();
    assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/start";
    env.http.forcedStatus = 404;
    env.http.forcedError = "not found";
    const Status missingApp = start(env);
    assert(missingApp.code == ErrorCode::AppNotRegistered);
    assert(missingApp.httpStatus == 404);
    assert(missingApp.detail == "app_not_registered");
    assert(env.store.value.pending.empty());
    assert(env.client->activationState() == ActivationState::Unconfigured);
  }
  {
    Env env; env.store.failLoad = true; env.construct();
    assert(env.client->initialize().code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.store.failInitialize = true; env.construct();
    assert(env.client->initialize().code == ErrorCode::Storage);
  }
  {
    Env env; env.store.corruptInitialize = true; env.construct();
    assert(env.client->initialize().code == ErrorCode::Storage);
  }
  {
    Env env; env.seedBound(); env.store.value.deviceId = "12x456"; env.construct();
    assert(env.client->initialize().code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.seedFingerprint(); env.store.value.deviceId = "654321";
    env.store.value.active = true; env.construct();
    assert(env.client->initialize().code == ErrorCode::Storage);
  }
  {
    Env env; env.seedBound(); env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->activationState() == ActivationState::Bound);
    assert(env.events.lastActivation == ActivationState::Bound);
    assert(env.events.lastActivationStatus.ok());
    assert(env.store.value.deviceId == "654321");
    assert(env.store.value.deviceToken == "device-token");
    assert(env.store.value.active);
    assert(env.http.requests.empty());
  }
  {
    Env env; env.seedBound(); env.store.value.active = false; env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->activationState() == ActivationState::PaymentRequired);
    assert(env.events.lastActivation == ActivationState::PaymentRequired);
    assert(env.events.lastActivationStatus.code == ErrorCode::PaymentRequired);
    assert(env.events.lastActivationStatus.httpStatus == 402);
    assert(env.events.lastActivationStatus.detail == "bound MyAI device is inactive");
    assert(env.store.value.deviceId == "654321");
    assert(env.store.value.deviceToken == "device-token");
    assert(!env.store.value.active);
    assert(env.store.runtimeClears == 0 && env.store.pendingClears == 0);
    assert(env.http.requests.empty());
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    env.store.failSavePending = true;
    assert(start(env).code == ErrorCode::Storage);
    const size_t requests = env.http.requests.size();
    assert(start(env).code == ErrorCode::Storage);
    assert(env.http.requests.size() == requests && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    env.store.failNextLoad = true;
    assert(start(env).code == ErrorCode::Storage);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    env.store.corruptSavePending = true;
    assert(start(env).code == ErrorCode::Storage);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    assert(start(env).ok()); env.store.failPromote = true; bool bound = true;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage && !bound);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    assert(start(env).ok()); env.store.failNextLoad = true; bool bound = true;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage && !bound);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    assert(start(env).ok()); env.store.corruptPromote = true; bool bound = true;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage && !bound);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.failClearPending = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 401;
    env.http.forcedError = "unauthorized"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.store.value.pending.valid());
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.failNextLoad = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 401;
    env.http.forcedError = "unauthorized"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
    assert(start(env).code == ErrorCode::Storage);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.corruptClearPending = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.store.value.pending.valid());
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.partialClearPending = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.store.value.pending.pairingToken == "pair-token");
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.failNextLoad = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
    assert(start(env).code == ErrorCode::Storage);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.store.failClearRuntime = true;
    env.http.forcedPath = "/devices/check"; env.http.forcedStatus = 401;
    env.http.forcedError = "unauthorized"; bool authorized = false;
    assert(env.client->checkAuthorization(authorized).code == ErrorCode::Storage);
    assert(env.store.value.deviceToken == "device-token");
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.store.failNextLoad = true;
    env.http.forcedPath = "/devices/check"; env.http.forcedStatus = 401;
    env.http.forcedError = "unauthorized"; bool authorized = false;
    assert(env.client->checkAuthorization(authorized).code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.store.corruptClearRuntime = true; env.http.authorized = false;
    bool authorized = true;
    assert(env.client->checkAuthorization(authorized).code == ErrorCode::Storage);
    assert(!authorized && env.store.value.deviceToken == "device-token");
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.failNextLoad = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.store.corruptNextLoad = true;
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Storage);
    assert(env.client->activationState() == ActivationState::Error);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::RecoveryRequired);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 409;
    env.http.forcedError = "some_other_conflict"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Protocol);
    assert(env.client->activationState() == ActivationState::Pairing);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/devices/check"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required"; bool authorized = false;
    assert(env.client->checkAuthorization(authorized).code == ErrorCode::RecoveryRequired);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/healthz"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required";
    assert(env.client->health().code == ErrorCode::Protocol);
    assert(env.client->activationState() == ActivationState::Unconfigured);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/model-preferences"; env.http.forcedStatus = 409;
    env.http.forcedError = "device_credential_recovery_required";
    assert(env.client->connectVoice().code == ErrorCode::Protocol);
    assert(env.client->activationState() == ActivationState::Bound);
    assert(env.store.value.deviceToken == "device-token" && env.store.runtimeClears == 0);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::PairingExpired);
    assert(!env.store.value.pending.valid() && env.store.pendingClears == 1);
  }
  {
    Env env; env.seedPending(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/status"; env.http.forcedStatus = 410;
    env.http.forcedError = "some_other_gone"; bool bound = false;
    assert(env.client->pollPairing(bound).code == ErrorCode::Protocol);
    assert(env.store.value.pending.valid() && env.store.pendingClears == 0);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/devices/check"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired"; bool authorized = false;
    assert(env.client->checkAuthorization(authorized).code == ErrorCode::Protocol);
    assert(env.store.value.deviceToken == "device-token" && env.store.runtimeClears == 0);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/gateway/sessions/start"; env.http.forcedStatus = 410;
    env.http.forcedError = "pairing_expired";
    assert(env.client->connectVoice().code == ErrorCode::Protocol);
    assert(env.store.value.deviceToken == "device-token" && env.store.runtimeClears == 0);
  }
  {
    Env env; env.seedFingerprint(); env.construct(); assert(env.client->initialize().ok());
    env.http.forcedPath = "/pairing/start"; env.http.forcedStatus = 400;
    env.http.forcedError = "invalid_input";
    assert(start(env).code == ErrorCode::Conflict);
    assert(!env.store.value.pending.valid() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.http.pairingDeviceId = "12x456";
    env.construct(); assert(env.client->initialize().ok());
    assert(start(env).code == ErrorCode::Protocol);
    assert(!env.store.value.pending.valid() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.http.pairingToken.clear();
    env.construct(); assert(env.client->initialize().ok());
    assert(start(env).code == ErrorCode::Protocol);
    assert(env.store.value.pending.empty() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.http.pairingBindingUrl.clear();
    env.construct(); assert(env.client->initialize().ok());
    assert(start(env).code == ErrorCode::Protocol);
    assert(env.store.value.pending.empty() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.http.pairingExpiresAt.clear();
    env.construct(); assert(env.client->initialize().ok());
    assert(start(env).code == ErrorCode::Protocol);
    assert(env.store.value.pending.empty() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedFingerprint(); env.http.pairingDeviceId = "123456";
    env.construct(); assert(env.client->initialize().ok());
    assert(start(env).code == ErrorCode::Protocol);
    assert(!env.store.value.pending.valid() && env.events.pairingReady == 0);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    const size_t requests = env.http.requests.size();
    assert(start(env).code == ErrorCode::Conflict);
    assert(env.http.requests.size() == requests);
  }
  {
    Env env; env.seedFingerprint(); env.security.rejectContains = "myai.mess.host";
    env.security.reason = "resolved private address"; env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->health().code == ErrorCode::Security);
    assert(env.http.requests.empty());
  }
  {
    Env env; env.seedFingerprint(); env.security.rejectContains = "myai.mess.host";
    env.security.reason = "TLS hostname verification unavailable"; env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->health().code == ErrorCode::Security);
    assert(env.http.requests.empty());
  }
  {
    Env env; env.seedBound(); env.security.rejectContains = "fast.example";
    env.security.reason = "resolved private address"; env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->connectVoice().code == ErrorCode::NoGateway);
    for (size_t index = 0; index < env.http.requests.size(); ++index)
      assert(env.http.requests[index].method != "HEAD");
  }
  {
    Env env; env.seedBound(); env.http.gatewayToken.clear(); env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->connectVoice().code == ErrorCode::Protocol);
    assert(env.ws.connects == 0);
    for (size_t index = 0; index < env.http.requests.size(); ++index) {
      assert(env.http.requests[index].url.find("/gateway/sessions/start") ==
          std::string::npos);
    }
  }
  {
    Env env; env.seedBound(); env.http.sessionId.clear(); env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->connectVoice().code == ErrorCode::Protocol);
    assert(env.ws.connects == 0);
  }
  {
    Env env; env.seedBound(); env.http.selectedGatewayId.clear(); env.construct();
    assert(env.client->initialize().ok());
    assert(env.client->connectVoice().code == ErrorCode::Protocol);
    assert(env.ws.connects == 0);
  }
  {
    Env env; env.seedBound(); env.construct(); assert(env.client->initialize().ok());
    ImageRequest image; image.prompt = "paper";
    image.maxEncodedBytes = 32; image.maxDecodedBytes = 16;
    AigcGenerateResponse generated;
    assert(env.client->startImage(image, generated).ok());
    AigcOutputRef ref; ref.filename = "paper.png"; ref.nodeId = "9"; ref.type = "output";
    AigcOutputMetadata metadata;
    Sink encoded; env.output.encodedBytes = 33; env.output.decodedBytes = 1;
    assert(env.client->downloadImage("prompt-1", ref, image, encoded, metadata).code == ErrorCode::TooLarge);
    assert(encoded.aborted && !encoded.committed);
    Sink decoded; env.output.encodedBytes = 32; env.output.decodedBytes = 17;
    assert(env.client->downloadImage("prompt-1", ref, image, decoded, metadata).code == ErrorCode::TooLarge);
    assert(decoded.aborted && !decoded.committed);
    Sink exact; env.output.encodedBytes = 32; env.output.decodedBytes = 16;
    assert(env.client->downloadImage("prompt-1", ref, image, exact, metadata).ok());
    assert(!exact.aborted && exact.committed);
    Sink truncated; env.output.encodedBytes = 16; env.output.decodedBytes = 4;
    env.output.payloadComplete = false;
    assert(env.client->downloadImage("prompt-1", ref, image, truncated, metadata).code == ErrorCode::Protocol);
    assert(truncated.aborted && !truncated.committed && env.output.calls == 4);
  }
  return 0;
}
`;

  try {
    await writeFile(harnessPath, harness);
    const includePath = new URL(source).pathname;
    const compile = spawnSync(
      "c++",
      [
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        `-I${includePath}`,
        harnessPath,
        join(includePath, "CanonicalJsonCodec.cpp"),
        join(includePath, "MyAiClient.cpp"),
        "-o",
        executablePath,
      ],
      { encoding: "utf8" },
    );
    assert.equal(compile.status, 0, compile.stderr || compile.stdout);
    const run = spawnSync(executablePath, [], { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr || run.stdout);

    const sanitizedPath = join(directory, "adversarial-sanitized");
    const sanitizedCompile = spawnSync(
      "c++",
      [
        "-std=c++11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        `-I${includePath}`,
        harnessPath,
        join(includePath, "CanonicalJsonCodec.cpp"),
        join(includePath, "MyAiClient.cpp"),
        "-o",
        sanitizedPath,
      ],
      { encoding: "utf8" },
    );
    assert.equal(
      sanitizedCompile.status,
      0,
      sanitizedCompile.stderr || sanitizedCompile.stdout,
    );
    const sanitizedRun = spawnSync(sanitizedPath, [], {
      encoding: "utf8",
      env: {
        ...process.env,
        ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
        UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
      },
    });
    assert.equal(
      sanitizedRun.status,
      0,
      sanitizedRun.stderr || sanitizedRun.stdout,
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
