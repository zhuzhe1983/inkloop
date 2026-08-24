import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const audio = join(repo, "firmware/inkloop-idf/components/inkloop_audio");
const native = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_audio_idf",
);

const espErrHeader = String.raw`
#pragma once
#include <cstdlib>
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0;
inline constexpr esp_err_t ESP_FAIL = -1;
inline constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
inline constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
inline constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
inline constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
inline constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;
inline constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
inline constexpr esp_err_t ESP_ERR_INVALID_RESPONSE = 0x108;
#define ESP_ERROR_CHECK(expression) do { \
  if ((expression) != ESP_OK) std::abort(); \
} while (0)
`;

const i2sHeader = String.raw`
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

using gpio_num_t = int;
inline constexpr gpio_num_t GPIO_NUM_NC = -1;
inline constexpr gpio_num_t GPIO_NUM_0 = 0;
inline constexpr gpio_num_t GPIO_NUM_MAX = 49;
inline constexpr int I2S_NUM_0 = 0;
inline constexpr int I2S_NUM_1 = 1;
inline constexpr int I2S_ROLE_MASTER = 1;
inline constexpr int I2S_CLK_SRC_PLL_160M = 1;
inline constexpr int I2S_MCLK_MULTIPLE_128 = 128;
inline constexpr int I2S_DATA_BIT_WIDTH_16BIT = 16;
inline constexpr int I2S_SLOT_MODE_MONO = 1;
inline constexpr int I2S_SLOT_MODE_STEREO = 2;
inline constexpr int I2S_STD_SLOT_LEFT = 1;
inline constexpr int I2S_STD_SLOT_BOTH = 3;

struct i2s_event_data_t {};
inline constexpr size_t FAKE_I2S_MAX_DMA_BYTES = 16U * 1024U * 4U;
struct FakeI2sChannel {
  int port = -1;
  bool tx = false;
  bool initialized = false;
  bool enabled = false;
  bool preloaded = false;
  bool deleted = false;
  uint32_t rate_hz = 0;
  size_t dma_desc_num = 0;
  size_t dma_frame_num = 0;
  size_t dma_capacity_bytes = 0;
  size_t preload_cursor = 0;
  std::array<uint8_t, FAKE_I2S_MAX_DMA_BYTES> dma{};
};
using i2s_chan_handle_t = FakeI2sChannel*;
using i2s_callback_t = bool (*)(i2s_chan_handle_t, i2s_event_data_t*, void*);
struct i2s_chan_info_t {
  uint32_t total_dma_buf_size = 0;
};

struct i2s_chan_config_t {
  int id = 0;
  int role = 0;
  uint32_t dma_desc_num = 0;
  uint32_t dma_frame_num = 0;
  bool auto_clear_after_cb = false;
};
inline i2s_chan_config_t fake_channel_config(int id, int role) {
  i2s_chan_config_t value{};
  value.id = id;
  value.role = role;
  return value;
}
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) fake_channel_config((id), (role))

struct i2s_std_clk_config_t {
  uint32_t sample_rate_hz = 0;
  int clk_src = 0;
  int mclk_multiple = 0;
};
inline i2s_std_clk_config_t fake_clock_config(uint32_t rate) {
  i2s_std_clk_config_t value{};
  value.sample_rate_hz = rate;
  return value;
}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) fake_clock_config((rate))

struct i2s_std_slot_config_t {
  int data_bit_width = 0;
  int slot_mode = 0;
  int slot_mask = 0;
};
inline i2s_std_slot_config_t fake_slot_config(int width, int mode) {
  i2s_std_slot_config_t value{};
  value.data_bit_width = width;
  value.slot_mode = mode;
  return value;
}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) \
  fake_slot_config((width), (mode))

struct i2s_std_gpio_config_t {
  gpio_num_t mclk = GPIO_NUM_NC;
  gpio_num_t bclk = GPIO_NUM_NC;
  gpio_num_t ws = GPIO_NUM_NC;
  gpio_num_t dout = GPIO_NUM_NC;
  gpio_num_t din = GPIO_NUM_NC;
  struct {
    bool mclk_inv = false;
    bool bclk_inv = false;
    bool ws_inv = false;
  } invert_flags{};
};
struct i2s_std_config_t {
  i2s_std_clk_config_t clk_cfg{};
  i2s_std_slot_config_t slot_cfg{};
  i2s_std_gpio_config_t gpio_cfg{};
};
struct i2s_event_callbacks_t {
  i2s_callback_t on_recv = nullptr;
  i2s_callback_t on_recv_q_ovf = nullptr;
  i2s_callback_t on_sent = nullptr;
  i2s_callback_t on_send_q_ovf = nullptr;
};

esp_err_t i2s_new_channel(const i2s_chan_config_t*, i2s_chan_handle_t*,
                          i2s_chan_handle_t*);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t,
                                    const i2s_std_config_t*);
esp_err_t i2s_channel_register_event_callback(
    i2s_chan_handle_t, const i2s_event_callbacks_t*, void*);
esp_err_t i2s_channel_get_info(i2s_chan_handle_t, i2s_chan_info_t*);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t,
                                         const i2s_std_clk_config_t*);
esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t,
                                        const i2s_std_gpio_config_t*);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void*, size_t, size_t*,
                           uint32_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void*, size_t, size_t*,
                            uint32_t);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t, const void*, size_t,
                                   size_t*);
`;

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "inkloop/esp_i2s_audio.hpp"

