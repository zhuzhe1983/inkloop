import { execFileSync } from "node:child_process";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const audio = join(repo, "firmware/inkloop-idf/components/inkloop_audio");
const audioIdf = join(repo, "firmware/inkloop-idf/components/inkloop_audio_idf");
const myaiIdf = join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include "inkloop/duplex_pcm_bridge.hpp"

using namespace inkloop;

int main() {
  std::array<uint8_t, 32768> playback{};
  std::array<uint8_t, 16384> capture{};
  DuplexPcmBridgeCore bridge(playback.data(), playback.size(), 8192, 20480,
                             12288, capture.data(), capture.size(), 4096,
                             12288, 2048);
  assert(bridge.valid());
  const uint32_t pg = bridge.beginPlayback(24000, 1);
  assert(pg != 0 && bridge.canAcceptPlayback(12288));
  std::array<uint8_t, 12288> tts{};
  for (size_t i = 0; i < tts.size(); ++i) tts[i] = uint8_t(i);
  assert(bridge.pushPlayback(pg, tts.data(), tts.size()).bytes == tts.size());
  assert(bridge.finishPlayback(pg).signal == PcmFlowSignal::Continue);
  std::array<uint8_t, 2048> drained{};
  size_t total = 0;
  while (!bridge.playbackComplete()) {
    const PcmTransfer part = bridge.popPlayback(drained.data(), drained.size());
    total += part.bytes;
    if (part.bytes != 0) {
      const size_t base = total - part.bytes;
      for (size_t i = 0; i < part.bytes; ++i)
        assert(drained[i] == uint8_t(base + i));
    }
  }
  assert(total == tts.size());
  assert(bridge.pushPlayback(pg, tts.data(), 2).signal == PcmFlowSignal::Closed);

  const uint32_t cg = bridge.beginCapture();
  assert(cg != 0);
  std::array<uint8_t, 640> mic{};
  for (size_t i = 0; i < mic.size(); ++i) mic[i] = uint8_t(255 - i);
  for (unsigned frame = 0; frame < 8; ++frame)
    assert(bridge.pushCapture(cg, mic.data(), mic.size()).bytes == mic.size());
  assert(bridge.finishCapture(cg).signal == PcmFlowSignal::Continue);
  total = 0;
  while (!bridge.captureComplete()) {
    const PcmTransfer part = bridge.popCapture(drained.data(), drained.size());
    total += part.bytes;
  }
  assert(total == mic.size() * 8U);
  assert(bridge.pushCapture(cg, mic.data(), mic.size()).signal ==
         PcmFlowSignal::Closed);

  const uint32_t stale = bridge.beginCapture();
  assert(stale > cg);
  assert(bridge.pushCapture(cg, mic.data(), mic.size()).signal ==
         PcmFlowSignal::StaleGeneration);
  assert(bridge.cancelCapture(stale).signal == PcmFlowSignal::Closed);

  std::array<uint8_t, 100> too_small{};
  DuplexPcmBridgeCore invalid(too_small.data(), too_small.size(), 20, 80,
                              200, capture.data(), capture.size(), 4096,
                              12288, 2048);
  assert(!invalid.valid());
  assert(invalid.beginPlayback(16000, 1) == 0);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-duplex-pcm-"));
  try {
    const source = join(scratch, "bridge.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(audio, "include"), source,
      join(audio, "pcm_backpressure.cpp"), join(audio, "audio_flow.cpp"),
      join(audio, "duplex_pcm_bridge.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
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

test("duplex PCM bridge preserves backpressure and generations", () => {
  buildAndRun(false);
});

test("duplex PCM bridge survives ASan/UBSan cancellation", () => {
  buildAndRun(true);
});

test("native bridge keeps PCM off control queues and gates WSS before reads", () => {
  const header = readFileSync(join(
    audioIdf, "include/inkloop/esp_cross_core_audio_bridge.hpp"), "utf8");
  const source = readFileSync(join(
    audioIdf, "esp_cross_core_audio_bridge.cpp"), "utf8");
  const wss = readFileSync(join(myaiIdf, "esp_wss_transport.cpp"), "utf8");
  assert.match(header, /kPlaybackCapacityBytes = 32U \* 1024U/);
  assert.match(header, /kCaptureCapacityBytes = 16U \* 1024U/);
  assert.match(header, /kMaximumWssAudioMessageBytes = 12U \* 1024U/);
  assert.match(source, /MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT/);
  assert.match(source, /device\.readCapture/);
  assert.match(source, /client\.sendPcm16/);
  assert.match(source, /client\.endVoiceTurn/);
  assert.doesNotMatch(source, /WorkEnvelope|xQueueSend|std::vector/);
  assert.match(wss, /ingressReady_[\s\S]*esp_transport_poll_read/);
  assert.ok(wss.indexOf("ingressReady_") < wss.indexOf("esp_transport_poll_read"));
});
