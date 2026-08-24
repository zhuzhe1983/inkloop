#include "inkloop/esp_i2s_audio.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_attr.h"
#include "esp_timer.h"

namespace inkloop {
namespace {

constexpr uint32_t kMinimumPlaybackRateHz = 8000;
constexpr uint32_t kMaximumPlaybackRateHz = 48000;
constexpr uint32_t kMaximumBlockingWriteMilliseconds = 20U;
constexpr uint32_t kPlaybackPreloadMilliseconds = 60U;
constexpr size_t kOutputBatchFrames = 256U;
constexpr size_t kPlaybackScrubChunkBytes = 256U;
// validConfig() caps the ring at 16 descriptors of 1024 stereo PCM16 frames.
// ESP-IDF may round/cap an individual descriptor, but never beyond this
// configured upper bound.
constexpr size_t kMaximumPlaybackDmaRingBytes =
    16U * 1024U * 2U * sizeof(int16_t);

bool validGpio(gpio_num_t gpio) {
  return gpio >= GPIO_NUM_0 && gpio < GPIO_NUM_MAX;
}

i2s_std_slot_config_t captureSlotConfig() {
  i2s_std_slot_config_t slot =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                         I2S_SLOT_MODE_MONO);
  slot.slot_mask = I2S_STD_SLOT_LEFT;
  return slot;
}

i2s_std_slot_config_t playbackSlotConfig() {
  i2s_std_slot_config_t slot =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                         I2S_SLOT_MODE_STEREO);
  slot.slot_mask = I2S_STD_SLOT_BOTH;
  return slot;
}

i2s_std_clk_config_t standardClockConfig(uint32_t sample_rate_hz) {
  i2s_std_clk_config_t clock =
      I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
  clock.clk_src = I2S_CLK_SRC_PLL_160M;
  clock.mclk_multiple = I2S_MCLK_MULTIPLE_128;
  return clock;
}

i2s_std_gpio_config_t captureGpioConfig(
    const EspI2sAudioConfig& config) {
  i2s_std_gpio_config_t gpio{};
  gpio.mclk = config.mclk;
  gpio.bclk = config.bclk;
  gpio.ws = config.word_select;
  gpio.dout = GPIO_NUM_NC;
  gpio.din = config.capture_data;
  gpio.invert_flags = {};
  return gpio;
}

i2s_std_gpio_config_t playbackGpioConfig(
    const EspI2sAudioConfig& config) {
  i2s_std_gpio_config_t gpio{};
  gpio.mclk = config.mclk;
  gpio.bclk = config.bclk;
  gpio.ws = config.word_select;
  gpio.dout = config.playback_data;
  gpio.din = GPIO_NUM_NC;
  gpio.invert_flags = {};
  return gpio;
}

}  // namespace

EspI2sAudioDevice::EspI2sAudioDevice(const EspI2sAudioConfig& config,
                                     IAudioCodecControl& codec)
    : config_(config), codec_(codec) {}

EspI2sAudioDevice::~EspI2sAudioDevice() { ESP_ERROR_CHECK(shutdown()); }

bool IRAM_ATTR EspI2sAudioDevice::onPlaybackSent(
    i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_context) {
  (void)handle;
  (void)event;
  auto* device = static_cast<EspI2sAudioDevice*>(user_context);
  if (!device) return false;
  __atomic_fetch_add(&device->playback_dma_callbacks_isr_, 1U,
                     __ATOMIC_RELAXED);
  return false;
}

bool IRAM_ATTR EspI2sAudioDevice::onPlaybackQueueOverflow(
    i2s_chan_handle_t handle, i2s_event_data_t* event, void* user_context) {
  (void)handle;
  (void)event;
  auto* device = static_cast<EspI2sAudioDevice*>(user_context);
  if (!device) return false;
  uint32_t* counter =
      __atomic_load_n(&device->playback_source_open_for_isr_,
                      __ATOMIC_RELAXED) != 0U
          ? &device->playback_dma_underruns_isr_
          : &device->playback_dma_drain_overflows_isr_;
  __atomic_fetch_add(counter, 1U, __ATOMIC_RELAXED);
  return false;
}

