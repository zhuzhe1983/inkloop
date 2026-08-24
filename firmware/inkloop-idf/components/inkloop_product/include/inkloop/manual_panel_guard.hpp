#pragma once

#include <cstdint>

namespace inkloop {

// Keeps a user-selected e-paper frame stable for a short, wrap-safe window.
// Background synchronization may continue during the hold; only a scheduled
// physical panel replacement is deferred. Callers arm it only after a real
// panel write and durable current-image commit both succeed.
class ManualPanelGuard final {
 public:
  static constexpr uint32_t kDefaultHoldMs = 30000U;

  explicit constexpr ManualPanelGuard(
      uint32_t hold_ms = kDefaultHoldMs) : hold_ms_(hold_ms) {}

  void noteCompletedUserRefresh(uint32_t now_ms);
  bool active(uint32_t now_ms) const;
  void reset();

 private:
  static bool due(uint32_t now_ms, uint32_t deadline_ms);

  uint32_t hold_ms_ = kDefaultHoldMs;
  uint32_t deadline_ms_ = 0U;
  bool armed_ = false;
};

}  // namespace inkloop
