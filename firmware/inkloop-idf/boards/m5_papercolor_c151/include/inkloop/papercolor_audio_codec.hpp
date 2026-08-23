#pragma once

#include "driver/i2c_master.h"
#include "inkloop/esp_i2s_audio.hpp"

namespace inkloop {

// C151 audio power/codec adapter. The board owner supplies the already-created
// internal I2C bus (SCL=2, SDA=3); this class never creates a competing bus.
class PaperColorAudioCodec final : public IAudioCodecControl {
 public:
  PaperColorAudioCodec() = default;
  ~PaperColorAudioCodec() override;

  PaperColorAudioCodec(const PaperColorAudioCodec&) = delete;
  PaperColorAudioCodec& operator=(const PaperColorAudioCodec&) = delete;

  esp_err_t initialize(i2c_master_bus_handle_t internal_bus);
  void shutdown();
  // Keeps the borrowed I2C device handles alive for an aborted sleep while
  // proving both half-duplex paths idle and removing codec/speaker power.
  esp_err_t prepareForDeepSleep();

  esp_err_t activateCapture() override;
  esp_err_t deactivateCapture() override;
  esp_err_t activatePlayback() override;
  esp_err_t deactivatePlayback() override;

  bool initialized() const { return initialized_; }
  bool captureActive() const { return capture_active_; }
  bool playbackActive() const { return playback_active_; }

 private:
  esp_err_t writeRegister(i2c_master_dev_handle_t device, uint8_t reg,
                          uint8_t value);
  esp_err_t setPower(bool codec_enabled, bool speaker_enabled);

  i2c_master_dev_handle_t capture_codec_ = nullptr;
  i2c_master_dev_handle_t playback_codec_ = nullptr;
  bool initialized_ = false;
  bool capture_active_ = false;
  bool playback_active_ = false;
};

EspI2sAudioConfig papercolor_audio_config();

}  // namespace inkloop
