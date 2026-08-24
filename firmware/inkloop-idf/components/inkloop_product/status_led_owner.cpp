#include "inkloop/status_led_owner.hpp"

#include "esp_timer.h"
#include "inkloop/product_opcodes.hpp"

namespace inkloop {
namespace {

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

}  // namespace

esp_err_t EspStatusLedOwner::configure() {
  if (configured_) return ESP_ERR_INVALID_STATE;
  const uint8_t available = board_.descriptor().rgb_pixels;
  if (available > StatusLedFrame::kRoleCount) return ESP_ERR_NOT_SUPPORTED;
  esp_err_t status = supervisor_.registerHandler(
      TaskLane::Led, &EspStatusLedOwner::handle, this);
  if (status == ESP_OK) {
    status = supervisor_.registerTickHandler(
        TaskLane::Led, &EspStatusLedOwner::tick, this, 20);
  }
  if (status == ESP_OK) {
    physical_pixels_ = available;
    configured_ = true;
  }
  return status;
}

void EspStatusLedOwner::shutdown() {
  if (physical_pixels_ > 0U) {
    std::array<BoardRgbPixel, StatusLedFrame::kRoleCount> dark{};
    board_.setRgb(dark.data(), physical_pixels_);
  }
  portENTER_CRITICAL(&mux_);
  core_ = StatusLedCore();
  physical_pixels_ = 0U;
  roles_swapped_ = false;
  configured_ = false;
  portEXIT_CRITICAL(&mux_);
}

StatusLedCore EspStatusLedOwner::snapshot() const {
  portENTER_CRITICAL(&mux_);
  const StatusLedCore value = core_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

void EspStatusLedOwner::setMaximumBrightnessPercent(
    uint8_t percent, bool run_hardware_test) {
  const uint8_t bounded = percent > 100U ? 100U : percent;
  const uint8_t raw = static_cast<uint8_t>(
      (static_cast<uint16_t>(bounded) * 255U) / 100U);
  portENTER_CRITICAL(&mux_);
  core_.setMaximumBrightness(raw);
  if (run_hardware_test) core_.startHardwareTest(nowMs());
  portEXIT_CRITICAL(&mux_);
}

void EspStatusLedOwner::setPresentation(
    uint8_t percent, bool roles_swapped, bool run_hardware_test) {
  const uint8_t bounded = percent > 100U ? 100U : percent;
  const uint8_t raw = static_cast<uint8_t>(
      (static_cast<uint16_t>(bounded) * 255U) / 100U);
  portENTER_CRITICAL(&mux_);
  core_.setMaximumBrightness(raw);
  roles_swapped_ = physical_pixels_ >= 2U && roles_swapped;
  if (run_hardware_test) core_.startHardwareTest(nowMs());
  portEXIT_CRITICAL(&mux_);
}

WorkDisposition EspStatusLedOwner::handle(const WorkEnvelope& envelope,
                                           void* context) {
  return context
             ? static_cast<EspStatusLedOwner*>(context)->handleCommand(envelope)
             : WorkDisposition::Failed;
}

void EspStatusLedOwner::tick(void* context) {
  if (context) static_cast<EspStatusLedOwner*>(context)->service();
}

WorkDisposition EspStatusLedOwner::handleCommand(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::LedStatus)
    return WorkDisposition::Failed;
  portENTER_CRITICAL(&mux_);
  bool valid = true;
  if (envelope.opcode == productOpcode(ProductOpcode::SetVoiceLed) &&
      envelope.flags <= static_cast<uint8_t>(VoiceLedMode::Error)) {
    core_.setVoiceMode(static_cast<VoiceLedMode>(envelope.flags));
  } else if (envelope.opcode == productOpcode(ProductOpcode::SetImageLed) &&
             envelope.flags <= static_cast<uint8_t>(ImageLedMode::Error)) {
    core_.setImageMode(static_cast<ImageLedMode>(envelope.flags));
  } else if (envelope.opcode ==
                 productOpcode(ProductOpcode::SetLedMaximumBrightness) &&
             envelope.flags <= 100U) {
    core_.setMaximumBrightness(static_cast<uint8_t>(
        static_cast<uint16_t>(envelope.flags) * 255U / 100U));
    core_.startHardwareTest(nowMs());
  } else {
    valid = false;
  }
  portEXIT_CRITICAL(&mux_);
  if (valid) service();
  return valid ? WorkDisposition::Complete : WorkDisposition::Failed;
}

void EspStatusLedOwner::service() {
  StatusLedFrame frame;
  bool swap_roles = false;
  portENTER_CRITICAL(&mux_);
  swap_roles = roles_swapped_;
  frame = core_.render(nowMs(), physical_pixels_, swap_roles);
  portEXIT_CRITICAL(&mux_);
  if (frame.count > 0U) {
    board_.setRgb(frame.pixels.data(), frame.count);
  }
}

}  // namespace inkloop