using namespace inkloop;

namespace {

struct DriverState {
  std::array<FakeI2sChannel, 24> channels{};
  size_t channel_count = 0;
  uint32_t new_calls = 0;
  uint32_t init_calls = 0;
  uint32_t callback_calls = 0;
  uint32_t get_info_calls = 0;
  uint32_t clock_calls = 0;
  uint32_t gpio_calls = 0;
  uint32_t enable_calls = 0;
  uint32_t disable_calls = 0;
  uint32_t preload_calls = 0;
  uint32_t delete_calls = 0;
  uint32_t fail_init_call = 0;
  bool fail_next_clock = false;
  bool fail_next_gpio = false;
  bool fail_next_enable = false;
  uint32_t fail_disable_call = 0;
  uint32_t fail_preload_call = 0;
  bool pretend_preload_never_full = false;
  bool fail_next_delete = false;
  i2s_event_callbacks_t tx_callbacks{};
  void* tx_callback_context = nullptr;
} driver;

int64_t now_us = 1000000;

void resetDriver() {
  driver = DriverState{};
  now_us = 1000000;
}

bool anyOtherEnabled(i2s_chan_handle_t selected) {
  for (size_t i = 0; i < driver.channel_count; ++i) {
    const auto* channel = &driver.channels[i];
    if (channel != selected && channel->enabled) return true;
  }
  return false;
}

struct FakeCodec final : IAudioCodecControl {
  bool capture_active = false;
  bool playback_active = false;
  bool fail_next_capture = false;
  bool fail_next_playback = false;
  uint32_t capture_activations = 0;
  uint32_t capture_deactivations = 0;
  uint32_t playback_activations = 0;
  uint32_t playback_deactivations = 0;

