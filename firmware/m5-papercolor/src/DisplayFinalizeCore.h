#pragma once

#include "DisplayTransactionPrimitives.h"

namespace inkloop {

enum class FinalizeResult : uint8_t { Complete, Retry, DecisionRequired };

template <typename Operations>
FinalizeResult finalizeDisplayMetadata(
  DisplayJournalStage& stage,
  DisplayJournalStage& persistedStage,
  bool hasTask,
  Operations& operations
) {
  if (persistedStage != stage) {
    if (!operations.persist(stage)) return FinalizeResult::Retry;
    persistedStage = stage;
  }
  if (stage == DisplayJournalStage::Prepared) return FinalizeResult::DecisionRequired;
  if (stage == DisplayJournalStage::Aborted) {
    return operations.clear() ? FinalizeResult::Complete : FinalizeResult::Retry;
  }
  if (stage == DisplayJournalStage::Displayed) {
    if (!operations.commitCurrent()) return FinalizeResult::Retry;
    stage = DisplayJournalStage::CurrentCommitted;
    if (!operations.persist(stage)) return FinalizeResult::Retry;
    persistedStage = stage;
  }
  if (stage == DisplayJournalStage::CurrentCommitted && hasTask) {
    if (!operations.taskAcknowledged() && !operations.acknowledgeTask()) {
      return FinalizeResult::Retry;
    }
    stage = DisplayJournalStage::TaskAcknowledged;
    if (!operations.persist(stage)) return FinalizeResult::Retry;
    persistedStage = stage;
  }
  if (stage == DisplayJournalStage::CurrentCommitted ||
      stage == DisplayJournalStage::TaskAcknowledged) {
    return operations.clear() ? FinalizeResult::Complete : FinalizeResult::Retry;
  }
  return FinalizeResult::Retry;
}

}  // namespace inkloop