bool EspI2sAudioDevice::validConfig() const {
  const uint32_t minimum_output_rate_hz =
      config_.playback_sample_rate_hz == 0U
          ? kMinimumPlaybackRateHz
          : config_.playback_sample_rate_hz;
  const bool playback_dma_fits_write_timeout =
      static_cast<uint64_t>(config_.playback_dma_frame_count) * 1000ULL <=
      static_cast<uint64_t>(minimum_output_rate_hz) *
          kMaximumBlockingWriteMilliseconds;
  return config_.capture_port != config_.playback_port &&
         validGpio(config_.mclk) && validGpio(config_.bclk) &&
         validGpio(config_.word_select) && validGpio(config_.capture_data) &&
         validGpio(config_.playback_data) &&
         config_.capture_sample_rate_hz == 16000 &&
         (config_.playback_sample_rate_hz == 0U ||
          (config_.playback_sample_rate_hz >= kMinimumPlaybackRateHz &&
           config_.playback_sample_rate_hz <= kMaximumPlaybackRateHz)) &&
         config_.dma_frame_count >= 160 && config_.dma_frame_count <= 1024 &&
         config_.dma_descriptor_count >= 2 &&
         config_.dma_descriptor_count <= 16 &&
         config_.playback_dma_frame_count >= 160 &&
         config_.playback_dma_frame_count <= 1024 &&
         playback_dma_fits_write_timeout &&
         config_.playback_dma_descriptor_count >= 2 &&
         config_.playback_dma_descriptor_count <= 16;
}

esp_err_t EspI2sAudioDevice::allocateCaptureChannel() {
  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(config_.capture_port, I2S_ROLE_MASTER);
  channel.dma_desc_num = config_.dma_descriptor_count;
  channel.dma_frame_num = config_.dma_frame_count;
  esp_err_t status =
      i2s_new_channel(&channel, nullptr, &capture_channel_);
  if (status != ESP_OK) return status;

  i2s_std_config_t standard{};
  standard.clk_cfg = standardClockConfig(config_.capture_sample_rate_hz);
  standard.slot_cfg = captureSlotConfig();
  standard.gpio_cfg = captureGpioConfig(config_);
  status = i2s_channel_init_std_mode(capture_channel_, &standard);
  if (status != ESP_OK) {
    const esp_err_t deleted = i2s_del_channel(capture_channel_);
    if (deleted == ESP_OK) {
      capture_channel_ = nullptr;
    } else {
      status = deleted;
    }
  }
  return status;
}

esp_err_t EspI2sAudioDevice::allocatePlaybackChannel(
    uint32_t sample_rate_hz) {
  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(config_.playback_port, I2S_ROLE_MASTER);
  channel.dma_desc_num = config_.playback_dma_descriptor_count;
  channel.dma_frame_num = config_.playback_dma_frame_count;
  channel.auto_clear_after_cb = true;
  esp_err_t status =
      i2s_new_channel(&channel, &playback_channel_, nullptr);
  if (status != ESP_OK) return status;

  i2s_std_config_t standard{};
  standard.clk_cfg = standardClockConfig(sample_rate_hz);
  standard.slot_cfg = playbackSlotConfig();
  standard.gpio_cfg = playbackGpioConfig(config_);
  status = i2s_channel_init_std_mode(playback_channel_, &standard);
  if (status == ESP_OK) {
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = &EspI2sAudioDevice::onPlaybackSent;
    callbacks.on_send_q_ovf =
        &EspI2sAudioDevice::onPlaybackQueueOverflow;
    status = i2s_channel_register_event_callback(
        playback_channel_, &callbacks, this);
  }
  if (status == ESP_OK) {
    i2s_chan_info_t info{};
    status = i2s_channel_get_info(playback_channel_, &info);
    if (status == ESP_OK &&
        (info.total_dma_buf_size == 0U ||
         info.total_dma_buf_size > kMaximumPlaybackDmaRingBytes ||
         (info.total_dma_buf_size % sizeof(StereoPcm16Frame)) != 0U)) {
      status = ESP_ERR_INVALID_SIZE;
    }
    if (status == ESP_OK) {
      playback_dma_ring_bytes_ = info.total_dma_buf_size;
    }
  }
  if (status != ESP_OK) {
    const esp_err_t deleted = i2s_del_channel(playback_channel_);
    if (deleted == ESP_OK) {
      playback_channel_ = nullptr;
      playback_dma_ring_bytes_ = 0U;
    } else {
      status = deleted;
    }
  }
  return status;
}

