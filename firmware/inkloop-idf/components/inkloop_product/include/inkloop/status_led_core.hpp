#pragma once

#include <array>
#include <cstdint>

#include "inkloop/board.hpp"

namespace inkloop {

enum class VoiceLedMode : uint8_t {
  Off,
  Ready,
  Listening,
  Thinking,
  Speaking,
  Blocked,
  Error,
};

enum class ImageLedMode : uint8_t {
  Off,
  Generating,
  Downloading,
  Converting,
  Writing,
  Complete,
  Error,
};

struct StatusLedFrame {
  static constexpr size_t kRoleCount = 2;
  std::array<BoardRgbPixel, kRoleCount> pixels{};
  uint8_t count = 0;
};

class StatusLedCore final {
 public:
  void setVoiceMode(VoiceLedMode mode) { voice_mode_ = mode; }
  void setImageMode(ImageLedMode mode) { image_mode_ = mode; }
  void setMaximumBrightness(uint8_t value) { maximum_brightness_ = value; }
  void startHardwareTest(uint32_t now_ms) {
    hardware_test_started_ms_ = now_ms;
    hardware_test_active_ = true;
  }

  StatusLedFrame render(uint32_t now_ms, uint8_t available_pixels,
                        bool roles_swapped = false) const;
  bool hardwareTestActive(uint32_t now_ms) const;
  VoiceLedMode voiceMode() const { return voice_mode_; }
  ImageLedMode imageMode() const { return image_mode_; }
  uint8_t maximumBrightness() const { return maximum_brightness_; }

 private:
  static BoardRgbPixel scale(BoardRgbPixel color, uint8_t level,
                             uint8_t maximum);
  static BoardRgbPixel merge(BoardRgbPixel first, BoardRgbPixel second);
  static uint8_t pulse(uint32_t now_ms);
  static uint8_t blink(uint32_t now_ms);
  BoardRgbPixel renderVoice(uint32_t now_ms) const;
  BoardRgbPixel renderImage(uint32_t now_ms) const;
  std::array<BoardRgbPixel, StatusLedFrame::kRoleCount> renderHardwareTest(
      uint32_t elapsed_ms) const;

  VoiceLedMode voice_mode_ = VoiceLedMode::Off;
  ImageLedMode image_mode_ = ImageLedMode::Off;
  uint8_t maximum_brightness_ = 144;
  uint32_t hardware_test_started_ms_ = 0;
  bool hardware_test_active_ = false;
};

}  // namespace inkloop
