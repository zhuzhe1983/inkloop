#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "inkloop/duplex_pcm_bridge.hpp"
#include "inkloop/esp_i2s_audio.hpp"
#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/myai/MyAiClient.h"

namespace inkloop {

struct CrossCoreAudioDiagnostics {
  uint32_t playback_overflows = 0;
  uint32_t playback_hardware_failures = 0;
  uint32_t capture_overflows = 0;
  uint32_t capture_hardware_failures = 0;
  uint32_t capture_network_failures = 0;
  uint32_t cancellations = 0;
  size_t peak_playback_bytes = 0;
  size_t peak_capture_bytes = 0;
};

// MyAI callback adapter plus voice-owner pump. Socket callbacks only copy into
// a bounded PSRAM ring under a short lock; only the high-priority voice owner
// touches I2S. Capture flows in the opposite ring and is sent by the network
// owner without putting PCM bytes in WorkEnvelope/control queues.
class EspCrossCoreAudioBridge final : public myai::IAudioSink {
 public:
  static constexpr size_t kMaximumWssAudioMessageBytes = 12U * 1024U;
  static constexpr size_t kPlaybackCapacityBytes = 32U * 1024U;
  static constexpr size_t kCaptureCapacityBytes = 16U * 1024U;
  // Playback pumps one dynamically sized 10 ms frame.  This upper bound is a
  // 48 kHz stereo frame; capture is the fixed MyAI 16 kHz mono 10 ms frame.
  static constexpr size_t kMaximumPlaybackPumpBytes = 1920U;
  static constexpr size_t kCapturePumpBytes = 320U;

  EspCrossCoreAudioBridge();
  ~EspCrossCoreAudioBridge() override;

  EspCrossCoreAudioBridge(const EspCrossCoreAudioBridge&) = delete;
  EspCrossCoreAudioBridge& operator=(const EspCrossCoreAudioBridge&) = delete;

  esp_err_t initialize();
  bool initialized() const { return core_ != nullptr; }

  myai::Status begin(uint32_t sample_rate_hz, uint8_t channels) override;
  myai::Status write(const uint8_t* bytes, size_t length) override;
  myai::Status end() override;
  void abort() override;

  // Suitable for EspWssTransport's cooperative ingress gate. Text control
  // events stay readable while no TTS stream is open; during TTS, one full
  // legal WSS binary message must fit before another socket read is allowed.
  bool canPollWssIngress() const;
  static bool ingressReady(void* context);

  // Voice-owner methods (core 1).
  esp_err_t servicePlayback(EspI2sAudioDevice& device);
  esp_err_t beginCapture(EspI2sAudioDevice& device);
  esp_err_t captureStep(EspI2sAudioDevice& device,
                        uint32_t timeout_ms = 20);
  esp_err_t finishCapture(EspI2sAudioDevice& device);
  void cancelCapture(EspI2sAudioDevice& device);

  // Network-owner method (core 0). Calls at most one bounded send per tick.
  myai::Status pumpCaptureToNetwork(myai::MyAiClient& client);

  bool playbackBusy() const;
  bool captureBusy() const;
  bool playbackComplete() const;
  uint32_t captureGeneration() const;
  CrossCoreAudioDiagnostics diagnostics() const;

 private:
  enum class PlaybackHardwareState : uint8_t {
    Idle,
    StartPending,
    Starting,
    Active,
    StopPending,
    AbortPending,
    Fault,
  };

  void releaseBuffers();
  myai::Status audioError(myai::ErrorCode code, const char* detail,
                          uint32_t retry_ms = 0) const;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t* playback_storage_ = nullptr;
  uint8_t* capture_storage_ = nullptr;
  DuplexPcmBridgeCore* core_ = nullptr;
  PlaybackHardwareState playback_hardware_ = PlaybackHardwareState::Idle;
  uint32_t playback_hardware_generation_ = 0;
  uint32_t playback_sample_rate_hz_ = 0;
  uint8_t playback_channels_ = 0;
  uint32_t capture_generation_ = 0;
  bool capture_stop_sent_ = false;
  CrossCoreAudioDiagnostics diagnostics_{};
};

}  // namespace inkloop
