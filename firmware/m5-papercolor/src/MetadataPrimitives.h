#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

constexpr size_t kLittleFsFinalReserveBytes = 320 * 1024;
constexpr size_t kSdFinalReserveBytes = 1024 * 1024;

struct MetadataBudget {
  size_t taskNextBytes;
  size_t journalRecordBytes;

  constexpr MetadataBudget(size_t taskBytes = 0, size_t journalBytes = 0)
    : taskNextBytes(taskBytes), journalRecordBytes(journalBytes) {}
};

constexpr bool checkedAdd(size_t left, size_t right, size_t& result) {
  return right <= static_cast<size_t>(-1) - left
    ? (result = left + right, true)
    : false;
}

constexpr bool checkedMultiply(size_t value, size_t multiplier, size_t& result) {
  return value == 0 || multiplier <= static_cast<size_t>(-1) / value
    ? (result = value * multiplier, true)
    : false;
}

inline bool metadataTransactionBytes(
  const MetadataBudget& budget,
  size_t indexNextBytes,
  bool sharedControlBackend,
  size_t& bytes
) {
  size_t journalCopies = 0;
  size_t controlBytes = 0;
  if (!checkedMultiply(budget.journalRecordBytes, 3, journalCopies) ||
      !checkedAdd(budget.taskNextBytes, journalCopies, controlBytes)) return false;
  return checkedAdd(indexNextBytes, sharedControlBackend ? controlBytes : 0, bytes);
}

inline bool controlTransactionBytes(const MetadataBudget& budget, size_t& bytes) {
  size_t journalCopies = 0;
  return checkedMultiply(budget.journalRecordBytes, 3, journalCopies) &&
    checkedAdd(budget.taskNextBytes, journalCopies, bytes);
}

constexpr bool storageCanPreserveReserve(
  size_t totalBytes,
  size_t usedBytes,
  size_t incomingBytes,
  size_t transactionBytes,
  size_t finalReserveBytes
) {
  return usedBytes <= totalBytes && incomingBytes <= totalBytes - usedBytes &&
    transactionBytes <= totalBytes - usedBytes - incomingBytes &&
    finalReserveBytes <= totalBytes - usedBytes - incomingBytes - transactionBytes;
}

}  // namespace inkloop
