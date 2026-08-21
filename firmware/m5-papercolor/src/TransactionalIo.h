#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

enum class RecordRecovery : uint8_t {
  UseCurrent,
  PromoteNext,
  RestorePrevious,
  Empty,
  Failed,
};

enum class RecordCommitResult : uint8_t {
  Committed,
  Unavailable,
  InvalidInput,
  RemoveNextFailed,
  WriteNextFailed,
  VerifyNextFailed,
  ValidateNextFailed,
  ValidateCurrentFailed,
  RemovePreviousFailed,
  RotateCurrentFailed,
  PromoteNextFailed,
};

inline const char* recordCommitResultName(RecordCommitResult result) {
  switch (result) {
    case RecordCommitResult::Committed: return "committed";
    case RecordCommitResult::Unavailable: return "unavailable";
    case RecordCommitResult::InvalidInput: return "invalid_input";
    case RecordCommitResult::RemoveNextFailed: return "remove_next";
    case RecordCommitResult::WriteNextFailed: return "write_next";
    case RecordCommitResult::VerifyNextFailed: return "verify_next";
    case RecordCommitResult::ValidateNextFailed: return "validate_next";
    case RecordCommitResult::ValidateCurrentFailed: return "validate_current";
    case RecordCommitResult::RemovePreviousFailed: return "remove_previous";
    case RecordCommitResult::RotateCurrentFailed: return "rotate_current";
    case RecordCommitResult::PromoteNextFailed: return "promote_next";
  }
  return "unknown";
}

class ITransactionalIo {
 public:
  virtual ~ITransactionalIo() = default;
  virtual bool available() const = 0;
  virtual bool exists(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool writeAll(const char* path, const uint8_t* bytes, size_t length) = 0;
  virtual bool contentEquals(const char* path, const uint8_t* bytes, size_t length) = 0;
};

class TransactionalFileStore {
 public:
  explicit TransactionalFileStore(ITransactionalIo& io) : io_(io) {}

  bool promoteBlob(
    const char* temporaryPath,
    const char* finalPath,
    const uint8_t* bytes,
    size_t length
  ) {
    if (!io_.available() || !bytes || !length) return false;
    if (!io_.remove(temporaryPath)) return false;
    if (!io_.writeAll(temporaryPath, bytes, length) ||
        !io_.contentEquals(temporaryPath, bytes, length) ||
        !io_.available()) {
      io_.remove(temporaryPath);
      return false;
    }
    if (io_.exists(finalPath)) {
      const bool alreadyCommitted = io_.contentEquals(finalPath, bytes, length);
      const bool removedTemporary = io_.remove(temporaryPath);
      return alreadyCommitted && removedTemporary;
    }
    if (!io_.rename(temporaryPath, finalPath)) {
      io_.remove(temporaryPath);
      return false;
    }
    return true;
  }

  bool commitRecord(
    const char* currentPath,
    const char* nextPath,
    const char* previousPath,
    const uint8_t* bytes,
    size_t length
  ) {
    if (!io_.available() || !bytes || !length) return false;
    if (!io_.remove(nextPath)) return false;
    if (!io_.writeAll(nextPath, bytes, length) ||
        !io_.contentEquals(nextPath, bytes, length) ||
        !io_.available()) {
      io_.remove(nextPath);
      return false;
    }
    if (!io_.remove(previousPath)) {
      io_.remove(nextPath);
      return false;
    }
    const bool hadCurrent = io_.exists(currentPath);
    if (hadCurrent && !io_.rename(currentPath, previousPath)) {
      io_.remove(nextPath);
      return false;
    }
    if (io_.available() && io_.rename(nextPath, currentPath)) return true;
    if (hadCurrent && io_.available()) io_.rename(previousPath, currentPath);
    io_.remove(nextPath);
    return false;
  }

  template <typename Validator>
  RecordCommitResult commitValidatedRecordDetailed(
    const char* currentPath,
    const char* nextPath,
    const char* previousPath,
    const uint8_t* bytes,
    size_t length,
    Validator validator
  ) {
    if (!bytes || !length) return RecordCommitResult::InvalidInput;
    if (!io_.available()) return RecordCommitResult::Unavailable;
    if (!io_.remove(nextPath)) return RecordCommitResult::RemoveNextFailed;
    if (!io_.writeAll(nextPath, bytes, length)) {
      io_.remove(nextPath);
      return RecordCommitResult::WriteNextFailed;
    }
    if (!io_.contentEquals(nextPath, bytes, length)) {
      io_.remove(nextPath);
      return RecordCommitResult::VerifyNextFailed;
    }
    if (!io_.available()) {
      io_.remove(nextPath);
      return RecordCommitResult::Unavailable;
    }
    if (!validator(nextPath)) {
      io_.remove(nextPath);
      return RecordCommitResult::ValidateNextFailed;
    }
    const bool hadCurrent = io_.exists(currentPath);
    if (hadCurrent && !validator(currentPath)) {
      io_.remove(nextPath);
      return RecordCommitResult::ValidateCurrentFailed;
    }
    if (!io_.remove(previousPath)) {
      io_.remove(nextPath);
      return RecordCommitResult::RemovePreviousFailed;
    }
    if (hadCurrent && !io_.rename(currentPath, previousPath)) {
      io_.remove(nextPath);
      return RecordCommitResult::RotateCurrentFailed;
    }
    if (io_.available() && io_.rename(nextPath, currentPath))
      return RecordCommitResult::Committed;
    if (hadCurrent && io_.available()) io_.rename(previousPath, currentPath);
    io_.remove(nextPath);
    return RecordCommitResult::PromoteNextFailed;
  }

  template <typename Validator>
  bool commitValidatedRecord(
    const char* currentPath,
    const char* nextPath,
    const char* previousPath,
    const uint8_t* bytes,
    size_t length,
    Validator validator
  ) {
    return commitValidatedRecordDetailed(
        currentPath, nextPath, previousPath, bytes, length, validator) ==
        RecordCommitResult::Committed;
  }

 private:
  ITransactionalIo& io_;
};

template <typename Validator>
RecordRecovery recoverTransactionalRecord(
  ITransactionalIo& io,
  const char* currentPath,
  const char* nextPath,
  const char* previousPath,
  Validator validator
) {
  if (!io.available()) return RecordRecovery::Failed;
  const bool currentExists = io.exists(currentPath);
  const bool nextExists = io.exists(nextPath);
  const bool previousExists = io.exists(previousPath);
  if (currentExists && validator(currentPath)) {
    if (nextExists && !io.remove(nextPath)) return RecordRecovery::Failed;
    return RecordRecovery::UseCurrent;
  }
  if (nextExists && validator(nextPath)) {
    if (currentExists && !io.remove(currentPath)) return RecordRecovery::Failed;
    if (!io.rename(nextPath, currentPath)) return RecordRecovery::Failed;
    return RecordRecovery::PromoteNext;
  }
  if (previousExists && validator(previousPath)) {
    if (currentExists && !io.remove(currentPath)) return RecordRecovery::Failed;
    if (nextExists && !io.remove(nextPath)) return RecordRecovery::Failed;
    if (!io.rename(previousPath, currentPath)) return RecordRecovery::Failed;
    return RecordRecovery::RestorePrevious;
  }
  return currentExists || nextExists || previousExists
    ? RecordRecovery::Failed
    : RecordRecovery::Empty;
}

}  // namespace inkloop
