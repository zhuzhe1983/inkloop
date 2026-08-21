import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const librarySource = new URL(
  "../firmware/m5-papercolor/lib/InkloopMyAi/src/",
  import.meta.url,
);

test("PaperColor MyAiClient executes pairing, routed voice, local interception, and AIGC under C++11", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-myai-client-"));
  const harnessPath = join(temporaryDirectory, "myai_client_test.cpp");
  const executablePath = join(temporaryDirectory, "myai_client_test");
  const harness = String.raw`
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "CanonicalJsonCodec.h"
#include "MyAiClient.h"

using namespace inkloop::myai;

struct FakeClock final : IClock {
  mutable uint64_t now = 1000;
  uint64_t monotonicMs() const override { return now; }
  std::string utcIso8601() const override { return "2026-08-21T01:00:00Z"; }
};

struct FakeSecurity final : IEndpointSecurity {
  std::vector<std::string> validated;
  Status validatePublicTlsEndpoint(const std::string& url) override {
    validated.push_back(url);
    return Status::success();
  }
};

struct FakeStore final : ICredentialStore {
  CredentialSnapshot value;
  int promoteCalls = 0;
  int runtimeClears = 0;

  Status load(CredentialSnapshot& output) override { output = value; return Status::success(); }
  Status initializeFingerprintAtomically(const std::string& fingerprint) override {
    value.installationFingerprint = fingerprint; ++value.generation; return Status::success();
  }
  Status savePendingAtomically(const PendingPairing& pending) override {
    value.pending = pending; value.deviceId = pending.deviceId; ++value.generation;
    return Status::success();
  }
  Status promoteBoundAtomically(const std::string& expected, const std::string& deviceId,
                                const std::string& token, bool active) override {
    assert(expected == value.pending.pairingToken);
    value.deviceId = deviceId; value.deviceToken = token; value.active = active;
    value.pending = PendingPairing(); ++value.generation; ++promoteCalls;
    return Status::success();
  }
  Status clearPendingAtomically() override {
    value.pending = PendingPairing();
    if (value.deviceToken.empty()) value.deviceId.clear();
    ++value.generation; return Status::success();
  }
  Status clearRuntimeCredentialAtomically() override {
    value.deviceId.clear(); value.deviceToken.clear(); value.active = false;
    ++value.generation; ++runtimeClears;
    return Status::success();
  }
};

struct FakeHttp final : IHttpTransport {
  FakeClock& clock;
  std::vector<HttpRequest> requests;
  int forcedStatus = 0;
  explicit FakeHttp(FakeClock& value) : clock(value) {}

  Status perform(const HttpRequest& request, HttpResponse& response) override {
    assert(request.tlsPeerVerificationRequired);
    assert(request.rejectPrivateResolvedAddresses);
    assert(!request.redirectsAllowed);
    requests.push_back(request);
    if (request.method == "HEAD") {
      response.status = 204;
      clock.now += request.url.find("fast.example") != std::string::npos ? 12 : 90;
      return Status::success();
    }
    if (forcedStatus != 0) {
      response.status = forcedStatus; forcedStatus = 0; response.body = "{\"error\":\"forced\"}";
      return Status::success();
    }
    response.status = 200;
    if (request.url.find("/healthz") != std::string::npos) {
      response.body = "{\"ok\":true}";
    } else if (request.url.find("/devices/pairing/start") != std::string::npos) {
      response.body = "{\"device_id\":\"654321\",\"app_id\":\"inkloop\",\"status\":\"pending\",\"pairing_token\":\"private-pairing-token\",\"binding_url\":\"https://myai.mess.host/?device_code=654321#devices\",\"expires_at\":\"2026-08-22T01:00:00Z\"}";
    } else if (request.url.find("/devices/pairing/status") != std::string::npos) {
      // Reproduce a stale pairing-level payment bit after the owned device
      // has already become active. The bound token and current device state
      // are authoritative and must terminate polling.
      response.body = "{\"device_id\":\"654321\",\"app_id\":\"inkloop\",\"status\":\"claimed\",\"bound\":true,\"payment_required\":true,\"device_token\":\"private-device-token\",\"device\":{\"active\":true},\"expires_at\":\"2026-08-22T01:00:00Z\"}";
    } else if (request.url.find("/devices/check") != std::string::npos) {
      response.body = "{\"authorized\":true,\"device\":{\"active\":true}}";
    } else if (request.url.find("/model-preferences") != std::string::npos) {
      response.body = "{\"provider_profile_id\":\"inkloop-profile\"}";
    } else if (request.url.find("/client/sessions/select") != std::string::npos) {
      response.body = "{\"gateway_token\":\"ram-only-gateway-token\",\"gateway\":{\"id\":\"fast\",\"base_url\":\"https://fast.example\",\"ping_url\":\"https://fast.example/ping\",\"status\":\"available\"}}";
    } else if (request.url.find("/client/sessions/heartbeat") != std::string::npos ||
               request.url.find("/client/sessions/disconnect") != std::string::npos ||
               request.url.find("/gateway/sessions/start") != std::string::npos) {
      response.body = "{\"session\":{\"status\":\"active\"}}";
    } else if (request.url.find("/client/sessions") != std::string::npos) {
      response.status = 201;
      response.body = "{\"session\":{\"id\":\"sess-1\"},\"probe_token\":\"probe\",\"gateways\":[{\"id\":\"slow\",\"base_url\":\"https://slow.example\",\"ping_url\":\"https://slow.example/ping\",\"status\":\"available\"},{\"id\":\"fast\",\"base_url\":\"https://fast.example\",\"ping_url\":\"https://fast.example/ping\",\"status\":\"available\"},{\"id\":\"private\",\"base_url\":\"http://192.168.1.8:8000\",\"ping_url\":\"http://192.168.1.8/ping\",\"status\":\"available\"}]}";
    } else if (request.url.find("/aigc/generate") != std::string::npos) {
      response.body = "{\"provider\":\"gateway\",\"model\":\"t2i\",\"prompt_id\":\"prompt-1\",\"status\":\"queued\"}";
    } else if (request.url.find("/aigc/status") != std::string::npos) {
      response.body = "{\"provider\":\"gateway\",\"prompt_id\":\"prompt-1\",\"status\":\"complete\",\"outputs\":[{\"node_id\":\"9\",\"filename\":\"paper.png\",\"subfolder\":\"\",\"type\":\"output\"}]}";
    } else if (request.url.find("/combo/voice") != std::string::npos) {
      response.body = "{\"transcript\":\"你好\",\"reply\":\"你好\",\"audio_base64\":\"AAEC\"}";
    } else {
      assert(false && "unexpected HTTP route");
    }
    return Status::success();
  }
};

struct FakeWebSocket final : IWebSocketTransport {
  IWebSocketListener* listener = nullptr;
  std::string url;
  std::map<std::string, std::string> headers;
  std::vector<std::string> texts;
  int binaries = 0;
  Status connect(const std::string& nextUrl,
                 const std::map<std::string, std::string>& nextHeaders,
                 IWebSocketListener& nextListener) override {
    url = nextUrl; headers = nextHeaders; listener = &nextListener; listener->onWebSocketOpen();
    return Status::success();
  }
  Status sendText(const std::string& message) override { texts.push_back(message); return Status::success(); }
  Status sendBinary(const uint8_t*, size_t length) override { assert(length > 0); ++binaries; return Status::success(); }
  void close(uint16_t, const std::string&) override { if (listener) listener->onWebSocketClosed(1000, "closed"); }
};

struct FakeAudio final : IAudioSink {
  int begins = 0, writes = 0, ends = 0, aborts = 0;
  Status begin(uint32_t rate, uint8_t channels) override { assert(rate == 16000 && channels == 1); ++begins; return Status::success(); }
  Status write(const uint8_t*, size_t length) override { assert(length > 0); ++writes; return Status::success(); }
  Status end() override { ++ends; return Status::success(); }
  void abort() override { ++aborts; }
};

struct FakeLocal final : ILocalTranscriptInterceptor {
  LocalTranscriptDecision inspect(const std::string& transcript) override {
    if (transcript == "runtime delegated")
      return LocalTranscriptDecision(true, "voice.runtime", true);
    return LocalTranscriptDecision(transcript == "剩余空间", "storage.free");
  }
};

struct FakeEvents final : IMyAiEvents {
  PairingView pairing;
  int localCommands = 0, actions = 0, errors = 0;
  std::vector<VoiceState> voice;
  void onActivationState(ActivationState, const Status&) override {}
  void onPairingReady(const PairingView& value) override { pairing = value; }
  void onVoiceState(VoiceState value) override { voice.push_back(value); }
  void onTranscript(const std::string&, bool) override {}
  void onLocalCommand(const std::string& name, const std::string&) override {
    assert(name == "storage.free" || name == "voice.runtime"); ++localCommands;
  }
  void onVoiceAction(const VoiceEvent& action) override { assert(action.kind == "aigc.generate"); ++actions; }
  void onAigcState(AigcState, const std::string&) override {}
  void onError(const Status&) override { ++errors; }
};

struct FakeImageSink final : IImageSink {
  bool began = false, committed = false, aborted = false;
  std::vector<uint8_t> bytes;
  Status begin(const AigcOutputMetadata& metadata) override { assert(metadata.contentType == "image/png"); began = true; return Status::success(); }
  Status write(const uint8_t* data, size_t length) override { bytes.insert(bytes.end(), data, data + length); return Status::success(); }
  Status commit(AigcOutputMetadata& metadata) override { metadata.decodedBytes = bytes.size(); committed = true; return Status::success(); }
  void abort() override { aborted = true; }
};

struct FakeOutput final : IAigcOutputTransport {
  Status postAndDecodeBase64(const HttpRequest& request, size_t encodedCap,
                             size_t decodedCap, IImageSink& sink,
                             AigcOutputMetadata& metadata) override {
    assert(request.url == "https://fast.example/gateway/v1/aigc/output");
    assert(request.body.find("\"app_id\":\"inkloop\"") != std::string::npos);
    assert(encodedCap == 4096 && decodedCap == 2048);
    metadata.promptId = "prompt-1"; metadata.filename = "paper.png"; metadata.contentType = "image/png";
    Status status = sink.begin(metadata); if (!status.ok()) return status;
    const uint8_t bytes[] = {0x89, 0x50, 0x4e, 0x47};
    status = sink.write(bytes, sizeof(bytes)); if (!status.ok()) return status;
    return sink.commit(metadata);
  }
};

int main() {
  FakeClock clock;
  FakeStore store;
  FakeHttp http(clock);
  FakeWebSocket ws;
  FakeOutput output;
  FakeSecurity security;
  CanonicalJsonCodec codec;
  FakeAudio audio;
  FakeLocal local;
  FakeEvents events;
  ClientConfig config;
  config.installationFingerprint = "papercolor-c151-0cda43858428";
  config.macAddress = "28:84:85:43:DA:0C";
  config.systemPrompt = "Help the user create 400x600 PaperColor artwork.";
  MyAiClient client(config, http, ws, output, security, store, codec, clock, audio, local, events);

  assert(std::string(kAppId) == "inkloop");
  assert(std::string(kCenterBaseUrl) == "https://myai.mess.host");
  assert(client.initialize().ok());
  assert(client.health().ok());

  PairingView pairing;
  assert(client.startPairing("654321", pairing).ok());
  assert(pairing.onboardingCode == "654321");
  assert(events.pairing.onboardingCode == "654321");
  assert(http.requests.back().body.find("\"app_id\":\"inkloop\"") != std::string::npos);
  assert(http.requests.back().body.find("\"hardware_sku\":\"m5-papercolor-c151\"") != std::string::npos);
  assert(http.requests.back().body.find("\"mac_address\":\"28:84:85:43:DA:0C\"") != std::string::npos);
  assert(http.requests.back().body.find("papercolor-c151-") == std::string::npos);
  PairingView resumed;
  assert(client.pendingPairing(resumed).ok());
  assert(resumed.onboardingCode == pairing.onboardingCode);
  assert(resumed.bindingUrl == pairing.bindingUrl);
  assert(resumed.expiresAt == pairing.expiresAt);
  bool bound = false;
  assert(client.pollPairing(bound).ok() && bound);
  assert(!client.pendingPairing(resumed).ok());
  assert(store.promoteCalls == 1 && store.value.pending.pairingToken.empty());
  assert(store.value.deviceToken == "private-device-token");
  assert(store.value.active);
  const size_t requestsAfterBinding = http.requests.size();
  bool staleBound = true;
  const Status stalePoll = client.pollPairing(staleBound);
  assert(stalePoll.code == ErrorCode::InvalidState && !staleBound);
  assert(http.requests.size() == requestsAfterBinding);

  assert(!MyAiClient::isPublicGatewayUrl("http://192.168.1.8:8000"));
  assert(!MyAiClient::isPublicGatewayUrl("https://localhost:8000"));
  assert(MyAiClient::isPublicGatewayUrl("https://fast.example"));
  assert(MyAiClient::reconnectDelayMs(0) == 1000);
  assert(MyAiClient::reconnectDelayMs(6) == 60000);

  assert(client.connectVoice().ok());
  assert(ws.url.find("mac_address=28%3A84%3A85%3A43%3ADA%3A0C") != std::string::npos);
  bool foundGatewayStart = false;
  bool foundCanonicalDeviceHeader = false;
  for (size_t index = 0; index < http.requests.size(); ++index) {
    const HttpRequest& request = http.requests[index];
    const std::map<std::string, std::string>::const_iterator deviceMac =
        request.headers.find("X-Device-MAC");
    if (deviceMac != request.headers.end()) {
      assert(deviceMac->second == "28:84:85:43:DA:0C");
      foundCanonicalDeviceHeader = true;
    }
    if (request.url == "https://fast.example/api/v1/gateway/sessions/start") {
      foundGatewayStart = true;
      assert(request.headers.at("X-Gateway-Session-Token") == "ram-only-gateway-token");
      assert(request.headers.count("Authorization") == 0);
      assert(request.headers.count("X-Device-ID") == 0);
      assert(request.headers.count("X-Device-MAC") == 0);
      assert(request.body.find("ram-only-gateway-token") == std::string::npos);
    }
  }
  assert(foundGatewayStart);
  assert(foundCanonicalDeviceHeader);
  assert(ws.url.find("wss://fast.example/gateway/v1/voice/ws") == 0);
  assert(ws.headers["Authorization"] == "Bearer private-device-token");
  assert(ws.texts.front().find("\"type\":\"session.update\"") != std::string::npos);
  assert(ws.texts.front().find("\"provider_profile_id\":\"inkloop-profile\"") != std::string::npos);
  assert(ws.texts.front().find("\"auto_response\":false") != std::string::npos);
  assert(ws.texts.front().find("myai.aigc.generate") != std::string::npos);
  ws.listener->onWebSocketText("{\"type\":\"session.ready\",\"payload\":{}}" );
  assert(client.voiceState() == VoiceState::Ready);
  assert(client.beginVoiceTurn("input-1").ok());
  const uint8_t pcm[] = {1, 0, 2, 0};
  assert(client.sendPcm16(pcm, sizeof(pcm)).ok());
  assert(client.endVoiceTurn().ok());
  assert(ws.texts.back().find("\"last_seq\":1") != std::string::npos);

  const size_t beforeLocal = ws.texts.size();
  ws.listener->onWebSocketText("{\"type\":\"asr.final\",\"payload\":{\"text\":\"剩余空间\"}}" );
  assert(events.localCommands == 1 && ws.texts.size() == beforeLocal);
  ws.listener->onWebSocketText("{\"type\":\"asr.final\",\"payload\":{\"text\":\"runtime delegated\"}}" );
  assert(events.localCommands == 2 && client.voiceState() == VoiceState::Thinking);
  ws.listener->onWebSocketText("{\"type\":\"response.done\",\"payload\":{\"status\":\"complete\"}}" );
  assert(client.voiceState() == VoiceState::Ready);
  ws.listener->onWebSocketText("{\"type\":\"asr.final\",\"payload\":{\"text\":\"给我讲个故事\"}}" );
  assert(ws.texts.back().find("\"type\":\"response.create\"") != std::string::npos);
  ws.listener->onWebSocketText("{\"type\":\"tts.start\",\"payload\":{\"sample_rate_hz\":16000,\"channels\":1}}" );
  ws.listener->onWebSocketBinary(pcm, sizeof(pcm));
  ws.listener->onWebSocketText("{\"type\":\"tts.stop\",\"payload\":{}}" );
  ws.listener->onWebSocketText("{\"type\":\"response.done\",\"payload\":{\"status\":\"complete\"}}" );
  assert(audio.begins == 1 && audio.writes == 1 && audio.ends == 1);
  ws.listener->onWebSocketText("{\"type\":\"action.execute\",\"payload\":{\"action_id\":\"a1\",\"kind\":\"aigc.generate\",\"prompt\":\"paper art\"}}" );
  assert(events.actions == 1);
  assert(client.heartbeatVoice().ok());

  std::string transcript, reply, comboAudio;
  assert(client.comboVoice("AAEC", 100, transcript, reply, comboAudio).ok());
  assert(transcript == "你好" && reply == "你好" && comboAudio == "AAEC");

  ImageRequest image;
  image.prompt = "400x600 six-color e-paper artwork";
  image.maxEncodedBytes = 4096;
  image.maxDecodedBytes = 2048;
  AigcGenerateResponse generated;
  assert(client.startImage(image, generated).ok() && generated.promptId == "prompt-1");
  AigcStatusResponse imageStatus;
  assert(client.pollImage(generated.promptId, imageStatus).ok());
  assert(imageStatus.outputs.size() == 1 && imageStatus.outputs[0].filename == "paper.png");
  FakeImageSink sink;
  AigcOutputMetadata metadata;
  assert(client.downloadImage(generated.promptId, imageStatus.outputs[0], image, sink, metadata).ok());
  assert(sink.began && sink.committed && !sink.aborted && metadata.decodedBytes == 4);
  assert(client.disconnectImage().ok());

  assert(client.disconnectVoice().ok());
  // Runtime gateway tokens are neither in the credential store nor persisted by any adapter.
  assert(store.value.deviceToken == "private-device-token");

  FakeStore errorStore;
  errorStore.value.installationFingerprint = config.installationFingerprint;
  errorStore.value.deviceId = "654321";
  errorStore.value.deviceToken = "keep-on-402";
  errorStore.value.active = true;
  FakeHttp errorHttp(clock);
  FakeWebSocket errorWs;
  FakeEvents errorEvents;
  MyAiClient errorClient(config, errorHttp, errorWs, output, security, errorStore, codec, clock,
                         audio, local, errorEvents);
  assert(errorClient.initialize().ok());
  errorHttp.forcedStatus = 402;
  bool authorized = false;
  Status payment = errorClient.checkAuthorization(authorized);
  assert(payment.code == ErrorCode::PaymentRequired);
  assert(payment.httpStatus == 402);
  assert(payment.detail.find("HTTP 402") != std::string::npos);
  assert(payment.detail.find("error=forced") != std::string::npos);
  assert(errorStore.value.deviceToken == "keep-on-402" && errorStore.runtimeClears == 0);
  errorHttp.forcedStatus = 401;
  Status unauthorized = errorClient.checkAuthorization(authorized);
  assert(unauthorized.code == ErrorCode::Unauthorized);
  assert(unauthorized.httpStatus == 401);
  assert(unauthorized.detail.find("HTTP 401") != std::string::npos);
  assert(unauthorized.detail.find("error=forced") != std::string::npos);
  assert(errorStore.value.deviceToken == "keep-on-402");
  assert(errorStore.runtimeClears == 0);
  assert(errorClient.activationState() == ActivationState::RecoveryRequired);
  return 0;
}
`;

  try {
    await writeFile(harnessPath, harness);
    const includePath = new URL(librarySource).pathname;
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
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
});