esp_err_t EspI2sAudioDevice::deletePreparedChannels() {
  esp_err_t first = ESP_OK;
  if (capture_channel_ && capture_channel_enabled_) {
    const esp_err_t status = i2s_channel_disable(capture_channel_);
    if (status == ESP_OK) capture_channel_enabled_ = false;
    if (first == ESP_OK) first = status;
  }
  if (playback_channel_ && playback_channel_enabled_) {
    const esp_err_t status = i2s_channel_disable(playback_channel_);
    if (status == ESP_OK) playback_channel_enabled_ = false;
    if (first == ESP_OK) first = status;
  }
  if (capture_channel_ && !capture_channel_enabled_) {
    const esp_err_t status = i2s_del_channel(capture_channel_);
    if (status == ESP_OK) capture_channel_ = nullptr;
    if (first == ESP_OK) first = status;
  }
  if (playback_channel_ && !playback_channel_enabled_) {
    const esp_err_t status = i2s_del_channel(playback_channel_);
    if (status == ESP_OK) {
      playback_channel_ = nullptr;
      playback_dma_ring_bytes_ = 0U;
    }
    if (first == ESP_OK) first = status;
  }
  if (!capture_channel_ && !playback_channel_) {
    prepared_ = false;
    playback_configured_rate_hz_ = 0U;
    diagnostics_.prepared_playback_rate_hz = 0U;
  }
  return first;
}