  esp_err_t activateCapture() override {
    ++capture_activations;
    if (playback_active) return ESP_ERR_INVALID_STATE;
    if (fail_next_capture) {
      fail_next_capture = false;
      return ESP_FAIL;
    }
    capture_active = true;
    return ESP_OK;
  }
  esp_err_t deactivateCapture() override {
    ++capture_deactivations;
    capture_active = false;
    return ESP_OK;
  }
  esp_err_t activatePlayback() override {
    ++playback_activations;
    if (capture_active) return ESP_ERR_INVALID_STATE;
    if (fail_next_playback) {
      fail_next_playback = false;
      return ESP_FAIL;
    }
    playback_active = true;
    return ESP_OK;
  }
  esp_err_t deactivatePlayback() override {
    ++playback_deactivations;
    playback_active = false;
    return ESP_OK;
  }
};

EspI2sAudioConfig config(uint32_t fixed_playback_rate) {
  EspI2sAudioConfig value;
  value.capture_port = I2S_NUM_1;
  value.playback_port = I2S_NUM_0;
  value.mclk = 10;
  value.bclk = 11;
  value.word_select = 12;
  value.capture_data = 13;
  value.playback_data = 14;
  value.capture_sample_rate_hz = 16000;
  value.playback_sample_rate_hz = fixed_playback_rate;
  value.dma_frame_count = 320;
  value.dma_descriptor_count = 6;
  value.playback_dma_frame_count = fixed_playback_rate == 0 ? 160 : 512;
  value.playback_dma_descriptor_count = fixed_playback_rate == 0 ? 6 : 8;
  return value;
}

void startTinyPlayback(EspI2sAudioDevice& device) {
  const std::array<uint8_t, 4> two_mono_samples{{0, 0, 1, 0}};
  assert(device.writePlayback(two_mono_samples.data(),
                              two_mono_samples.size()) == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_OK);
  assert(device.playbackRunning());
}

void assertDmaSilence(const FakeI2sChannel& channel, size_t from = 0U) {
  assert(channel.dma_capacity_bytes != 0U);
  assert(from <= channel.dma_capacity_bytes);
  assert(std::all_of(channel.dma.begin() + from,
                     channel.dma.begin() + channel.dma_capacity_bytes,
                     [](uint8_t value) { return value == 0U; }));
}

void firePlaybackQueueOverflow() {
  assert(driver.tx_callbacks.on_send_q_ovf != nullptr);
  i2s_event_data_t event{};
  assert(!driver.tx_callbacks.on_send_q_ovf(
      &driver.channels[1], &event, driver.tx_callback_context));
}

void ttsContinuationGraceIsNotAnUnderrun() {
  resetDriver();
  FakeCodec codec;
  EspI2sAudioDevice device(config(44100), codec);
  const std::array<uint8_t, 4> samples{{1, 0, 2, 0}};
  assert(device.prepare() == ESP_OK);

  // A completed short segment closes logical ingress before its preload
  // begins draining. Both the hardware callback and cadence estimator must
  // classify the entire continuation grace as an expected drain.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.pausePlaybackIngress() == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_OK);
  firePlaybackQueueOverflow();
  now_us += 150000;
  assert(device.finishPlaybackSource() == ESP_OK);
  auto diagnostics = device.diagnostics();
  assert(diagnostics.playback_dma_underruns == 0U);
  assert(diagnostics.playback_dma_expected_drain_overflows == 1U);
  assert(diagnostics.playback_feed.estimated_underrun_count == 0U);
  device.abort();

  // A same-format continuation reopens only when its real PCM is ready. The
  // paused gap and the following terminal grace remain underrun-free.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.pausePlaybackIngress() == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_OK);
  now_us += 100000;
  firePlaybackQueueOverflow();
  assert(device.resumePlaybackIngress() == ESP_OK);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.pausePlaybackIngress() == ESP_OK);
  now_us += 150000;
  assert(device.finishPlaybackSource() == ESP_OK);
  diagnostics = device.diagnostics();
  assert(diagnostics.playback_dma_underruns == 0U);
  assert(diagnostics.playback_dma_expected_drain_overflows == 2U);
  assert(diagnostics.playback_feed.estimated_underrun_count == 0U);
  device.abort();

  // With ingress genuinely open, the same hardware signal and a late refill
  // remain real underruns rather than being hidden by the grace handling.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_OK);
  firePlaybackQueueOverflow();
  now_us += 2000000;
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  diagnostics = device.diagnostics();
  assert(diagnostics.playback_dma_underruns == 1U);
  assert(diagnostics.playback_feed.estimated_underrun_count == 1U);
  device.abort();
  assert(device.shutdown() == ESP_OK);
}

