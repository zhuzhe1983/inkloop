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
  bool prepared = false;
  bool capture_channel_enabled = false;
  bool playback_channel_enabled = false;
  uint32_t prepare_attempts = 0;
  uint32_t prepare_successes = 0;
  uint32_t prepare_failures = 0;
  uint32_t shutdowns = 0;
  uint32_t shutdown_failures = 0;
  uint32_t playback_clock_reconfigurations = 0;
  uint32_t playback_clock_reconfiguration_failures = 0;
  uint32_t shared_pin_selections = 0;
  uint32_t shared_pin_selection_failures = 0;
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
  uint32_t prepared_playback_rate_hz = 0;
  int32_t last_prepare_error = ESP_OK;
  PlaybackFeedDiagnostics playback_feed{};
};

// Native half-duplex I2S standard-mode owner. It creates no task and owns no
// unbounded queue: only the high-priority voice task may call it. prepare()
// allocates and initializes both DMA rings once, before interactive work can
// fragment internal SRAM. Runtime turns only select the shared pins, re-clock
// a disabled TX channel when needed, and enable/disable the prepared rings.
// PaperColor's microphone and speaker share MCLK/BCLK/WS, so capture and
// playback are mutually exclusive even though they use separate controllers.
class EspI2sAudioDevice final {
 public:
  enum class Mode : uint8_t { Idle, Capture, Playback };

  EspI2sAudioDevice(const EspI2sAudioConfig& config,
                    IAudioCodecControl& codec);
  ~EspI2sAudioDevice();

  EspI2sAudioDevice(const EspI2sAudioDevice&) = delete;
  EspI2sAudioDevice& operator=(const EspI2sAudioDevice&) = delete;

  // Idempotent while idle. This is the only normal-runtime path that calls
  // i2s_new_channel/i2s_channel_init_std_mode and therefore allocates DMA.
  esp_err_t prepare();
  // Abort preserves prepared DMA. Only shutdown/destruction releases it.
  esp_err_t shutdown();

  esp_err_t beginCapture();
  esp_err_t readCapture(int16_t* samples, size_t sample_capacity,
                        size_t& samples_read, uint32_t timeout_ms = 20);
  esp_err_t endCapture();

  esp_err_t beginPlayback(uint32_t sample_rate_hz, uint8_t channels);
  esp_err_t writePlayback(const uint8_t* pcm16, size_t length,
                          uint32_t timeout_ms = 20);
  // Close only the current logical TTS segment for feed diagnostics. The
  // prepared TX channel and streaming resampler stay resumable during the
  // bridge's bounded continuation grace window.
  esp_err_t pausePlaybackIngress();
  esp_err_t resumePlaybackIngress();
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
  bool prepared() const { return prepared_; }
  uint32_t playbackSampleRateHz() const { return playback_sample_rate_hz_; }
  uint32_t playbackOutputRateHz() const {
    return playback_output_rate_hz_;
  }
  uint8_t playbackChannels() const { return playback_channels_; }
  bool playbackRunning() const { return playback_channel_enabled_; }
  EspI2sAudioDiagnostics diagnostics() const;

 private:
  bool validConfig() const;
  esp_err_t allocateCaptureChannel();
  esp_err_t allocatePlaybackChannel(uint32_t sample_rate_hz);
  esp_err_t selectCapturePins();
  esp_err_t selectPlaybackPins();
  esp_err_t configurePlaybackClock(uint32_t sample_rate_hz);
  esp_err_t stopCaptureSession(bool deactivate_codec);
  esp_err_t stopPlaybackSession(bool deactivate_codec);
  // IDF resets TX cursors/queues on disable but deliberately retains DMA
  // buffer bytes. Overwrite the complete disabled ring with silence before
  // reusing it so an aborted prompt cannot leak into the next turn.
  esp_err_t scrubPlaybackDmaRing();
  esp_err_t deletePreparedChannels();
  void resetPlaybackSession();
  esp_err_t enablePlaybackChannel();
  esp_err_t writeOutput(const StereoPcm16Frame* frames, size_t frame_count,
                        uint32_t timeout_ms,
                        bool terminal_tail = false);
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
  bool prepared_ = false;
  bool capture_channel_enabled_ = false;
  bool capture_codec_active_ = false;
  uint32_t playback_sample_rate_hz_ = 0;
  uint32_t playback_output_rate_hz_ = 0;
  uint32_t playback_configured_rate_hz_ = 0;
  // Exact driver-owned TX ring size captured after std-mode initialization.
  // Cleanup uses this rather than the requested descriptor geometry because
  // ESP-IDF may clamp or round individual DMA descriptors.
  size_t playback_dma_ring_bytes_ = 0;
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
