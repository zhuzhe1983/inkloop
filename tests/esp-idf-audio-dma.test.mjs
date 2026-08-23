import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const audio = join(repo, "firmware/inkloop-idf/components/inkloop_audio");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_audio_idf");
const board = join(repo, "firmware/inkloop-idf/boards/m5_papercolor_c151");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "inkloop/audio_flow.hpp"

using namespace inkloop;

int main() {
  std::array<uint8_t, 8192> storage{};
  PcmBackpressureRing ring(storage.data(), storage.size(), 2048, 6144, 4096);
  AudioPlaybackFlow flow(ring);
  std::array<uint8_t, 4096> input{};
  std::array<uint8_t, 4096> output{};

  const uint32_t first = flow.begin(16000, 1);
  assert(first != 0 && flow.byteLimit() == 1920000);
  auto transfer = flow.accept(first, input.data(), input.size());
  assert(transfer.signal == PcmFlowSignal::Continue);
  transfer = flow.accept(first, input.data(), 2048);
  assert(transfer.signal == PcmFlowSignal::PauseIngress);
  transfer = flow.drain(output.data(), output.size());
  assert(transfer.signal == PcmFlowSignal::ResumeIngress);
  assert(flow.finish(first).signal == PcmFlowSignal::Continue);
  assert(flow.drain(output.data(), output.size()).signal == PcmFlowSignal::Closed);
  assert(flow.state() == AudioFlowState::Complete);
  assert(flow.diagnostics().pause_signals == 1);
  assert(flow.diagnostics().resume_signals == 1);
  assert(flow.diagnostics().accepted_bytes == 6144);
  assert(flow.diagnostics().drained_bytes == 6144);

  const uint32_t limited = flow.begin(8000, 1);
  assert(limited != first && flow.byteLimit() == 960000);
  size_t remaining = flow.byteLimit();
  while (remaining != 0) {
    const size_t count = remaining > input.size() ? input.size() : remaining;
    transfer = flow.accept(limited, input.data(), count);
    assert(transfer.bytes == count);
    transfer = flow.drain(output.data(), output.size());
    assert(transfer.bytes == count);
    remaining -= count;
  }
  transfer = flow.accept(limited, input.data(), 2);
  assert(transfer.signal == PcmFlowSignal::LimitExceeded);
  assert(flow.state() == AudioFlowState::Fault);
  assert(flow.diagnostics().limit_rejections == 1);
  assert(flow.cancel(first).signal == PcmFlowSignal::Closed ||
         flow.cancel(first).signal == PcmFlowSignal::StaleGeneration);

  AudioCaptureContract capture;
  const uint32_t capture_generation = capture.begin();
  assert(capture.validate(capture_generation, input.data(), 640) ==
         PcmFlowSignal::Continue);
  assert(capture.validate(capture_generation + 1, input.data(), 640) ==
         PcmFlowSignal::StaleGeneration);
  assert(capture.validate(capture_generation, input.data(), 3) ==
         PcmFlowSignal::InvalidFrame);
  size_t capture_remaining =
      AudioCaptureContract::kMaximumCaptureBytes - capture.acceptedBytes();
  while (capture_remaining != 0) {
    size_t count = capture_remaining > AudioCaptureContract::kMaximumFrameBytes
        ? AudioCaptureContract::kMaximumFrameBytes : capture_remaining;
    assert(capture.validate(capture_generation, input.data(), count) ==
           PcmFlowSignal::Continue);
    capture_remaining -= count;
  }
  assert(capture.validate(capture_generation, input.data(), 2) ==
         PcmFlowSignal::LimitExceeded);
  assert(!capture.active());
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-audio-flow-"));
  try {
    const source = join(scratch, "audio.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(audio, "include"), source,
      join(audio, "pcm_backpressure.cpp"), join(audio, "audio_flow.cpp"),
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

test("bounded playback and capture flows pass strict C++17", () => {
  buildAndRun(false);
});

test("60-second audio limits and generation cancellation pass ASan/UBSan", () => {
  buildAndRun(true);
});

test("native I2S and C151 codecs preserve the official PaperColor wiring", () => {
  const i2s = readFileSync(join(native, "esp_i2s_audio.cpp"), "utf8");
  const codec = readFileSync(join(board, "papercolor_audio_codec.cpp"), "utf8");
  const cmake = readFileSync(join(native, "CMakeLists.txt"), "utf8");

  assert.match(i2s, /I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG/);
  assert.match(i2s, /I2S_STD_SLOT_LEFT/);
  assert.match(i2s, /I2S_STD_SLOT_BOTH/);
  assert.match(i2s, /kMonoExpansionSamples = 512/);
  assert.match(i2s, /volume_percent_ \/ 100/);
  assert.match(i2s, /mode_ != Mode::Idle/);
  assert.match(i2s, /i2s_channel_read/);
  assert.match(i2s, /i2s_channel_write/);
  assert.match(i2s, /bool EspI2sAudioDevice::playbackDrained\(\) const/);
  assert.match(i2s, /playback_drain_wait_us_/);
  assert.doesNotMatch(i2s, /xTaskCreate|std::thread|malloc\s*\(|new\s+/);

  assert.match(codec, /kEs7210Address = 0x40/);
  assert.match(codec, /kEs8311Address = 0x18/);
  assert.match(codec, /GPIO_NUM_45/);
  assert.match(codec, /GPIO_NUM_46/);
  assert.match(codec, /config\.capture_port = I2S_NUM_1/);
  assert.match(codec, /config\.playback_port = I2S_NUM_0/);
  assert.match(codec, /config\.mclk = GPIO_NUM_42/);
  assert.match(codec, /config\.bclk = GPIO_NUM_40/);
  assert.match(codec, /config\.word_select = GPIO_NUM_41/);
  assert.match(codec, /config\.capture_data = GPIO_NUM_39/);
  assert.match(codec, /config\.playback_data = GPIO_NUM_38/);
  assert.doesNotMatch(codec, /M5Unified|Arduino\.h|delay\s*\(/);
  assert.match(cmake, /esp_driver_i2s/);
  assert.match(cmake, /esp_timer/);
});

test("local prompts and TTS use scheduler-sized frames and drain DMA tails", () => {
  const bridgeHeader = readFileSync(
    join(native, "include/inkloop/esp_cross_core_audio_bridge.hpp"), "utf8");
  const bridge = readFileSync(
    join(native, "esp_cross_core_audio_bridge.cpp"), "utf8");
  const prompt = readFileSync(join(
    repo, "firmware/inkloop-idf/components/inkloop_product/local_prompt_player.cpp"), "utf8");

  assert.match(bridgeHeader, /kMaximumPlaybackPumpBytes = 1920U/);
  assert.match(bridgeHeader, /kCapturePumpBytes = 320U/);
  assert.match(bridge, /size_t playbackPumpBytes/);
  assert.match(bridge, /\(sample_rate_hz \+ 99U\) \/ 100U/);
  assert.match(bridge, /device\.playbackDrained\(\)/);
  assert.match(prompt, /kPlaybackChunkBytes = 320U/);
  assert.match(prompt, /requestVolumePreview/);
  assert.match(prompt, /kPreviewToneSamples = 4800U/);
  assert.match(prompt, /if \(!device\.playbackDrained\(\)\) return ESP_OK/);
});
