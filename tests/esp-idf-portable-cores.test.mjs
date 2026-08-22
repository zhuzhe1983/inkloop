import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf");
const myai = join(idf, "components/inkloop_myai");
const voice = join(idf, "components/inkloop_voice");

test("portable MyAI and Voice cores compile under strict C++17 and sanitizers", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-idf-core-"));
  const harness = join(scratch, "portable_cores.cpp");
  const binary = join(scratch, "portable_cores");
  writeFileSync(harness, String.raw`
#include <cassert>
#include <string>
#include <vector>

#include "CanonicalJsonCodec.h"
#include "GatewayProbeContract.h"
#include "LocalCommandParser.h"
#include "MyAiClient.h"

using namespace inkloop::myai;

GatewayCandidate candidate(const char* id) {
  GatewayCandidate value;
  value.id = id;
  value.baseUrl = std::string("https://") + id + ".example.test";
  value.pingUrl = value.baseUrl + "/ping";
  return value;
}

GatewayProbe result(const char* id, bool ok, uint32_t latency) {
  GatewayProbe value;
  value.gatewayId = id;
  value.ok = ok;
  value.latencyMs = latency;
  return value;
}

struct Clock final : IClock {
  uint64_t monotonicMs() const override { return 1000; }
  std::string utcIso8601() const override { return "2026-08-22T03:00:00Z"; }
};

struct Security final : IEndpointSecurity {
  Status validatePublicTlsEndpoint(const std::string& url) override {
    assert(url.rfind("https://", 0) == 0);
    return Status::success();
  }
};

struct Store final : ICredentialStore {
  CredentialSnapshot value;
  Store() {
    value.generation = 1;
    value.installationFingerprint = "papercolor-installation";
    value.deviceId = "692639";
    value.deviceToken = "device-token";
    value.active = true;
  }
  Status load(CredentialSnapshot& output) override { output = value; return Status::success(); }
  Status initializeFingerprintAtomically(const std::string&) override { return Status::success(); }
  Status savePendingAtomically(const PendingPairing&) override { return Status::success(); }
  Status promoteBoundAtomically(const std::string&, const std::string&,
                                const std::string&, bool) override { return Status::success(); }
  Status clearPendingAtomically() override { return Status::success(); }
  Status clearRuntimeCredentialAtomically() override { return Status::success(); }
};

struct Http final : IHttpTransport {
  int head_calls = 0;
  std::string selected_body;
  Status perform(const HttpRequest& request, HttpResponse& response) override {
    if (request.method == "HEAD") ++head_calls;
    response.status = 200;
    if (request.url.find("/devices/check") != std::string::npos) {
      response.body = "{\"authorized\":true,\"device\":{\"active\":true}}";
    } else if (request.url.find("/model-preferences") != std::string::npos) {
      response.body = "{\"provider_profile_id\":\"profile\"}";
    } else if (request.url.find("/client/sessions/select") != std::string::npos) {
      selected_body = request.body;
      response.body = "{\"gateway_token\":\"gateway-token\",\"gateway\":{\"id\":\"fast\",\"base_url\":\"https://fast.example.com\",\"ping_url\":\"https://fast.example.com/ping\",\"status\":\"available\"}}";
    } else if (request.url.find("/client/sessions") != std::string::npos) {
      response.status = 201;
      response.body = "{\"session\":{\"id\":\"session-1\"},\"probe_token\":\"probe\",\"gateways\":[{\"id\":\"slow\",\"base_url\":\"https://slow.example.com\",\"ping_url\":\"https://slow.example.com/ping\",\"status\":\"available\"},{\"id\":\"fast\",\"base_url\":\"https://fast.example.com\",\"ping_url\":\"https://fast.example.com/ping\",\"status\":\"available\"}]}";
    } else if (request.url.find("/gateway/sessions/start") != std::string::npos) {
      response.body = "{\"session\":{\"status\":\"active\"}}";
    } else {
      assert(false && "unexpected HTTP route");
    }
    return Status::success();
  }
};

struct Probes final : IGatewayProbeSet {
  int calls = 0;
  Status probeConcurrent(const std::vector<GatewayCandidate>& candidates,
                         const std::map<std::string, std::string>& headers,
                         uint32_t deadline,
                         std::vector<GatewayProbe>& results) override {
    ++calls;
    assert(candidates.size() == 2);
    assert(headers.at("Authorization") == "Bearer device-token");
    assert(deadline == GatewayProbeContract::kTotalDeadlineMs);
    results = {result("slow", true, 90), result("fast", true, 12)};
    return Status::success();
  }
};

struct WebSocket final : IWebSocketTransport {
  std::string url;
  Status connect(const std::string& value,
                 const std::map<std::string, std::string>&,
                 IWebSocketListener& listener) override {
    url = value;
    listener.onWebSocketOpen();
    return Status::success();
  }
  Status sendText(const std::string&) override { return Status::success(); }
  Status sendBinary(const uint8_t*, size_t) override { return Status::success(); }
  void close(uint16_t, const std::string&) override {}
};

struct Output final : IAigcOutputTransport {
  Status postAndDecodeBase64(const HttpRequest&, size_t, size_t, IImageSink&,
                             AigcOutputMetadata&) override {
    return Status(ErrorCode::InvalidState);
  }
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
  void onActivationState(ActivationState, const Status&) override {}
  void onPairingReady(const PairingView&) override {}
  void onVoiceState(VoiceState) override {}
  void onTranscript(const std::string&, bool) override {}
  void onLocalCommand(const std::string&, const std::string&) override {}
  void onVoiceAction(const VoiceEvent&) override {}
  void onAigcState(AigcState, const std::string&) override {}
  void onError(const Status&) override {}
};

int main() {
  const std::vector<GatewayCandidate> candidates{
      candidate("first"), candidate("second"), candidate("third")};
  GatewayCandidate selected;
  std::vector<GatewayProbe> results{
      result("third", false, 1), result("second", true, 10),
      result("first", true, 30)};
  assert(GatewayProbeContract::selectFastest(candidates, results, selected).ok());
  assert(selected.id == "second");

  results = {result("second", true, 10), result("first", true, 10),
             result("third", false, 1)};
  assert(GatewayProbeContract::selectFastest(candidates, results, selected).ok());
  assert(selected.id == "first");

  std::vector<GatewayProbe> incomplete{result("first", true, 1)};
  assert(!GatewayProbeContract::selectFastest(candidates, incomplete, selected).ok());
  std::vector<GatewayProbe> duplicate{
      result("first", true, 1), result("first", true, 2),
      result("third", false, 3)};
  assert(!GatewayProbeContract::selectFastest(candidates, duplicate, selected).ok());
  std::vector<GatewayProbe> unknown{
      result("first", true, 1), result("second", true, 2),
      result("other", true, 3)};
  assert(!GatewayProbeContract::selectFastest(candidates, unknown, selected).ok());
  std::vector<GatewayProbe> none{
      result("first", false, 1), result("second", false, 2),
      result("third", false, 3)};
  const Status no_gateway =
      GatewayProbeContract::selectFastest(candidates, none, selected);
  assert(no_gateway.code == ErrorCode::NoGateway);

  std::vector<GatewayCandidate> duplicate_candidates{
      candidate("same"), candidate("same")};
  assert(!GatewayProbeContract::validateCandidates(duplicate_candidates).ok());
  std::vector<GatewayCandidate> too_many(9, candidate("x"));
  assert(!GatewayProbeContract::validateCandidates(too_many).ok());

  CanonicalJsonCodec codec;
  const std::string pairing =
      codec.pairingStartBody("692639", "28:84:85:43:DA:0C", "PaperColor");
  assert(pairing.find("\"app_id\":\"inkloop\"") != std::string::npos);
  assert(pairing.find("\"device_id\":\"692639\"") != std::string::npos);
  assert(pairing.find("\"hardware_sku\":\"m5-papercolor-c151\"") !=
         std::string::npos);
  CanonicalJsonCodec alternate_codec("mock-minimal");
  const std::string alternate_pairing = alternate_codec.pairingStartBody(
      "692639", "28:84:85:43:DA:0C", "Mock");
  assert(alternate_pairing.find("\"hardware_sku\":\"mock-minimal\"") !=
         std::string::npos);

  inkloop::voice::LocalCommandParser commands;
  const inkloop::voice::ParsedCommand parsed = commands.parse("第二十七张");
  assert(parsed.kind == inkloop::voice::CommandKind::SelectImage);
  assert(parsed.number == 27 && parsed.targetId == "@27");

  Clock clock;
  Security security;
  Store store;
  Http http;
  Probes probes;
  WebSocket websocket;
  Output output;
  Audio audio;
  Local local;
  Events events;
  ClientConfig config;
  config.installationFingerprint = "papercolor-installation";
  config.macAddress = "28:84:85:43:DA:0C";
  MyAiClient client(config, http, probes, websocket, output, security, store,
                    codec, clock, audio, local, events);
  assert(client.initialize().ok());
  assert(client.connectVoice().ok());
  assert(probes.calls == 1);
  assert(http.head_calls == 0);
  assert(http.selected_body.find("\"gateway_id\":\"fast\"") !=
         std::string::npos);
  assert(websocket.url.rfind("wss://fast.example.com/", 0) == 0);
  return 0;
}
`);

  try {
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", join(myai, "include/inkloop/myai"),
      "-I", join(voice, "include/inkloop/voice"),
      harness,
      join(myai, "CanonicalJsonCodec.cpp"),
      join(myai, "EndpointPolicy.cpp"),
      join(myai, "GatewayProbeContract.cpp"),
      join(myai, "MyAiClient.cpp"),
      join(voice, "AudioPromptController.cpp"),
      join(voice, "LocalCommandParser.cpp"),
      join(voice, "VoiceRuntime.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], {
      env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" },
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

test("ported core is independent from Arduino and gateway probing is one bounded set", () => {
  const files = [
    join(myai, "CanonicalJsonCodec.cpp"),
    join(myai, "EndpointPolicy.cpp"),
    join(myai, "GatewayProbeContract.cpp"),
    join(myai, "MyAiClient.cpp"),
    join(myai, "include/inkloop/myai/MyAiAdapters.h"),
    join(voice, "AudioPromptController.cpp"),
    join(voice, "LocalCommandParser.cpp"),
    join(voice, "VoiceRuntime.cpp"),
  ];
  const combined = files.map((path) => readFileSync(path, "utf8")).join("\n");
  assert.doesNotMatch(combined,
    /#include\s*[<"]Arduino\.h[>"]|M5Unified|HTTPClient|WebServer|WiFiManager|Preferences/);
  assert.match(combined, /probeConcurrent/);
  assert.match(combined, /kTotalDeadlineMs/);
  assert.doesNotMatch(combined,
    /for \(size_t index = 0; index < requested\.gateways\.size\(\); \+\+index\)/);
});

test("unchanged portable contracts and voice state machines match their audited source", () => {
  const pairs = [
    ["firmware/m5-papercolor/lib/InkloopMyAi/src/MyAiTypes.h",
     "firmware/inkloop-idf/components/inkloop_myai/include/inkloop/myai/MyAiTypes.h"],
    ["firmware/m5-papercolor/lib/InkloopMyAi/src/CanonicalJsonCodec.h",
     "firmware/inkloop-idf/components/inkloop_myai/include/inkloop/myai/CanonicalJsonCodec.h"],
    // The native codec intentionally has a stricter JSON key scanner: the
    // public pairing response may contain status="bound" before the `bound`
    // boolean. Workstream 29 is client-only and must not edit Arduino sources.
    ["firmware/m5-papercolor/lib/InkloopVoice/src/VoiceTypes.h",
     "firmware/inkloop-idf/components/inkloop_voice/include/inkloop/voice/VoiceTypes.h"],
    ["firmware/m5-papercolor/lib/InkloopVoice/src/VoiceAdapters.h",
     "firmware/inkloop-idf/components/inkloop_voice/include/inkloop/voice/VoiceAdapters.h"],
    ["firmware/m5-papercolor/lib/InkloopVoice/src/LocalCommandParser.cpp",
     "firmware/inkloop-idf/components/inkloop_voice/LocalCommandParser.cpp"],
    ["firmware/m5-papercolor/lib/InkloopVoice/src/VoiceRuntime.cpp",
     "firmware/inkloop-idf/components/inkloop_voice/VoiceRuntime.cpp"],
  ];
  for (const [legacy, ported] of pairs) {
    assert.equal(readFileSync(join(repo, ported), "utf8"),
                 readFileSync(join(repo, legacy), "utf8"), ported);
  }
});
