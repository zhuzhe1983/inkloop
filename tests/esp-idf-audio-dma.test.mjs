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

const cadenceHarness = String.raw`
#include <cassert>
#include <cstdint>

#include "inkloop/playback_feed_monitor.hpp"

using namespace inkloop;

int main() {
  PlaybackFeedMonitor steady;
  assert(!steady.begin(0, 4096));
  assert(steady.begin(44100, 4096));
  assert(steady.preload(2646)); // exactly 60 ms at 44.1 kHz
  assert(steady.start(1000000));
  for (uint64_t tick = 1; tick <= 100; ++tick) {
    assert(steady.submit(441, 1000000 + tick * 10000));
  }
  steady.finishSource(2000000);
  const auto healthy = steady.diagnostics();
  assert(healthy.streams == 1);
  assert(healthy.submit_calls == 100);
  assert(healthy.max_submit_gap_us == 10000);
  // The producer wakes every 10 ms: lead reaches 50 ms immediately before
  // each refill and returns to 60 ms afterwards.
  assert(healthy.minimum_queue_lead_us == 50000);
  assert(healthy.estimated_underrun_count == 0);
  assert(healthy.late_submit_count == 0);
  assert(healthy.queue_clamp_count == 0);
  assert(healthy.peak_queue_frames == 2646);

  PlaybackFeedMonitor short_prompt;
  assert(short_prompt.begin(44100, 4096));
  assert(short_prompt.preload(2205)); // 50 ms: source closes before DMA starts
  short_prompt.finishSource(1000000);
  assert(short_prompt.start(1000000));
  // start() must not reopen a source which was completely preloaded. Its
  // eventual queue overflow is an expected drain, not an underrun.
  assert(!short_prompt.submit(441, 1010000));
  assert(short_prompt.diagnostics().estimated_underrun_count == 0);

  PlaybackFeedMonitor grace_without_continuation;
  assert(grace_without_continuation.begin(44100, 4096));
  assert(grace_without_continuation.preload(2205)); // 50 ms segment
  assert(grace_without_continuation.start(1000000));
  assert(grace_without_continuation.pauseSource(1050000));
  assert(!grace_without_continuation.sourceOpen());
  // The 150 ms resampler continuation window is an intentional drain. A
  // final held interval can still enter DMA without reopening starvation.
  assert(grace_without_continuation.submitTerminal(1, 1200000));
  grace_without_continuation.finishSource(1200000);
  assert(grace_without_continuation.diagnostics().estimated_underrun_count ==
         0);
  assert(grace_without_continuation.diagnostics().late_submit_count == 0);

  PlaybackFeedMonitor grace_with_continuation;
  assert(grace_with_continuation.begin(44100, 4096));
  assert(grace_with_continuation.preload(2205));
  assert(grace_with_continuation.start(1000000));
  assert(grace_with_continuation.pauseSource(1050000));
  assert(!grace_with_continuation.submit(441, 1100000));
  assert(grace_with_continuation.resumeSource(1150000));
  assert(grace_with_continuation.submit(441, 1150000));
  assert(grace_with_continuation.pauseSource(1160000));
  assert(grace_with_continuation.submitTerminal(1, 1310000));
  const auto continued = grace_with_continuation.diagnostics();
  assert(continued.estimated_underrun_count == 0);
  assert(continued.late_submit_count == 0);

  PlaybackFeedMonitor delayed;
  assert(delayed.begin(44100, 4096));
  assert(delayed.preload(2646));
  assert(delayed.start(0));
  assert(delayed.submit(441, 75000));
  const auto starved = delayed.diagnostics();
  assert(starved.max_submit_gap_us == 75000);
  assert(starved.late_submit_count == 1);
  assert(starved.estimated_underrun_count == 1);
  assert(starved.estimated_underrun_frames == 661);
  assert(starved.minimum_queue_lead_us == 0);
  assert(starved.current_queue_frames == 441);

  PlaybackFeedMonitor bounded;
  assert(bounded.begin(44100, 4096));
  assert(bounded.preload(5000));
  const auto queue = bounded.diagnostics();
  assert(queue.queue_clamp_count == 1);
  assert(queue.queue_overflow_frames == 904);
  assert(queue.current_queue_frames == 4096);
  assert(queue.peak_queue_frames == 4096);
  return 0;
}
`;

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

