#pragma once

#include "Storage.h"
#include "TransactionalIo.h"

namespace inkloop {

class BackendTransactionIo final : public ITransactionalIo {
 public:
  explicit BackendTransactionIo(IStorageBackend& storage) : storage_(storage) {}

  bool available() const override { return storage_.capabilities().mounted; }
  bool exists(const char* path) override { return storage_.exists(path); }
  bool remove(const char* path) override { return !storage_.exists(path) || storage_.remove(path); }
  bool rename(const char* from, const char* to) override { return storage_.rename(from, to); }
  bool writeAll(const char* path, const uint8_t* bytes, size_t length) override;
  bool contentEquals(const char* path, const uint8_t* bytes, size_t length) override;

 private:
  IStorageBackend& storage_;
};

}  // namespace inkloop
