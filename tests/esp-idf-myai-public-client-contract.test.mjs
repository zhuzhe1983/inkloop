import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");

const harness = String.raw`
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "CanonicalJsonCodec.h"
#include "GatewayProbeContract.h"
#include "MyAiClient.h"

using namespace inkloop::myai;

struct Clock final : IClock {
  uint64_t now = 1000;
  uint64_t monotonicMs() const override { return now; }
  std::string utcIso8601() const override { return "2026-08-23T00:00:00Z"; }
};

struct Security final : IEndpointSecurity {
  Status validatePublicTlsEndpoint(const std::string& url) override {
    assert(url.rfind("https://", 0) == 0);
    return Status::success();
  }
  Status validatePublicEndpoint(const std::string& url) override {
    assert(url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0);
    return Status::success();
  }
};

struct Store final : ICredentialStore {
  CredentialSnapshot value;
  int promotions = 0;
  int pendingClears = 0;

  explicit Store(bool pending = false) {
    value.generation = 1;
    value.installationFingerprint = "install-fingerprint";
    value.deviceId = "123456";
    if (pending) {
      value.pending.deviceId = value.deviceId;
      value.pending.pairingToken = "pairing-secret";
      value.pending.bindingUrl = "https://myai.mess.host/bind/123456";
      value.pending.expiresAt = "2026-08-23T00:10:00Z";
    } else {
      value.deviceToken = "device-secret";
      value.active = true;
    }
  }
  Status load(CredentialSnapshot& output) override {
    output = value;
    return Status::success();
  }
  Status initializeFingerprintAtomically(const std::string&) override {
    return Status::success();
  }
  Status savePendingAtomically(const PendingPairing& pending) override {
    value.deviceId = pending.deviceId;
    value.pending = pending;
    value.deviceToken.clear();
    value.active = false;
    return Status::success();
  }
  Status promoteBoundAtomically(const std::string& expected,
                                const std::string& deviceId,
                                const std::string& deviceToken,
                                bool active) override {
    assert(expected == value.pending.pairingToken);
    ++promotions;
    value.deviceId = deviceId;
    value.pending.clearSensitive();
    value.pending = PendingPairing();
    value.deviceToken = deviceToken;
    value.active = active;
    return Status::success();
  }
  Status clearPendingAtomically() override {
    ++pendingClears;
    value.pending.clearSensitive();
    value.pending = PendingPairing();
    if (value.deviceToken.empty()) value.deviceId.clear();
    return Status::success();
  }
  Status clearRuntimeCredentialAtomically() override {
    value.deviceId.clear();
    value.pending.clearSensitive();
    value.pending = PendingPairing();
    value.deviceToken.clear();
    value.active = false;
    return Status::success();
  }
};

enum class HttpMode { PairSuccess, PairError, Voice, Image, ImageGatewayUnauthorized };

struct Http final : IHttpTransport {
  HttpMode mode;
  int pairingPolls = 0;
  int preferences = 0;
  int business = 0;
  int disconnects = 0;
  int pairingErrorStatus = 0;
  std::string pairingErrorCode;

  explicit Http(HttpMode value) : mode(value) {}

  Status perform(const HttpRequest& request, HttpResponse& response) override {
    response.status = 200;
    if (request.url.find("/devices/pairing/status") != std::string::npos) {
      ++pairingPolls;
      assert(request.headers.find("Authorization") == request.headers.end());
      assert(request.body.find("pairing-secret") != std::string::npos);
      if (mode == HttpMode::PairError) {
        response.status = pairingErrorStatus;
        response.body = "{\"error\":\"" + pairingErrorCode + "\"}";
      } else {
        response.body =
            "{\"device_id\":\"123456\",\"app_id\":\"inkloop\","
            "\"status\":\"bound\",\"expires_at\":\"later\","
            "\"bound\":true,\"device_token\":\"newest-device-secret\","
            "\"device\":{\"active\":true}}";
      }
      return Status::success();
    }
    const auto authorization = request.headers.find("Authorization");
    if (request.url.find("/devices/check") != std::string::npos) {
      assert(authorization != request.headers.end());
      assert(authorization->second == "Bearer device-secret" ||
             authorization->second == "Bearer newest-device-secret");
      assert(request.body.find("pairing-secret") == std::string::npos);
      response.body = "{\"authorized\":true,\"device\":{\"active\":true}}";
    } else if (request.url.find("/model-preferences") != std::string::npos) {
      ++preferences;
      assert(false && "session routing must not prefetch model preferences");
    } else if (request.url.find("/client/sessions/select") != std::string::npos) {
      assert(request.body.find("\"gateway_id\":\"fast\"") !=
             std::string::npos);
      response.body =
          "{\"gateway_token\":\"gateway-secret\",\"gateway\":{"
          "\"id\":\"fast\",\"base_url\":\"http://183.128.44.67:18090\","
          "\"ping_url\":\"http://183.128.44.67:18090/gateway/v1/gateway-ping\"}}";
    } else if (request.url.find("/client/sessions/disconnect") !=
               std::string::npos) {
      ++disconnects;
      assert(authorization != request.headers.end());
      response.body = "{}";
    } else if (request.url.find("/client/sessions") != std::string::npos) {
      assert(request.body.find("\"app_id\":\"inkloop\"") !=
             std::string::npos);
      if (mode == HttpMode::Voice) {
        assert(request.body.find("\"asr\"") != std::string::npos);
        assert(request.body.find("\"vad\"") != std::string::npos);
      } else {
        assert(request.body.find("\"aigc\"") != std::string::npos);
      }
      response.status = 201;
      response.body =
          "{\"session\":{\"id\":\"session-1\"},"
          "\"probe_token\":\"probe-secret\",\"gateways\":[{"
          "\"id\":\"fast\",\"base_url\":\"http://183.128.44.67:18090\","
          "\"ping_url\":\"http://183.128.44.67:18090/gateway/v1/gateway-ping\"}]}";
    } else if (request.url.find("/gateway/v1/gateway/sessions/start") !=
               std::string::npos) {
      assert(request.url.rfind("http://183.128.44.67:18090/", 0) == 0);
      assert(!request.tlsPeerVerificationRequired);
      assert(request.plaintextPublicGatewayAllowed);
      assert(request.headers.at("X-Gateway-Session-Token") ==
             "gateway-secret");
      assert(request.headers.at("Authorization") == "Bearer gateway-secret");
      assert(request.headers.at("X-Gateway-Session-ID") == "session-1");
      assert(request.headers.at("X-Gateway-ID") == "fast");
      response.body = mode == HttpMode::Voice
          ? "{\"provider_profile_id\":\"voice-profile\"}" : "{}";
    } else if (request.url.find("/gateway/v1/aigc/generate") !=
               std::string::npos) {
      ++business;
      assert(mode == HttpMode::Image ||
             mode == HttpMode::ImageGatewayUnauthorized);
      assert(request.url.rfind("http://183.128.44.67:18090/", 0) == 0);
      assert(!request.tlsPeerVerificationRequired);
      assert(request.plaintextPublicGatewayAllowed);
      assert(authorization != request.headers.end() &&
             authorization->second == "Bearer gateway-secret");
      assert(request.headers.at("X-Gateway-Session-Token") ==
             "gateway-secret");
      assert(request.body.find("pairing-secret") == std::string::npos);
      if (mode == HttpMode::ImageGatewayUnauthorized) {
        response.status = 401;
        response.body = "{\"error\":\"gateway_token_expired\"}";
      } else {
        response.body = "{\"prompt_id\":\"prompt-1\",\"status\":\"queued\"}";
      }
    } else {
      assert(false && "unexpected HTTP route");
    }
    return Status::success();
  }
};

struct Probes final : IGatewayProbeSet {
  Status probeConcurrent(const std::vector<GatewayCandidate>& candidates,
                         const std::map<std::string, std::string>& headers,
                         uint32_t deadline,
                         std::vector<GatewayProbe>& results) override {
    assert(candidates.size() == 1);
    assert(headers.size() == 1);
    assert(headers.at("Authorization") == "Bearer probe-secret");
    assert(headers.find("X-Device-ID") == headers.end());
    assert(headers.find("X-Device-MAC") == headers.end());
    assert(deadline == GatewayProbeContract::kTotalDeadlineMs);
    GatewayProbe probe;
    probe.gatewayId = "fast";
    probe.ok = true;
    probe.latencyMs = 4;
    probe.checkedAt = "2026-08-23T00:00:00Z";
    results = {probe};
    return Status::success();
  }
};

struct WebSocket final : IWebSocketTransport {
  IWebSocketListener* listener = nullptr;
  std::vector<std::string> text;
  size_t binaryBytes = 0;
  int closes = 0;
  Status connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 IWebSocketListener& next) override {
    assert(url.rfind("ws://183.128.44.67:18090/gateway/v1/voice/ws?", 0) == 0);
    assert(url.find("app_id=inkloop") != std::string::npos);
    assert(url.find("session_id=session-1") != std::string::npos);
    assert(url.find("gateway_id=fast") != std::string::npos);
    assert(headers.at("Authorization") == "Bearer gateway-secret");
    assert(headers.at("X-Gateway-Session-Token") == "gateway-secret");
    assert(headers.at("X-Gateway-Session-ID") == "session-1");
    assert(headers.at("X-Gateway-ID") == "fast");
    listener = &next;
    listener->onWebSocketOpen();
    return Status::success();
  }
  Status sendText(const std::string& message) override {
    text.push_back(message);
    return Status::success();
  }
  Status sendBinary(const uint8_t*, size_t length) override {
    binaryBytes += length;
    return Status::success();
  }
  void close(uint16_t, const std::string&) override {
    ++closes;
    listener = nullptr;
  }
};

struct Output final : IAigcOutputTransport {
  Status postAndDecodeBase64(const HttpRequest&, size_t, size_t, IImageSink&,
                             AigcOutputMetadata&) override {
    return Status(ErrorCode::InvalidState);
  }
};

struct Audio final : IAudioSink {
  int begins = 0;
  int writes = 0;
  int ends = 0;
  int aborts = 0;
  Status begin(uint32_t rate, uint8_t channels) override {
    assert(rate == 24000 && channels == 1);
    ++begins;
    return Status::success();
  }
  Status write(const uint8_t* bytes, size_t length) override {
    assert(bytes && length == 4);
    ++writes;
    return Status::success();
  }
  Status end() override { ++ends; return Status::success(); }
  void abort() override { ++aborts; }
};

struct Local final : ILocalTranscriptInterceptor {
  LocalTranscriptDecision inspect(const std::string&) override {
    return LocalTranscriptDecision();
  }
};

struct Events final : IMyAiEvents {
  int partialAsr = 0;
  int finalAsr = 0;
  int partialAssistant = 0;
  int finalAssistant = 0;
  void onActivationState(ActivationState, const Status&) override {}
  void onPairingReady(const PairingView&) override {}
  void onVoiceState(VoiceState) override {}
  void onTranscript(const std::string& text, bool final) override {
    assert(!text.empty());
    final ? ++finalAsr : ++partialAsr;
  }
  void onAssistantText(const std::string& text, bool final) override {
    assert(!text.empty());
    final ? ++finalAssistant : ++partialAssistant;
  }
  void onLocalCommand(const std::string&, const std::string&) override {}
  void onVoiceAction(const VoiceEvent&) override {}
  void onAigcState(AigcState, const std::string&) override {}
  void onError(const Status&) override {}
};

struct Sink final : IImageSink {
  Status begin(const AigcOutputMetadata&) override { return Status::success(); }
  Status write(const uint8_t*, size_t) override { return Status::success(); }
  Status commit(AigcOutputMetadata&) override { return Status::success(); }
  void abort() override {}
};

ClientConfig config() {
  ClientConfig value;
  value.installationFingerprint = "install-fingerprint";
  value.macAddress = "AA:BB:CC:DD:EE:FF";
  return value;
}

void checkPairingPromotion(CanonicalJsonCodec& codec, Security& security,
                           Clock& clock, Probes& probes, Output& output,
                           Audio& audio, Local& local, Events& events) {
  Store store(true);
  Http http(HttpMode::PairSuccess);
  WebSocket socket;
  MyAiClient client(config(), http, probes, socket, output, security, store,
                    codec, clock, audio, local, events);
  assert(client.initialize().ok());
  bool bound = false;
  const Status promoted = client.pollPairing(bound);
  if (!promoted.ok() || !bound) {
    std::fprintf(stderr, "promotion failed code=%u http=%d detail=%s bound=%d\\n",
                 static_cast<unsigned>(promoted.code), promoted.httpStatus,
                 promoted.detail.c_str(), bound ? 1 : 0);
  }
  assert(promoted.ok() && bound);
  assert(store.promotions == 1 && store.value.pending.empty());
  assert(store.value.deviceToken == "newest-device-secret");
  assert(client.pollPairing(bound).code == ErrorCode::InvalidState);
  assert(http.pairingPolls == 1);
  bool authorized = false;
  assert(client.checkAuthorization(authorized).ok() && authorized);
}

void checkPairingDiagnostic(int httpStatus, const char* error,
                            ErrorCode expected, CanonicalJsonCodec& codec,
                            Security& security, Clock& clock, Probes& probes,
                            Output& output, Audio& audio, Local& local,
                            Events& events) {
  Store store(true);
  Http http(HttpMode::PairError);
  http.pairingErrorStatus = httpStatus;
  http.pairingErrorCode = error;
  WebSocket socket;
  MyAiClient client(config(), http, probes, socket, output, security, store,
                    codec, clock, audio, local, events);
  assert(client.initialize().ok());
  bool bound = false;
  const Status status = client.pollPairing(bound);
  assert(status.code == expected && status.httpStatus == httpStatus);
  assert(status.detail.find(error) != std::string::npos);
}

int main() {
  CanonicalJsonCodec codec("contract-test");
  Security security;
  Clock clock;
  Probes probes;
  Output output;
  Audio audio;
  Local local;
  Events events;

  checkPairingPromotion(codec, security, clock, probes, output, audio, local,
                        events);
  checkPairingDiagnostic(401, "invalid_pairing_token", ErrorCode::Unauthorized,
                         codec, security, clock, probes, output, audio, local,
                         events);
  checkPairingDiagnostic(402, "subscription_required",
                         ErrorCode::PaymentRequired, codec, security, clock,
                         probes, output, audio, local, events);
  checkPairingDiagnostic(409, "device_credential_recovery_required",
                         ErrorCode::RecoveryRequired, codec, security, clock,
                         probes, output, audio, local, events);
  checkPairingDiagnostic(410, "pairing_expired", ErrorCode::PairingExpired,
                         codec, security, clock, probes, output, audio, local,
                         events);

  {
    Store store;
    Http http(HttpMode::Image);
    WebSocket socket;
    MyAiClient client(config(), http, probes, socket, output, security, store,
                      codec, clock, audio, local, events);
    assert(client.initialize().ok());
    ImageRequest request;
    request.prompt = "high contrast poster";
    AigcGenerateResponse generated;
    assert(client.startImage(request, generated).ok());
    assert(generated.promptId == "prompt-1");
    assert(http.preferences == 0);
    assert(http.business == 1);
    assert(client.disconnectImage().ok() && http.disconnects == 1);
  }

  {
    Store store;
    Http http(HttpMode::Voice);
    WebSocket socket;
    Audio voiceAudio;
    Events voiceEvents;
    MyAiClient client(config(), http, probes, socket, output, security, store,
                      codec, clock, voiceAudio, local, voiceEvents);
    assert(client.initialize().ok());
    assert(client.connectVoice().ok());
    assert(http.preferences == 0);
    assert(socket.text.size() == 1);
    assert(socket.text[0].find("\"type\":\"session.update\"") !=
           std::string::npos);
    assert(socket.text[0].find("\"provider_profile_id\":\"voice-profile\"") !=
           std::string::npos);
    client.onWebSocketText("{\"type\":\"session.ready\",\"payload\":{}}");
    assert(client.beginVoiceTurn("turn-1").ok());
    const uint8_t uplink[] = {1, 0, 2, 0};
    assert(client.sendPcm16(uplink, sizeof(uplink)).ok());
    assert(client.endVoiceTurn().ok());
    assert(socket.binaryBytes == sizeof(uplink));
    assert(socket.text[1].find("audio.start") != std::string::npos);
    assert(socket.text[2].find("audio.stop") != std::string::npos);
    client.onWebSocketText("{\"type\":\"vad.state\",\"payload\":{\"state\":\"speech\"}}");
    client.onWebSocketText("{\"type\":\"asr.partial\",\"payload\":{\"text\":\"hel\"}}");
    client.onWebSocketText("{\"type\":\"asr.final\",\"payload\":{\"text\":\"hello\"}}");
    client.onWebSocketText("{\"type\":\"llm.delta\",\"payload\":{\"text\":\"world\"}}");
    client.onWebSocketText("{\"type\":\"llm.done\",\"payload\":{\"text\":\"world\"}}");
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":24000,\"channels\":1}}");
    const uint8_t downlink[] = {3, 0, 4, 0};
    client.onWebSocketBinary(downlink, sizeof(downlink));
    client.onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}");
    client.onWebSocketText("{\"type\":\"response.done\",\"payload\":{}}");
    assert(voiceEvents.partialAsr == 1 && voiceEvents.finalAsr == 1);
    assert(voiceEvents.partialAssistant == 1 && voiceEvents.finalAssistant == 1);
    assert(voiceAudio.begins == 1 && voiceAudio.writes == 1 &&
           voiceAudio.ends == 1);
    assert(client.disconnectVoice("cancel_generation").ok());
    const int writesBeforeStale = voiceAudio.writes;
    client.onWebSocketBinary(downlink, sizeof(downlink));
    assert(voiceAudio.writes == writesBeforeStale);
    assert(socket.closes == 1 && voiceAudio.aborts >= 1);
  }

  {
    Store store;
    Http http(HttpMode::ImageGatewayUnauthorized);
    WebSocket socket;
    MyAiClient client(config(), http, probes, socket, output, security, store,
                      codec, clock, audio, local, events);
    assert(client.initialize().ok());
    ImageRequest request;
    request.prompt = "gateway expiry boundary";
    AigcGenerateResponse generated;
    const Status expired = client.startImage(request, generated);
    assert(expired.code == ErrorCode::Unauthorized && expired.httpStatus == 401);
    assert(store.value.deviceToken == "device-secret" && store.value.active);
  }
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-myai-contract-"));
  try {
    const source = join(scratch, "contract.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(myai, "include/inkloop/myai"), source,
      join(myai, "CanonicalJsonCodec.cpp"),
      join(myai, "EndpointPolicy.cpp"),
      join(myai, "GatewayProbeContract.cpp"),
      join(myai, "MyAiClient.cpp"),
      "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    }
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("MyAI public pairing, Voice and AIGC flows pass strict C++17", () => {
  buildAndRun(false);
});

test("MyAI public flow survives ASan and UBSan", () => {
  buildAndRun(true);
});

test("native product keeps AIGC cadence, diagnostics and button ACK bounded", () => {
  const source = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
  const button = source.slice(
    source.indexOf("AdmissionResult NativeVoiceService::enqueueTopButton"),
    source.indexOf("AdmissionResult NativeVoiceService::enqueueAlbumOrdinal"),
  );
  assert.match(button, /supervisor_|post\(/);
  assert.doesNotMatch(
    button,
    /client_->|http_|wss_|credential|chat_log_|album_store_|renderer|display|esp_http|open\(|fopen|stat\(/,
  );

  const aigc = source.slice(
    source.indexOf("void NativeVoiceService::serviceAigc"),
    source.indexOf("void NativeVoiceService::finishAigc"),
  );
  assert.match(aigc, /phase == AigcPhase::Poll/);
  assert.match(source, /kAigcTimeoutMs = 3U \* 60U \* 1000U/);
  assert.match(aigc, /due\(now, aigc_deadline_ms_\)/);
  assert.match(aigc, /due\(now, next_aigc_poll_ms_\)/);
  assert.match(aigc, /next_aigc_poll_ms_ = now \+ kAigcPollMs/);
  assert.match(aigc, /AigcAlbumSink[\s\S]*downloadImage/);
  assert.match(aigc, /safeAigcFailureState\(status\)/);
  assert.match(source, /bool explicitImageIntent\(/);
  assert.match(
    source,
    /onTranscript[\s\S]*voice_aigc_action_armed_ = explicitImageIntent\(text\)/,
  );
  assert.match(
    source,
    /onVoiceAction[\s\S]*const bool armed = voice_aigc_action_armed_[\s\S]*aigc\.rejected_no_explicit_voice_intent/,
  );
  const diagnostic = source.slice(
    source.indexOf("std::string safeAigcFailureState"),
    source.indexOf("bool sixDigits"),
  );
  for (const marker of ["token", "authorization", "bearer", "http://", "https://"])
    assert.match(diagnostic, new RegExp(`\"${marker.replace("/", "\\/")}`));
});