void abortedPlaybackScrubsPersistentDmaTail() {
  resetDriver();
  FakeCodec codec;
  EspI2sAudioDevice device(config(44100), codec);
  assert(device.prepare() == ESP_OK);
  FakeI2sChannel& tx = driver.channels[1];
  const std::array<uint8_t, 4> samples{{1, 0, 2, 0}};

  // A cancellation before TX starts must reset the preload cursor, overwrite
  // every retained descriptor byte, and leave the prepared ring reusable.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  std::fill(tx.dma.begin(), tx.dma.begin() + tx.dma_capacity_bytes, 0x5aU);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(!device.playbackRunning());
  driver.fail_next_enable = true;
  device.abort();
  assert(device.mode() == EspI2sAudioDevice::Mode::Playback);
  assert(std::any_of(tx.dma.begin(), tx.dma.begin() + tx.dma_capacity_bytes,
                     [](uint8_t value) { return value != 0U; }));
  // A cleanup error never marks dirty DMA clean; a later owner cleanup can
  // retry the bounded cursor reset and complete the scrub.
  device.abort();
  assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
  assertDmaSilence(tx);

  // A following short prompt may replace only its prefix; the untouched tail
  // must remain silence rather than PCM from the cancelled prompt.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(tx.preload_cursor > 0U);
  assertDmaSilence(tx, tx.preload_cursor);
  assert(device.startPreloadedPlayback() == ESP_OK);
  device.abort();
  assertDmaSilence(tx);

  // The same guarantee applies when abort stops an actively running ring;
  // disable alone deliberately does not mutate the fake DMA contents.
  assert(device.beginPlayback(16000, 1) == ESP_OK);
  std::fill(tx.dma.begin(), tx.dma.begin() + tx.dma_capacity_bytes, 0x6bU);
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_OK);
  assert(device.playbackRunning());
  device.abort();
  assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
  assertDmaSilence(tx);
  assert(device.shutdown() == ESP_OK);
}

void endPlaybackScrubFailuresRemainRetryable() {
  enum class Failure {
    Preload,
    FinalEnable,
    FinalDisable,
    NeverFull,
  };
  constexpr std::array failures{
      Failure::Preload,
      Failure::FinalEnable,
      Failure::FinalDisable,
      Failure::NeverFull,
  };
  const std::array<uint8_t, 4> samples{{1, 0, 2, 0}};

  for (const Failure failure : failures) {
    resetDriver();
    FakeCodec codec;
    EspI2sAudioDevice device(config(44100), codec);
    assert(device.prepare() == ESP_OK);
    assert(driver.get_info_calls == 1U);
    FakeI2sChannel& tx = driver.channels[1];
    std::fill(tx.dma.begin(), tx.dma.begin() + tx.dma_capacity_bytes, 0x6dU);
    assert(device.beginPlayback(16000, 1) == ESP_OK);
    assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
    assert(device.startPreloadedPlayback() == ESP_OK);
    assert(device.finishPlaybackSource() == ESP_OK);
    now_us += 2000000;
    assert(device.playbackDrained());

    switch (failure) {
      case Failure::Preload:
        driver.fail_preload_call = driver.preload_calls + 1U;
        break;
      case Failure::FinalEnable:
        driver.fail_next_enable = true;
        break;
      case Failure::FinalDisable:
        // endPlayback first disables the active prompt; the scrub's bounded
        // cursor reset is the following disable call.
        driver.fail_disable_call = driver.disable_calls + 2U;
        break;
      case Failure::NeverFull:
        driver.pretend_preload_never_full = true;
        break;
    }
    assert(device.endPlayback() != ESP_OK);
    assert(device.mode() == EspI2sAudioDevice::Mode::Playback);

    // The owner can retry a failed terminal cleanup. No failure path may make
    // the dirty prepared ring available to beginPlayback as an Idle device.
    driver.fail_disable_call = 0U;
    driver.fail_preload_call = 0U;
    driver.pretend_preload_never_full = false;
    device.abort();
    assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
    assert(!device.playbackRunning());
    assertDmaSilence(tx);

    // A following short prompt replaces only its prefix; every untouched byte
    // must remain zero after each kind of failed-then-retried scrub.
    assert(device.beginPlayback(16000, 1) == ESP_OK);
    assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
    assert(tx.preload_cursor > 0U);
    assertDmaSilence(tx, tx.preload_cursor);
    device.abort();
    assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
    assertDmaSilence(tx);
    assert(device.shutdown() == ESP_OK);
  }
}

