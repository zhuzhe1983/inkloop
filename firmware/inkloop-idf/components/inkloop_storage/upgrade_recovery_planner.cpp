#include "inkloop/storage/upgrade_recovery_planner.hpp"

#include <algorithm>
#include <cstddef>

namespace inkloop {
namespace storage {
namespace {

class Crc32 final {
 public:
  void byte(std::uint8_t value) {
    value_ ^= value;
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(value_ & 1U);
      value_ = (value_ >> 1U) ^ (0xEDB88320U & mask);
    }
  }

  void u16(std::uint16_t value) {
    byte(static_cast<std::uint8_t>(value));
    byte(static_cast<std::uint8_t>(value >> 8U));
  }

  void u64(std::uint64_t value) {
    for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
      byte(static_cast<std::uint8_t>(value >> shift));
  }

  std::uint32_t finish() const { return value_ ^ 0xFFFFFFFFU; }

 private:
  std::uint32_t value_ = 0xFFFFFFFFU;
};

bool fingerprintValid(const MigrationFingerprint& fingerprint) {
  return std::any_of(fingerprint.begin(), fingerprint.end(),
                     [](std::uint8_t byte) { return byte != 0U; });
}

bool phaseValid(MigrationPhase phase) {
  switch (phase) {
    case MigrationPhase::Prepared:
    case MigrationPhase::TargetWritten:
    case MigrationPhase::TargetVerified:
    case MigrationPhase::CommitRecorded:
    case MigrationPhase::RollbackRequired:
    case MigrationPhase::Complete:
      return true;
    case MigrationPhase::None:
      return false;
  }
  return false;
}

bool slotValid(MigrationSlot slot) {
  return slot == MigrationSlot::SlotA || slot == MigrationSlot::SlotB;
}

bool rollbackSourceValid(MigrationRollbackSource source) {
  switch (source) {
    case MigrationRollbackSource::LegacySnapshot:
    case MigrationRollbackSource::NativeSlotA:
    case MigrationRollbackSource::NativeSlotB:
      return true;
    case MigrationRollbackSource::None:
      return false;
  }
  return false;
}

bool rollbackAliasesTarget(const MigrationMarker& marker) {
  return (marker.target_slot == MigrationSlot::SlotA &&
          marker.rollback_source == MigrationRollbackSource::NativeSlotA) ||
      (marker.target_slot == MigrationSlot::SlotB &&
       marker.rollback_source == MigrationRollbackSource::NativeSlotB);
}

const MigrationSlotEvidence* slotEvidence(
    const UpgradeRecoveryEvidence& evidence, MigrationSlot slot) {
  if (slot == MigrationSlot::SlotA) return &evidence.native_slots[0];
  if (slot == MigrationSlot::SlotB) return &evidence.native_slots[1];
  return nullptr;
}

bool validSlotProbe(MigrationSlotProbe probe) {
  switch (probe) {
    case MigrationSlotProbe::Unknown:
    case MigrationSlotProbe::Missing:
    case MigrationSlotProbe::Valid:
    case MigrationSlotProbe::Invalid:
    case MigrationSlotProbe::IoError:
      return true;
  }
  return false;
}

bool slotMatchesTarget(const MigrationSlotEvidence& slot,
                       const MigrationMarker& marker) {
  return slot.probe == MigrationSlotProbe::Valid &&
      slot.migration_generation == marker.generation &&
      slot.source_fingerprint == marker.source_fingerprint;
}

bool slotIsDurableRollback(const MigrationSlotEvidence& slot,
                           const MigrationMarker& marker) {
  return slot.probe == MigrationSlotProbe::Valid &&
      slot.migration_generation != 0U &&
      slot.source_fingerprint == marker.source_fingerprint;
}

bool rollbackSourceDurable(const UpgradeRecoveryEvidence& evidence,
                           const MigrationMarker& marker) {
  switch (marker.rollback_source) {
    case MigrationRollbackSource::LegacySnapshot:
      return evidence.legacy_source.durable &&
          fingerprintValid(evidence.legacy_source.fingerprint) &&
          evidence.legacy_source.fingerprint == marker.source_fingerprint;
    case MigrationRollbackSource::NativeSlotA:
      return slotIsDurableRollback(evidence.native_slots[0], marker);
    case MigrationRollbackSource::NativeSlotB:
      return slotIsDurableRollback(evidence.native_slots[1], marker);
    case MigrationRollbackSource::None:
      return false;
  }
  return false;
}

UpgradeRecoveryPlan basicPlan(UpgradeRecoveryDecision decision,
                              UpgradeRecoveryReason reason) {
  UpgradeRecoveryPlan plan;
  plan.decision = decision;
  plan.reason = reason;
  return plan;
}

UpgradeRecoveryPlan markerPlan(const MigrationMarker& marker,
                               UpgradeRecoveryDecision decision,
                               UpgradeRecoveryReason reason,
                               MigrationPhase next_phase) {
  UpgradeRecoveryPlan plan = basicPlan(decision, reason);
  plan.generation = marker.generation;
  plan.phase = marker.phase;
  plan.next_phase = next_phase;
  plan.target_slot = marker.target_slot;
  plan.rollback_source = marker.rollback_source;
  return plan;
}

UpgradeRecoveryPlan rollbackOrRecover(
    const UpgradeRecoveryEvidence& evidence, UpgradeRecoveryReason reason) {
  if (!rollbackSourceDurable(evidence, evidence.marker)) {
    return markerPlan(evidence.marker,
                      UpgradeRecoveryDecision::ReadOnlyRecovery,
                      UpgradeRecoveryReason::RollbackSourceUnavailable,
                      MigrationPhase::RollbackRequired);
  }
  return markerPlan(evidence.marker, UpgradeRecoveryDecision::Rollback,
                    reason, MigrationPhase::RollbackRequired);
}

bool supportedSource(const UpgradeRecoveryEvidence& evidence) {
  if (!fingerprintValid(evidence.source_fingerprint)) return false;
  if (evidence.source_layout == UpgradeSourceLayout::Legacy) {
    return evidence.source_layout_schema_version ==
        kSupportedLegacyLayoutSchemaVersion;
  }
  if (evidence.source_layout == UpgradeSourceLayout::Native) {
    return evidence.source_layout_schema_version ==
        kSupportedNativeLayoutSchemaVersion;
  }
  return false;
}

bool transactionAuditKnown(TransactionAudit audit) {
  switch (audit) {
    case TransactionAudit::Empty:
    case TransactionAudit::Clean:
    case TransactionAudit::RecoveryRequired:
    case TransactionAudit::Ambiguous:
    case TransactionAudit::SourceUnavailable:
      return true;
  }
  return false;
}

bool auditReportCoherent(const UpgradeAuditReport& audit) {
  if (!transactionAuditKnown(audit.tasks) ||
      !transactionAuditKnown(audit.album))
    return false;
  const bool normal_transactions =
      (audit.tasks == TransactionAudit::Empty ||
       audit.tasks == TransactionAudit::Clean) &&
      (audit.album == TransactionAudit::Empty ||
       audit.album == TransactionAudit::Clean);
  const bool recoverable_transactions =
      audit.tasks != TransactionAudit::Ambiguous &&
      audit.tasks != TransactionAudit::SourceUnavailable &&
      audit.album != TransactionAudit::Ambiguous &&
      audit.album != TransactionAudit::SourceUnavailable;
  switch (audit.result) {
    case UpgradeAuditResult::Fresh:
      return audit.tasks == TransactionAudit::Empty &&
          audit.album == TransactionAudit::Empty &&
          audit.protected_records_present == 0U;
    case UpgradeAuditResult::Compatible:
      return normal_transactions && audit.protected_records_present != 0U;
    case UpgradeAuditResult::RecoveryRequired:
      return recoverable_transactions &&
          audit.protected_records_present != 0U;
    case UpgradeAuditResult::DisplayResolutionRequired:
    case UpgradeAuditResult::Ambiguous:
    case UpgradeAuditResult::SourceUnavailable:
      return true;
  }
  return false;
}

bool markerSlotSetCoherent(const UpgradeRecoveryEvidence& evidence,
                           const MigrationMarker& marker) {
  const std::size_t target = marker.target_slot == MigrationSlot::SlotA
      ? 0U
      : 1U;
  const std::size_t other = target == 0U ? 1U : 0U;
  const MigrationRollbackSource other_source = other == 0U
      ? MigrationRollbackSource::NativeSlotA
      : MigrationRollbackSource::NativeSlotB;
  if (marker.rollback_source == other_source)
    return slotIsDurableRollback(evidence.native_slots[other], marker);
  return evidence.native_slots[other].probe == MigrationSlotProbe::Missing;
}

}  // namespace

std::uint32_t migrationMarkerChecksum(const MigrationMarker& marker) {
  Crc32 crc;
  crc.u16(marker.schema_version);
  crc.u16(marker.source_layout_schema_version);
  crc.u64(marker.generation);
  for (std::uint8_t byte : marker.source_fingerprint) crc.byte(byte);
  crc.byte(static_cast<std::uint8_t>(marker.phase));
  crc.byte(static_cast<std::uint8_t>(marker.target_slot));
  crc.byte(static_cast<std::uint8_t>(marker.rollback_source));
  return crc.finish();
}

bool migrationMarkerValid(const MigrationMarker& marker) {
  return marker.schema_version == kMigrationMarkerSchemaVersion &&
      marker.source_layout_schema_version != 0U && marker.generation != 0U &&
      fingerprintValid(marker.source_fingerprint) && phaseValid(marker.phase) &&
      slotValid(marker.target_slot) &&
      rollbackSourceValid(marker.rollback_source) &&
      !rollbackAliasesTarget(marker) &&
      marker.checksum == migrationMarkerChecksum(marker);
}

UpgradeRecoveryPlan planUpgradeRecovery(
    const UpgradeRecoveryEvidence& evidence) {
  if (evidence.evidence_schema_version !=
      kUpgradePlannerEvidenceSchemaVersion) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     UpgradeRecoveryReason::UnsupportedEvidenceSchema);
  }

  switch (evidence.audit.result) {
    case UpgradeAuditResult::DisplayResolutionRequired:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::DisplayResolutionRequired);
    case UpgradeAuditResult::Ambiguous:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::AuditAmbiguous);
    case UpgradeAuditResult::SourceUnavailable:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::SourceUnavailable);
    case UpgradeAuditResult::Fresh:
    case UpgradeAuditResult::Compatible:
    case UpgradeAuditResult::RecoveryRequired:
      break;
    default:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::InvalidEvidence);
  }
  if (!auditReportCoherent(evidence.audit)) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     UpgradeRecoveryReason::InvalidEvidence);
  }

  if (evidence.source_layout == UpgradeSourceLayout::Unsupported) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     UpgradeRecoveryReason::UnsupportedSourceSchema);
  }
  if (evidence.source_layout == UpgradeSourceLayout::Unknown) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     UpgradeRecoveryReason::InvalidEvidence);
  }

  if (evidence.audit.result == UpgradeAuditResult::Fresh) {
    if (evidence.source_layout != UpgradeSourceLayout::Fresh)
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::InvalidEvidence);
    if (evidence.source_layout_schema_version != 0U ||
        fingerprintValid(evidence.source_fingerprint)) {
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::InvalidEvidence);
    }
    if (evidence.marker_probe == MigrationMarkerProbe::Missing) {
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::FreshNoMigration);
    }
    if (evidence.marker_probe == MigrationMarkerProbe::IoError) {
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::MarkerUnavailable);
    }
    return basicPlan(UpgradeRecoveryDecision::ReadOnlyRecovery,
                     evidence.marker_probe == MigrationMarkerProbe::Valid
                         ? UpgradeRecoveryReason::OrphanedMarker
                         : UpgradeRecoveryReason::CorruptMarker);
  }

  if (evidence.source_layout == UpgradeSourceLayout::Fresh ||
      !supportedSource(evidence)) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     evidence.source_layout == UpgradeSourceLayout::Fresh
                         ? UpgradeRecoveryReason::InvalidEvidence
                         : UpgradeRecoveryReason::UnsupportedSourceSchema);
  }
  for (const MigrationSlotEvidence& slot : evidence.native_slots) {
    if (!validSlotProbe(slot.probe)) {
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::InvalidEvidence);
    }
  }

  switch (evidence.marker_probe) {
    case MigrationMarkerProbe::Missing: {
      if (evidence.audit.result == UpgradeAuditResult::RecoveryRequired) {
        return basicPlan(UpgradeRecoveryDecision::ReadOnlyRecovery,
                         UpgradeRecoveryReason::AuditRecoveryWithoutMarker);
      }
      if (evidence.source_layout == UpgradeSourceLayout::Native) {
        return basicPlan(UpgradeRecoveryDecision::Refuse,
                         UpgradeRecoveryReason::NativeNoMigration);
      }
      if (!evidence.legacy_source.durable ||
          evidence.legacy_source.fingerprint != evidence.source_fingerprint ||
          evidence.native_slots[0].probe != MigrationSlotProbe::Missing ||
          evidence.native_slots[1].probe != MigrationSlotProbe::Missing) {
        return basicPlan(UpgradeRecoveryDecision::ReadOnlyRecovery,
                         UpgradeRecoveryReason::TargetAmbiguous);
      }
      UpgradeRecoveryPlan plan =
          basicPlan(UpgradeRecoveryDecision::Start,
                    UpgradeRecoveryReason::EligibleLegacy);
      plan.generation = 1U;
      plan.phase = MigrationPhase::None;
      plan.next_phase = MigrationPhase::Prepared;
      plan.target_slot = MigrationSlot::SlotA;
      plan.rollback_source = MigrationRollbackSource::LegacySnapshot;
      return plan;
    }
    case MigrationMarkerProbe::Torn:
    case MigrationMarkerProbe::Corrupt:
      return basicPlan(UpgradeRecoveryDecision::ReadOnlyRecovery,
                       UpgradeRecoveryReason::CorruptMarker);
    case MigrationMarkerProbe::IoError:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::MarkerUnavailable);
    case MigrationMarkerProbe::Valid:
      break;
    default:
      return basicPlan(UpgradeRecoveryDecision::Refuse,
                       UpgradeRecoveryReason::InvalidEvidence);
  }

  if (evidence.marker.schema_version != kMigrationMarkerSchemaVersion) {
    return basicPlan(UpgradeRecoveryDecision::Refuse,
                     UpgradeRecoveryReason::UnsupportedMarkerSchema);
  }
  if (!migrationMarkerValid(evidence.marker)) {
    return basicPlan(UpgradeRecoveryDecision::ReadOnlyRecovery,
                     UpgradeRecoveryReason::CorruptMarker);
  }
  if (evidence.marker.source_layout_schema_version !=
          evidence.source_layout_schema_version ||
      evidence.marker.source_fingerprint != evidence.source_fingerprint) {
    return markerPlan(evidence.marker,
                      UpgradeRecoveryDecision::ReadOnlyRecovery,
                      UpgradeRecoveryReason::SourceFingerprintChanged,
                      evidence.marker.phase);
  }

  const MigrationSlotEvidence* target =
      slotEvidence(evidence, evidence.marker.target_slot);
  if (!target) {
    return markerPlan(evidence.marker,
                      UpgradeRecoveryDecision::ReadOnlyRecovery,
                      UpgradeRecoveryReason::CorruptMarker,
                      evidence.marker.phase);
  }
  if (!markerSlotSetCoherent(evidence, evidence.marker)) {
    return markerPlan(evidence.marker,
                      UpgradeRecoveryDecision::ReadOnlyRecovery,
                      UpgradeRecoveryReason::TargetAmbiguous,
                      evidence.marker.phase);
  }
  const bool target_matches = slotMatchesTarget(*target, evidence.marker);
  const bool target_missing = target->probe == MigrationSlotProbe::Missing;

  if (evidence.marker.phase != MigrationPhase::Complete &&
      !rollbackSourceDurable(evidence, evidence.marker)) {
    return markerPlan(evidence.marker,
                      UpgradeRecoveryDecision::ReadOnlyRecovery,
                      UpgradeRecoveryReason::RollbackSourceUnavailable,
                      MigrationPhase::RollbackRequired);
  }

  switch (evidence.marker.phase) {
    case MigrationPhase::Prepared:
      if (!target_missing && !target_matches) {
        return markerPlan(evidence.marker,
                          UpgradeRecoveryDecision::ReadOnlyRecovery,
                          UpgradeRecoveryReason::TargetAmbiguous,
                          MigrationPhase::Prepared);
      }
      return markerPlan(evidence.marker, UpgradeRecoveryDecision::Resume,
                        UpgradeRecoveryReason::MigrationInProgress,
                        MigrationPhase::TargetWritten);
    case MigrationPhase::TargetWritten:
      if (target_missing) {
        return rollbackOrRecover(evidence,
                                 UpgradeRecoveryReason::TargetLostAfterWrite);
      }
      if (!target_matches) {
        return markerPlan(evidence.marker,
                          UpgradeRecoveryDecision::ReadOnlyRecovery,
                          UpgradeRecoveryReason::TargetAmbiguous,
                          MigrationPhase::RollbackRequired);
      }
      return markerPlan(evidence.marker, UpgradeRecoveryDecision::Resume,
                        UpgradeRecoveryReason::MigrationInProgress,
                        MigrationPhase::TargetVerified);
    case MigrationPhase::TargetVerified:
      if (target_missing) {
        return rollbackOrRecover(evidence,
                                 UpgradeRecoveryReason::TargetLostAfterWrite);
      }
      if (!target_matches) {
        return markerPlan(evidence.marker,
                          UpgradeRecoveryDecision::ReadOnlyRecovery,
                          UpgradeRecoveryReason::TargetAmbiguous,
                          MigrationPhase::RollbackRequired);
      }
      return markerPlan(evidence.marker, UpgradeRecoveryDecision::Resume,
                        UpgradeRecoveryReason::MigrationInProgress,
                        MigrationPhase::CommitRecorded);
    case MigrationPhase::CommitRecorded:
      if (target_missing) {
        return rollbackOrRecover(evidence,
                                 UpgradeRecoveryReason::TargetLostAfterWrite);
      }
      if (!target_matches) {
        return markerPlan(evidence.marker,
                          UpgradeRecoveryDecision::ReadOnlyRecovery,
                          UpgradeRecoveryReason::TargetAmbiguous,
                          MigrationPhase::RollbackRequired);
      }
      return markerPlan(evidence.marker, UpgradeRecoveryDecision::Resume,
                        UpgradeRecoveryReason::MigrationInProgress,
                        MigrationPhase::Complete);
    case MigrationPhase::RollbackRequired:
      if (!target_missing && !target_matches) {
        return markerPlan(evidence.marker,
                          UpgradeRecoveryDecision::ReadOnlyRecovery,
                          UpgradeRecoveryReason::TargetAmbiguous,
                          MigrationPhase::RollbackRequired);
      }
      return rollbackOrRecover(evidence,
                               UpgradeRecoveryReason::MarkerRequestsRollback);
    case MigrationPhase::Complete:
      if (evidence.source_layout == UpgradeSourceLayout::Native &&
          target_matches) {
        return markerPlan(evidence.marker, UpgradeRecoveryDecision::Refuse,
                          UpgradeRecoveryReason::NativeNoMigration,
                          MigrationPhase::Complete);
      }
      return markerPlan(evidence.marker,
                        UpgradeRecoveryDecision::ReadOnlyRecovery,
                        UpgradeRecoveryReason::OrphanedMarker,
                        MigrationPhase::Complete);
    case MigrationPhase::None:
      return markerPlan(evidence.marker,
                        UpgradeRecoveryDecision::ReadOnlyRecovery,
                        UpgradeRecoveryReason::CorruptMarker,
                        MigrationPhase::None);
  }
  return markerPlan(evidence.marker, UpgradeRecoveryDecision::ReadOnlyRecovery,
                    UpgradeRecoveryReason::CorruptMarker,
                    evidence.marker.phase);
}

