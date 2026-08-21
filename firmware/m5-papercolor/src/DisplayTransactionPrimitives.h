#pragma once

#include <stdint.h>

namespace inkloop {

enum class DisplayJournalStage : uint8_t {
  None,
  Prepared,
  Displayed,
  CurrentCommitted,
  TaskAcknowledged,
  Aborted,
};

constexpr bool displayStageIsAmbiguous(DisplayJournalStage stage) {
  return stage == DisplayJournalStage::Prepared;
}

constexpr bool displayStageMustNotRedraw(DisplayJournalStage stage) {
  return stage == DisplayJournalStage::Displayed ||
    stage == DisplayJournalStage::CurrentCommitted ||
    stage == DisplayJournalStage::TaskAcknowledged;
}

constexpr bool displayStageNeedsCurrentCommit(DisplayJournalStage stage) {
  return stage == DisplayJournalStage::Displayed;
}

constexpr bool displayStageNeedsTaskAck(DisplayJournalStage stage, bool hasTask) {
  return hasTask && stage == DisplayJournalStage::CurrentCommitted;
}

}  // namespace inkloop
