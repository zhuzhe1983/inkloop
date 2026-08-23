#include "inkloop/esp_i2s_audio.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_timer.h"

namespace inkloop {
namespace {

constexpr uint32_t kMinimumPlaybackRateHz = 8000;
constexpr uint32_t kMaximumPlaybackRateHz = 48000;
constexpr size_t kMonoExpansionSamples = 512;
constexpr size_t kStereoScaleSamples = 1024;

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

bool EspI2sAudioDevice::validConfig() const {
  return config_.capture_port != config_.playback_port &&
         validGpio(config_.mclk) && validGpio(config_.bclk) &&
         validGpio(config_.word_select) && validGpio(config_.capture_data) &&
         validGpio(config_.playback_data) &&
         config_.capture_sample_rate_hz == 16000 &&
         config_.dma_frame_count >= 160 && config_.dma_frame_count <= 1024 &&
         config_.dma_descriptor_count >= 2 &&
         config_.dma_descriptor_count <= 16;
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
  channel.dma_desc_num = config_.dma_descriptor_count;
  channel.dma_frame_num = config_.dma_frame_count;
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
  if (status == ESP_OK) status = i2s_channel_enable(playback_channel_);
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
  esp_err_t status = createPlaybackChannel(sample_rate_hz);
  if (status == ESP_OK) status = codec_.activatePlayback();
  if (status != ESP_OK) {
    ++diagnostics_.playback_failures;
    releasePlaybackChannel(false);
    return status;
  }
  playback_sample_rate_hz_ = sample_rate_hz;
  playback_channels_ = channels;
  playback_last_write_us_ = 0;
  const uint64_t dma_frames =
      static_cast<uint64_t>(config_.dma_frame_count) *
      config_.dma_descriptor_count;
  playback_drain_wait_us_ = static_cast<uint32_t>(
      (dma_frames * 1000000ULL + sample_rate_hz - 1U) / sample_rate_hz);
  mode_ = Mode::Playback;
  ++diagnostics_.playback_starts;
  return ESP_OK;
}

esp_err_t EspI2sAudioDevice::writeAll(const void* bytes, size_t length,
                                      uint32_t timeout_ms) {
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

esp_err_t EspI2sAudioDevice::writePlayback(const uint8_t* pcm16,
                                           size_t length,
                                           uint32_t timeout_ms) {
  if (mode_ != Mode::Playback || !playback_channel_)
    return ESP_ERR_INVALID_STATE;
  const size_t source_frame_bytes = static_cast<size_t>(playback_channels_) * 2U;
  if (!pcm16 || length == 0 || length % source_frame_bytes != 0)
    return ESP_ERR_INVALID_ARG;

  esp_err_t status = ESP_OK;
  if (playback_channels_ == 2 && volume_percent_ == 100) {
    status = writeAll(pcm16, length, timeout_ms);
  } else if (playback_channels_ == 2) {
    std::array<int16_t, kStereoScaleSamples> scaled{};
    size_t source_offset = 0;
    while (source_offset < length) {
      const size_t source_bytes = std::min(
          length - source_offset, scaled.size() * sizeof(int16_t));
      const size_t sample_count = source_bytes / sizeof(int16_t);
      for (size_t index = 0; index < sample_count; ++index) {
        int16_t sample = 0;
        std::memcpy(&sample, pcm16 + source_offset + index * sizeof(int16_t),
                    sizeof(sample));
        scaled[index] = static_cast<int16_t>(
            static_cast<int32_t>(sample) * volume_percent_ / 100);
      }
      status = writeAll(scaled.data(), source_bytes, timeout_ms);
      if (status != ESP_OK) break;
      source_offset += source_bytes;
    }
  } else {
    std::array<int16_t, kMonoExpansionSamples * 2U> stereo{};
    size_t source_offset = 0;
    while (source_offset < length) {
      const size_t source_bytes = std::min(
          length - source_offset, kMonoExpansionSamples * sizeof(int16_t));
      const size_t sample_count = source_bytes / sizeof(int16_t);
      for (size_t index = 0; index < sample_count; ++index) {
        int16_t sample = 0;
        std::memcpy(&sample, pcm16 + source_offset + index * sizeof(int16_t),
                    sizeof(sample));
        const int16_t scaled = static_cast<int16_t>(
            static_cast<int32_t>(sample) * volume_percent_ / 100);
        stereo[index * 2U] = scaled;
        stereo[index * 2U + 1U] = scaled;
      }
      status = writeAll(stereo.data(), sample_count * 2U * sizeof(int16_t),
                        timeout_ms);
      if (status != ESP_OK) break;
      source_offset += source_bytes;
    }
  }
  if (status == ESP_OK) {
    diagnostics_.played_source_bytes += length;
    playback_last_write_us_ = esp_timer_get_time();
  }
  return status;
}

bool EspI2sAudioDevice::playbackDrained() const {
  if (mode_ != Mode::Playback || !playback_channel_) return true;
  if (playback_last_write_us_ == 0 || playback_drain_wait_us_ == 0)
    return true;
  const int64_t elapsed = esp_timer_get_time() - playback_last_write_us_;
  return elapsed >= static_cast<int64_t>(playback_drain_wait_us_);
}

esp_err_t EspI2sAudioDevice::releasePlaybackChannel(bool deactivate_codec) {
  esp_err_t first = ESP_OK;
  if (deactivate_codec) {
    const esp_err_t status = codec_.deactivatePlayback();
    if (first == ESP_OK) first = status;
  }
  if (playback_channel_) {
    const esp_err_t disabled = i2s_channel_disable(playback_channel_);
    if (first == ESP_OK && disabled != ESP_OK) first = disabled;
    const esp_err_t deleted = i2s_del_channel(playback_channel_);
    if (first == ESP_OK && deleted != ESP_OK) first = deleted;
    playback_channel_ = nullptr;
  }
  playback_sample_rate_hz_ = 0;
  playback_channels_ = 0;
  playback_last_write_us_ = 0;
  playback_drain_wait_us_ = 0;
  return first;
}

esp_err_t EspI2sAudioDevice::endPlayback() {
  if (mode_ != Mode::Playback) return ESP_ERR_INVALID_STATE;
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

}  // namespace inkloop
