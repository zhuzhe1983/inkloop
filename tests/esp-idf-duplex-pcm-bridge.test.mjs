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

  // Adjacent same-format tts.start/tts.stop segments keep one generation and
  // preserve the bounded duration accounting. A mismatched format or a
  // duplicate start while ingress is open remains fail-closed.
  assert(bridge.resumePlayback(pg, 16000, 1) == 0);
  assert(bridge.resumePlayback(pg + 1, 24000, 1) == 0);
  assert(bridge.resumePlayback(pg, 24000, 1) == pg);
  assert(bridge.resumePlayback(pg, 24000, 1) == 0);
  std::array<uint8_t, 1024> segmentOne{};
  std::array<uint8_t, 1024> segmentTwo{};
  segmentOne.fill(0x11);
  segmentTwo.fill(0x22);
  assert(bridge.pushPlayback(pg, segmentOne.data(), segmentOne.size()).bytes ==
         segmentOne.size());
  assert(bridge.finishPlayback(pg).signal == PcmFlowSignal::Continue);
  // Resume while the prior segment still has a ring tail. The next segment is
  // appended behind it instead of clearing or reordering already-buffered PCM.
  const PcmTransfer firstTail = bridge.popPlayback(drained.data(), 512);
  assert(firstTail.bytes == 512);
  for (size_t i = 0; i < firstTail.bytes; ++i) assert(drained[i] == 0x11);
  assert(bridge.resumePlayback(pg, 24000, 1) == pg);
  assert(bridge.pushPlayback(pg, segmentTwo.data(), segmentTwo.size()).bytes ==
         segmentTwo.size());
  assert(bridge.finishPlayback(pg).signal == PcmFlowSignal::Continue);
  total = firstTail.bytes;
  while (!bridge.playbackComplete()) {
    const PcmTransfer part =
        bridge.popPlayback(drained.data(), drained.size());
    for (size_t i = 0; i < part.bytes; ++i) {
      assert(drained[i] == (total + i < segmentOne.size() ? 0x11 : 0x22));
    }
    total += part.bytes;
  }
  assert(total == 2048);

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

