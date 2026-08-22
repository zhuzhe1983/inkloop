#pragma once

#include <array>
#include <cstdint>

#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {

// The evidence schema versions the composition contract used by this portable
// planner. It is deliberately separate from the schema of any individual NVS
// namespace or file record.
inline constexpr std::uint16_t kUpgradePlannerEvidenceSchemaVersion = 1U;
inline constexpr std::uint16_t kMigrationMarkerSchemaVersion = 1U;
inline constexpr std::uint16_t kSupportedLegacyLayoutSchemaVersion = 1U;
inline constexpr std::uint16_t kSupportedNativeLayoutSchemaVersion = 1U;

using MigrationFingerprint = std::array<std::uint8_t, 32>;

enum class UpgradeSourceLayout : std::uint8_t {
  Unknown,
  Fresh,
  Legacy,
  Native,
  Unsupported,
};

enum class MigrationMarkerProbe : std::uint8_t {
  Missing,
  Valid,
  Torn,
  Corrupt,
  IoError,
};

enum class MigrationPhase : std::uint8_t {
  None,
  Prepared,
  TargetWritten,
  TargetVerified,
  CommitRecorded,
  RollbackRequired,
  Complete,
};

enum class MigrationSlot : std::uint8_t {
  None,
  SlotA,
  SlotB,
};

enum class MigrationRollbackSource : std::uint8_t {
  None,
  LegacySnapshot,
  NativeSlotA,
  NativeSlotB,
};

// This is a value model only. Persistence and torn-read classification belong
// to a later adapter. The checksum covers every preceding field in canonical
// little-endian order and never depends on struct padding.
struct MigrationMarker {
  std::uint16_t schema_version = kMigrationMarkerSchemaVersion;
  std::uint16_t source_layout_schema_version = 0U;
  std::uint64_t generation = 0U;
  MigrationFingerprint source_fingerprint{};
  MigrationPhase phase = MigrationPhase::None;
  MigrationSlot target_slot = MigrationSlot::None;
  MigrationRollbackSource rollback_source =
      MigrationRollbackSource::None;
  std::uint32_t checksum = 0U;
};

enum class MigrationSlotProbe : std::uint8_t {
  Unknown,
  Missing,
  Valid,
  Invalid,
  IoError,
};

// A Valid slot is considered related to a marker only when both its migration
// generation and source fingerprint match that marker. A rollback slot may use
// an older nonzero generation but must still name the same source fingerprint.
struct MigrationSlotEvidence {
  MigrationSlotProbe probe = MigrationSlotProbe::Unknown;
  std::uint64_t migration_generation = 0U;
  MigrationFingerprint source_fingerprint{};
};

struct DurableLegacySourceEvidence {
  bool durable = false;
  MigrationFingerprint fingerprint{};
};

struct UpgradeRecoveryEvidence {
  std::uint16_t evidence_schema_version =
      kUpgradePlannerEvidenceSchemaVersion;
  UpgradeAuditReport audit{};
  UpgradeSourceLayout source_layout = UpgradeSourceLayout::Unknown;
  std::uint16_t source_layout_schema_version = 0U;
  MigrationFingerprint source_fingerprint{};

  MigrationMarkerProbe marker_probe = MigrationMarkerProbe::Missing;
  MigrationMarker marker{};

  DurableLegacySourceEvidence legacy_source{};
  std::array<MigrationSlotEvidence, 2> native_slots{};
};

enum class UpgradeRecoveryDecision : std::uint8_t {
  Start,
  Resume,
  Rollback,
  ReadOnlyRecovery,
  // Refuse means that this planner authorizes no migration action. The
  // FreshNoMigration and NativeNoMigration reasons are benign no-op outcomes;
  // other Refuse reasons remain safety stops for a future boot composer.
  Refuse,
};

enum class UpgradeRecoveryReason : std::uint8_t {
  EligibleLegacy,
  MigrationInProgress,
  MarkerRequestsRollback,
  TargetLostAfterWrite,
  FreshNoMigration,
  NativeNoMigration,
  DisplayResolutionRequired,
  AuditAmbiguous,
  AuditRecoveryWithoutMarker,
  SourceUnavailable,
  UnsupportedEvidenceSchema,
  UnsupportedSourceSchema,
  UnsupportedMarkerSchema,
  CorruptMarker,
  MarkerUnavailable,
  SourceFingerprintChanged,
  OrphanedMarker,
  TargetAmbiguous,
  RollbackSourceUnavailable,
  InvalidEvidence,
};

struct UpgradeRecoveryPlan {
  UpgradeRecoveryDecision decision = UpgradeRecoveryDecision::Refuse;
  UpgradeRecoveryReason reason = UpgradeRecoveryReason::InvalidEvidence;
  std::uint64_t generation = 0U;
  MigrationPhase phase = MigrationPhase::None;
  MigrationPhase next_phase = MigrationPhase::None;
  MigrationSlot target_slot = MigrationSlot::None;
  MigrationRollbackSource rollback_source =
      MigrationRollbackSource::None;

  bool authorizesMutation() const {
    return decision == UpgradeRecoveryDecision::Start ||
        decision == UpgradeRecoveryDecision::Resume ||
        decision == UpgradeRecoveryDecision::Rollback;
  }

  bool noMigrationNeeded() const {
    return decision == UpgradeRecoveryDecision::Refuse &&
        (reason == UpgradeRecoveryReason::FreshNoMigration ||
         reason == UpgradeRecoveryReason::NativeNoMigration);
  }
};

std::uint32_t migrationMarkerChecksum(const MigrationMarker& marker);
bool migrationMarkerValid(const MigrationMarker& marker);
UpgradeRecoveryPlan planUpgradeRecovery(
    const UpgradeRecoveryEvidence& evidence);

const char* upgradeRecoveryDecisionName(UpgradeRecoveryDecision decision);
const char* upgradeRecoveryReasonName(UpgradeRecoveryReason reason);
const char* migrationPhaseName(MigrationPhase phase);

}  // namespace storage
}  // namespace inkloop
