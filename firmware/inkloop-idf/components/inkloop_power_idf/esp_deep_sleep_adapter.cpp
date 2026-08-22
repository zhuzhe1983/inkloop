#include "inkloop/esp_deep_sleep_adapter.hpp"

#include <array>
#include <limits>

namespace inkloop {
namespace {

constexpr uint64_t kMicrosecondsPerSecond = 1000000ULL;
constexpr std::array kWakeButtons{
    WakeButton::Voice,
    WakeButton::Previous,
    WakeButton::Next,
};

}  // namespace

EspDeepSleepAdapter::EspDeepSleepAdapter(
    IWakePinCapabilities& capabilities,
    const EspSleepFunctions& functions)
    : capabilities_(capabilities), functions_(functions) {}

bool EspDeepSleepAdapter::functionsValid() const {
  if (!functions_.reset_wake_sources ||
      !functions_.enable_timer_wakeup_us || !functions_.wakeup_flags ||
      !functions_.start_deep_sleep) {
    return false;
  }
  return wakeButtonMask() == 0U ||
      (functions_.enable_ext1_any_low && functions_.ext1_wakeup_mask);
}

bool EspDeepSleepAdapter::capabilitiesValid() const {
  uint64_t pins = 0;
  for (const WakeButton button : kWakeButtons) {
    if (!capabilities_.supportsWakeButton(button)) continue;
    const int pin = capabilities_.wakePin(button);
    if (pin < 0 || pin >= 64) return false;
    const uint64_t bit = 1ULL << static_cast<unsigned>(pin);
    if ((pins & bit) != 0U) return false;
    pins |= bit;
  }
  return true;
}

uint64_t EspDeepSleepAdapter::wakeButtonMask() const {
  if (!capabilitiesValid()) return 0;
  uint64_t mask = 0;
  for (const WakeButton button : kWakeButtons) {
    if (!capabilities_.supportsWakeButton(button)) continue;
    mask |= 1ULL << static_cast<unsigned>(capabilities_.wakePin(button));
  }
  return mask;
}

bool EspDeepSleepAdapter::wakeButtonsReleased() const {
  if (!capabilitiesValid()) return false;
  for (const WakeButton button : kWakeButtons) {
    if (!capabilities_.supportsWakeButton(button)) continue;
    if (capabilities_.wakeButtonPressed(button)) return false;
  }
  return true;
}

void EspDeepSleepAdapter::rollbackWakeSources() const {
  if (functions_.reset_wake_sources) functions_.reset_wake_sources();
}

DeepSleepResult EspDeepSleepAdapter::enterAfterSeconds(
    uint64_t timer_delay_seconds) {
  if (!functionsValid() || !capabilitiesValid()) {
    return DeepSleepResult::InvalidCapabilities;
  }
  if (timer_delay_seconds == 0 ||
      timer_delay_seconds >
          std::numeric_limits<uint64_t>::max() / kMicrosecondsPerSecond) {
    return DeepSleepResult::InvalidTimer;
  }
  if (!wakeButtonsReleased()) return DeepSleepResult::ButtonsHeld;

  if (functions_.reset_wake_sources() != 0) {
    return DeepSleepResult::WakeResetFailed;
  }
  if (functions_.enable_timer_wakeup_us(
          timer_delay_seconds * kMicrosecondsPerSecond) != 0) {
    rollbackWakeSources();
    return DeepSleepResult::TimerConfigurationFailed;
  }
  const uint64_t button_mask = wakeButtonMask();
  if (button_mask != 0U &&
      functions_.enable_ext1_any_low(button_mask) != 0) {
    rollbackWakeSources();
    return DeepSleepResult::ButtonConfigurationFailed;
  }

  functions_.start_deep_sleep();
  // Production never returns. Returning is a fail-closed test/fault path.
  rollbackWakeSources();
  return DeepSleepResult::EnterReturned;
}

WakeCause EspDeepSleepAdapter::decodeExt1Mask(uint64_t mask) const {
  const uint64_t supported = wakeButtonMask();
  if (supported == 0 || mask == 0 || (mask & ~supported) != 0) {
    return WakeCause::Unknown;
  }
  if ((mask & (mask - 1ULL)) != 0) return WakeCause::MultipleButtons;
  for (const WakeButton button : kWakeButtons) {
    if (!capabilities_.supportsWakeButton(button)) continue;
    if (mask != (1ULL << static_cast<unsigned>(
                         capabilities_.wakePin(button)))) {
      continue;
    }
    switch (button) {
      case WakeButton::Voice:
        return WakeCause::VoiceButton;
      case WakeButton::Previous:
        return WakeCause::PreviousButton;
      case WakeButton::Next:
        return WakeCause::NextButton;
    }
  }
  return WakeCause::Unknown;
}

WakeCause EspDeepSleepAdapter::wakeCause() const {
  if (!functionsValid() || !capabilitiesValid()) return WakeCause::Unknown;
  const uint32_t flags = functions_.wakeup_flags();
  if (flags == 0) return WakeCause::ColdBoot;
  if ((flags & ~kKnownSystemWakeFlags) != 0) return WakeCause::Unknown;
  const bool timer = (flags & kSystemWakeFlagTimer) != 0;
  const bool ext1 = (flags & kSystemWakeFlagExt1) != 0;
  if (timer && ext1) return WakeCause::MultipleSources;
  if (timer) return WakeCause::Timer;
  if (ext1) {
    return functions_.ext1_wakeup_mask
        ? decodeExt1Mask(functions_.ext1_wakeup_mask())
        : WakeCause::Unknown;
  }
  return WakeCause::Unknown;
}

}  // namespace inkloop