const idfHarness = String.raw`
#include <cassert>
#include <cstring>
#include <cstdint>
#include <vector>

#include "inkloop/esp_cross_core_audio_bridge.hpp"

int64_t inkloop_test_time_us = 1000000;

using namespace inkloop;

void pumpUntilIdle(EspCrossCoreAudioBridge& bridge,
                   EspI2sAudioDevice& device, size_t maximum_steps) {
  for (size_t step = 0; step < maximum_steps && bridge.playbackBusy(); ++step) {
    assert(bridge.servicePlayback(device) == ESP_OK);
    inkloop_test_time_us += 200000;
  }
}

void testShortTailFormatSwitch() {
  EspCrossCoreAudioBridge bridge;
  assert(bridge.initialize() == ESP_OK);
  EspI2sAudioDevice device;
  device.modelPreload = true;
  device.preloadTargetBytes = 64;

  const uint8_t oldTail[] = {1, 0, 2, 0};
  const uint8_t newTail[] = {3, 0, 4, 0};
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(bridge.write(oldTail, sizeof(oldTail)).ok());
  assert(bridge.end().ok());

  // Model the exact cross-core interleaving: old PCM has reached a READY I2S
  // preload buffer, but TX has not started. Network installs a different-rate
  // generation before the Voice owner reaches its short-tail start check.
  bool switched = false;
  device.onWrite = [&]() {
    if (switched) return;
    switched = true;
    assert(bridge.begin(16000, 1).ok());
    assert(bridge.write(newTail, sizeof(newTail)).ok());
    assert(bridge.end().ok());
  };
  assert(bridge.servicePlayback(device) == ESP_OK);
  pumpUntilIdle(bridge, device, 12);

  assert(!bridge.playbackBusy());
  assert(device.preloadedStartCalls >= 2);
  assert(device.endCalls == 2);
  assert(device.beginRates == std::vector<uint32_t>({24000, 16000}));
}

void testNoAudioCompletion(bool one_frame) {
  EspCrossCoreAudioBridge bridge;
  assert(bridge.initialize() == ESP_OK);
  EspI2sAudioDevice device;
  device.modelPreload = true;
  device.preloadTargetBytes = 64;
  device.emulateResamplerPriming = one_frame;

  assert(bridge.begin(16000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  if (one_frame) {
    const uint8_t frame[] = {0x34, 0x12};
    assert(bridge.write(frame, sizeof(frame)).ok());
  }
  assert(bridge.end().ok());
  pumpUntilIdle(bridge, device, 8);

  assert(!bridge.playbackBusy());
  assert(bridge.diagnostics().playback_hardware_failures == 0);
  assert(device.abortCalls == 0);
  assert(device.endCalls == 1);
  assert(device.preloadedStartCalls == (one_frame ? 1 : 0));
}

void testSameFormatRestartAfterSourceFinish() {
  EspCrossCoreAudioBridge bridge;
  assert(bridge.initialize() == ESP_OK);
  EspI2sAudioDevice device;
  device.modelPreload = true;
  device.preloadTargetBytes = 64;

  const uint8_t first[] = {1, 0, 2, 0};
  const uint8_t adjacent[] = {3, 0, 4, 0};
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(bridge.write(first, sizeof(first)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  inkloop_test_time_us += 150000;
  assert(bridge.servicePlayback(device) == ESP_OK);

  // StopPending now commits the old source finish, but DMA still owns its
  // tail. A same-format tts.start must get a fresh core generation queued
  // behind teardown, rather than reactivating the closed source.
  device.drained = false;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.finishCalls == 1);
  assert(device.playbackSourceFinished);
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.write(adjacent, sizeof(adjacent)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.finishCalls == 1);
  assert(device.invalidWriteCalls == 0);
  assert(device.played == std::vector<uint8_t>(first, first + sizeof(first)));

  device.drained = true;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.endCalls == 1);
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.beginCalls == 2);
  assert(device.beginRates == std::vector<uint32_t>({24000, 24000}));
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.invalidWriteCalls == 0);
  const std::vector<uint8_t> expected = {1, 0, 2, 0, 3, 0, 4, 0};
  assert(device.played == expected);

  bridge.abort();
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(!bridge.playbackBusy());
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "short-tail-format-switch") == 0) {
    testShortTailFormatSwitch();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "zero-byte") == 0) {
    testNoAudioCompletion(false);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "single-frame") == 0) {
    testNoAudioCompletion(true);
    return 0;
  }
  if (argc == 2 &&
      std::strcmp(argv[1], "same-format-after-source-finish") == 0) {
    testSameFormatRestartAfterSourceFinish();
    return 0;
  }

  EspCrossCoreAudioBridge bridge;
  assert(bridge.initialize() == ESP_OK);
  EspI2sAudioDevice device;

  const uint8_t first[] = {1, 0, 2, 0};
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.beginCalls == 1);
  assert(bridge.write(first, sizeof(first)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  inkloop_test_time_us += 150000;
  assert(bridge.servicePlayback(device) == ESP_OK);

  // A start in StopPending cancels teardown and keeps the existing I2S run.
  const uint8_t second[] = {3, 0, 4, 0};
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.write(second, sizeof(second)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  inkloop_test_time_us += 150000;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.endCalls == 0);

  // servicePlayback has changed StopPending to Stopping before calling the
  // device. Re-enter begin/write/end from inside endPlayback to model the exact
  // cross-core interleaving; teardown must finish, then restart the generation.
  const uint8_t third[] = {5, 0, 6, 0};
  device.onEnd = [&]() {
    assert(bridge.begin(16000, 1).ok());
    assert(bridge.write(third, sizeof(third)).ok());
    assert(bridge.end().ok());
  };
  assert(bridge.servicePlayback(device) == ESP_OK);
  device.onEnd = nullptr;
  assert(device.endCalls == 1);
  assert(device.beginCalls == 1);
  assert(bridge.playbackBusy());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.beginCalls == 2);
  assert(device.beginRates == std::vector<uint32_t>({24000, 16000}));
  assert(bridge.servicePlayback(device) == ESP_OK);

  const std::vector<uint8_t> expected = {
      1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0};
  assert(device.played == expected);

  // Abort winning inside the same unlocked stop window must not be overwritten
  // by the stale successful stop completion.
  inkloop_test_time_us += 150000;
  assert(bridge.servicePlayback(device) == ESP_OK);
  device.onEnd = [&]() { bridge.abort(); };
  assert(bridge.servicePlayback(device) == ESP_OK);
  device.onEnd = nullptr;
  assert(bridge.playbackBusy());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(!bridge.playbackBusy());
  assert(device.abortCalls == 1);

  // A provider may pause much longer than the short I2S continuation grace
  // between two TTS segments in the same response. Once hardware has shut
  // down, a late tts.start must create a fresh generation and restart I2S
  // without losing the new PCM.
  const uint8_t preGap[] = {9, 0, 10, 0};
  const uint8_t postGap[] = {11, 0, 12, 0};
  const int beginsBeforeGap = device.beginCalls;
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.beginCalls == beginsBeforeGap + 1);
  assert(bridge.write(preGap, sizeof(preGap)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  inkloop_test_time_us += 150000;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(!bridge.playbackBusy());

  inkloop_test_time_us += 2000000;
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.beginCalls == beginsBeforeGap + 2);
  assert(bridge.write(postGap, sizeof(postGap)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.played.size() >= sizeof(preGap) + sizeof(postGap));
  const size_t lateBase = device.played.size() - sizeof(postGap);
  for (size_t i = 0; i < sizeof(postGap); ++i)
    assert(device.played[lateBase + i] == postGap[i]);
  bridge.abort();
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(!bridge.playbackBusy());

  // A format switch may be accepted once the old ring is empty, but must not
  // end the old I2S run until its final DMA samples have actually drained.
  const uint8_t fourth[] = {7, 0, 8, 0};
  assert(bridge.begin(24000, 1).ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  device.drained = false;
  assert(bridge.write(fourth, sizeof(fourth)).ok());
  assert(bridge.end().ok());
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(bridge.begin(16000, 1).ok());
  const int endsBeforeDmaDrain = device.endCalls;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.endCalls == endsBeforeDmaDrain);
  device.drained = true;
  assert(bridge.servicePlayback(device) == ESP_OK);
  assert(device.endCalls == endsBeforeDmaDrain + 1);
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

function buildAndRunIdfAdapter(scenario = "baseline", sanitized = false) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-idf-audio-state-"));
  try {
    const stubs = join(scratch, "stubs");
    const source = join(scratch, "adapter.cpp");
    const binary = join(scratch, sanitized ? "adapter-sanitized" : "adapter");
    const stubFiles = new Map([
      ["freertos/FreeRTOS.h", String.raw`
