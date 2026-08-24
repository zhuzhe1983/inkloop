#include "recovery_action_owner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/esp_upgrade_boot_audit.hpp"
#include "inkloop/storage/persistence_compatibility.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "inkloop/storage/sha256.hpp"
#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace {

constexpr std::uint32_t kRestartResponseGraceMs = 2000U;
// A maximum custody pass streams the bounded 288-asset union and then hashes
// the source a second time. Ten minutes can expire between those two passes at
// the deliberately throttled Recovery rate, so retain a finite but sufficient
// slow-card window.
constexpr std::uint32_t kExportSessionLifetimeMs = 30U * 60U * 1000U;
constexpr std::size_t kExportReadBufferBytes = 4096U;

static_assert(storage::kMaximumAlbumEntries *
                  recovery::kRecoveryActionCandidateCount ==
              recovery::kMaximumRecoveryExportAssets);
static_assert(storage::kMaximumAlbumIndexBytes ==
              recovery::kMaximumRecoveryExportIndexBytes);
static_assert(storage::kMaximumAlbumAssetBytes ==
              recovery::kMaximumRecoveryExportAssetBytes);

template <size_t Size>
bool constantTimeBytes(const std::array<std::uint8_t, Size>& left,
                       const std::array<std::uint8_t, Size>& right) {
  std::uint8_t difference = 0U;
  for (size_t at = 0U; at < Size; ++at)
    difference |= static_cast<std::uint8_t>(left[at] ^ right[at]);
  return difference == 0U;
}

bool readIndexFile(const std::string& path, std::string& bytes,
                   std::array<std::uint8_t, 32>& digest,
                   struct stat& status) {
  bytes.clear();
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  bool valid = ::fstat(::fileno(file), &status) == 0 &&
      S_ISREG(status.st_mode) && status.st_size > 0 &&
      static_cast<std::uint64_t>(status.st_size) <=
          storage::kMaximumAlbumIndexBytes;
  if (valid) bytes.resize(static_cast<size_t>(status.st_size));
  storage::Sha256 hash;
  size_t offset = 0U;
  while (valid && offset < bytes.size()) {
    const size_t count = std::fread(bytes.data() + offset, 1U,
                                    bytes.size() - offset, file);
    if (count == 0U || !hash.update(
            reinterpret_cast<const std::uint8_t*>(bytes.data() + offset),
            count)) {
      valid = false;
      break;
    }
    offset += count;
  }
  struct stat after {};
  valid = valid && offset == bytes.size() && !std::ferror(file) &&
      ::fstat(::fileno(file), &after) == 0 &&
      after.st_size == status.st_size && after.st_mtime == status.st_mtime &&
      hash.finish(digest);
  if (std::fclose(file) != 0) valid = false;
  if (!valid) {
    std::fill(bytes.begin(), bytes.end(), '\0');
    bytes.clear();
  }
  return valid;
}

bool statRegular(const std::string& path, std::uint64_t expected_bytes,
                 struct stat& status) {
  return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
      status.st_size >= 0 &&
      static_cast<std::uint64_t>(status.st_size) == expected_bytes;
}

bool hashReadOnlyFile(
    const std::string& path, std::uint64_t expected_bytes,
    std::int64_t expected_modified,
    const std::array<std::uint8_t, 32>& expected_digest) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  struct stat before {};
  bool valid = ::fstat(::fileno(file), &before) == 0 &&
      S_ISREG(before.st_mode) && before.st_size >= 0 &&
      static_cast<std::uint64_t>(before.st_size) == expected_bytes &&
      static_cast<std::int64_t>(before.st_mtime) == expected_modified;
  storage::Sha256 hash;
  std::array<std::uint8_t, kExportReadBufferBytes> buffer{};
  std::uint64_t offset = 0U;
  std::uint32_t chunks = 0U;
  while (valid && offset < expected_bytes) {
    const size_t wanted = static_cast<size_t>(std::min<std::uint64_t>(
        buffer.size(), expected_bytes - offset));
    const size_t count = std::fread(buffer.data(), 1U, wanted, file);
    if (count == 0U || !hash.update(buffer.data(), count)) {
      valid = false;
      break;
    }
    offset += count;
    // Final verification can cover the maximum 288 assets. Yield regularly
    // so a large, but still bounded, read-only pass cannot starve the system.
    if (++chunks % 16U == 0U) vTaskDelay(1U);
  }
  struct stat after {};
  std::array<std::uint8_t, 32> digest{};
  const int extra = valid ? std::fgetc(file) : 0;
  valid = valid && offset == expected_bytes && extra == EOF &&
      std::feof(file) && !std::ferror(file) &&
      ::fstat(::fileno(file), &after) == 0 &&
      after.st_size == before.st_size && after.st_mtime == before.st_mtime &&
      hash.finish(digest) && constantTimeBytes(digest, expected_digest);
  std::fill(buffer.begin(), buffer.end(), 0U);
  if (std::fclose(file) != 0) valid = false;
  return valid;
}

class HashBuilder final {
 public:
  bool byte(std::uint8_t value) { return hash_.update(&value, 1U); }

  bool u32(std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{{
        static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value)}};
    return hash_.update(bytes.data(), bytes.size());
  }

  bool u64(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t at = 0U; at < bytes.size(); ++at) {
      bytes[bytes.size() - at - 1U] = static_cast<std::uint8_t>(value);
      value >>= 8U;
    }
    return hash_.update(bytes.data(), bytes.size());
  }

  bool boolean(bool value) { return byte(value ? 1U : 0U); }

  bool text(const std::string& value) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
      if (value.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    }
    return u32(static_cast<std::uint32_t>(value.size())) &&
        (value.empty() || hash_.update(
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size()));
  }

  bool bytes(const std::uint8_t* value, std::size_t length) {
    return length == 0U || (value && hash_.update(value, length));
  }

  bool finish(std::array<std::uint8_t, 32>& output) {
    return hash_.finish(output);
  }

 private:
  storage::Sha256 hash_{};
};

