#pragma once

#include <cstddef>
#include <cstdint>

#include "inkloop/storage/upgrade_snapshot_collector.hpp"

namespace inkloop {
namespace storage {

// One marker generation always represents this complete logical group. Its
// members are streamed one namespace/file record at a time; a caller cannot
// substitute a partial group that the v1 marker would be unable to identify.
enum class UpgradeLogicalGroup : std::uint8_t {
  ProtectedSnapshotV1,
};

const char* upgradeLogicalGroupName(UpgradeLogicalGroup group);

enum class UpgradeTargetStoreCode : std::uint8_t {
  Ok,
  InvalidArgument,
  Conflict,
  IoError,
};

// The target adapter owns an isolated transaction for the explicitly named
// inactive slot. Nothing staged by begin/write/end is authoritative or visible
// as a Valid group until commitTargetGroup succeeds.
class IUpgradeMigrationTargetStore {
 public:
  virtual ~IUpgradeMigrationTargetStore() = default;
  virtual UpgradeTargetStoreCode inspectTargetGroup(
      MigrationSlot slot, UpgradeLogicalGroup group,
      MigrationSlotEvidence& output) const = 0;
  virtual UpgradeTargetStoreCode beginTargetGroup(
      MigrationSlot slot, UpgradeLogicalGroup group,
      std::uint64_t generation,
      const MigrationFingerprint& source_fingerprint) = 0;
  virtual UpgradeTargetStoreCode beginTargetRecord(
      UpgradeRecordId record, bool present) = 0;
  virtual UpgradeTargetStoreCode writeTargetBytes(
      const std::uint8_t* bytes, std::size_t length) = 0;
  virtual UpgradeTargetStoreCode finishTargetRecord() = 0;
  virtual UpgradeTargetStoreCode commitTargetGroup() = 0;
  virtual UpgradeRecordStreamCode streamTargetRecord(
      MigrationSlot slot, UpgradeLogicalGroup group, UpgradeRecordId record,
      std::uint64_t maximum_bytes, IUpgradeByteSink& sink) const = 0;
};

enum class UpgradeAuthorityKind : std::uint8_t {
  Unknown,
  LegacySnapshot,
  NativeSlotA,
  NativeSlotB,
};

struct UpgradeAuthority {
  UpgradeAuthorityKind kind = UpgradeAuthorityKind::Unknown;
  std::uint64_t generation = 0U;
  MigrationFingerprint source_fingerprint{};
};

enum class UpgradeAuthorityCode : std::uint8_t {
  Ok,
  InvalidArgument,
  Conflict,
  IoError,
};

// This explicit seam is the only authority-changing capability visible to the
// portable executor. No ESP/boot implementation is provided in this tranche.
class IUpgradeAuthorityHead {
 public:
  virtual ~IUpgradeAuthorityHead() = default;
  virtual UpgradeAuthorityCode inspectAuthority(
      UpgradeAuthority& output) const = 0;
  virtual UpgradeAuthorityCode activateTarget(
      MigrationSlot target, std::uint64_t generation,
      const MigrationFingerprint& source_fingerprint) = 0;
  virtual UpgradeAuthorityCode activateRollback(
      MigrationRollbackSource rollback_source,
      const MigrationFingerprint& source_fingerprint) = 0;
};

struct UpgradeExecutionRequest {
  UpgradeRecoveryPlan plan{};
  MigrationFingerprint source_fingerprint{};
  std::uint64_t marker_sequence = 0U;
  bool marker_present = false;
  MigrationMarker marker{};
  UpgradeLogicalGroup group = UpgradeLogicalGroup::ProtectedSnapshotV1;
};

// Bind all fields that are absent from UpgradeRecoveryPlan itself. Executors
// compare this request to an independently collected fresh result before any
// mutation.
bool bindUpgradeExecutionRequest(
    const CollectedUpgradeRecovery& collected, UpgradeLogicalGroup group,
    UpgradeExecutionRequest& output);

enum class UpgradeExecutorCode : std::uint8_t {
  Ok,
  InvalidArgument,
  EvidenceChanged,
  EvidenceAmbiguous,
  EvidenceIoError,
  UnsupportedSchema,
  TooLarge,
  PlanMismatch,
  NotAuthorized,
  UnsafeAuthority,
  UnsafeTarget,
  SourceChanged,
  SourceReadError,
  TargetWriteError,
  TargetCommitError,
  TargetReadError,
  TargetMismatch,
  MarkerError,
  AuthoritySwitchError,
  // Rollback authority is restored (or was already restored), but v1 has no
  // safe post-rollback terminal marker transition. Boot wiring must not loop
  // this automatically; a later tranche must define that terminal state.
  RollbackAppliedAwaitingTerminal,
};

struct UpgradeExecutionOutcome {
  UpgradeExecutorCode code = UpgradeExecutorCode::InvalidArgument;
  MigrationPhase durable_phase = MigrationPhase::None;
  std::uint64_t marker_sequence = 0U;
  bool target_group_committed = false;
  bool authority_switched = false;
};

class UpgradeExecutorCore final {
 public:
  UpgradeExecutorCore(
      const IUpgradeSnapshotSource& source,
      IUpgradeMigrationTargetStore& target,
      IUpgradeAuthorityHead& authority,
      IMigrationMarkerJournalStore& journal)
      : source_(source), target_(target), authority_(authority),
        journal_(journal) {}

  // Executes no more than one planner phase. Start only persists Prepared;
  // target mutation can begin on a later, freshly collected Resume step.
  UpgradeExecutorCode execute(
      const UpgradeExecutionRequest& request,
      UpgradeExecutionOutcome& outcome);

 private:
  const IUpgradeSnapshotSource& source_;
  IUpgradeMigrationTargetStore& target_;
  IUpgradeAuthorityHead& authority_;
  IMigrationMarkerJournalStore& journal_;
};

const char* upgradeTargetStoreCodeName(UpgradeTargetStoreCode code);
const char* upgradeAuthorityCodeName(UpgradeAuthorityCode code);
const char* upgradeExecutorCodeName(UpgradeExecutorCode code);

}  // namespace storage
}  // namespace inkloop
