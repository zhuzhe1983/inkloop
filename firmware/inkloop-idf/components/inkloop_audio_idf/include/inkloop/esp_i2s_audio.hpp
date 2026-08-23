#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_err.h"

namespace inkloop {

class IAudioCodecControl {
 public:
  virtual ~IAudioCodecControl() = default;
  virtual esp_err_t activateCapture() = 0;
  virtual esp_err_t deactivateCapture() = 0;
  virtual esp_err_t activatePlayback() = 0;
  virtual esp_err_t deactivatePlayback() = 0;
};

struct EspI2sAudioConfig {
  int capture_port = I2S_NUM_1;
  int playback_port = I2S_NUM_0;
  gpio_num_t mclk = GPIO_NUM_NC;
  gpio_num_t bclk = GPIO_NUM_NC;
  gpio_num_t word_select = GPIO_NUM_NC;
  gpio_num_t capture_data = GPIO_NUM_NC;
  gpio_num_t playback_data = GPIO_NUM_NC;
  uint32_t capture_sample_rate_hz = 16000;
  uint16_t dma_frame_count = 320;
  uint8_t dma_descriptor_count = 6;
};

struct EspI2sAudioDiagnostics {
  uint32_t capture_starts = 0;
  uint32_t playback_starts = 0;
  uint32_t capture_timeouts = 0;
  uint32_t playback_timeouts = 0;
  uint32_t capture_failures = 0;
  uint32_t playback_failures = 0;
  uint32_t forced_aborts = 0;
  size_t captured_bytes = 0;
  size_t played_source_bytes = 0;
};

// Native half-duplex I2S standard-mode owner. It creates no task and owns no
// unbounded queue: only the high-priority voice task may call it. PaperColor's
// microphone and speaker share MCLK/BCLK/WS, so capture and playback are
// mutually exclusive even though they use separate I2S controllers.
class EspI2sAudioDevice final {
 public:
  enum class Mode : uint8_t { Idle, Capture, Playback };

  EspI2sAudioDevice(const EspI2sAudioConfig& config,
                    IAudioCodecControl& codec);
  ~EspI2sAudioDevice();

  EspI2sAudioDevice(const EspI2sAudioDevice&) = delete;
  EspI2sAudioDevice& operator=(const EspI2sAudioDevice&) = delete;

  esp_err_t beginCapture();
  esp_err_t readCapture(int16_t* samples, size_t sample_capacity,
                        size_t& samples_read, uint32_t timeout_ms = 20);
  esp_err_t endCapture();

  esp_err_t beginPlayback(uint32_t sample_rate_hz, uint8_t channels);
  esp_err_t writePlayback(const uint8_t* pcm16, size_t length,
                          uint32_t timeout_ms = 20);
  // The IDF TX write call completes when PCM has entered DMA, not when the
  // final sample has reached the codec.  Keep the channel alive until one
  // complete DMA ring has elapsed so short prompts and TTS tails are not cut.
  bool playbackDrained() const;
  esp_err_t endPlayback();
  void setVolumePercent(uint8_t value) {
    volume_percent_ = value > 100 ? 100 : value;
  }
  void abort();

  Mode mode() const { return mode_; }
  uint32_t playbackSampleRateHz() const { return playback_sample_rate_hz_; }
  uint8_t playbackChannels() const { return playback_channels_; }
  const EspI2sAudioDiagnostics& diagnostics() const { return diagnostics_; }

 private:
  bool validConfig() const;
  esp_err_t createCaptureChannel();
  esp_err_t createPlaybackChannel(uint32_t sample_rate_hz);
  esp_err_t releaseCaptureChannel(bool deactivate_codec);
  esp_err_t releasePlaybackChannel(bool deactivate_codec);
  esp_err_t writeAll(const void* bytes, size_t length, uint32_t timeout_ms);

  EspI2sAudioConfig config_;
  IAudioCodecControl& codec_;
  i2s_chan_handle_t capture_channel_ = nullptr;
  i2s_chan_handle_t playback_channel_ = nullptr;
  Mode mode_ = Mode::Idle;
  uint32_t playback_sample_rate_hz_ = 0;
  uint8_t playback_channels_ = 0;
  int64_t playback_last_write_us_ = 0;
  uint32_t playback_drain_wait_us_ = 0;
  uint8_t volume_percent_ = 60;
  EspI2sAudioDiagnostics diagnostics_{};
};

}  // namespace inkloop