void preparedRingsAreReusedAcrossTurns() {
  resetDriver();
  FakeCodec codec;
  EspI2sAudioDevice device(config(44100), codec);
  assert(device.beginCapture() == ESP_ERR_INVALID_STATE);
  assert(device.prepare() == ESP_OK);
  assert(device.prepared());
  assert(device.prepare() == ESP_OK);
  assert(driver.new_calls == 2);
  assert(driver.init_calls == 2);
  assert(driver.delete_calls == 0);

  for (int turn = 0; turn < 3; ++turn) {
    assert(device.beginCapture() == ESP_OK);
    const uint32_t enables = driver.enable_calls;
    assert(device.beginPlayback(16000, 1) == ESP_ERR_INVALID_STATE);
    assert(driver.enable_calls == enables);
    assert(device.endCapture() == ESP_OK);
  }
  assert(driver.new_calls == 2 && driver.init_calls == 2);
  assert(driver.delete_calls == 0);

  assert(device.beginPlayback(16000, 1) == ESP_OK);
  startTinyPlayback(device);
  const uint32_t enables = driver.enable_calls;
  assert(device.beginCapture() == ESP_ERR_INVALID_STATE);
  assert(driver.enable_calls == enables);
  device.abort();
  assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
  assert(!driver.channels[1].preloaded);
  assert(driver.delete_calls == 0);
  assert(device.beginCapture() == ESP_OK);
  assert(device.endCapture() == ESP_OK);
  assert(driver.new_calls == 2 && driver.init_calls == 2);

  const auto diagnostics = device.diagnostics();
  assert(diagnostics.prepare_attempts == 2);
  assert(diagnostics.prepare_successes == 1);
  assert(diagnostics.prepared_playback_rate_hz == 44100);
  assert(diagnostics.shared_pin_selections >= 5);
  assert(device.shutdown() == ESP_OK);
  assert(!device.prepared());
  assert(driver.delete_calls == 2);
  assert(device.shutdown() == ESP_OK);
  assert(driver.delete_calls == 2);
}

void variableRateUsesClockReconfigurationWithoutDmaAllocation() {
  resetDriver();
  FakeCodec codec;
  EspI2sAudioDevice device(config(0), codec);
  assert(device.prepare() == ESP_OK);
  assert(device.diagnostics().prepared_playback_rate_hz == 16000);
  assert(device.beginPlayback(24000, 1) == ESP_OK);
  assert(driver.clock_calls == 1);
  device.abort();
  assert(device.beginPlayback(24000, 2) == ESP_OK);
  assert(driver.clock_calls == 1);
  device.abort();
  assert(device.beginPlayback(32000, 1) == ESP_OK);
  assert(driver.clock_calls == 2);
  device.abort();
  assert(driver.new_calls == 2 && driver.init_calls == 2);
  assert(driver.delete_calls == 0);
  const auto diagnostics = device.diagnostics();
  assert(diagnostics.playback_clock_reconfigurations == 2);
  assert(diagnostics.prepared_playback_rate_hz == 32000);
  assert(device.shutdown() == ESP_OK);
}