esp_err_t EspI2sAudioDevice::prepare() {
  ++diagnostics_.prepare_attempts;
  if (!validConfig()) {
    ++diagnostics_.prepare_failures;
    diagnostics_.last_prepare_error = ESP_ERR_INVALID_ARG;
    return ESP_ERR_INVALID_ARG;
  }
  if (prepared_) {
    const esp_err_t status = mode_ == Mode::Idle
                                 ? ESP_OK
                                 : ESP_ERR_INVALID_STATE;
    if (status != ESP_OK) ++diagnostics_.prepare_failures;
    diagnostics_.last_prepare_error = status;
    return status;
  }
  if (mode_ != Mode::Idle || capture_channel_ || playback_channel_) {
    ++diagnostics_.prepare_failures;
    diagnostics_.last_prepare_error = ESP_ERR_INVALID_STATE;
    return ESP_ERR_INVALID_STATE;
  }

  const uint32_t initial_playback_rate =
      config_.playback_sample_rate_hz == 0U
          ? config_.capture_sample_rate_hz
          : config_.playback_sample_rate_hz;
  esp_err_t status = allocateCaptureChannel();
  if (status == ESP_OK) {
    status = allocatePlaybackChannel(initial_playback_rate);
  }
  if (status != ESP_OK) {
    (void)deletePreparedChannels();
    ++diagnostics_.prepare_failures;
    diagnostics_.last_prepare_error = status;
    return status;
  }
  prepared_ = true;
  playback_configured_rate_hz_ = initial_playback_rate;
  diagnostics_.prepared_playback_rate_hz = initial_playback_rate;
  diagnostics_.last_prepare_error = ESP_OK;
  ++diagnostics_.prepare_successes;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::selectCapturePins() {
  const i2s_std_gpio_config_t gpio = captureGpioConfig(config_);
  const esp_err_t status =
      i2s_channel_reconfig_std_gpio(capture_channel_, &gpio);
  if (status == ESP_OK) {
    ++diagnostics_.shared_pin_selections;
  } else {
    ++diagnostics_.shared_pin_selection_failures;
  }
  return status;
}

esp_err_t EspI2sAudioDevice::selectPlaybackPins() {
  const i2s_std_gpio_config_t gpio = playbackGpioConfig(config_);
  const esp_err_t status =
      i2s_channel_reconfig_std_gpio(playback_channel_, &gpio);
  if (status == ESP_OK) {
    ++diagnostics_.shared_pin_selections;
  } else {
    ++diagnostics_.shared_pin_selection_failures;
  }
  return status;
}

esp_err_t EspI2sAudioDevice::configurePlaybackClock(
    uint32_t sample_rate_hz) {
  if (playback_configured_rate_hz_ == sample_rate_hz) return ESP_OK;
  const i2s_std_clk_config_t clock = standardClockConfig(sample_rate_hz);
  const esp_err_t status =
      i2s_channel_reconfig_std_clock(playback_channel_, &clock);
  if (status == ESP_OK) {
    playback_configured_rate_hz_ = sample_rate_hz;
    diagnostics_.prepared_playback_rate_hz = sample_rate_hz;
    ++diagnostics_.playback_clock_reconfigurations;
  } else {
    ++diagnostics_.playback_clock_reconfiguration_failures;
  }
  return status;
}

esp_err_t EspI2sAudioDevice::beginCapture() {
  if (!prepared_ || !capture_channel_ || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (mode_ != Mode::Idle || capture_channel_enabled_ ||
      playback_channel_enabled_ || capture_codec_active_ ||
      playback_codec_active_)
    return ESP_ERR_INVALID_STATE;
  esp_err_t status = selectCapturePins();
  if (status == ESP_OK) status = i2s_channel_enable(capture_channel_);
  if (status == ESP_OK) capture_channel_enabled_ = true;
  if (status == ESP_OK) status = codec_.activateCapture();
  if (status == ESP_OK) capture_codec_active_ = true;
  if (status != ESP_OK) {
    ++diagnostics_.capture_failures;
    (void)stopCaptureSession(true);
    return status;
  }
  mode_ = Mode::Capture;
  ++diagnostics_.capture_starts;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::readCapture(int16_t* samples,
                                         size_t sample_capacity,
                                         size_t& samples_read,
                                         uint32_t timeout_ms) {
  samples_read = 0;
  if (mode_ != Mode::Capture || !capture_channel_)
    return ESP_ERR_INVALID_STATE;
  if (!samples || sample_capacity == 0 ||
      sample_capacity > SIZE_MAX / sizeof(int16_t))
    return ESP_ERR_INVALID_ARG;
  size_t bytes_read = 0;
  const esp_err_t status = i2s_channel_read(
      capture_channel_, samples, sample_capacity * sizeof(int16_t),
      &bytes_read, timeout_ms);
  if (status == ESP_ERR_TIMEOUT) {
    ++diagnostics_.capture_timeouts;
    return status;
  }
  if (status != ESP_OK || (bytes_read & 1U) != 0) {
    ++diagnostics_.capture_failures;
    return status == ESP_OK ? ESP_ERR_INVALID_SIZE : status;
  }
  samples_read = bytes_read / sizeof(int16_t);
  diagnostics_.captured_bytes += bytes_read;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::stopCaptureSession(bool deactivate_codec) {
  esp_err_t first = ESP_OK;
  if (deactivate_codec && capture_codec_active_) {
    const esp_err_t status = codec_.deactivateCapture();
    if (first == ESP_OK) first = status;
    // Codec adapters guarantee an attempted deactivate leaves the path off,
    // even when they report the GPIO/I2C failure which caused it.
    capture_codec_active_ = false;
  }
  if (capture_channel_ && capture_channel_enabled_) {
    const esp_err_t disabled = i2s_channel_disable(capture_channel_);
    if (first == ESP_OK && disabled != ESP_OK) first = disabled;
    if (disabled == ESP_OK) capture_channel_enabled_ = false;
  }
  return first;
}

esp_err_t EspI2sAudioDevice::endCapture() {
  if (mode_ != Mode::Capture) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = stopCaptureSession(true);
  if (!capture_channel_enabled_) mode_ = Mode::Idle;
  return status;
}

esp_err_t EspI2sAudioDevice::beginPlayback(uint32_t sample_rate_hz,
                                           uint8_t channels) {
  if (!prepared_ || !capture_channel_ || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (sample_rate_hz < kMinimumPlaybackRateHz ||
      sample_rate_hz > kMaximumPlaybackRateHz ||
      (channels != 1 && channels != 2))
    return ESP_ERR_INVALID_ARG;
  if (mode_ != Mode::Idle || capture_channel_enabled_ ||
      playback_channel_enabled_ || capture_codec_active_ ||
      playback_codec_active_)
    return ESP_ERR_INVALID_STATE;
  const uint32_t output_rate = config_.playback_sample_rate_hz == 0U
      ? sample_rate_hz
      : config_.playback_sample_rate_hz;
  if (!playback_resampler_.begin(sample_rate_hz, output_rate, channels))
    return ESP_ERR_INVALID_ARG;
  esp_err_t status = configurePlaybackClock(output_rate);
  if (status == ESP_OK) status = selectPlaybackPins();
  if (status != ESP_OK) {
    ++diagnostics_.playback_failures;
    playback_resampler_.reset();
    return status;
  }
  playback_sample_rate_hz_ = sample_rate_hz;
  playback_output_rate_hz_ = output_rate;
  playback_channels_ = channels;
  playback_channel_enabled_ = false;
  playback_codec_active_ = false;
  playback_source_finished_ = false;
  playback_preloaded_bytes_ = 0;
  playback_last_write_us_ = 0;
  const uint64_t dma_frames =
      static_cast<uint64_t>(config_.playback_dma_frame_count) *
      config_.playback_dma_descriptor_count;
  const uint64_t preload_frames = std::min<uint64_t>(
      dma_frames,
      (static_cast<uint64_t>(output_rate) *
           kPlaybackPreloadMilliseconds +
       999U) /
          1000U);
  playback_preload_target_bytes_ = static_cast<size_t>(
      preload_frames * 2U * sizeof(int16_t));
  playback_drain_wait_us_ = static_cast<uint32_t>(
      (dma_frames * 1000000ULL + output_rate - 1U) / output_rate);
  if (!playback_feed_monitor_.begin(output_rate,
                                    static_cast<size_t>(dma_frames))) {
    ++diagnostics_.playback_failures;
    resetPlaybackSession();
    return ESP_ERR_INVALID_ARG;
  }
  __atomic_store_n(&playback_source_open_for_isr_, 0U, __ATOMIC_RELAXED);
  mode_ = Mode::Playback;
  ++diagnostics_.playback_starts;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::writeAll(const void* bytes, size_t length,
                                      uint32_t timeout_ms) {
  if (!playback_channel_enabled_) return ESP_ERR_INVALID_STATE;
  const auto* cursor = static_cast<const uint8_t*>(bytes);
  size_t remaining = length;
  while (remaining != 0) {
    size_t written = 0;
    const esp_err_t status = i2s_channel_write(
        playback_channel_, cursor, remaining, &written, timeout_ms);
    if (status != ESP_OK) {
      if (status == ESP_ERR_TIMEOUT) ++diagnostics_.playback_timeouts;
      else ++diagnostics_.playback_failures;
      return status;
    }
    if (written == 0 || written > remaining) {
      ++diagnostics_.playback_failures;
      return ESP_ERR_INVALID_RESPONSE;
    }
    cursor += written;
    remaining -= written;
  }
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::enablePlaybackChannel() {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (capture_channel_enabled_ || capture_codec_active_)
    return ESP_ERR_INVALID_STATE;
  if (playback_channel_enabled_) return ESP_OK;
  if (playback_preloaded_bytes_ == 0U) return ESP_ERR_INVALID_STATE;
  esp_err_t status = codec_.activatePlayback();
  if (status == ESP_OK) playback_codec_active_ = true;
  const int64_t started_us = esp_timer_get_time();
  if (status == ESP_OK &&
      !playback_feed_monitor_.start(static_cast<uint64_t>(started_us))) {
    status = ESP_ERR_INVALID_STATE;
  }
  if (status == ESP_OK) {
    __atomic_store_n(&playback_source_open_for_isr_,
                     playback_feed_monitor_.sourceOpen() ? 1U : 0U,
                     __ATOMIC_RELAXED);
  }
  if (status == ESP_OK) status = i2s_channel_enable(playback_channel_);
  if (status != ESP_OK) {
    __atomic_store_n(&playback_source_open_for_isr_, 0U,
                     __ATOMIC_RELAXED);
    playback_feed_monitor_.stop();
    if (playback_codec_active_) {
      codec_.deactivatePlayback();
      playback_codec_active_ = false;
    }
    ++diagnostics_.playback_failures;
    return status;
  }
  playback_channel_enabled_ = true;
  playback_last_write_us_ = started_us;
  ++diagnostics_.playback_preload_starts;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::startPreloadedPlayback() {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (playback_channel_enabled_ || playback_preloaded_bytes_ == 0U)
    return ESP_OK;
  return enablePlaybackChannel();
}

esp_err_t EspI2sAudioDevice::pausePlaybackIngress() {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (!playback_feed_monitor_.pauseSource(
          static_cast<uint64_t>(esp_timer_get_time()))) {
    return ESP_ERR_INVALID_STATE;
  }
  __atomic_store_n(&playback_source_open_for_isr_, 0U,
                   __ATOMIC_RELAXED);
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::resumePlaybackIngress() {
  if (mode_ != Mode::Playback || !playback_channel_ ||
      playback_source_finished_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!playback_feed_monitor_.resumeSource(
          static_cast<uint64_t>(esp_timer_get_time()))) {
    return ESP_ERR_INVALID_STATE;
  }
  __atomic_store_n(&playback_source_open_for_isr_,
                   playback_channel_enabled_ ? 1U : 0U,
                   __ATOMIC_RELAXED);
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::finishPlaybackSource(uint32_t timeout_ms) {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (!playback_source_finished_) {
    // The segment already ended before the continuation grace. Keep its
    // expected drain excluded from both ISR and cadence underrun accounting
    // while the resampler emits its held terminal interval.
    const esp_err_t paused = pausePlaybackIngress();
    if (paused != ESP_OK) return paused;
    std::array<StereoPcm16Frame,
               StreamingStereoResampler::kMaximumOutputFramesPerInput>
        tail{};
    const size_t count = playback_resampler_.finish(tail);
    if (!playback_resampler_.valid()) {
      ++diagnostics_.playback_failures;
      return ESP_ERR_INVALID_STATE;
    }
    playback_source_finished_ = true;
    if (count != 0U) {
      const esp_err_t written = writeOutput(
          tail.data(), count, timeout_ms, true);
      if (written != ESP_OK) return written;
    }
    const int64_t finished_us = esp_timer_get_time();
    playback_feed_monitor_.finishSource(
        static_cast<uint64_t>(finished_us));
    __atomic_store_n(&playback_source_open_for_isr_, 0U,
                     __ATOMIC_RELAXED);
  }
  return startPreloadedPlayback();
}

esp_err_t EspI2sAudioDevice::writeOutput(
    const StereoPcm16Frame* frames, size_t frame_count,
    uint32_t timeout_ms, bool terminal_tail) {
  if (!frames || frame_count == 0U ||
      frame_count > SIZE_MAX / sizeof(StereoPcm16Frame)) {
    return ESP_ERR_INVALID_ARG;
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(frames);
  const size_t length = frame_count * sizeof(StereoPcm16Frame);
  size_t offset = 0;
  if (!playback_channel_enabled_) {
    size_t loaded = 0;
    const esp_err_t preloaded = i2s_channel_preload_data(
        playback_channel_, bytes, length, &loaded);
    if (preloaded != ESP_OK || loaded > length) {
      ++diagnostics_.playback_failures;
      return preloaded == ESP_OK ? ESP_ERR_INVALID_RESPONSE : preloaded;
    }
    playback_preloaded_bytes_ += loaded;
    diagnostics_.peak_preloaded_bytes = std::max(
        diagnostics_.peak_preloaded_bytes, playback_preloaded_bytes_);
    if ((loaded % sizeof(StereoPcm16Frame)) != 0U ||
        (loaded != 0U && !playback_feed_monitor_.preload(
                            loaded / sizeof(StereoPcm16Frame)))) {
      ++diagnostics_.playback_failures;
      return ESP_ERR_INVALID_SIZE;
    }
    offset = loaded;
    if (playback_preloaded_bytes_ >= playback_preload_target_bytes_ ||
        loaded < length) {
      const esp_err_t enabled = enablePlaybackChannel();
      if (enabled != ESP_OK) return enabled;
    }
  }
  if (offset < length) {
    const esp_err_t written = writeAll(bytes + offset, length - offset,
                                       timeout_ms);
    if (written != ESP_OK) return written;
    playback_last_write_us_ = esp_timer_get_time();
    const size_t submitted_frames =
        (length - offset) / sizeof(StereoPcm16Frame);
    const uint64_t submitted_at =
        static_cast<uint64_t>(playback_last_write_us_);
    const bool monitored = terminal_tail
        ? playback_feed_monitor_.submitTerminal(submitted_frames,
                                                submitted_at)
        : playback_feed_monitor_.submit(submitted_frames, submitted_at);
    if (!monitored) {
      ++diagnostics_.playback_failures;
      return ESP_ERR_INVALID_STATE;
    }
  }
  diagnostics_.played_output_frames += frame_count;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::writePlayback(const uint8_t* pcm16,
                                           size_t length,
                                           uint32_t timeout_ms) {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (playback_source_finished_) return ESP_ERR_INVALID_STATE;
  const size_t source_frame_bytes = static_cast<size_t>(playback_channels_) * 2U;
  if (!pcm16 || length == 0 || length % source_frame_bytes != 0)
    return ESP_ERR_INVALID_ARG;

  esp_err_t status = ESP_OK;
  std::array<StereoPcm16Frame, kOutputBatchFrames> batch{};
  size_t batch_count = 0;
  for (size_t source_offset = 0; source_offset < length;
       source_offset += source_frame_bytes) {
    int16_t left = 0;
    int16_t right = 0;
    std::memcpy(&left, pcm16 + source_offset, sizeof(left));
    if (playback_channels_ == 2U) {
      std::memcpy(&right, pcm16 + source_offset + sizeof(left),
                  sizeof(right));
    } else {
      right = left;
    }
    std::array<StereoPcm16Frame,
               StreamingStereoResampler::kMaximumOutputFramesPerInput>
        converted{};
    const size_t converted_count = playback_resampler_.push(
        left, right, volume_percent_, converted);
    if (!playback_resampler_.valid()) {
      status = ESP_ERR_INVALID_STATE;
      break;
    }
    for (size_t index = 0; index < converted_count; ++index) {
      batch[batch_count++] = converted[index];
      if (batch_count == batch.size()) {
        status = writeOutput(batch.data(), batch_count, timeout_ms);
        batch_count = 0;
        if (status != ESP_OK) break;
      }
    }
    if (status != ESP_OK) break;
  }
  if (status == ESP_OK && batch_count != 0U) {
    status = writeOutput(batch.data(), batch_count, timeout_ms);
  }
  if (status == ESP_OK) {
    diagnostics_.played_source_bytes += length;
  }
  return status;
}

bool EspI2sAudioDevice::playbackDrained() const {
  if (mode_ != Mode::Playback || !playback_channel_) return true;
  if (!playback_channel_enabled_) return playback_preloaded_bytes_ == 0U;
  if (playback_last_write_us_ == 0 || playback_drain_wait_us_ == 0)
    return true;
  const int64_t elapsed = esp_timer_get_time() - playback_last_write_us_;
  return elapsed >= static_cast<int64_t>(playback_drain_wait_us_);
}

void EspI2sAudioDevice::resetPlaybackSession() {
  playback_sample_rate_hz_ = 0;
  playback_output_rate_hz_ = 0;
  playback_channels_ = 0;
  playback_source_finished_ = false;
  playback_preloaded_bytes_ = 0;
  playback_preload_target_bytes_ = 0;
  playback_last_write_us_ = 0;
  playback_drain_wait_us_ = 0;
  playback_resampler_.reset();
}

esp_err_t EspI2sAudioDevice::scrubPlaybackDmaRing() {
  if (!playback_channel_ || playback_channel_enabled_)
    return ESP_ERR_INVALID_STATE;
  if (playback_dma_ring_bytes_ == 0U ||
      playback_dma_ring_bytes_ > kMaximumPlaybackDmaRingBytes) {
    return ESP_ERR_INVALID_SIZE;
  }

  // i2s_channel_preload_data() advances through every READY TX descriptor.
  // Overwrite the exact driver-reported ring size, then require a one-byte
  // probe to report full. This performs no allocation and does not depend on
  // TX EOF callbacks, which might never run during an abort.
  const std::array<uint8_t, kPlaybackScrubChunkBytes> silence{};
  size_t scrubbed = 0U;
  while (scrubbed < playback_dma_ring_bytes_) {
    const size_t requested = std::min(
        silence.size(), playback_dma_ring_bytes_ - scrubbed);
    size_t loaded = 0U;
    const esp_err_t status = i2s_channel_preload_data(
        playback_channel_, silence.data(), requested, &loaded);
    if (status != ESP_OK) return status;
    if (loaded > requested) {
      return ESP_ERR_INVALID_RESPONSE;
    }
    // A short preload means the driver reached its actual ring end before the
    // size reported by i2s_channel_get_info(); fail closed as still dirty.
    if (loaded != requested) return ESP_ERR_INVALID_SIZE;
    scrubbed += loaded;
  }
  size_t probe_loaded = 0U;
  const esp_err_t probe = i2s_channel_preload_data(
      playback_channel_, silence.data(), 1U, &probe_loaded);
  if (probe != ESP_OK) return probe;
  if (probe_loaded != 0U) return ESP_ERR_INVALID_SIZE;

  // Preload leaves the cursor at the end of the now-silent ring. The codec is
  // already off, so one bounded enable/disable cycle resets the IDF cursor
  // without making any stale or scrub audio audible.
  esp_err_t status = i2s_channel_enable(playback_channel_);
  if (status != ESP_OK) return status;
  playback_channel_enabled_ = true;
  status = i2s_channel_disable(playback_channel_);
  if (status == ESP_OK) playback_channel_enabled_ = false;
  return status;
}

esp_err_t EspI2sAudioDevice::stopPlaybackSession(bool deactivate_codec) {
  esp_err_t first = ESP_OK;
  bool dma_clean = playback_preloaded_bytes_ == 0U;
  bool dma_cursor_reset = dma_clean;
  __atomic_store_n(&playback_source_open_for_isr_, 0U, __ATOMIC_RELAXED);
  playback_feed_monitor_.stop();
  if (deactivate_codec && playback_codec_active_) {
    const esp_err_t status = codec_.deactivatePlayback();
    if (first == ESP_OK) first = status;
    // See stopCaptureSession(): an attempted deactivate is a terminal codec
    // transition even when the adapter reports why power-down was imperfect.
    playback_codec_active_ = false;
  }
  if (playback_channel_ && playback_channel_enabled_) {
    const esp_err_t disabled = i2s_channel_disable(playback_channel_);
    if (first == ESP_OK && disabled != ESP_OK) first = disabled;
    if (disabled == ESP_OK) {
      playback_channel_enabled_ = false;
      dma_cursor_reset = true;
    }
  } else if (playback_channel_ && !dma_clean) {
    // A never-started preload has not passed through disable(), so first reset
    // its cursor. This does not clean DMA bytes; the explicit scrub below does.
    const esp_err_t enabled = i2s_channel_enable(playback_channel_);
    if (enabled == ESP_OK) {
      playback_channel_enabled_ = true;
      const esp_err_t disabled = i2s_channel_disable(playback_channel_);
      if (first == ESP_OK && disabled != ESP_OK) first = disabled;
      if (disabled == ESP_OK) {
        playback_channel_enabled_ = false;
        dma_cursor_reset = true;
      }
    } else if (first == ESP_OK) {
      first = enabled;
    }
  }
  if (playback_channel_ && !playback_channel_enabled_ && !dma_clean &&
      dma_cursor_reset) {
    const esp_err_t scrubbed = scrubPlaybackDmaRing();
    if (first == ESP_OK && scrubbed != ESP_OK) first = scrubbed;
    dma_clean = scrubbed == ESP_OK;
  }
  if (!playback_channel_enabled_ && dma_clean) resetPlaybackSession();
  return first;
}

esp_err_t EspI2sAudioDevice::endPlayback() {
  if (mode_ != Mode::Playback) return ESP_ERR_INVALID_STATE;
  if (!playback_source_finished_) return ESP_ERR_INVALID_STATE;
  if (!playbackDrained()) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = stopPlaybackSession(true);
  if (!playback_channel_enabled_ && playback_preloaded_bytes_ == 0U) {
    mode_ = Mode::Idle;
  }
  return status;
}

void EspI2sAudioDevice::abort() {
  const bool had_active_session =
      mode_ != Mode::Idle || capture_channel_enabled_ ||
      playback_channel_enabled_ || capture_codec_active_ ||
      playback_codec_active_;
  if (mode_ == Mode::Capture || capture_channel_enabled_ ||
      capture_codec_active_) {
    (void)stopCaptureSession(true);
  }
  if (mode_ == Mode::Playback || playback_channel_enabled_ ||
      playback_codec_active_) {
    (void)stopPlaybackSession(true);
  }
  if (!capture_channel_enabled_ && !playback_channel_enabled_ &&
      playback_preloaded_bytes_ == 0U) {
    mode_ = Mode::Idle;
  }
  if (had_active_session) ++diagnostics_.forced_aborts;
}

esp_err_t EspI2sAudioDevice::shutdown() {
  const bool had_prepared_channels = capture_channel_ || playback_channel_;
  abort();
  const esp_err_t status = deletePreparedChannels();
  if (had_prepared_channels && !capture_channel_ && !playback_channel_) {
    ++diagnostics_.shutdowns;
  }
  if (status != ESP_OK) ++diagnostics_.shutdown_failures;
  return status;
}

EspI2sAudioDiagnostics EspI2sAudioDevice::diagnostics() const {
  EspI2sAudioDiagnostics value = diagnostics_;
  value.prepared = prepared_;
  value.capture_channel_enabled = capture_channel_enabled_;
  value.playback_channel_enabled = playback_channel_enabled_;
  value.playback_dma_callbacks = __atomic_load_n(
      &playback_dma_callbacks_isr_, __ATOMIC_RELAXED);
  value.playback_dma_underruns = __atomic_load_n(
      &playback_dma_underruns_isr_, __ATOMIC_RELAXED);
  value.playback_dma_expected_drain_overflows = __atomic_load_n(
      &playback_dma_drain_overflows_isr_, __ATOMIC_RELAXED);
  value.playback_feed = playback_feed_monitor_.diagnostics();
  return value;
}

}  // namespace inkloop
