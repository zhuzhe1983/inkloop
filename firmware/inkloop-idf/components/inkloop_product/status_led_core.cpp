#include "inkloop/status_led_core.hpp"

#include <algorithm>

namespace inkloop {

BoardRgbPixel StatusLedCore::scale(BoardRgbPixel color, uint8_t level,
                                   uint8_t maximum) {
  const uint16_t bounded = std::min<uint16_t>(level, maximum);
  color.red = static_cast<uint8_t>(color.red * bounded / 255U);
  color.green = static_cast<uint8_t>(color.green * bounded / 255U);
  color.blue = static_cast<uint8_t>(color.blue * bounded / 255U);
  return color;
}

BoardRgbPixel StatusLedCore::merge(BoardRgbPixel first,
                                   BoardRgbPixel second) {
  return {
      std::max(first.red, second.red),
      std::max(first.green, second.green),
      std::max(first.blue, second.blue),
  };
}

uint8_t StatusLedCore::pulse(uint32_t now_ms) {
  constexpr uint32_t kPeriodMs = 1600;
  const uint32_t phase = now_ms % kPeriodMs;
  const uint32_t ramp = phase < kPeriodMs / 2U ? phase : kPeriodMs - phase;
  return static_cast<uint8_t>(64U + ramp * 191U / (kPeriodMs / 2U));
}

uint8_t StatusLedCore::blink(uint32_t now_ms) {
  return (now_ms / 350U) % 2U == 0U ? 255U : 24U;
}

BoardRgbPixel StatusLedCore::renderVoice(uint32_t now_ms) const {
  switch (voice_mode_) {
    case VoiceLedMode::Off:
      return {};
    case VoiceLedMode::Ready:
      return scale({0, 96, 255}, 180, maximum_brightness_);
    case VoiceLedMode::Listening:
      return scale({0, 255, 48}, 255, maximum_brightness_);
    case VoiceLedMode::Thinking:
      return scale({0, 220, 255}, pulse(now_ms), maximum_brightness_);
    case VoiceLedMode::Speaking:
      return scale({180, 40, 255}, pulse(now_ms), maximum_brightness_);
    case VoiceLedMode::Blocked:
      return scale({255, 0, 0}, blink(now_ms), maximum_brightness_);
    case VoiceLedMode::Error:
      return scale({255, 0, 0}, 255, maximum_brightness_);
  }
  return {};
}

BoardRgbPixel StatusLedCore::renderImage(uint32_t now_ms) const {
  switch (image_mode_) {
    case ImageLedMode::Off:
      return {};
    case ImageLedMode::Generating:
      return scale({0, 255, 48}, pulse(now_ms), maximum_brightness_);
    case ImageLedMode::Downloading:
      return scale({0, 96, 255}, pulse(now_ms), maximum_brightness_);
    case ImageLedMode::Converting:
      return scale({0, 220, 255}, pulse(now_ms), maximum_brightness_);
    case ImageLedMode::Writing:
      return scale({255, 180, 0}, blink(now_ms), maximum_brightness_);
    case ImageLedMode::Complete:
      return scale({0, 255, 48}, 255, maximum_brightness_);
    case ImageLedMode::Error:
      return scale({255, 0, 0}, 255, maximum_brightness_);
  }
  return {};
}

bool StatusLedCore::hardwareTestActive(uint32_t now_ms) const {
  return hardware_test_active_ &&
      static_cast<uint32_t>(now_ms - hardware_test_started_ms_) < 4600U;
}

std::array<BoardRgbPixel, StatusLedFrame::kRoleCount>
StatusLedCore::renderHardwareTest(uint32_t elapsed_ms) const {
  const auto color = [this](BoardRgbPixel value) {
    return scale(value, 255U, maximum_brightness_);
  };
  if (elapsed_ms < 800U) {
    const BoardRgbPixel white = color({255, 255, 255});
    return {{white, white}};
  }
  if (elapsed_ms < 1000U) return {};
  if (elapsed_ms < 2600U) {
    const uint32_t phase = (elapsed_ms - 1000U) / 400U;
    const BoardRgbPixel voice = (phase & 1U) == 0U
        ? color({0, 96, 255}) : color({0, 220, 255});
    return {{voice, {}}};
  }
  if (elapsed_ms < 2800U) return {};
  if (elapsed_ms < 4400U) {
    const uint32_t phase = (elapsed_ms - 2800U) / 400U;
    const BoardRgbPixel image = (phase & 1U) == 0U
        ? color({255, 210, 0}) : color({255, 96, 0});
    return {{{}, image}};
  }
  return {};
}

StatusLedFrame StatusLedCore::render(uint32_t now_ms,
                                     uint8_t available_pixels) const {
  StatusLedFrame output;
  if (available_pixels == 0U) return output;
  const std::array<BoardRgbPixel, StatusLedFrame::kRoleCount> logical =
      hardwareTestActive(now_ms)
          ? renderHardwareTest(
                static_cast<uint32_t>(now_ms - hardware_test_started_ms_))
          : std::array<BoardRgbPixel, StatusLedFrame::kRoleCount>{{
                renderVoice(now_ms), renderImage(now_ms)}};
  if (available_pixels == 1U) {
    output.pixels[0] = merge(logical[0], logical[1]);
    output.count = 1U;
    return output;
  }
  // Two-pixel boards preserve the established physical role order.
  output.pixels = logical;
  output.count = static_cast<uint8_t>(output.pixels.size());
  return output;
}

}  // namespace inkloop