std::uint32_t nowMs() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool due(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return deadline_ms != 0U &&
      static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decodeDigest(const std::string& text,
                  std::array<std::uint8_t, 32>& output) {
  output.fill(0U);
  if (text.size() != output.size() * 2U) return false;
  for (std::size_t at = 0U; at < output.size(); ++at) {
    const int high = hexNibble(text[at * 2U]);
    const int low = hexNibble(text[at * 2U + 1U]);
    if (high < 0 || low < 0) {
      output.fill(0U);
      return false;
    }
    output[at] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

recovery::RecoveryActionCandidateState mapCandidate(
    storage::LegacyFileCandidateProbe value) {
  using Source = storage::LegacyFileCandidateProbe;
  using Target = recovery::RecoveryActionCandidateState;
  switch (value) {
    case Source::Missing: return Target::Missing;
    case Source::Valid: return Target::Valid;
    case Source::Invalid: return Target::Invalid;
    case Source::IoError: return Target::IoError;
  }
  return Target::IoError;
}

recovery::RecoveryActionState mapFileState(
    storage::LegacyFileTransactionProbe value) {
  using Source = storage::LegacyFileTransactionProbe;
  using Target = recovery::RecoveryActionState;
  switch (value) {
    case Source::Empty: return Target::Empty;
    case Source::Recoverable: return Target::Recoverable;
    case Source::ChoiceRequired: return Target::ChoiceRequired;
    case Source::Corrupt: return Target::Corrupt;
    case Source::IoError: return Target::IoError;
    case Source::InvalidTarget: return Target::Disabled;
  }
  return Target::Disabled;
}

storage::LegacyFileTransactionSlot mapFileChoice(
    recovery::RecoveryActionChoice value) {
  switch (value) {
    case recovery::RecoveryActionChoice::Current:
      return storage::LegacyFileTransactionSlot::Current;
    case recovery::RecoveryActionChoice::Next:
      return storage::LegacyFileTransactionSlot::Next;
    case recovery::RecoveryActionChoice::Previous:
      return storage::LegacyFileTransactionSlot::Previous;
  }
  return storage::LegacyFileTransactionSlot::Current;
}

recovery::RecoveryActionResolveResult mapFileResult(
    storage::LegacyFileTransactionResolveCode value) {
  using Source = storage::LegacyFileTransactionResolveCode;
  using Target = recovery::RecoveryActionResolveResult;
  switch (value) {
    case Source::Ok: return Target::Ok;
    case Source::InvalidArgument:
    case Source::CrossBackend: return Target::InvalidRequest;
    case Source::SelectedUnavailable: return Target::SelectedUnavailable;
    case Source::SourceChanged: return Target::SourceChanged;
    case Source::SourceUnavailable: return Target::SourceUnavailable;
    case Source::VerificationFailed: return Target::VerificationFailed;
    case Source::IoError:
    case Source::PowerCutSimulated: return Target::IoError;
  }
  return Target::IoError;
}

recovery::RecoveryActionResolveResult mapDisplayResult(
    storage::LegacyDisplayResolutionCode value) {
  using Source = storage::LegacyDisplayResolutionCode;
  using Target = recovery::RecoveryActionResolveResult;
  switch (value) {
    case Source::Ok: return Target::Ok;
    case Source::InvalidArgument: return Target::InvalidRequest;
    case Source::SourceChanged: return Target::SourceChanged;
    case Source::SourceUnavailable: return Target::SourceUnavailable;
    case Source::TargetUnavailable: return Target::SelectedUnavailable;
    case Source::TargetCommitFailed:
    case Source::TaskCommitFailed:
    case Source::JournalClearFailed: return Target::IoError;
  }
  return Target::IoError;
}

bool sameId(const std::array<std::uint8_t, 32>& left,
            const std::array<std::uint8_t, 32>& right) {
  std::uint8_t difference = 0U;
  for (std::size_t at = 0U; at < left.size(); ++at)
    difference |= static_cast<std::uint8_t>(left[at] ^ right[at]);
  return difference == 0U;
}

bool hashPublicPrefix(const recovery::RecoveryActionSnapshot& value,
                      HashBuilder& hash) {
  if (!hash.byte(static_cast<std::uint8_t>(value.domain)) ||
      !hash.byte(static_cast<std::uint8_t>(value.backend)) ||
      !hash.byte(static_cast<std::uint8_t>(value.state)) ||
      !hash.byte(value.valid_candidates)) {
    return false;
  }
  for (const auto& candidate : value.candidates) {
    if (!hash.byte(static_cast<std::uint8_t>(candidate.state)) ||
        !hash.u64(candidate.byte_count) ||
        !hash.boolean(candidate.digest_present) ||
        !hash.bytes(candidate.digest.data(), candidate.digest.size()) ||
        !hash.u32(candidate.item_count) ||
        !hash.boolean(candidate.item_count_present) ||
        !hash.u32(candidate.modified_unix_seconds) ||
        !hash.boolean(candidate.modified_time_present)) {
      return false;
    }
  }
  return true;
}

bool hashDisplaySnapshot(
    const recovery::RecoveryActionSnapshot& public_snapshot,
    const storage::LegacyDisplayRecoverySnapshot& source,
    std::array<std::uint8_t, 32>& output) {
  HashBuilder hash;
  const auto& journal = source.journal;
  if (!hashPublicPrefix(public_snapshot, hash) ||
      !hash.byte(static_cast<std::uint8_t>(source.probe)) ||
      !hash.byte(static_cast<std::uint8_t>(source.selected_slot))) {
    return false;
  }
  for (bool present : source.present)
    if (!hash.boolean(present)) return false;
  for (bool valid : source.valid)
    if (!hash.boolean(valid)) return false;
  return hash.byte(static_cast<std::uint8_t>(journal.stage)) &&
      hash.text(journal.backend) && hash.text(journal.asset_id) &&
      hash.text(journal.asset_path) && hash.text(journal.previous_current) &&
      hash.text(journal.operation) && hash.u32(journal.asset_bytes) &&
      hash.boolean(journal.landscape) && hash.u32(journal.page) &&
      hash.boolean(journal.has_task) && hash.u32(journal.run_at) &&
      hash.u32(journal.run_day) && hash.text(journal.task_id) &&
      hash.u32(journal.task_revision) && hash.text(journal.task_frame_url) &&
      hash.text(journal.task_frame_hash) && hash.finish(output);
}

bool hashFileSnapshot(
    const recovery::RecoveryActionSnapshot& public_snapshot,
    const storage::LegacyFileTransactionSnapshot& source,
    std::array<std::uint8_t, 32>& output) {
  HashBuilder hash;
  if (!hashPublicPrefix(public_snapshot, hash) ||
      !hash.byte(static_cast<std::uint8_t>(source.target.domain)) ||
      !hash.byte(static_cast<std::uint8_t>(source.target.backend)) ||
      !hash.byte(static_cast<std::uint8_t>(source.probe)) ||
      !hash.byte(source.valid_candidates)) {
    return false;
  }
  for (const auto& candidate : source.candidates) {
    if (!hash.byte(static_cast<std::uint8_t>(candidate.probe)) ||
        !hash.u64(candidate.byte_count) ||
        !hash.boolean(candidate.digest_present) ||
        !hash.bytes(candidate.sha256.data(), candidate.sha256.size()) ||
        !hash.u32(candidate.item_count) ||
        !hash.boolean(candidate.item_count_present) ||
        !hash.u32(candidate.modified_unix_seconds) ||
        !hash.boolean(candidate.modified_time_present)) {
      return false;
    }
  }
  return hash.finish(output);
}

}  // namespace

struct EspRecoveryActionOwner::ExportState {
  struct AssetRecord {
    recovery::RecoveryExportAsset summary{};
    std::int64_t modified = 0;
  };

  bool active = false;
  std::array<std::uint8_t, recovery::kRecoveryExportSessionBytes> session{};
  recovery::RecoveryExportSnapshot snapshot{};
  std::array<AssetRecord, recovery::kMaximumRecoveryExportAssets> assets{};
  std::array<bool, recovery::kRecoveryActionCandidateCount +
                       recovery::kMaximumRecoveryExportAssets>
      verified{};
  std::array<std::int64_t, recovery::kRecoveryActionCandidateCount>
      candidate_modified{};
  std::uint32_t expires_ms = 0U;
  std::FILE* file = nullptr;
  std::uint32_t handle = 0U;
  std::uint32_t item = 0U;
  std::uint64_t streamed = 0U;
  std::uint64_t expected_bytes = 0U;
  std::int64_t opened_modified = 0;
  std::array<std::uint8_t, 32> expected_digest{};
  storage::Sha256 stream_hash{};
};

EspRecoveryActionOwner::EspRecoveryActionOwner(
    storage::EspStorageMountOwner& storage)
    : storage_(storage),
      display_(storage),
      mutex_(xSemaphoreCreateMutex()),
      export_(new (std::nothrow) ExportState()) {
  recovery_prepared_ = storage_.prepareRecoveryReadOnly() == ESP_OK;
  if (!recovery_prepared_) return;
  const char* task = storage_.recoveryReadTaskRoot();
  const char* internal = storage_.recoveryReadInternalRoot();
  const char* removable = storage_.recoveryReadRemovableRoot();
  if (!task || !internal) {
    recovery_prepared_ = false;
    return;
  }
  files_.reset(new (std::nothrow) storage::PosixLegacyFileTransactionRecovery(
      {task, internal, removable ? removable : ""}));
  if (!files_) recovery_prepared_ = false;
}

EspRecoveryActionOwner::~EspRecoveryActionOwner() {
  if (mutex_ && lock()) {
    resetExportLocked();
    unlock();
  }
  delete export_;
  export_ = nullptr;
  if (mutex_) vSemaphoreDelete(mutex_);
  mutex_ = nullptr;
}

bool EspRecoveryActionOwner::lock() {
  return mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE;
}

bool EspRecoveryActionOwner::lockActions() {
  if (!actions_available_.load(std::memory_order_acquire) || !lock())
    return false;
  if (!actions_available_.load(std::memory_order_acquire)) {
    unlock();
    return false;
  }
  return true;
}

void EspRecoveryActionOwner::unlock() {
  if (mutex_) xSemaphoreGive(mutex_);
}

void EspRecoveryActionOwner::resetExportLocked() {
  if (!export_) return;
  if (export_->file) std::fclose(export_->file);
  const std::uint32_t previous_handle = export_->handle;
  *export_ = ExportState{};
  // Keep stream handles monotonic across sessions so a delayed request from a
  // prior authenticated export cannot alias the first stream of a new one.
  export_->handle = previous_handle;
}

void EspRecoveryActionOwner::invalidateActionsAndLatchForcedRestartLocked(
    std::uint32_t now_ms) {
  // Publish revocation first. A caller that passed ready() just before this
  // store must still pass lockActions(), which rechecks the permanent latch
  // while holding the mutex before it can touch any cached capability.
  actions_available_.store(false, std::memory_order_release);
  resetExportLocked();
  if (export_) export_->handle = 0U;
  cached_inventory_ = recovery::RecoveryActionInventory{};
  display_snapshot_ = storage::LegacyDisplayRecoverySnapshot{};
  for (auto& snapshot : file_snapshots_)
    snapshot = storage::LegacyFileTransactionSnapshot{};
  file_snapshot_valid_.fill(false);
  display_snapshot_valid_ = false;

  // A containment restart is deliberately distinct from the clean-audit
  // restart. Store the deadline before publishing the latch so an acquire
  // reader always observes a complete forced-restart truth state.
  restart_not_before_ms_.store(0U, std::memory_order_release);
  forced_restart_not_before_ms_.store(now_ms + kRestartResponseGraceMs,
                                      std::memory_order_relaxed);
  forced_restart_latched_.store(true, std::memory_order_release);
}

bool EspRecoveryActionOwner::exportSessionMatches(
    const std::array<uint8_t, recovery::kRecoveryExportSessionBytes>&
        session_id) const {
  return export_ && export_->active &&
      constantTimeBytes(export_->session, session_id) &&
      static_cast<std::int32_t>(nowMs() - export_->expires_ms) < 0;
}

bool EspRecoveryActionOwner::inspectDisplay(
    recovery::RecoveryActionSnapshot& output) {
  output = recovery::RecoveryActionSnapshot{};
  output.domain = recovery::RecoveryActionDomain::Display;
  output.backend = recovery::RecoveryActionBackend::None;
  const storage::LegacyDisplayRecoveryProbe probe =
      display_.inspect(display_snapshot_);
  display_snapshot_valid_ = true;
  using DisplayProbe = storage::LegacyDisplayRecoveryProbe;
  switch (probe) {
    case DisplayProbe::Empty:
      output.state = recovery::RecoveryActionState::Empty;
      break;
    case DisplayProbe::Recoverable: {
      output.state = recovery::RecoveryActionState::ChoiceRequired;
      auto& target = output.candidates[0];
      const bool target_valid = display_snapshot_.journal.stage !=
          storage::LegacyDisplayJournalStage::Aborted;
      target.state = target_valid
          ? recovery::RecoveryActionCandidateState::Valid
          : recovery::RecoveryActionCandidateState::Invalid;
      target.byte_count = display_snapshot_.journal.asset_bytes;
      target.digest_present = target_valid && decodeDigest(
          display_snapshot_.journal.asset_id, target.digest);
      output.valid_candidates += static_cast<std::uint8_t>(target_valid);

      auto& previous = output.candidates[2];
      previous.state = recovery::RecoveryActionCandidateState::Valid;
      previous.digest_present = decodeDigest(
          display_snapshot_.journal.previous_current, previous.digest);
      ++output.valid_candidates;
      break;
    }
    case DisplayProbe::Corrupt:
      output.state = recovery::RecoveryActionState::Recoverable;
      output.candidates[0].state =
          recovery::RecoveryActionCandidateState::Invalid;
      output.candidates[2].state =
          recovery::RecoveryActionCandidateState::Valid;
      output.valid_candidates = 1U;
      break;
    case DisplayProbe::IoError:
      output.state = recovery::RecoveryActionState::IoError;
      display_snapshot_valid_ = false;
      break;
  }
  return hashDisplaySnapshot(output, display_snapshot_, output.inspection_id);
}

bool EspRecoveryActionOwner::inspectFile(
    std::size_t cache_index, storage::LegacyFileTransactionTarget target,
    recovery::RecoveryActionDomain domain,
    recovery::RecoveryActionBackend backend,
    recovery::RecoveryActionSnapshot& output) {
  if (cache_index >= file_snapshots_.size()) return false;
  output = recovery::RecoveryActionSnapshot{};
  output.domain = domain;
  output.backend = backend;
  const storage::LegacyFileTransactionProbe probe =
      files_->inspect(target, file_snapshots_[cache_index]);
  output.state = mapFileState(probe);
  output.valid_candidates = file_snapshots_[cache_index].valid_candidates;
  for (std::size_t at = 0U; at < output.candidates.size(); ++at) {
    const auto& source = file_snapshots_[cache_index].candidates[at];
    auto& destination = output.candidates[at];
    destination.state = mapCandidate(source.probe);
    destination.byte_count = source.byte_count;
    destination.digest = source.sha256;
    destination.digest_present = source.digest_present;
    destination.item_count = source.item_count;
    destination.item_count_present = source.item_count_present;
    destination.modified_unix_seconds = source.modified_unix_seconds;
    destination.modified_time_present = source.modified_time_present;
  }
  file_snapshot_valid_[cache_index] =
      probe != storage::LegacyFileTransactionProbe::InvalidTarget;
  return hashFileSnapshot(output, file_snapshots_[cache_index],
                          output.inspection_id);
}

recovery::RecoveryActionReadResult
EspRecoveryActionOwner::inspectRecoveryActions(
    recovery::RecoveryActionInventory& output) {
  output = recovery::RecoveryActionInventory{};
  if (!ready()) return recovery::RecoveryActionReadResult::Unavailable;
  if (!lockActions()) return recovery::RecoveryActionReadResult::Busy;
  cached_inventory_ = recovery::RecoveryActionInventory{};
  display_snapshot_valid_ = false;
  file_snapshot_valid_.fill(false);

  bool valid = inspectDisplay(cached_inventory_.snapshots[0]);
  cached_inventory_.count = 1U;
  valid = inspectFile(
      0U, {storage::LegacyFileTransactionDomain::Tasks,
           storage::LegacyFileTransactionBackend::TaskRoot},
      recovery::RecoveryActionDomain::Tasks,
      recovery::RecoveryActionBackend::None,
      cached_inventory_.snapshots[1]) && valid;
  cached_inventory_.count = 2U;
  valid = inspectFile(
      1U, {storage::LegacyFileTransactionDomain::Album,
           storage::LegacyFileTransactionBackend::Internal},
      recovery::RecoveryActionDomain::Album,
      recovery::RecoveryActionBackend::Internal,
      cached_inventory_.snapshots[2]) && valid;
  cached_inventory_.count = 3U;
  if (storage_.recoveryReadRemovableRoot()) {
    valid = inspectFile(
        2U, {storage::LegacyFileTransactionDomain::Album,
             storage::LegacyFileTransactionBackend::Removable},
        recovery::RecoveryActionDomain::Album,
        recovery::RecoveryActionBackend::Removable,
        cached_inventory_.snapshots[3]) && valid;
    cached_inventory_.count = 4U;
  }
  output = cached_inventory_;
  unlock();
  return valid ? recovery::RecoveryActionReadResult::Ok
               : recovery::RecoveryActionReadResult::InvalidData;
}

const recovery::RecoveryActionSnapshot*
EspRecoveryActionOwner::findCached(
    recovery::RecoveryActionDomain domain,
    recovery::RecoveryActionBackend backend) const {
  for (std::size_t at = 0U; at < cached_inventory_.count; ++at) {
    const auto& value = cached_inventory_.snapshots[at];
    if (value.domain == domain && value.backend == backend) return &value;
  }
  return nullptr;
}

recovery::RecoveryActionResolveResult
EspRecoveryActionOwner::resolveRecoveryAction(
    const recovery::RecoveryActionRequest& request) {
  using Result = recovery::RecoveryActionResolveResult;
  if (!ready()) return Result::SourceUnavailable;
  const bool removable_album =
      request.domain == recovery::RecoveryActionDomain::Album &&
      request.backend == recovery::RecoveryActionBackend::Removable;
  if (removable_album != request.external_backup_confirmed)
    return Result::InvalidRequest;
  if (!lockActions()) return Result::Busy;
  if (export_ && export_->active) {
    unlock();
    return Result::Busy;
  }
  const recovery::RecoveryActionSnapshot* cached =
      findCached(request.domain, request.backend);
  if (!cached || !sameId(cached->inspection_id, request.inspection_id)) {
    unlock();
    return Result::SourceChanged;
  }

  const bool display_request =
      request.domain == recovery::RecoveryActionDomain::Display &&
      request.backend == recovery::RecoveryActionBackend::None &&
      request.choice != recovery::RecoveryActionChoice::Next &&
      display_snapshot_valid_;
  std::size_t index = file_snapshots_.size();
  storage::LegacyFileTransactionTarget target{};
  storage::RecoveryMutationDomain mutation =
      storage::RecoveryMutationDomain::None;
  if (display_request) {
    mutation = storage::RecoveryMutationDomain::Display;
  } else if (request.domain == recovery::RecoveryActionDomain::Tasks &&
             request.backend == recovery::RecoveryActionBackend::None) {
    index = 0U;
    target = {storage::LegacyFileTransactionDomain::Tasks,
              storage::LegacyFileTransactionBackend::TaskRoot};
    mutation = storage::RecoveryMutationDomain::Tasks;
  } else if (request.domain == recovery::RecoveryActionDomain::Album &&
             request.backend == recovery::RecoveryActionBackend::Internal) {
    index = 1U;
    target = {storage::LegacyFileTransactionDomain::Album,
              storage::LegacyFileTransactionBackend::Internal};
    mutation = storage::RecoveryMutationDomain::InternalAlbum;
  } else if (request.domain == recovery::RecoveryActionDomain::Album &&
             request.backend == recovery::RecoveryActionBackend::Removable) {
    index = 2U;
    target = {storage::LegacyFileTransactionDomain::Album,
              storage::LegacyFileTransactionBackend::Removable};
    mutation = storage::RecoveryMutationDomain::RemovableAlbum;
  }
  if (mutation == storage::RecoveryMutationDomain::None ||
      (!display_request &&
       (index >= file_snapshots_.size() || !file_snapshot_valid_[index]))) {
    unlock();
    return Result::InvalidRequest;
  }
  if (storage_.beginRecoveryMutation(mutation) != ESP_OK) {
    unlock();
    return Result::SourceUnavailable;
  }

  Result result = Result::InvalidRequest;
  if (display_request) {
    result = mapDisplayResult(display_.resolve(
        display_snapshot_,
        request.choice == recovery::RecoveryActionChoice::Current
            ? storage::LegacyDisplayResolutionChoice::Target
            : storage::LegacyDisplayResolutionChoice::Previous));
  } else {
    result = mapFileResult(files_->resolve(
        file_snapshots_[index], {target, mapFileChoice(request.choice)}));
  }

  // Mutation authority is consumed exactly once. Regardless of executor
  // outcome, revoke it, unmount RW internal storage, and restore the hardened
  // RO descriptor before any audit or HTTP response.
  const esp_err_t revoked =
      storage_.endRecoveryMutationAndRemountReadOnly();
  if (revoked != ESP_OK) {
    invalidateActionsAndLatchForcedRestartLocked(nowMs());
    result = Result::IoError;
  } else if (result == Result::Ok) {
    if (postActionAuditClean()) {
      restart_not_before_ms_.store(nowMs() + kRestartResponseGraceMs,
                                   std::memory_order_release);
    } else {
      result = Result::VerificationFailed;
    }
  }
  unlock();
  return result;
}

recovery::RecoveryExportResult
EspRecoveryActionOwner::prepareRecoveryExport(
    const recovery::RecoveryExportExpectedIndexes& expected,
    recovery::RecoveryExportSnapshot& output) {
  using Result = recovery::RecoveryExportResult;
  output = recovery::RecoveryExportSnapshot{};
  if (!ready()) return Result::SourceUnavailable;
  if (!lockActions()) return Result::Busy;
  if (export_->active &&
      static_cast<std::int32_t>(nowMs() - export_->expires_ms) >= 0) {
    resetExportLocked();
  }
  if (export_->active) {
    unlock();
    return Result::Busy;
  }
  if (constantTimeBytes(expected.digests[0], expected.digests[1]) ||
      constantTimeBytes(expected.digests[0], expected.digests[2]) ||
      constantTimeBytes(expected.digests[1], expected.digests[2]) ||
      std::any_of(expected.digests.begin(), expected.digests.end(),
                  [](const auto& digest) {
                    return std::all_of(digest.begin(), digest.end(),
                                       [](std::uint8_t byte) {
                                         return byte == 0U;
                                       });
                  })) {
    unlock();
    return Result::InvalidRequest;
  }
  const char* removable = storage_.recoveryReadRemovableRoot();
  if (!removable) {
    unlock();
    return Result::SourceUnavailable;
  }
  resetExportLocked();
  const std::string album = std::string(removable) + "/inkloop-album";
  const std::array<const char*, recovery::kRecoveryActionCandidateCount>
      names{{"index.json", "index.next", "index.prev"}};
  Result result = Result::Ok;
  std::uint64_t total = 0U;
  for (size_t slot = 0U; slot < names.size() && result == Result::Ok;
       ++slot) {
    std::string bytes;
    std::array<std::uint8_t, 32> digest{};
    struct stat status {};
    if (!readIndexFile(album + "/" + names[slot], bytes, digest, status)) {
      result = Result::SourceUnavailable;
      continue;
    }
    if (!constantTimeBytes(digest, expected.digests[slot])) {
      std::fill(bytes.begin(), bytes.end(), '\0');
      result = Result::SourceChanged;
      continue;
    }
    storage::AlbumIndex index;
    if (storage::parseAlbumIndex(bytes, index) !=
        storage::AlbumIndexCode::Ok) {
      std::fill(bytes.begin(), bytes.end(), '\0');
      result = Result::VerificationFailed;
      continue;
    }
    auto& candidate = export_->snapshot.candidates[slot];
    candidate.byte_count = bytes.size();
    candidate.digest = digest;
    candidate.asset_entries = static_cast<std::uint32_t>(index.assets.size());
    export_->candidate_modified[slot] =
        static_cast<std::int64_t>(status.st_mtime);
    if (candidate.byte_count > std::numeric_limits<std::uint64_t>::max() -
                                   total) {
      result = Result::VerificationFailed;
    } else {
      total += candidate.byte_count;
    }
    for (const storage::AlbumIndexAsset& asset : index.assets) {
      if (result != Result::Ok) break;
      std::array<std::uint8_t, 32> asset_digest{};
      if (!decodeDigest(asset.content_sha256, asset_digest)) {
        result = Result::VerificationFailed;
        break;
      }
      size_t found = export_->snapshot.asset_count;
      for (size_t at = 0U; at < export_->snapshot.asset_count; ++at) {
        if (constantTimeBytes(export_->assets[at].summary.digest,
                              asset_digest)) {
          found = at;
          break;
        }
      }
      if (found == export_->snapshot.asset_count) {
        if (found >= export_->assets.size()) {
          result = Result::VerificationFailed;
          break;
        }
        auto& record = export_->assets[found];
        record.summary.byte_count = asset.bytes;
        record.summary.digest = asset_digest;
        struct stat asset_status {};
        if (!statRegular(std::string(removable) + asset.path, asset.bytes,
                         asset_status)) {
          result = Result::SourceUnavailable;
          break;
        }
        record.modified = static_cast<std::int64_t>(asset_status.st_mtime);
        ++export_->snapshot.asset_count;
        if (asset.bytes > std::numeric_limits<std::uint64_t>::max() - total) {
          result = Result::VerificationFailed;
          break;
        }
        total += asset.bytes;
      } else if (export_->assets[found].summary.byte_count != asset.bytes) {
        result = Result::VerificationFailed;
        break;
      }
      export_->assets[found].summary.candidate_mask |=
          static_cast<std::uint8_t>(1U << slot);
    }
    std::fill(bytes.begin(), bytes.end(), '\0');
  }
  if (result != Result::Ok) {
    resetExportLocked();
    unlock();
    return result;
  }
  esp_fill_random(export_->session.data(), export_->session.size());
  bool session_empty = true;
  for (const std::uint8_t byte : export_->session)
    session_empty = session_empty && byte == 0U;
  if (session_empty) export_->session[0] = 1U;
  export_->snapshot.session_id = export_->session;
  export_->snapshot.inventory_pages =
      (export_->snapshot.asset_count +
       recovery::kRecoveryExportInventoryPageAssets - 1U) /
      recovery::kRecoveryExportInventoryPageAssets;
  export_->snapshot.total_bytes = total;
  export_->expires_ms = nowMs() + kExportSessionLifetimeMs;
  export_->active = true;
  output = export_->snapshot;
  unlock();
  return Result::Ok;
}

recovery::RecoveryExportResult
EspRecoveryActionOwner::readRecoveryExportInventory(
    const std::array<uint8_t, recovery::kRecoveryExportSessionBytes>&
        session_id,
    uint32_t page, recovery::RecoveryExportInventoryPage& output) {
  using Result = recovery::RecoveryExportResult;
  output = recovery::RecoveryExportInventoryPage{};
  if (!ready()) return Result::SourceUnavailable;
  if (!lockActions()) return Result::Busy;
  if (!exportSessionMatches(session_id)) {
    if (export_->active &&
        static_cast<std::int32_t>(nowMs() - export_->expires_ms) >= 0)
      resetExportLocked();
    unlock();
    return Result::SessionStale;
  }
  if (page >= export_->snapshot.inventory_pages) {
    unlock();
    return Result::InvalidRequest;
  }
  const size_t first = page * recovery::kRecoveryExportInventoryPageAssets;
  const size_t remaining = export_->snapshot.asset_count - first;
  output.session_id = export_->session;
  output.page = page;
  output.asset_offset = first;
  output.count = static_cast<std::uint8_t>(std::min(
      remaining, recovery::kRecoveryExportInventoryPageAssets));
  for (size_t at = 0U; at < output.count; ++at)
    output.assets[at] = export_->assets[first + at].summary;
  unlock();
  return Result::Ok;
}

recovery::RecoveryExportResult EspRecoveryActionOwner::openRecoveryExport(
    const recovery::RecoveryExportOpenRequest& request,
    recovery::RecoveryExportStream& output) {
  using Result = recovery::RecoveryExportResult;
  output = recovery::RecoveryExportStream{};
  if (!ready()) return Result::SourceUnavailable;
  if (!lockActions()) return Result::Busy;
  if (!exportSessionMatches(request.session_id)) {
    if (export_->active &&
        static_cast<std::int32_t>(nowMs() - export_->expires_ms) >= 0)
      resetExportLocked();
    unlock();
    return Result::SessionStale;
  }
  if (export_->file) {
    unlock();
    return Result::Busy;
  }
  const size_t maximum_item = recovery::kRecoveryActionCandidateCount +
      export_->snapshot.asset_count;
  if (request.item >= maximum_item) {
    unlock();
    return Result::InvalidRequest;
  }
  const char* removable = storage_.recoveryReadRemovableRoot();
  if (!removable) {
    unlock();
    return Result::SourceUnavailable;
  }
  std::string path = std::string(removable) + "/inkloop-album/";
  std::uint64_t expected_bytes = 0U;
  std::int64_t expected_modified = 0;
  std::array<std::uint8_t, 32> expected_digest{};
  if (request.item < recovery::kRecoveryActionCandidateCount) {
    static constexpr std::array<const char*,
        recovery::kRecoveryActionCandidateCount> names{{
            "index.json", "index.next", "index.prev"}};
    path += names[request.item];
    expected_bytes = export_->snapshot.candidates[request.item].byte_count;
    expected_digest = export_->snapshot.candidates[request.item].digest;
    expected_modified = export_->candidate_modified[request.item];
  } else {
    const size_t asset =
        request.item - recovery::kRecoveryActionCandidateCount;
    const auto& record = export_->assets[asset];
    constexpr char hex[] = "0123456789abcdef";
    std::string name(64U, '0');
    for (size_t at = 0U; at < record.summary.digest.size(); ++at) {
      name[at * 2U] = hex[record.summary.digest[at] >> 4U];
      name[at * 2U + 1U] = hex[record.summary.digest[at] & 0x0fU];
    }
    path += name + ".png";
    expected_bytes = record.summary.byte_count;
    expected_digest = record.summary.digest;
    expected_modified = record.modified;
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  struct stat status {};
  if (!file || ::fstat(::fileno(file), &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != expected_bytes ||
      static_cast<std::int64_t>(status.st_mtime) != expected_modified) {
    if (file) std::fclose(file);
    unlock();
    return Result::SourceChanged;
  }
  export_->file = file;
  export_->item = request.item;
  export_->streamed = 0U;
  export_->expected_bytes = expected_bytes;
  export_->expected_digest = expected_digest;
  export_->opened_modified = expected_modified;
  export_->stream_hash = storage::Sha256{};
  if (++export_->handle == 0U) ++export_->handle;
  output.handle = export_->handle;
  output.byte_count = expected_bytes;
  output.digest = expected_digest;
  output.item = request.item;
  unlock();
  return Result::Ok;
}

recovery::RecoveryExportResult EspRecoveryActionOwner::readRecoveryExport(
    uint32_t handle, uint8_t* output, size_t capacity, size_t& bytes_read) {
  using Result = recovery::RecoveryExportResult;
  bytes_read = 0U;
  if (!ready() || !output || capacity == 0U ||
      capacity > recovery::kMaximumRecoveryExportChunkBytes)
    return Result::InvalidRequest;
  if (!lockActions()) return Result::Busy;
  if (!export_->active || !export_->file || export_->handle != handle) {
    unlock();
    return Result::SessionStale;
  }
  if (static_cast<std::int32_t>(nowMs() - export_->expires_ms) >= 0) {
    resetExportLocked();
    unlock();
    return Result::SessionStale;
  }
  if (export_->streamed == export_->expected_bytes) {
    const int extra = std::fgetc(export_->file);
    struct stat status {};
    std::array<std::uint8_t, 32> digest{};
    const bool valid = extra == EOF && std::feof(export_->file) &&
        !std::ferror(export_->file) &&
        ::fstat(::fileno(export_->file), &status) == 0 &&
        static_cast<std::uint64_t>(status.st_size) ==
            export_->expected_bytes &&
        static_cast<std::int64_t>(status.st_mtime) ==
            export_->opened_modified && export_->stream_hash.finish(digest) &&
        constantTimeBytes(digest, export_->expected_digest);
    std::fclose(export_->file);
    export_->file = nullptr;
    if (valid) export_->verified[export_->item] = true;
    unlock();
    return valid ? Result::Complete : Result::VerificationFailed;
  }
  const size_t wanted = static_cast<size_t>(std::min<std::uint64_t>(
      capacity, export_->expected_bytes - export_->streamed));
  const size_t count = std::fread(output, 1U, wanted, export_->file);
  if (count == 0U || !export_->stream_hash.update(output, count)) {
    std::fclose(export_->file);
    export_->file = nullptr;
    unlock();
    return Result::IoError;
  }
  export_->streamed += count;
  bytes_read = count;
  unlock();
  return Result::Ok;
}

void EspRecoveryActionOwner::closeRecoveryExport(uint32_t handle) {
  if (!ready() || !lockActions()) return;
  if (export_->file && export_->handle == handle) {
    std::fclose(export_->file);
    export_->file = nullptr;
  }
  unlock();
}

recovery::RecoveryExportResult
EspRecoveryActionOwner::finishRecoveryExport(
    const std::array<uint8_t, recovery::kRecoveryExportSessionBytes>&
        session_id) {
  using Result = recovery::RecoveryExportResult;
  if (!ready()) return Result::SourceUnavailable;
  if (!lockActions()) return Result::Busy;
  if (!exportSessionMatches(session_id)) {
    if (export_->active &&
        static_cast<std::int32_t>(nowMs() - export_->expires_ms) >= 0)
      resetExportLocked();
    unlock();
    return Result::SessionStale;
  }
  if (export_->file) {
    unlock();
    return Result::Busy;
  }
  const size_t item_count = recovery::kRecoveryActionCandidateCount +
      export_->snapshot.asset_count;
  for (size_t at = 0U; at < item_count; ++at) {
    if (!export_->verified[at]) {
      unlock();
      return Result::VerificationFailed;
    }
  }
  const char* removable = storage_.recoveryReadRemovableRoot();
  if (!removable) {
    unlock();
    return Result::SourceUnavailable;
  }
  const std::string album = std::string(removable) + "/inkloop-album";
  const std::array<const char*, recovery::kRecoveryActionCandidateCount>
      names{{"index.json", "index.next", "index.prev"}};
  Result result = Result::Complete;
  for (size_t slot = 0U; slot < names.size() && result == Result::Complete;
       ++slot) {
    std::string bytes;
    std::array<std::uint8_t, 32> digest{};
    struct stat status {};
    if (!readIndexFile(album + "/" + names[slot], bytes, digest, status) ||
        !constantTimeBytes(
            digest, export_->snapshot.candidates[slot].digest) ||
        static_cast<std::int64_t>(status.st_mtime) !=
            export_->candidate_modified[slot]) {
      result = Result::SourceChanged;
    }
    std::fill(bytes.begin(), bytes.end(), '\0');
  }
  for (size_t at = 0U;
       at < export_->snapshot.asset_count && result == Result::Complete;
       ++at) {
    const auto& record = export_->assets[at];
    constexpr char hex[] = "0123456789abcdef";
    std::string name(64U, '0');
    for (size_t byte = 0U; byte < record.summary.digest.size(); ++byte) {
      name[byte * 2U] = hex[record.summary.digest[byte] >> 4U];
      name[byte * 2U + 1U] = hex[record.summary.digest[byte] & 0x0fU];
    }
    if (!hashReadOnlyFile(album + "/" + name + ".png",
                          record.summary.byte_count, record.modified,
                          record.summary.digest)) {
      result = Result::SourceChanged;
    }
  }
  resetExportLocked();
  unlock();
  return result;
}

void EspRecoveryActionOwner::abortRecoveryExport(
    const std::array<uint8_t, recovery::kRecoveryExportSessionBytes>&
        session_id) {
  if (!ready() || !lockActions()) return;
  if (export_->active && constantTimeBytes(export_->session, session_id))
    resetExportLocked();
  unlock();
}

bool EspRecoveryActionOwner::postActionAuditClean() const {
  const char* internal_root = storage_.recoveryReadTaskRoot();
  if (!storage::persistenceCompatibilityContractValid() ||
      !internal_root) {
    return false;
  }
  const storage::EspNvsUpgradeInventory nvs;
  const storage::PosixUpgradeInventory files(internal_root);
  const storage::UpgradeAuditReport internal =
      storage::auditUpgrade(files.inspect(nvs.inspect()));
  if (!internal.allowsInitialization()) return false;
  const char* removable = storage_.recoveryReadRemovableRoot();
  return !removable ||
      storage::runReadOnlyMountedFileUpgradeAudit(removable)
          .allowsInitialization();
}

bool EspRecoveryActionOwner::restartReady(std::uint32_t now_ms) const {
  return due(now_ms,
             restart_not_before_ms_.load(std::memory_order_acquire));
}

bool EspRecoveryActionOwner::forcedRestartReady(std::uint32_t now_ms) const {
  if (!forced_restart_latched_.load(std::memory_order_acquire)) return false;
  const std::uint32_t deadline =
      forced_restart_not_before_ms_.load(std::memory_order_relaxed);
  return static_cast<std::int32_t>(now_ms - deadline) >= 0;
}

}  // namespace inkloop
