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

}  // namespace

EspI2sAudioDevice::EspI2sAudioDevice(const EspI2sAudioConfig& config,
                                     IAudioCodecControl& codec)
    : config_(config), codec_(codec) {}

EspI2sAudioDevice::~EspI2sAudioDevice() { abort(); }

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

esp_err_t EspI2sAudioDevice::createCaptureChannel() {
  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(config_.capture_port, I2S_ROLE_MASTER);
  channel.dma_desc_num = config_.dma_descriptor_count;
  channel.dma_frame_num = config_.dma_frame_count;
  esp_err_t status =
      i2s_new_channel(&channel, nullptr, &capture_channel_);
  if (status != ESP_OK) return status;

  i2s_std_config_t standard{};
  standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.capture_sample_rate_hz);
  standard.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
  standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
  standard.slot_cfg = captureSlotConfig();
  standard.gpio_cfg.mclk = config_.mclk;
  standard.gpio_cfg.bclk = config_.bclk;
  standard.gpio_cfg.ws = config_.word_select;
  standard.gpio_cfg.dout = GPIO_NUM_NC;
  standard.gpio_cfg.din = config_.capture_data;
  standard.gpio_cfg.invert_flags = {};
  status = i2s_channel_init_std_mode(capture_channel_, &standard);
  if (status == ESP_OK) status = i2s_channel_enable(capture_channel_);
  if (status != ESP_OK) {
    i2s_del_channel(capture_channel_);
    capture_channel_ = nullptr;
  }
  return status;
}

esp_err_t EspI2sAudioDevice::createPlaybackChannel(uint32_t sample_rate_hz) {
  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(config_.playback_port, I2S_ROLE_MASTER);
  channel.dma_desc_num = config_.playback_dma_descriptor_count;
  channel.dma_frame_num = config_.playback_dma_frame_count;
  channel.auto_clear_after_cb = true;
  esp_err_t status =
      i2s_new_channel(&channel, &playback_channel_, nullptr);
  if (status != ESP_OK) return status;

  i2s_std_config_t standard{};
  standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
  standard.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
  standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
  standard.slot_cfg = playbackSlotConfig();
  standard.gpio_cfg.mclk = config_.mclk;
  standard.gpio_cfg.bclk = config_.bclk;
  standard.gpio_cfg.ws = config_.word_select;
  standard.gpio_cfg.dout = config_.playback_data;
  standard.gpio_cfg.din = GPIO_NUM_NC;
  standard.gpio_cfg.invert_flags = {};
  status = i2s_channel_init_std_mode(playback_channel_, &standard);
  if (status == ESP_OK) {
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = &EspI2sAudioDevice::onPlaybackSent;
    callbacks.on_send_q_ovf =
        &EspI2sAudioDevice::onPlaybackQueueOverflow;
    status = i2s_channel_register_event_callback(
        playback_channel_, &callbacks, this);
  }
  if (status != ESP_OK) {
    i2s_del_channel(playback_channel_);
    playback_channel_ = nullptr;
  }
  return status;
}

