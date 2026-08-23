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
#include "inkloop/voice_aigc_handoff_policy.hpp"
#include "inkloop/voice_aigc_intent_policy.hpp"

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
  std::string pairingErrorMessage;
  std::string aigcStatusPromptId = "prompt-1";

  explicit Http(HttpMode value) : mode(value) {}

  Status perform(const HttpRequest& request, HttpResponse& response) override {
    response.status = 200;
    if (request.url.find("/devices/pairing/status") != std::string::npos) {
      ++pairingPolls;
      assert(request.headers.find("Authorization") == request.headers.end());
      assert(request.body.find("pairing-secret") != std::string::npos);
      if (mode == HttpMode::PairError) {
        response.status = pairingErrorStatus;
        response.body = "{\"error\":{\"code\":\"" + pairingErrorCode +
                        "\",\"message\":\"" + pairingErrorMessage +
                        "\"}}";
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
    } else if (request.url.find("/client/sessions/heartbeat") !=
               std::string::npos) {
      assert(mode == HttpMode::Voice);
      assert(authorization != request.headers.end());
      assert(request.body.find("\"session_id\":\"session-1\"") !=
             std::string::npos);
      assert(request.body.find("\"gateway_id\":\"fast\"") !=
             std::string::npos);
      response.body = "{\"session\":{\"status\":\"active\"}}";
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
    } else if (request.url.find("/gateway/v1/aigc/status") !=
               std::string::npos) {
      ++business;
      response.body = "{\"prompt_id\":\"" + aigcStatusPromptId +
          "\",\"status\":\"complete\",\"outputs\":[{"
          "\"node_id\":\"9\",\"filename\":\"paper.png\","
          "\"subfolder\":\"\",\"type\":\"output\"}]}";
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
    assert(headers.size() == 3);
    assert(headers.at("Authorization") == "Bearer probe-secret");
    assert(headers.at("X-Device-ID") == "123456");
    assert(headers.at("X-Device-MAC") == "AA:BB:CC:DD:EE:FF");
    for (const auto& header : headers) {
      assert(header.second.find("device-secret") == std::string::npos);
      assert(header.second.find("pairing-secret") == std::string::npos);
    }
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
  int calls = 0;
  Status postAndDecodeBase64(const HttpRequest&, size_t, size_t, IImageSink&,
                             AigcOutputMetadata&) override {
    ++calls;
    return Status(ErrorCode::InvalidState);
  }
};

struct Audio final : IAudioSink {
  int begins = 0;
  int writes = 0;
  int ends = 0;
  int aborts = 0;
  std::vector<uint32_t> rates;
  std::vector<uint8_t> channelsSeen;
  std::vector<uint8_t> played;
  std::vector<size_t> writeSizes;
  Status begin(uint32_t rate, uint8_t channels) override {
    rates.push_back(rate);
    channelsSeen.push_back(channels);
    ++begins;
    return Status::success();
  }
  Status write(const uint8_t* bytes, size_t length) override {
    assert(bytes && length != 0 && (length & 1U) == 0);
    played.insert(played.end(), bytes, bytes + length);
    writeSizes.push_back(length);
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
  int errors = 0;
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
  void onError(const Status&) override { ++errors; }
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
  http.pairingErrorMessage = "Center diagnostic preserved";
  WebSocket socket;
  MyAiClient client(config(), http, probes, socket, output, security, store,
                    codec, clock, audio, local, events);
  assert(client.initialize().ok());
  bool bound = false;
  const Status status = client.pollPairing(bound);
  assert(status.code == expected && status.httpStatus == httpStatus);
  assert(status.detail.find(error) != std::string::npos);
  assert(status.detail.find("message=Center diagnostic preserved") !=
         std::string::npos);
}

int main() {
  CanonicalJsonCodec codec("contract-test");
  GatewayLease schema_lease;
  schema_lease.sessionId = "session-1";
  schema_lease.providerProfileId = "voice-profile";
  std::printf("SESSION_UPDATE:%s\n",
              codec.sessionUpdateMessage(
                  schema_lease, "123456", "system prompt").c_str());
  assert(codec.parseErrorDiagnostic(
      "{\"error\":{\"code\":\"subscription_required\","
      "\"message\":\"Activate this device in MyAI\"}}") ==
      "Activate this device in MyAI");
  assert(codec.parseErrorDiagnostic(
      "{\"code\":\"unauthorized\",\"detail\":\"credential revoked\"}") ==
      "credential revoked");
  assert(codec.parseErrorDiagnostic(
      "{\"error\":{\"message\":\"nested fallback\"},"
      "\"message\":\"top-level first\"}") == "top-level first");
  assert(codec.parseErrorDiagnostic(
      "{\"message\":\"forged\\nline\"}").empty());
  assert(codec.parseErrorDiagnostic(
      std::string("{\"message\":\"") + std::string(257, 'x') +
      "\"}").empty());
  assert(codec.parseErrorDiagnostic(
      "{\"message\":\"Bearer device-secret\"}").empty());

  VoiceEvent action;
  assert(codec.parseVoiceEvent(
      "{\"type\":\"action.execute\",\"payload\":{"
      "\"kind\":\"myai.aigc.generate\","
      "\"payload\":{\"prompt\":\"first prompt\","
      "\"original_request\":\"请生成图片\"},"
      "\"arguments\":{\"count\":2},"
      "\"input\":{\"prompts\":[\"first prompt\",\"second prompt\"]}}}",
      action).ok());
  assert(action.kind == "myai.aigc.generate");
  assert(action.prompt == "first prompt");
  assert(action.originalRequest == "请生成图片");
  assert(action.prompts ==
         std::vector<std::string>({"first prompt", "second prompt"}));
  assert(action.requestedImageCount == 2);

  assert(!inkloop::nextVoiceAigcIntentArmed(false, false, "好的"));
  assert(inkloop::nextVoiceAigcIntentArmed(false, false, "请生成一张图片"));
  assert(inkloop::nextVoiceAigcIntentArmed(true, true, "好的"));
  assert(!inkloop::nextVoiceAigcIntentArmed(true, true, "不用了"));
  assert(!inkloop::nextVoiceAigcIntentArmed(true, false, "确认"));
  const uint64_t confirmed = inkloop::voiceAigcRequestFingerprint("好的");
  assert(confirmed != 0U);
  assert(confirmed == inkloop::voiceAigcRequestFingerprint(" 好的。 "));
  assert(confirmed !=
         inkloop::voiceAigcRequestFingerprint("请生成一张图片"));
  assert(inkloop::voiceAigcRequestFingerprint("") == 0U);
  const auto explicit_intent = inkloop::nextVoiceAigcIntentCorrelation(
      {}, false, "请生成一张东方明珠图片");
  assert(explicit_intent.armed);
  assert(explicit_intent.explicit_request ==
         inkloop::voiceAigcRequestFingerprint("请生成一张东方明珠图片"));
  const auto short_confirmation =
      inkloop::nextVoiceAigcIntentCorrelation(
          explicit_intent, true, "好的");
  assert(short_confirmation.armed);
  assert(short_confirmation.explicit_request ==
         explicit_intent.explicit_request);
  assert(short_confirmation.latest_utterance == confirmed);
  assert(inkloop::voiceAigcIntentCorrelationMatches(
      short_confirmation, explicit_intent.explicit_request));
  assert(inkloop::voiceAigcIntentCorrelationMatches(
      short_confirmation, confirmed));
  assert(!inkloop::voiceAigcIntentCorrelationMatches(
      short_confirmation,
      inkloop::voiceAigcRequestFingerprint("请生成另一张图片")));
  assert(!inkloop::nextVoiceAigcIntentCorrelation(
      short_confirmation, true, "不用了").armed);
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
    http.aigcStatusPromptId = "prompt-other";
    AigcStatusResponse mismatched;
    const Status mismatch = client.pollImage(generated.promptId, mismatched);
    assert(mismatch.code == ErrorCode::Protocol);
    assert(mismatched.promptId.empty() && mismatched.status.empty() &&
           mismatched.outputs.empty());
    assert(output.calls == 0);
    assert(http.preferences == 0);
    assert(http.business == 2);
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
    VoiceHeartbeatWork heartbeat;
    assert(client.prepareVoiceHeartbeat(heartbeat).ok());
    assert(heartbeat.valid());
    assert(heartbeat.request.timeoutMs == 5000U);
    assert(heartbeat.request.maxResponseBytes == 4096U);
    assert(heartbeat.request.headers.at("Authorization") ==
           "Bearer device-secret");
    HttpResponse heartbeatResponse;
    const Status heartbeatTransport =
        http.perform(heartbeat.request, heartbeatResponse);
    heartbeat.clearRequestSensitive();
    assert(!heartbeat.valid() && heartbeat.correlationValid());
    assert(client.completeVoiceHeartbeat(
        heartbeat, heartbeatTransport, heartbeatResponse).ok());
    heartbeat.clearSensitive();
    assert(!heartbeat.correlationValid());
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
    // sample_rate_hz is authoritative. There is no provider/model field with
    // which a client could safely distinguish true 16 kHz from Qwen3 audio.
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"channels\":1}}");
    assert(voiceAudio.begins == 0 && voiceEvents.errors == 1);
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":16000,\"channels\":1}}");
    const uint8_t downlink[] = {3, 0, 4, 0};
    // WebSocket message boundaries are not PCM frame boundaries. Preserve one
    // partial sample between callbacks and emit the exact original byte stream.
    client.onWebSocketBinary(downlink, 1);
    client.onWebSocketBinary(downlink + 1, 2);
    client.onWebSocketBinary(downlink + 3, 1);
    client.onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}");
    client.onWebSocketText("{\"type\":\"response.done\",\"payload\":{}}");
    assert(client.voiceState() == VoiceState::Ready);
    assert(!client.voiceResponseInFlight());
    assert(!inkloop::voiceBlocksAigcHandoff(
        client.voiceState(), client.voiceResponseInFlight(), false));

    // tts.stop ends only one segment. A provider may spend longer than the
    // former 1500 ms fallback window preparing its next segment, so silence
    // cannot make the response Ready or allow the exclusive AIGC handoff.
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":24000,\"channels\":1}}");
    client.onWebSocketBinary(downlink, sizeof(downlink));
    client.onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}");
    clock.now = 10000;
    assert(client.voiceState() == VoiceState::Speaking);
    assert(client.voiceResponseInFlight());
    assert(inkloop::voiceBlocksAigcHandoff(
        client.voiceState(), client.voiceResponseInFlight(), false));
    bool aigcHandoffTriggered = false;
    clock.now += 1501;
    if (!inkloop::voiceBlocksAigcHandoff(
            client.voiceState(), client.voiceResponseInFlight(), false)) {
      aigcHandoffTriggered = true;
    }
    assert(!aigcHandoffTriggered);
    assert(client.voiceState() == VoiceState::Speaking);
    assert(client.voiceResponseInFlight());

    // The late adjacent segment still belongs to the same response. It must
    // play normally even though its tts.start arrived after the old timeout.
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":24000,\"channels\":1}}");
    assert(client.voiceState() == VoiceState::Speaking);
    assert(client.voiceResponseInFlight());
    client.onWebSocketBinary(downlink, sizeof(downlink));
    client.onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}");
    assert(client.voiceResponseInFlight());
    client.onWebSocketText("{\"type\":\"response.done\",\"payload\":{}}");
    assert(client.voiceState() == VoiceState::Ready);
    assert(!client.voiceResponseInFlight());
    assert(!inkloop::voiceBlocksAigcHandoff(
        client.voiceState(), client.voiceResponseInFlight(), false));

    // Without response.done, fail closed regardless of elapsed time. The
    // official protocol uses response.done for completed, failed, or cancelled
    // turns; cancellation or socket failure is the other terminal path.
    client.onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":16000,\"channels\":1}}");
    client.onWebSocketBinary(downlink, sizeof(downlink));
    client.onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}");
    clock.now = 30000;
    clock.now += 120000;
    assert(client.voiceResponseInFlight());
    assert(inkloop::voiceBlocksAigcHandoff(
        client.voiceState(), client.voiceResponseInFlight(), false));
    client.onWebSocketText("{\"type\":\"response.done\",\"payload\":{}}");
    assert(client.voiceState() == VoiceState::Ready);
    assert(!client.voiceResponseInFlight());

    assert(voiceEvents.partialAsr == 1 && voiceEvents.finalAsr == 1);
    assert(voiceEvents.partialAssistant == 1 && voiceEvents.finalAssistant == 1);
    assert(voiceAudio.rates ==
           std::vector<uint32_t>({16000, 24000, 24000, 16000}));
    assert(voiceAudio.channelsSeen ==
           std::vector<uint8_t>({1, 1, 1, 1}));
    assert(voiceAudio.ends == 4);
    assert(voiceAudio.writeSizes[0] == 2 && voiceAudio.writeSizes[1] == 2);
    assert(std::vector<uint8_t>(voiceAudio.played.begin(),
                                voiceAudio.played.begin() + 4) ==
           std::vector<uint8_t>(downlink, downlink + sizeof(downlink)));
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
      "-I", join(myai, "include/inkloop/myai"),
      "-I", join(myai, "include"),
      "-I", join(product, "include"), source,
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
    const output = execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      encoding: "utf8",
    });
    const session = output.split("\n").find(
      (line) => line.startsWith("SESSION_UPDATE:"),
    );
    assert.ok(session);
    const value = JSON.parse(session.slice("SESSION_UPDATE:".length));
    assert.equal(value.type, "session.update");
    assert.equal(
      value.payload.voice_assistant.metadata.mcp_tools[0]
        .input_schema.properties.count.maximum,
      8,
    );
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
  assert.match(
    source,
    /kAigcTimeoutMs = 5U \* 60U \* 1000U \+ 30U \* 1000U/,
  );
  assert.match(aigc, /due\(now, aigc_deadline_ms_\)/);
  assert.match(aigc, /due\(now, next_aigc_poll_ms_\)/);
  assert.match(aigc, /next_aigc_poll_ms_ = now \+ kAigcPollMs/);
  assert.match(aigc, /AigcAlbumSink[\s\S]*downloadImage/);
  assert.match(aigc, /safeAigcFailureState\(status\)/);
  assert.match(source, /voiceBlocksAigcHandoff\([\s\S]*voiceResponseInFlight/);
  assert.doesNotMatch(
    source,
    /voiceResponseCompletionDue|completeVoiceResponseAfterTtsStop/,
  );
  assert.match(source, /nextVoiceAigcIntentCorrelation\(/);
  assert.match(
    source,
    /onTranscript[\s\S]*voice_aigc_action_deadline_ms_[\s\S]*nextVoiceAigcIntentCorrelation/,
  );
  assert.match(
    source,
    /onVoiceAction[\s\S]*const bool armed = voice_aigc_action_armed_[\s\S]*aigc\.rejected_no_explicit_voice_intent/,
  );
  assert.match(
    source,
    /onVoiceAction[\s\S]*voiceAigcRequestFingerprint\(action\.originalRequest\)[\s\S]*voiceAigcIntentCorrelationMatches[\s\S]*aigc\.rejected_mismatched_voice_request/,
  );
  assert.match(source, /sanitizeDiagnosticDetail\(status\.detail\)/);
  assert.doesNotMatch(source, /detail=status\.detail|status\.detail\.c_str\(\)/);
  assert.match(source, /PortalRunVoiceHeartbeat/);
  assert.match(source, /scheduleVoiceHeartbeat\(\)/);
  assert.match(source, /performVoiceHeartbeat\(envelope\)/);
  assert.match(source, /completeVoiceHeartbeat/);
  assert.doesNotMatch(
    source.slice(
      source.indexOf("void NativeVoiceService::networkTick"),
      source.indexOf("void NativeVoiceService::startPairingIfNeeded"),
    ),
    /client_->heartbeatVoice\(\)/,
  );
});
