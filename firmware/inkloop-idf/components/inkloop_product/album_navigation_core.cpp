#include "inkloop/album_navigation_core.hpp"

namespace inkloop {

bool AlbumNavigationCore::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool AlbumNavigationCore::synchronize(size_t count, size_t current_ordinal) {
  if (refreshing_) return false;
  if (current_ordinal != kNoOrdinal && current_ordinal >= count) return false;
  count_ = count;
  current_ordinal_ = current_ordinal;
  if (!pending_ || selected_ordinal_ >= count_) {
    selected_ordinal_ = current_ordinal_;
    pending_ = false;
  }
  ready_ = true;
  return true;
}

AlbumStepResult AlbumNavigationCore::step(int direction, uint32_t now_ms,
                                          size_t& ordinal) {
  ordinal = kNoOrdinal;
  if (!ready_ || (direction != -1 && direction != 1))
    return AlbumStepResult::NotReady;
  if (refreshing_) return AlbumStepResult::Busy;
  if (count_ == 0) return AlbumStepResult::Empty;

  if (pending_) {
    selected_ordinal_ = direction > 0
                            ? (selected_ordinal_ + 1U) % count_
                            : (selected_ordinal_ + count_ - 1U) % count_;
  } else if (current_ordinal_ == kNoOrdinal) {
    selected_ordinal_ = direction > 0 ? 0U : count_ - 1U;
  } else {
    selected_ordinal_ = direction > 0
                            ? (current_ordinal_ + 1U) % count_
                            : (current_ordinal_ + count_ - 1U) % count_;
  }
  pending_ = true;
  settle_deadline_ms_ = now_ms + settle_ms_;
  ordinal = selected_ordinal_;
  return AlbumStepResult::Selected;
}

bool AlbumNavigationCore::takeSettled(uint32_t now_ms, size_t& ordinal,
                                      bool& changed) {
  ordinal = kNoOrdinal;
  changed = false;
  if (!ready_ || refreshing_ || !pending_ ||
      !due(now_ms, settle_deadline_ms_)) {
    return false;
  }
  pending_ = false;
  ordinal = selected_ordinal_;
  changed = ordinal != current_ordinal_;
  if (changed) refreshing_ = true;
  return true;
}

bool AlbumNavigationCore::beginImmediate(size_t ordinal) {
  if (!ready_ || refreshing_ || ordinal >= count_) return false;
  pending_ = false;
  selected_ordinal_ = ordinal;
  refreshing_ = ordinal != current_ordinal_;
  return true;
}

void AlbumNavigationCore::finish(size_t count, size_t current_ordinal) {
  if (current_ordinal != kNoOrdinal && current_ordinal >= count) {
    ready_ = false;
    count_ = 0;
    current_ordinal_ = kNoOrdinal;
    selected_ordinal_ = kNoOrdinal;
    pending_ = false;
    refreshing_ = false;
    return;
  }
  count_ = count;
  current_ordinal_ = current_ordinal;
  selected_ordinal_ = current_ordinal;
  pending_ = false;
  refreshing_ = false;
  ready_ = true;
}

void AlbumNavigationCore::invalidate() {
  ready_ = false;
  count_ = 0;
  current_ordinal_ = kNoOrdinal;
  selected_ordinal_ = kNoOrdinal;
  pending_ = false;
  refreshing_ = false;
}

}  // namespace inkloop
