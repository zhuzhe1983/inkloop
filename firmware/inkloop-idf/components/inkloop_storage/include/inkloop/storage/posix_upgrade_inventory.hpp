#pragma once

#include <array>
#include <string>

#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {

// Read-only compatibility inventory for an already mounted internal
// filesystem. It never creates directories, promotes fallback files, removes
// transaction artifacts, or rewrites a record. NVS probes are supplied by the
// native NVS owner so this portable reader remains host-testable.
class PosixUpgradeInventory final {
 public:
  explicit PosixUpgradeInventory(std::string internal_root);

  UpgradeAuditInput inspect(
      const std::array<RecordProbe, kProtectedNvsNamespaces.size()>&
          application_nvs) const;
  // Exact per-path classifications in kProtectedFilePaths order. Native
  // snapshot collectors use this once per pass so raw streaming cannot
  // accidentally upgrade a syntactically invalid record to Valid.
  std::array<RecordProbe, kProtectedFilePaths.size()> inspectFiles() const;
  bool pathsValid() const { return paths_valid_; }

 private:
  RecordProbe probeTasks(const char* relative_path) const;
  RecordProbe probeAlbum(const char* relative_path) const;
  RecordProbe probeDisplay(const char* relative_path) const;
  void probeChatPair(RecordProbe& current, RecordProbe& previous) const;
  std::string path(const char* relative_path) const;

  std::string internal_root_;
  bool paths_valid_ = false;
};

}  // namespace storage
}  // namespace inkloop