function buildAndRunCadence(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-audio-cadence-"));
  try {
    const source = join(scratch, "cadence.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, cadenceHarness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(audio, "include"), source, "-o", binary,
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

test("playback feed cadence and bounded queue detect a repeatable underrun", () => {
  buildAndRunCadence(false);
  buildAndRunCadence(true);
});

test("native I2S and C151 codecs preserve the official PaperColor wiring", () => {
  const i2s = readFileSync(join(native, "esp_i2s_audio.cpp"), "utf8");
  const codec = readFileSync(join(board, "papercolor_audio_codec.cpp"), "utf8");
  const voiceHeader = readFileSync(join(
    repo,
    "firmware/inkloop-idf/components/inkloop_product/include/inkloop/native_voice_service.hpp",
  ), "utf8");
  const voice = readFileSync(join(
    repo,
    "firmware/inkloop-idf/components/inkloop_product/native_voice_service.cpp",
  ), "utf8");
  const cmake = readFileSync(join(native, "CMakeLists.txt"), "utf8");

  assert.match(i2s, /I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG/);
  assert.match(i2s, /I2S_STD_SLOT_LEFT/);
  assert.match(i2s, /I2S_STD_SLOT_BOTH/);
  assert.match(i2s, /StreamingStereoResampler/);
  assert.match(i2s, /i2s_channel_preload_data/);
  assert.match(i2s, /i2s_channel_register_event_callback/);
  assert.match(i2s, /callbacks\.on_sent/);
  assert.match(i2s, /callbacks\.on_send_q_ovf/);
  assert.match(i2s, /IRAM_ATTR EspI2sAudioDevice::onPlaybackQueueOverflow/);
  assert.match(i2s, /playback_dma_underruns_isr_/);
  assert.match(i2s, /playback_feed_monitor_\.submit/);
  assert.match(voiceHeader,
    /unique_ptr<EspI2sAudioDevice, InternalAudioDeviceDeleter>/);
  assert.match(voice,
    /heap_caps_malloc\([\s\S]*MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT/);
  assert.match(voice, /new \(audio_storage\)\s*EspI2sAudioDevice/);
  assert.match(voice, /esp_ptr_internal\(audio_device\)/);
  assert.match(voice,
    /InternalAudioDeviceDeleter::operator\([\s\S]*~EspI2sAudioDevice\(\)[\s\S]*heap_caps_free/);
  assert.match(i2s, /kPlaybackPreloadMilliseconds = 60U/);
  assert.match(i2s, /kMaximumBlockingWriteMilliseconds = 20U/);
  assert.match(i2s, /playback_dma_fits_write_timeout/);
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
  assert.match(codec, /config\.playback_sample_rate_hz = 44100/);
  assert.match(codec, /config\.playback_dma_frame_count = 512/);
  assert.match(codec, /config\.playback_dma_descriptor_count = 8/);
  assert.doesNotMatch(
    codec, /#include\s*[<"](?:M5Unified|Arduino\.h)|(?:^|\W)delay\s*\(/m);
  assert.match(cmake, /esp_driver_i2s/);
  assert.match(cmake, /esp_timer/);
});

test("generic playback DMA descriptor cannot outlast its bounded write", () => {
  const header = readFileSync(
    join(native, "include/inkloop/esp_i2s_audio.hpp"), "utf8");
  const i2s = readFileSync(join(native, "esp_i2s_audio.cpp"), "utf8");

  assert.match(header, /playback_dma_frame_count = 160/);
  assert.match(
    i2s,
    /playback_dma_frame_count\) \* 1000ULL[\s\S]*minimum_output_rate_hz[\s\S]*kMaximumBlockingWriteMilliseconds/,
  );
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
  assert.match(bridge, /core_->resumePlayback/);
  assert.match(bridge, /playback_drained_since_us_/);
  assert.match(prompt, /kPlaybackStartupChunkBytes/);
  assert.match(prompt, /kPlaybackSteadyChunkBytes/);
  assert.match(prompt, /requestVolumePreview/);
  assert.match(prompt, /kPreviewToneSteadySamples/);
  assert.match(prompt, /if \(!device\.playbackDrained\(\)\) return ESP_OK/);
});

test("local prompt steady-state feed is bounded to one 10 ms Voice tick", () => {
  const prompt = readFileSync(join(
    repo, "firmware/inkloop-idf/components/inkloop_product/local_prompt_player.cpp"),
  "utf8");
  const match = prompt.match(
    /constexpr size_t kPlaybackSteadyChunkBytes\s*=\s*(\d+)U/,
  );
  assert.ok(match, "local prompt must declare a steady-state feed bound");
  const steadyBytes = Number(match[1]);
  const pcm16MonoBytesPer10ms = 16000 * 2 / 100;
  assert.ok(
    steadyBytes <= pcm16MonoBytesPer10ms,
    `one Voice tick submitted ${steadyBytes} bytes; maximum is ` +
      `${pcm16MonoBytesPer10ms} bytes (10 ms at 16 kHz mono PCM16)`,
  );
  assert.match(
    prompt,
    /startup_feed_complete_\s*\?\s*kPlaybackSteadyChunkBytes\s*:\s*kPlaybackStartupChunkBytes/,
  );
  assert.match(prompt, /startup_feed_complete_\s*=\s*true/);
  assert.doesNotMatch(prompt, /kPlaybackSteadyChunkBytes\s*=\s*2880U/);
});
