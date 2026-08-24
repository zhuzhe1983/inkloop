#include "inkloop/storage/esp_legacy_display_recovery.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "inkloop/storage/posix_task_store.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"

namespace inkloop {
namespace storage {
namespace {

constexpr std::size_t kMaximumJournalBytes = 16U * 1024U;
constexpr std::array<const char*, 3> kJournalPaths{{
    "/display-txn.json", "/display-txn.next", "/display-txn.prev"}};

LegacyDisplayResolutionAdapterCode io(bool ok) {
  return ok ? LegacyDisplayResolutionAdapterCode::Ok
            : LegacyDisplayResolutionAdapterCode::IoError;
}

TransactionProbe transaction(
    const std::array<RecordProbe, kProtectedFilePaths.size()>& files,
    std::size_t first) {
  return {files[first], files[first + 1U], files[first + 2U]};
}

}  // namespace

EspLegacyDisplayRecovery::EspLegacyDisplayRecovery(
    EspStorageMountOwner& storage)
    : storage_(storage) {}

bool EspLegacyDisplayRecovery::readRecord(
    const std::string& path, RawLegacyDisplayRecord& output) {
  output.clear();
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    output.io_error = errno != ENOENT;
    return !output.io_error;
  }
  output.present = true;
  if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uint64_t>(info.st_size) > kMaximumJournalBytes) {
    return true;
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    output.io_error = true;
    return false;
  }
  output.bytes.resize(static_cast<std::size_t>(info.st_size));
  const std::size_t count =
      std::fread(output.bytes.data(), 1U, output.bytes.size(), file);
  const bool valid = count == output.bytes.size() &&
      std::ferror(file) == 0 && std::fclose(file) == 0;
  if (!valid) {
    output.clear();
    output.io_error = true;
  }
  return valid;
}

LegacyDisplayRecoveryProbe EspLegacyDisplayRecovery::inspect(
    LegacyDisplayRecoverySnapshot& output) const {
  output.clear();
  const char* root = storage_.recoveryReadTaskRoot();
  if (!root) {
    // executeLegacyDisplayResolution re-inspects after the typed Display
    // capability has been granted. This remains unavailable to every other
    // recovery domain and to Product callers.
    root = storage_.recoveryMutationTaskRoot(
        RecoveryMutationDomain::Display);
  }
  if (!root) {
    output.probe = LegacyDisplayRecoveryProbe::IoError;
    return output.probe;
  }
  std::array<RawLegacyDisplayRecord, 3> records{};
  for (std::size_t at = 0U; at < records.size(); ++at) {
    if (!readRecord(std::string(root) + kJournalPaths[at], records[at])) {
      for (RawLegacyDisplayRecord& record : records) record.clear();
      output.probe = LegacyDisplayRecoveryProbe::IoError;
      return output.probe;
    }
  }
  const LegacyDisplayRecoveryProbe probe =
      inspectLegacyDisplayRecovery(records, output);
  for (RawLegacyDisplayRecord& record : records) record.clear();
  return probe;
}

LegacyDisplayResolutionCode EspLegacyDisplayRecovery::resolve(
    const LegacyDisplayRecoverySnapshot& expected,
    LegacyDisplayResolutionChoice choice) {
  return executeLegacyDisplayResolution(expected, choice, *this, *this);
}

LegacyDisplayResolutionAdapterCode EspLegacyDisplayRecovery::albumResult(
    const myai::Status& status) {
  if (status.ok()) return LegacyDisplayResolutionAdapterCode::Ok;
  if (status.code == myai::ErrorCode::InvalidState)
    return LegacyDisplayResolutionAdapterCode::Conflict;
  return LegacyDisplayResolutionAdapterCode::IoError;
}

LegacyDisplayResolutionAdapterCode
EspLegacyDisplayRecovery::applyTargetCurrent(
    const LegacyDisplayJournal& journal) {
  const char* root = journal.backend == "sd"
      ? storage_.recoveryMutationRemovableRoot(
            RecoveryMutationDomain::Display)
      : storage_.recoveryMutationInternalRoot(
            RecoveryMutationDomain::Display);
  PosixAtomicAlbumStore* album =
      storage_.recoveryMutationAlbumStore(
          RecoveryMutationDomain::Display, journal.backend.c_str());
  if (!root || !album) return LegacyDisplayResolutionAdapterCode::Unavailable;
  const PosixUpgradeInventory inventory(root);
  const auto files = inventory.inspectFiles();
  if (classifyTransaction(transaction(files, 6U)) != TransactionAudit::Clean)
    return LegacyDisplayResolutionAdapterCode::Conflict;
  return albumResult(album->markCurrent(journal.asset_id));
}

LegacyDisplayResolutionAdapterCode EspLegacyDisplayRecovery::acknowledgeTask(
    const LegacyDisplayJournal& journal) {
  if (!journal.has_task) return LegacyDisplayResolutionAdapterCode::Ok;
  const char* root = storage_.recoveryMutationTaskRoot(
      RecoveryMutationDomain::Display);
  if (!root) return LegacyDisplayResolutionAdapterCode::Unavailable;
  const PosixUpgradeInventory inventory(root);
  const auto files = inventory.inspectFiles();
  if (classifyTransaction(transaction(files, 0U)) != TransactionAudit::Clean)
    return LegacyDisplayResolutionAdapterCode::Conflict;
  PosixTaskStore tasks(root);
  if (tasks.initialize() != TaskStoreCode::Ok)
    return LegacyDisplayResolutionAdapterCode::Conflict;
  return tasks.markRun(journal.task_id, journal.task_revision,
                       static_cast<std::time_t>(journal.run_at),
                       journal.run_day) == TaskStoreCode::Ok
      ? LegacyDisplayResolutionAdapterCode::Ok
      : LegacyDisplayResolutionAdapterCode::IoError;
}

LegacyDisplayResolutionAdapterCode
EspLegacyDisplayRecovery::clearJournalSet() {
  const char* root = storage_.recoveryMutationTaskRoot(
      RecoveryMutationDomain::Display);
  if (!root) return LegacyDisplayResolutionAdapterCode::Unavailable;
  // Current is deliberately removed last. A reset during cleanup therefore
  // leaves the authoritative record available for an idempotent retry.
  const std::size_t order[] = {1U, 2U, 0U};
  for (std::size_t at : order) {
    const std::string path = std::string(root) + kJournalPaths[at];
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return io(false);
  }
  return io(true);
}

}  // namespace storage
}  // namespace inkloop