#pragma once
#include <cassert>
struct portMUX_TYPE { bool locked = false; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(value) \
  do { assert(!(value)->locked); (value)->locked = true; } while (false)
#define portEXIT_CRITICAL(value) \
  do { assert((value)->locked); (value)->locked = false; } while (false)
`],
      ["esp_err.h", String.raw`
#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x105;
`],
      ["esp_heap_caps.h", String.raw`
#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1U;
constexpr unsigned MALLOC_CAP_8BIT = 2U;
inline void* heap_caps_calloc(size_t count, size_t size, unsigned) {
  return std::calloc(count, size);
}
inline void heap_caps_free(void* value) { std::free(value); }
`],
      ["esp_timer.h", String.raw`
#pragma once
#include <cstdint>
extern int64_t inkloop_test_time_us;
inline int64_t esp_timer_get_time() { return inkloop_test_time_us; }
`],
      ["inkloop/myai/MyAiAdapters.h", String.raw`
#pragma once
#include <cstddef>
#include <cstdint>
namespace inkloop { namespace myai {
enum class ErrorCode : uint8_t {
  None, InvalidArgument, InvalidState, Storage, Security, Transport
};
struct Status {
  ErrorCode code;
  int httpStatus;
  uint32_t retryAfterMs;
  const char* detail;
  Status(ErrorCode value = ErrorCode::None, int http = 0,
         const char* message = "", uint32_t retry = 0)
      : code(value), httpStatus(http), retryAfterMs(retry), detail(message) {}
  bool ok() const { return code == ErrorCode::None; }
  static Status success() { return Status(); }
};
class IAudioSink {
 public:
  virtual ~IAudioSink() = default;
  virtual Status begin(uint32_t, uint8_t) = 0;
  virtual Status write(const uint8_t*, size_t) = 0;
  virtual Status end() = 0;
  virtual void abort() = 0;
};
} }
`],
      ["inkloop/myai/MyAiClient.h", String.raw`
#pragma once
#include "inkloop/myai/MyAiAdapters.h"
namespace inkloop { namespace myai {
class MyAiClient {
 public:
  Status sendPcm16(const uint8_t*, size_t) { return Status::success(); }
  Status endVoiceTurn() { return Status::success(); }
};
} }
`],
      ["inkloop/esp_i2s_audio.hpp", String.raw`
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "esp_err.h"
namespace inkloop {
class EspI2sAudioDevice {
 public:
  int beginCalls = 0;
  int endCalls = 0;
  int abortCalls = 0;
  int preloadedStartCalls = 0;
  int finishCalls = 0;
  int pauseIngressCalls = 0;
  int resumeIngressCalls = 0;
  int invalidWriteCalls = 0;
  bool drained = true;
  bool modelPreload = false;
  bool playbackOpen = false;
  bool playbackEnabled = false;
  bool playbackSourceFinished = false;
  bool playbackIngressOpen = false;
  bool emulateResamplerPriming = false;
  size_t preloadTargetBytes = 0;
  size_t preloadedBytes = 0;
  size_t sourceFrames = 0;
  uint8_t playbackChannels = 0;
  std::vector<uint8_t> played;
  std::vector<uint32_t> beginRates;
  std::function<void()> onEnd;
  std::function<void()> onWrite;
  esp_err_t beginPlayback(uint32_t rate, uint8_t channels) {
    ++beginCalls;
    beginRates.push_back(rate);
    if (modelPreload) {
      if (playbackOpen) return ESP_ERR_INVALID_STATE;
      playbackOpen = true;
      playbackEnabled = false;
      playbackSourceFinished = false;
      playbackIngressOpen = true;
      preloadedBytes = 0;
      sourceFrames = 0;
      playbackChannels = channels;
    }
    return ESP_OK;
  }
  esp_err_t writePlayback(const uint8_t* bytes, size_t length, uint32_t) {
    if (modelPreload) {
      if (!playbackOpen || playbackChannels == 0 || playbackSourceFinished) {
        ++invalidWriteCalls;
        return ESP_ERR_INVALID_STATE;
      }
      const size_t sourceFrameBytes = static_cast<size_t>(playbackChannels) * 2U;
      if (length % sourceFrameBytes != 0) return ESP_ERR_INVALID_SIZE;
      const size_t frames = length / sourceFrameBytes;
      size_t produced = frames;
      if (emulateResamplerPriming && sourceFrames == 0 && produced != 0)
        --produced;
      sourceFrames += frames;
      if (!playbackEnabled) {
        preloadedBytes += produced * 2U * sizeof(int16_t);
        if (preloadTargetBytes != 0 && preloadedBytes >= preloadTargetBytes) {
          playbackEnabled = true;
          ++preloadedStartCalls;
        }
      }
    }
    played.insert(played.end(), bytes, bytes + length);
    if (onWrite) onWrite();
    return ESP_OK;
  }
  esp_err_t pausePlaybackIngress() {
    ++pauseIngressCalls;
    if (modelPreload && !playbackOpen) return ESP_ERR_INVALID_STATE;
    playbackIngressOpen = false;
    return ESP_OK;
  }
  esp_err_t resumePlaybackIngress() {
    ++resumeIngressCalls;
    if (modelPreload &&
        (!playbackOpen || playbackSourceFinished)) {
      return ESP_ERR_INVALID_STATE;
    }
    playbackIngressOpen = true;
    return ESP_OK;
  }
  esp_err_t startPreloadedPlayback() {
    if (!modelPreload) return ESP_OK;
    if (!playbackOpen) return ESP_ERR_INVALID_STATE;
    if (playbackEnabled) return ESP_OK;
    if (preloadedBytes == 0) return ESP_OK;
    playbackEnabled = true;
    ++preloadedStartCalls;
    return ESP_OK;
  }
  esp_err_t finishPlaybackSource(uint32_t = 20) {
    ++finishCalls;
    if (!modelPreload) return ESP_OK;
    if (!playbackOpen) return ESP_ERR_INVALID_STATE;
    if (!playbackSourceFinished) {
      // The real streaming resampler holds its first source sample until
      // finish(), then emits a held tail so a one-frame TTS is not discarded.
      if (emulateResamplerPriming && sourceFrames != 0 && preloadedBytes == 0)
        preloadedBytes = 2U * sizeof(int16_t);
      playbackSourceFinished = true;
    }
    return startPreloadedPlayback();
  }
  bool playbackDrained() const {
    if (!modelPreload) return drained;
    if (!playbackOpen) return true;
    if (!playbackEnabled) return preloadedBytes == 0;
    return drained;
  }
  esp_err_t endPlayback() {
    if (modelPreload) {
      if (!playbackOpen || !playbackSourceFinished || !playbackDrained())
        return ESP_ERR_INVALID_STATE;
      playbackOpen = false;
      playbackEnabled = false;
      playbackSourceFinished = false;
      playbackIngressOpen = false;
      preloadedBytes = 0;
      sourceFrames = 0;
      playbackChannels = 0;
    }
    ++endCalls;
    if (onEnd) onEnd();
    return ESP_OK;
  }
  esp_err_t beginCapture() { return ESP_OK; }
  esp_err_t readCapture(int16_t*, size_t, size_t& count, uint32_t) {
    count = 0;
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t endCapture() { return ESP_OK; }
  void abort() {
    ++abortCalls;
    playbackOpen = false;
    playbackEnabled = false;
    playbackSourceFinished = false;
    playbackIngressOpen = false;
    preloadedBytes = 0;
    sourceFrames = 0;
    playbackChannels = 0;
  }
};
}
`],
    ]);
    for (const [relative, contents] of stubFiles) {
      const target = join(stubs, relative);
      const directory = target.slice(0, target.lastIndexOf("/"));
      execFileSync("mkdir", ["-p", directory]);
      writeFileSync(target, contents);
    }
    writeFileSync(source, idfHarness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", stubs, "-I", join(audioIdf, "include"),
      "-I", join(audio, "include"), source,
      join(audioIdf, "esp_cross_core_audio_bridge.cpp"),
      join(audio, "pcm_backpressure.cpp"), join(audio, "audio_flow.cpp"),
      join(audio, "duplex_pcm_bridge.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, scenario === "baseline" ? [] : [scenario], {
      encoding: "utf8",
      env: sanitized
        ? {
            ...process.env,
            ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
            UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
          }
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

test("IDF bridge accepts tts.start across StopPending and Stopping", () => {
  buildAndRunIdfAdapter();
});

test("IDF bridge finishes a preloaded short tail before switching format", () => {
  buildAndRunIdfAdapter("short-tail-format-switch");
});

test("IDF bridge treats an empty TTS segment as a benign completion", () => {
  buildAndRunIdfAdapter("zero-byte");
});

test("IDF bridge treats a one-frame TTS segment as a benign completion", () => {
  buildAndRunIdfAdapter("single-frame");
});

test("IDF bridge strictly restarts same-format TTS after source finish", () => {
  buildAndRunIdfAdapter("same-format-after-source-finish");
});

test("IDF bridge restart-after-source-finish survives ASan/UBSan", () => {
  buildAndRunIdfAdapter("same-format-after-source-finish", true);
});

test("native bridge keeps PCM off control queues and gates WSS data after control reads", () => {
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
  assert.match(source, /resumePlayback/);
  assert.match(source, /kPlaybackContinuationGraceUs = 150000/);
  assert.match(source, /PlaybackHardwareState::Stopping/);
  assert.doesNotMatch(source, /WorkEnvelope|xQueueSend|std::vector/);
  assert.match(wss, /esp_transport_poll_read[\s\S]*handleControlFrame[\s\S]*ingressDataReady/);
  const controlDispatch = wss.indexOf(
    "const Status control = handleControlFrame(");
  assert.ok(controlDispatch >= 0 && controlDispatch <
            wss.indexOf("const bool data_ready", controlDispatch));
});
