#pragma once

#include "TransactionalIo.h"

namespace inkloop {

class TaskPersistenceCore {
 public:
  explicit TaskPersistenceCore(ITransactionalIo& io) : io_(io), records_(io) {}

  bool commit(const uint8_t* bytes, size_t length) {
    return records_.commitRecord(currentPath(), nextPath(), previousPath(), bytes, length);
  }

  template <typename Validator>
  bool commitValidated(const uint8_t* bytes, size_t length, Validator validator) {
    return commitValidatedDetailed(bytes, length, validator) ==
        RecordCommitResult::Committed;
  }

  template <typename Validator>
  RecordCommitResult commitValidatedDetailed(
      const uint8_t* bytes, size_t length, Validator validator) {
    return records_.commitValidatedRecordDetailed(
        currentPath(), nextPath(), previousPath(), bytes, length, validator);
  }

  template <typename Validator>
  RecordRecovery recover(Validator validator) {
    return recoverTransactionalRecord(
      io_, currentPath(), nextPath(), previousPath(), validator
    );
  }

  static constexpr const char* currentPath() { return "/tasks.json"; }
  static constexpr const char* nextPath() { return "/tasks.next"; }
  static constexpr const char* previousPath() { return "/tasks.prev"; }

 private:
  ITransactionalIo& io_;
  TransactionalFileStore records_;
};

}  // namespace inkloop