void prepareAndTurnFailuresRollBackWithoutRuntimeDeletion() {
  resetDriver();
  driver.fail_init_call = 2;
  driver.fail_next_delete = true;
  FakeCodec codec;
  EspI2sAudioDevice device(config(44100), codec);
  assert(device.prepare() == ESP_FAIL);
  assert(!device.prepared());
  // The first rollback delete also fails; prepare() retains the handle and
  // its outer rollback retries until neither DMA ring remains owned.
  assert(driver.delete_calls == 3);
  assert(device.diagnostics().prepare_failures == 1);
  driver.fail_init_call = 0;
  assert(device.prepare() == ESP_OK);
  const uint32_t allocations = driver.new_calls;
  const uint32_t initializations = driver.init_calls;
  const uint32_t deletions = driver.delete_calls;

  codec.fail_next_capture = true;
  assert(device.beginCapture() == ESP_FAIL);
  assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
  assert(!codec.capture_active);
  assert(device.beginCapture() == ESP_OK);
  assert(device.endCapture() == ESP_OK);

  assert(device.beginPlayback(16000, 1) == ESP_OK);
  driver.fail_next_enable = true;
  const std::array<uint8_t, 4> samples{{0, 0, 1, 0}};
  assert(device.writePlayback(samples.data(), samples.size()) == ESP_OK);
  assert(device.startPreloadedPlayback() == ESP_FAIL);
  assert(!codec.playback_active);
  device.abort();
  assert(device.mode() == EspI2sAudioDevice::Mode::Idle);
  assert(device.beginCapture() == ESP_OK);
  assert(device.endCapture() == ESP_OK);

  assert(driver.new_calls == allocations);
  assert(driver.init_calls == initializations);
  assert(driver.delete_calls == deletions);
  assert(device.shutdown() == ESP_OK);
}

void clockFailureAndDeleteFailureRemainRecoverable() {
  resetDriver();
  FakeCodec codec;
  auto* device = new EspI2sAudioDevice(config(0), codec);
  assert(device->prepare() == ESP_OK);
  driver.fail_next_clock = true;
  assert(device->beginPlayback(22050, 1) == ESP_FAIL);
  assert(device->mode() == EspI2sAudioDevice::Mode::Idle);
  assert(driver.new_calls == 2 && driver.init_calls == 2);
  assert(driver.delete_calls == 0);
  assert(device->beginPlayback(22050, 1) == ESP_OK);
  device->abort();

  driver.fail_next_delete = true;
  assert(device->shutdown() == ESP_FAIL);
  assert(device->diagnostics().shutdown_failures == 1);
  // The object and callback context remain alive; a later controlled shutdown
  // can finish deleting the ring which the driver initially refused.
  assert(device->shutdown() == ESP_OK);
  assert(driver.delete_calls == 3);
  delete device;
}

}  // namespace

