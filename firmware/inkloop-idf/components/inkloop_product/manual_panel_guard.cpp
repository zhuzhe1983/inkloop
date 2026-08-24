#include "inkloop/manual_panel_guard.hpp"

namespace inkloop {

bool ManualPanelGuard::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void ManualPanelGuard::noteCompletedUserRefresh(uint32_t now_ms) {
  // A zero or half-range duration cannot be ordered by signed wrap-safe
  // comparison. The production duration is 30 seconds; fail closed to no hold
  // for an invalid injected test/configuration value.
  if (hold_ms_ == 0U || hold_ms_ > 0x7fffffffU) {
    reset();
    return;
  }
  deadline_ms_ = now_ms + hold_ms_;
  armed_ = true;
}

bool ManualPanelGuard::active(uint32_t now_ms) const {
  return armed_ && !due(now_ms, deadline_ms_);
}

void ManualPanelGuard::reset() {
  deadline_ms_ = 0U;
  armed_ = false;
}

}  // namespace inkloop
