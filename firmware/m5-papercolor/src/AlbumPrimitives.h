#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

enum class CacheAdmission : uint8_t {
  Accept,
  Deduplicated,
  Invalid,
  EntryLimit,
  AlbumLimit,
  FreeSpace,
};

struct CacheCapacity {
  size_t totalBytes;
  size_t usedBytes;
  size_t albumBytes;
  size_t albumLimitBytes;
  size_t reserveBytes;
  size_t entryCount;
  size_t entryLimit;

  constexpr CacheCapacity(
    size_t total = 0,
    size_t used = 0,
    size_t album = 0,
    size_t albumLimit = 0,
    size_t reserve = 0,
    size_t entries = 0,
    size_t entryLimitValue = 0
  ) : totalBytes(total), usedBytes(used), albumBytes(album), albumLimitBytes(albumLimit),
      reserveBytes(reserve), entryCount(entries), entryLimit(entryLimitValue) {}
};

constexpr bool sumFits(size_t left, size_t right, size_t limit) {
  return left <= limit && right <= limit - left;
}

constexpr CacheAdmission cacheAdmission(
  CacheCapacity capacity,
  size_t incomingBytes,
  bool alreadyCached
) {
  return alreadyCached ? CacheAdmission::Deduplicated
    : incomingBytes == 0 ? CacheAdmission::Invalid
    : capacity.entryCount >= capacity.entryLimit ? CacheAdmission::EntryLimit
    : !sumFits(capacity.albumBytes, incomingBytes, capacity.albumLimitBytes)
      ? CacheAdmission::AlbumLimit
    : !sumFits(capacity.usedBytes, incomingBytes, capacity.totalBytes) ||
        capacity.totalBytes - capacity.usedBytes - incomingBytes < capacity.reserveBytes
      ? CacheAdmission::FreeSpace
    : CacheAdmission::Accept;
}

struct PageSelection {
  bool accepted;
  size_t page;

  constexpr PageSelection(bool acceptedValue = false, size_t pageValue = 0)
    : accepted(acceptedValue), page(pageValue) {}
};

constexpr PageSelection selectAdjacentPage(size_t current, size_t count, int8_t direction, bool busy) {
  return busy || count == 0 || (direction != -1 && direction != 1)
    ? PageSelection(false, current)
    : direction < 0
      ? (current == 0 ? PageSelection(false, current) : PageSelection(true, current - 1))
      : (current + 1 >= count ? PageSelection(false, current) : PageSelection(true, current + 1));
}

enum class IndexTransactionStage : uint8_t {
  Stable,
  AssetTempWritten,
  AssetPromoted,
  IndexTempWritten,
  IndexCommitted,
  Failed,
};

constexpr bool transactionMayChangeCurrent(IndexTransactionStage stage) {
  return stage == IndexTransactionStage::IndexCommitted;
}

constexpr bool transactionLeavesRecoverableIndex(IndexTransactionStage stage) {
  return stage == IndexTransactionStage::Stable ||
    stage == IndexTransactionStage::AssetTempWritten ||
    stage == IndexTransactionStage::AssetPromoted ||
    stage == IndexTransactionStage::IndexTempWritten ||
    stage == IndexTransactionStage::IndexCommitted ||
    stage == IndexTransactionStage::Failed;
}

enum class IndexRecoveryAction : uint8_t { UseCurrent, RestoreBackup, InitializeEmpty };

constexpr IndexRecoveryAction selectIndexRecovery(bool currentValid, bool backupValid) {
  return currentValid ? IndexRecoveryAction::UseCurrent
    : backupValid ? IndexRecoveryAction::RestoreBackup
    : IndexRecoveryAction::InitializeEmpty;
}

struct IndexCommitOutcome {
  bool success;
  bool restoreBackup;
  bool priorIndexStillValid;

  constexpr IndexCommitOutcome(bool ok = false, bool restore = false, bool priorValid = true)
    : success(ok), restoreBackup(restore), priorIndexStillValid(priorValid) {}
};

constexpr IndexCommitOutcome resolveIndexCommit(
  bool hadPriorIndex,
  bool backupMoveSucceeded,
  bool nextPromoteSucceeded
) {
  return hadPriorIndex && !backupMoveSucceeded
    ? IndexCommitOutcome(false, false, true)
    : nextPromoteSucceeded
      ? IndexCommitOutcome(true, false, true)
      : IndexCommitOutcome(false, hadPriorIndex && backupMoveSucceeded, true);
}

constexpr bool assetRecordUsable(
  bool identityValid,
  bool scopedPath,
  bool fileExists,
  size_t metadataBytes,
  size_t fileBytes,
  bool pngHeaderValid
) {
  return identityValid && scopedPath && fileExists && metadataBytes > 0 &&
    metadataBytes == fileBytes && pngHeaderValid;
}

}  // namespace inkloop