esp_err_t i2s_new_channel(const i2s_chan_config_t* config,
                          i2s_chan_handle_t* tx,
                          i2s_chan_handle_t* rx) {
  ++driver.new_calls;
  if (!config || driver.channel_count >= driver.channels.size() ||
      ((tx == nullptr) == (rx == nullptr))) return ESP_ERR_INVALID_ARG;
  FakeI2sChannel* channel = &driver.channels[driver.channel_count++];
  *channel = FakeI2sChannel{};
  channel->port = config->id;
  channel->tx = tx != nullptr;
  channel->dma_desc_num = config->dma_desc_num;
  channel->dma_frame_num = config->dma_frame_num;
  if (tx) *tx = channel;
  if (rx) *rx = channel;
  return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel,
                                    const i2s_std_config_t* config) {
  ++driver.init_calls;
  if (driver.fail_init_call == driver.init_calls) return ESP_FAIL;
  if (!channel || !config || channel->deleted || channel->initialized)
    return ESP_ERR_INVALID_STATE;
  channel->initialized = true;
  channel->rate_hz = config->clk_cfg.sample_rate_hz;
  const size_t bytes_per_frame = channel->tx ? 4U : 2U;
  channel->dma_capacity_bytes =
      channel->dma_desc_num * channel->dma_frame_num * bytes_per_frame;
  if (channel->dma_capacity_bytes == 0U ||
      channel->dma_capacity_bytes > channel->dma.size()) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

esp_err_t i2s_channel_register_event_callback(
    i2s_chan_handle_t channel, const i2s_event_callbacks_t* callbacks,
    void* user_context) {
  ++driver.callback_calls;
  if (!channel || !channel->initialized || !callbacks)
    return ESP_ERR_INVALID_STATE;
  if (channel->tx) {
    driver.tx_callbacks = *callbacks;
    driver.tx_callback_context = user_context;
  }
  return ESP_OK;
}

esp_err_t i2s_channel_get_info(i2s_chan_handle_t channel,
                               i2s_chan_info_t* info) {
  ++driver.get_info_calls;
  if (!channel || !info || !channel->initialized || channel->deleted)
    return ESP_ERR_INVALID_ARG;
  info->total_dma_buf_size =
      static_cast<uint32_t>(channel->dma_capacity_bytes);
  return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_clock(
    i2s_chan_handle_t channel, const i2s_std_clk_config_t* config) {
  ++driver.clock_calls;
  if (driver.fail_next_clock) {
    driver.fail_next_clock = false;
    return ESP_FAIL;
  }
  if (!channel || !config || !channel->initialized || channel->enabled)
    return ESP_ERR_INVALID_STATE;
  channel->rate_hz = config->sample_rate_hz;
  return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_gpio(
    i2s_chan_handle_t channel, const i2s_std_gpio_config_t*) {
  ++driver.gpio_calls;
  if (driver.fail_next_gpio) {
    driver.fail_next_gpio = false;
    return ESP_FAIL;
  }
  return channel && channel->initialized && !channel->enabled
      ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t channel) {
  ++driver.enable_calls;
  if (driver.fail_next_enable) {
    driver.fail_next_enable = false;
    return ESP_FAIL;
  }
  if (!channel || !channel->initialized || channel->enabled ||
      anyOtherEnabled(channel)) return ESP_ERR_INVALID_STATE;
  channel->enabled = true;
  return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t channel) {
  ++driver.disable_calls;
  if (driver.fail_disable_call == driver.disable_calls) return ESP_FAIL;
  if (!channel || !channel->enabled) return ESP_ERR_INVALID_STATE;
  channel->enabled = false;
  channel->preload_cursor = 0U;
  channel->preloaded = false;
  return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t channel) {
  ++driver.delete_calls;
  if (driver.fail_next_delete) {
    driver.fail_next_delete = false;
    return ESP_FAIL;
  }
  if (!channel || channel->enabled || channel->deleted)
    return ESP_ERR_INVALID_STATE;
  channel->deleted = true;
  return ESP_OK;
}

esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void*, size_t bytes,
                           size_t* read, uint32_t) {
  if (!channel || !channel->enabled || channel->tx) return ESP_ERR_INVALID_STATE;
  *read = bytes;
  return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void*,
                            size_t bytes, size_t* written, uint32_t) {
  if (!channel || !channel->enabled || !channel->tx)
    return ESP_ERR_INVALID_STATE;
  *written = bytes;
  return ESP_OK;
}

esp_err_t i2s_channel_preload_data(i2s_chan_handle_t channel, const void* source,
                                   size_t size, size_t* loaded) {
  ++driver.preload_calls;
  if (driver.fail_preload_call == driver.preload_calls) return ESP_FAIL;
  if (!channel || channel->enabled || !channel->tx || !source || !loaded)
    return ESP_ERR_INVALID_STATE;
  const size_t remaining =
      channel->dma_capacity_bytes - channel->preload_cursor;
  if (remaining == 0U && driver.pretend_preload_never_full) {
    *loaded = std::min<size_t>(size, 1U);
    return ESP_OK;
  }
  *loaded = std::min(size, remaining);
  if (*loaded != 0U) {
    std::memcpy(channel->dma.data() + channel->preload_cursor,
                source, *loaded);
    channel->preload_cursor += *loaded;
    channel->preloaded = true;
  }
  return ESP_OK;
}

int64_t esp_timer_get_time() { return now_us; }

int main() {
  preparedRingsAreReusedAcrossTurns();
  ttsContinuationGraceIsNotAnUnderrun();
  abortedPlaybackScrubsPersistentDmaTail();
  endPlaybackScrubFailuresRemainRetryable();
  variableRateUsesClockReconfigurationWithoutDmaAllocation();
  prepareAndTurnFailuresRollBackWithoutRuntimeDeletion();
  clockFailureAndDeleteFailureRemainRecoverable();
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-i2s-lifecycle-"));
  try {
    const include = join(scratch, "include");
    mkdirSync(join(include, "driver"), { recursive: true });
    writeFileSync(join(include, "esp_err.h"), espErrHeader);
    writeFileSync(join(include, "esp_attr.h"), "#pragma once\n#define IRAM_ATTR\n");
    writeFileSync(
      join(include, "esp_timer.h"),
      "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
    );
    writeFileSync(join(include, "driver/i2s_std.h"), i2sHeader);
    const source = join(scratch, "lifecycle.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      include,
      "-I",
      join(native, "include"),
      "-I",
      join(audio, "include"),
      source,
      join(native, "esp_i2s_audio.cpp"),
      join(audio, "streaming_stereo_resampler.cpp"),
      "-o",
      binary,
    ];
    if (sanitized) {
      args.splice(
        1,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
    }
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? {
            ...process.env,
            ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
            UBSAN_OPTIONS: "halt_on_error=1",
          }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("prepared I2S DMA lifecycle passes strict and ASan/UBSan host seams", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("interactive audio paths contain no DMA allocation or channel deletion", () => {
  const source = readFileSync(join(native, "esp_i2s_audio.cpp"), "utf8");
  const beginCapture = source.match(
    /esp_err_t EspI2sAudioDevice::beginCapture\(\)[\s\S]*?\n}\n\n/,
  )?.[0];
  const beginPlayback = source.match(
    /esp_err_t EspI2sAudioDevice::beginPlayback\([\s\S]*?\n}\n\n/,
  )?.[0];
  const abort = source.match(
    /void EspI2sAudioDevice::abort\(\)[\s\S]*?\n}\n\n/,
  )?.[0];
  assert.ok(beginCapture && beginPlayback && abort);
  for (const runtimePath of [beginCapture, beginPlayback, abort]) {
    assert.doesNotMatch(
      runtimePath,
      /i2s_new_channel|i2s_channel_init_std_mode|i2s_del_channel/,
    );
  }
  assert.match(source, /EspI2sAudioDevice::prepare\(\)/);
  assert.match(source, /i2s_channel_reconfig_std_clock/);
  assert.match(source, /i2s_channel_reconfig_std_gpio/);
  assert.match(source, /EspI2sAudioDevice::shutdown\(\)/);
});

test("voice initialization prepares DMA before runtime stores and throttles telemetry", () => {
  const voice = readFileSync(
    join(
      repo,
      "firmware/inkloop-idf/components/inkloop_product/native_voice_service.cpp",
    ),
    "utf8",
  );
  const initialize = voice.match(
    /esp_err_t NativeVoiceService::initialize\(\)[\s\S]*?\n}\n\nvoid NativeVoiceService::shutdown/,
  )?.[0];
  assert.ok(initialize);
  assert.ok(
    initialize.indexOf("audio_device_->prepare()") <
      initialize.indexOf("xSemaphoreCreateMutexStatic"),
  );
  assert.match(voice, /kAudioDiagnosticsPublishMs = 250U/);
  assert.match(
    voice,
    /if \(!force && audio_diagnostics_published_[\s\S]*kAudioDiagnosticsPublishMs/,
  );
  assert.match(
    voice,
    /I2S shutdown failed; retaining callback storage/,
  );
});
