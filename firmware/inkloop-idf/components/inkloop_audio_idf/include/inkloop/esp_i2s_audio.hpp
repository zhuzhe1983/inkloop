#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "inkloop/playback_feed_monitor.hpp"
#include "inkloop/streaming_stereo_resampler.hpp"

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
  // Zero follows the incoming PCM rate. Boards with a fixed codec clock set
  // this explicitly and the voice owner resamples without allocating.
  uint32_t playback_sample_rate_hz = 0;
  uint16_t dma_frame_count = 320;
  uint8_t dma_descriptor_count = 6;
  // At the minimum supported 8 kHz rate this is one 20 ms descriptor, which
  // matches the bounded write timeout used by the Voice owner.
  uint16_t playback_dma_frame_count = 160;
  uint8_t playback_dma_descriptor_count = 6;
};

struct EspI2sAudioDiagnostics {
  uint32_t capture_starts = 0;
  uint32_t playback_starts = 0;
  uint32_t capture_timeouts = 0;
  uint32_t playback_timeouts = 0;
  uint32_t capture_failures = 0;
  uint32_t playback_failures = 0;
  uint32_t playback_preload_starts = 0;
  uint32_t forced_aborts = 0;
  size_t captured_bytes = 0;
  size_t played_source_bytes = 0;
  size_t played_output_frames = 0;
  size_t peak_preloaded_bytes = 0;
  // `on_send_q_ovf` means the producer left the IDF free-buffer queue full
  // for an entire DMA-ring turn. With auto-clear enabled, the recycled
  // descriptor carries silence, so a source-open event is a real underrun.
  uint32_t playback_dma_callbacks = 0;
  uint32_t playback_dma_underruns = 0;
  uint32_t playback_dma_expected_drain_overflows = 0;
  PlaybackFeedDiagnostics playback_feed{};
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
  // Starts already-converted preload without closing the source stream. This
  // lets adjacent same-format TTS segments resume during the grace window.
  // An empty prepared stream is a successful no-op.
  esp_err_t startPreloadedPlayback();
  // Closes the source resampler, emits its held final interval, and starts any
  // remaining preload. Idempotent; callers use this immediately before final
  // drain/teardown, never at a resumable TTS segment boundary.
  esp_err_t finishPlaybackSource(uint32_t timeout_ms = 20);
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
  uint32_t playbackOutputRateHz() const {
    return playback_output_rate_hz_;
  }
  uint8_t playbackChannels() const { return playback_channels_; }
  bool playbackRunning() const { return playback_channel_enabled_; }
  EspI2sAudioDiagnostics diagnostics() const;

 private:
  bool validConfig() const;
  esp_err_t createCaptureChannel();
  esp_err_t createPlaybackChannel(uint32_t sample_rate_hz);
  esp_err_t releaseCaptureChannel(bool deactivate_codec);
  esp_err_t releasePlaybackChannel(bool deactivate_codec);
  esp_err_t enablePlaybackChannel();
  esp_err_t writeOutput(const StereoPcm16Frame* frames, size_t frame_count,
                        uint32_t timeout_ms);
  esp_err_t writeAll(const void* bytes, size_t length, uint32_t timeout_ms);
  static bool onPlaybackSent(i2s_chan_handle_t handle,
                             i2s_event_data_t* event, void* user_context);
  static bool onPlaybackQueueOverflow(i2s_chan_handle_t handle,
                                      i2s_event_data_t* event,
                                      void* user_context);

  EspI2sAudioConfig config_;
  IAudioCodecControl& codec_;
  i2s_chan_handle_t capture_channel_ = nullptr;
  i2s_chan_handle_t playback_channel_ = nullptr;
  Mode mode_ = Mode::Idle;
  uint32_t playback_sample_rate_hz_ = 0;
  uint32_t playback_output_rate_hz_ = 0;
  uint8_t playback_channels_ = 0;
  bool playback_channel_enabled_ = false;
  bool playback_codec_active_ = false;
  bool playback_source_finished_ = false;
  size_t playback_preloaded_bytes_ = 0;
  size_t playback_preload_target_bytes_ = 0;
  int64_t playback_last_write_us_ = 0;
  uint32_t playback_drain_wait_us_ = 0;
  uint8_t volume_percent_ = 60;
  StreamingStereoResampler playback_resampler_{};
  PlaybackFeedMonitor playback_feed_monitor_{};
  // ISR-owned cumulative counters. IDF places the relaxed 32-bit atomic
  // helper and its short critical section in IRAM; callbacks never allocate,
  // log, touch PSRAM or take a task mutex while the cache may be unavailable.
  uint32_t playback_dma_callbacks_isr_ = 0;
  uint32_t playback_dma_underruns_isr_ = 0;
  uint32_t playback_dma_drain_overflows_isr_ = 0;
  uint32_t playback_source_open_for_isr_ = 0;
  EspI2sAudioDiagnostics diagnostics_{};
};

}  // namespace inkloop
