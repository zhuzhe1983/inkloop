#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace inkloop {

enum class AlbumStepResult : uint8_t {
  Selected,
  Empty,
  Busy,
  NotReady,
};

// Allocation-free selection/debounce policy shared by every future device
// adapter. Button presses update only this state; filesystem reads, rendering
// and e-paper I/O remain on the slow display owner.
class AlbumNavigationCore final {
 public:
  static constexpr size_t kNoOrdinal = std::numeric_limits<size_t>::max();

  explicit AlbumNavigationCore(uint32_t settle_ms = 1000U)
      : settle_ms_(settle_ms) {}

  bool synchronize(size_t count, size_t current_ordinal);
  AlbumStepResult step(int direction, uint32_t now_ms, size_t& ordinal);
  bool takeSettled(uint32_t now_ms, size_t& ordinal, bool& changed);
  bool beginImmediate(size_t ordinal);
  void finish(size_t count, size_t current_ordinal);
  void invalidate();

  size_t count() const { return count_; }
  size_t currentOrdinal() const { return current_ordinal_; }
  size_t selectedOrdinal() const { return selected_ordinal_; }
  bool pending() const { return pending_; }
  bool refreshing() const { return refreshing_; }

 private:
  static bool due(uint32_t now_ms, uint32_t deadline_ms);

  uint32_t settle_ms_ = 1000U;
  uint32_t settle_deadline_ms_ = 0;
  size_t count_ = 0;
  size_t current_ordinal_ = kNoOrdinal;
  size_t selected_ordinal_ = kNoOrdinal;
  bool ready_ = false;
  bool pending_ = false;
  bool refreshing_ = false;
};

}  // namespace inkloop
