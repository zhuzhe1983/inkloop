#include "recovery_action_owner.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "esp_timer.h"
#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/esp_upgrade_boot_audit.hpp"
#include "inkloop/storage/persistence_compatibility.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "inkloop/storage/sha256.hpp"
#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace {

constexpr std::uint32_t kRestartResponseGraceMs = 2000U;

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

EspRecoveryActionOwner::EspRecoveryActionOwner(
    storage::EspStorageMountOwner& storage)
    : storage_(storage),
      display_(storage),
      files_({storage.taskRoot() ? storage.taskRoot() : "",
              storage.internalRoot() ? storage.internalRoot() : "",
              storage.removableRoot() ? storage.removableRoot() : ""}),
      mutex_(xSemaphoreCreateMutex()) {}

EspRecoveryActionOwner::~EspRecoveryActionOwner() {
  if (mutex_) vSemaphoreDelete(mutex_);
  mutex_ = nullptr;
}

bool EspRecoveryActionOwner::lock() {
  return mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE;
}

void EspRecoveryActionOwner::unlock() {
  if (mutex_) xSemaphoreGive(mutex_);
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
      files_.inspect(target, file_snapshots_[cache_index]);
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
  if (!lock()) return recovery::RecoveryActionReadResult::Busy;
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
  if (storage_.removableRoot()) {
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
  if (!lock()) return Result::Busy;
  const recovery::RecoveryActionSnapshot* cached =
      findCached(request.domain, request.backend);
  if (!cached || !sameId(cached->inspection_id, request.inspection_id)) {
    unlock();
    return Result::SourceChanged;
  }

  Result result = Result::InvalidRequest;
  if (request.domain == recovery::RecoveryActionDomain::Display &&
      request.backend == recovery::RecoveryActionBackend::None &&
      request.choice != recovery::RecoveryActionChoice::Next &&
      display_snapshot_valid_) {
    result = mapDisplayResult(display_.resolve(
        display_snapshot_,
        request.choice == recovery::RecoveryActionChoice::Current
            ? storage::LegacyDisplayResolutionChoice::Target
            : storage::LegacyDisplayResolutionChoice::Previous));
  } else {
    std::size_t index = file_snapshots_.size();
    storage::LegacyFileTransactionTarget target{};
    if (request.domain == recovery::RecoveryActionDomain::Tasks &&
        request.backend == recovery::RecoveryActionBackend::None) {
      index = 0U;
      target = {storage::LegacyFileTransactionDomain::Tasks,
                storage::LegacyFileTransactionBackend::TaskRoot};
    } else if (request.domain == recovery::RecoveryActionDomain::Album &&
               request.backend == recovery::RecoveryActionBackend::Internal) {
      index = 1U;
      target = {storage::LegacyFileTransactionDomain::Album,
                storage::LegacyFileTransactionBackend::Internal};
    } else if (request.domain == recovery::RecoveryActionDomain::Album &&
               request.backend ==
                   recovery::RecoveryActionBackend::Removable) {
      index = 2U;
      target = {storage::LegacyFileTransactionDomain::Album,
                storage::LegacyFileTransactionBackend::Removable};
    }
    if (index < file_snapshots_.size() && file_snapshot_valid_[index]) {
      result = mapFileResult(files_.resolve(
          file_snapshots_[index], {target, mapFileChoice(request.choice)}));
    }
  }

  if (result == Result::Ok && postActionAuditClean()) {
    restart_not_before_ms_.store(nowMs() + kRestartResponseGraceMs,
                                 std::memory_order_release);
  }
  unlock();
  return result;
}

bool EspRecoveryActionOwner::postActionAuditClean() const {
  if (!storage::persistenceCompatibilityContractValid() ||
      !storage_.taskRoot()) {
    return false;
  }
  const storage::EspNvsUpgradeInventory nvs;
  const storage::PosixUpgradeInventory files(storage_.taskRoot());
  const storage::UpgradeAuditReport internal =
      storage::auditUpgrade(files.inspect(nvs.inspect()));
  if (!internal.allowsInitialization()) return false;
  const char* removable = storage_.removableRoot();
  return !removable ||
      storage::runReadOnlyMountedFileUpgradeAudit(removable)
          .allowsInitialization();
}

bool EspRecoveryActionOwner::restartReady(std::uint32_t now_ms) const {
  return due(now_ms,
             restart_not_before_ms_.load(std::memory_order_acquire));
}

}  // namespace inkloop