esp_err_t EspI2sAudioDevice::beginCapture() {
  if (!validConfig()) return ESP_ERR_INVALID_ARG;
  if (mode_ != Mode::Idle || capture_channel_ || playback_channel_)
    return ESP_ERR_INVALID_STATE;
  esp_err_t status = createCaptureChannel();
  if (status == ESP_OK) status = codec_.activateCapture();
  if (status != ESP_OK) {
    ++diagnostics_.capture_failures;
    releaseCaptureChannel(false);
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

esp_err_t EspI2sAudioDevice::releaseCaptureChannel(bool deactivate_codec) {
  esp_err_t first = ESP_OK;
  if (deactivate_codec) {
    const esp_err_t status = codec_.deactivateCapture();
    if (first == ESP_OK) first = status;
  }
  if (capture_channel_) {
    const esp_err_t disabled = i2s_channel_disable(capture_channel_);
    if (first == ESP_OK && disabled != ESP_OK) first = disabled;
    const esp_err_t deleted = i2s_del_channel(capture_channel_);
    if (first == ESP_OK && deleted != ESP_OK) first = deleted;
    capture_channel_ = nullptr;
  }
  return first;
}

esp_err_t EspI2sAudioDevice::endCapture() {
  if (mode_ != Mode::Capture) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = releaseCaptureChannel(true);
  mode_ = Mode::Idle;
  return status;
}

esp_err_t EspI2sAudioDevice::beginPlayback(uint32_t sample_rate_hz,
                                           uint8_t channels) {
  if (!validConfig() || sample_rate_hz < kMinimumPlaybackRateHz ||
      sample_rate_hz > kMaximumPlaybackRateHz ||
      (channels != 1 && channels != 2))
    return ESP_ERR_INVALID_ARG;
  if (mode_ != Mode::Idle || capture_channel_ || playback_channel_)
    return ESP_ERR_INVALID_STATE;
  const uint32_t output_rate = config_.playback_sample_rate_hz == 0U
      ? sample_rate_hz
      : config_.playback_sample_rate_hz;
  if (!playback_resampler_.begin(sample_rate_hz, output_rate, channels))
    return ESP_ERR_INVALID_ARG;
  esp_err_t status = createPlaybackChannel(output_rate);
  if (status != ESP_OK) {
    ++diagnostics_.playback_failures;
    releasePlaybackChannel(false);
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
    releasePlaybackChannel(false);
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

esp_err_t EspI2sAudioDevice::finishPlaybackSource(uint32_t timeout_ms) {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  if (!playback_source_finished_) {
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
      const esp_err_t written = writeOutput(tail.data(), count, timeout_ms);
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
    uint32_t timeout_ms) {
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
    if (!playback_feed_monitor_.submit(
            (length - offset) / sizeof(StereoPcm16Frame),
            static_cast<uint64_t>(playback_last_write_us_))) {
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

esp_err_t EspI2sAudioDevice::releasePlaybackChannel(bool deactivate_codec) {
  esp_err_t first = ESP_OK;
  __atomic_store_n(&playback_source_open_for_isr_, 0U, __ATOMIC_RELAXED);
  playback_feed_monitor_.stop();
  if (deactivate_codec && playback_codec_active_) {
    const esp_err_t status = codec_.deactivatePlayback();
    if (first == ESP_OK) first = status;
    playback_codec_active_ = false;
  }
  if (playback_channel_) {
    if (playback_channel_enabled_) {
      const esp_err_t disabled = i2s_channel_disable(playback_channel_);
      if (first == ESP_OK && disabled != ESP_OK) first = disabled;
    }
    const esp_err_t deleted = i2s_del_channel(playback_channel_);
    if (first == ESP_OK && deleted != ESP_OK) first = deleted;
    playback_channel_ = nullptr;
  }
  playback_sample_rate_hz_ = 0;
  playback_output_rate_hz_ = 0;
  playback_channels_ = 0;
  playback_channel_enabled_ = false;
  playback_codec_active_ = false;
  playback_source_finished_ = false;
  playback_preloaded_bytes_ = 0;
  playback_preload_target_bytes_ = 0;
  playback_last_write_us_ = 0;
  playback_drain_wait_us_ = 0;
  playback_resampler_.reset();
  return first;
}

esp_err_t EspI2sAudioDevice::endPlayback() {
  if (mode_ != Mode::Playback) return ESP_ERR_INVALID_STATE;
  if (!playback_source_finished_) return ESP_ERR_INVALID_STATE;
  if (!playbackDrained()) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = releasePlaybackChannel(true);
  mode_ = Mode::Idle;
  return status;
}

void EspI2sAudioDevice::abort() {
  if (mode_ == Mode::Capture || capture_channel_) {
    releaseCaptureChannel(true);
    ++diagnostics_.forced_aborts;
  }
  if (mode_ == Mode::Playback || playback_channel_) {
    releasePlaybackChannel(true);
    ++diagnostics_.forced_aborts;
  }
  mode_ = Mode::Idle;
}

EspI2sAudioDiagnostics EspI2sAudioDevice::diagnostics() const {
  EspI2sAudioDiagnostics value = diagnostics_;
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