const char* upgradeRecoveryDecisionName(UpgradeRecoveryDecision decision) {
  switch (decision) {
    case UpgradeRecoveryDecision::Start: return "START";
    case UpgradeRecoveryDecision::Resume: return "RESUME";
    case UpgradeRecoveryDecision::Rollback: return "ROLLBACK";
    case UpgradeRecoveryDecision::ReadOnlyRecovery:
      return "READ_ONLY_RECOVERY";
    case UpgradeRecoveryDecision::Refuse: return "REFUSE";
  }
  return "UNKNOWN";
}

const char* upgradeRecoveryReasonName(UpgradeRecoveryReason reason) {
  switch (reason) {
    case UpgradeRecoveryReason::EligibleLegacy: return "ELIGIBLE_LEGACY";
    case UpgradeRecoveryReason::MigrationInProgress:
      return "MIGRATION_IN_PROGRESS";
    case UpgradeRecoveryReason::MarkerRequestsRollback:
      return "MARKER_REQUESTS_ROLLBACK";
    case UpgradeRecoveryReason::TargetLostAfterWrite:
      return "TARGET_LOST_AFTER_WRITE";
    case UpgradeRecoveryReason::FreshNoMigration: return "FRESH_NO_MIGRATION";
    case UpgradeRecoveryReason::NativeNoMigration:
      return "NATIVE_NO_MIGRATION";
    case UpgradeRecoveryReason::DisplayResolutionRequired:
      return "DISPLAY_RESOLUTION_REQUIRED";
    case UpgradeRecoveryReason::AuditAmbiguous: return "AUDIT_AMBIGUOUS";
    case UpgradeRecoveryReason::AuditRecoveryWithoutMarker:
      return "AUDIT_RECOVERY_WITHOUT_MARKER";
    case UpgradeRecoveryReason::SourceUnavailable:
      return "SOURCE_UNAVAILABLE";
    case UpgradeRecoveryReason::UnsupportedEvidenceSchema:
      return "UNSUPPORTED_EVIDENCE_SCHEMA";
    case UpgradeRecoveryReason::UnsupportedSourceSchema:
      return "UNSUPPORTED_SOURCE_SCHEMA";
    case UpgradeRecoveryReason::UnsupportedMarkerSchema:
      return "UNSUPPORTED_MARKER_SCHEMA";
    case UpgradeRecoveryReason::CorruptMarker: return "CORRUPT_MARKER";
    case UpgradeRecoveryReason::MarkerUnavailable:
      return "MARKER_UNAVAILABLE";
    case UpgradeRecoveryReason::SourceFingerprintChanged:
      return "SOURCE_FINGERPRINT_CHANGED";
    case UpgradeRecoveryReason::OrphanedMarker: return "ORPHANED_MARKER";
    case UpgradeRecoveryReason::TargetAmbiguous: return "TARGET_AMBIGUOUS";
    case UpgradeRecoveryReason::RollbackSourceUnavailable:
      return "ROLLBACK_SOURCE_UNAVAILABLE";
    case UpgradeRecoveryReason::InvalidEvidence: return "INVALID_EVIDENCE";
  }
  return "UNKNOWN";
}

const char* migrationPhaseName(MigrationPhase phase) {
  switch (phase) {
    case MigrationPhase::None: return "NONE";
    case MigrationPhase::Prepared: return "PREPARED";
    case MigrationPhase::TargetWritten: return "TARGET_WRITTEN";
    case MigrationPhase::TargetVerified: return "TARGET_VERIFIED";
    case MigrationPhase::CommitRecorded: return "COMMIT_RECORDED";
    case MigrationPhase::RollbackRequired: return "ROLLBACK_REQUIRED";
    case MigrationPhase::Complete: return "COMPLETE";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