test("PaperColor MyAI package contains only the third-party public contract", async () => {
  const { readFile } = await import("node:fs/promises");
  const client = await readFile(
    new URL("../firmware/m5-papercolor/lib/InkloopMyAi/src/MyAiClient.cpp", import.meta.url),
    "utf8",
  );
  const codec = await readFile(
    new URL("../firmware/m5-papercolor/lib/InkloopMyAi/src/CanonicalJsonCodec.cpp", import.meta.url),
    "utf8",
  );
  const types = await readFile(
    new URL("../firmware/m5-papercolor/lib/InkloopMyAi/src/MyAiTypes.h", import.meta.url),
    "utf8",
  );
  const combined = `${client}\n${codec}\n${types}`;

  assert.match(types, /kAppId\[\] = "inkloop"/);
  assert.match(types, /kCenterBaseUrl\[\] = "https:\/\/myai\.mess\.host"/);
  assert.match(combined, /\/api\/v1\/devices\/pairing\/start/);
  assert.match(combined, /\/api\/v1\/devices\/pairing\/status/);
  assert.match(combined, /\/api\/v1\/client\/sessions\/select/);
  assert.match(combined, /\/gateway\/v1\/voice\/ws/);
  assert.match(combined, /\/gateway\/v1\/aigc\/output/);
  assert.match(codec, /"auto_response\\":false/);
  assert.doesNotMatch(combined, /myai-flutter-client/);
  assert.doesNotMatch(combined, /127\.0\.0\.1:18080/);
  assert.doesNotMatch(combined, /api\/v1\/ui\/developer/);
  assert.doesNotMatch(combined, /devices\/connect/);
  assert.doesNotMatch(combined, /ComfyUI|LocalAI|Ollama|provider[_-]key/i);
});
