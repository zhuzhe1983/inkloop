#include "inkloop/storage/upgrade_executor.hpp"

#include <algorithm>
#include <limits>

#include "inkloop/storage/sha256.hpp"

namespace inkloop {
namespace storage {
namespace {

bool fingerprintPresent(const MigrationFingerprint& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0U; });
}

bool markerEqual(const MigrationMarker& left,
                 const MigrationMarker& right) {
  return left.schema_version == right.schema_version &&
      left.source_layout_schema_version ==
          right.source_layout_schema_version &&
      left.generation == right.generation &&
      left.source_fingerprint == right.source_fingerprint &&
      left.phase == right.phase && left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source &&
      left.checksum == right.checksum;
}

bool planEqual(const UpgradeRecoveryPlan& left,
               const UpgradeRecoveryPlan& right) {
  return left.decision == right.decision && left.reason == right.reason &&
      left.generation == right.generation && left.phase == right.phase &&
      left.next_phase == right.next_phase &&
      left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source;
}

bool groupValid(UpgradeLogicalGroup group) {
  return group == UpgradeLogicalGroup::ProtectedSnapshotV1;
}

const FingerprintedUpgradeRecord* expectedRecord(
    const UpgradeEvidenceSnapshot& snapshot, UpgradeRecordId record) {
  if (!upgradeRecordIdValid(record)) return nullptr;
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? &snapshot.nvs[record.index]
      : &snapshot.files[record.index];
}

class FingerprintSink : public IUpgradeByteSink {
 public:
  explicit FingerprintSink(std::uint64_t maximum) : maximum_(maximum) {}

  bool write(const std::uint8_t* bytes, std::size_t length) override {
    if (failed_ || (!bytes && length != 0U) ||
        length > std::numeric_limits<std::uint64_t>::max() - bytes_ ||
        bytes_ + static_cast<std::uint64_t>(length) > maximum_ ||
        !hash_.update(bytes, length)) {
      failed_ = true;
      return false;
    }
    bytes_ += static_cast<std::uint64_t>(length);
    return true;
  }

  bool finish(FingerprintedUpgradeRecord& output) {
    output.logical_bytes = bytes_;
    return !failed_ && hash_.finish(output.content_fingerprint);
  }

  bool failed() const { return failed_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  Sha256 hash_;
  std::uint64_t maximum_ = 0U;
  std::uint64_t bytes_ = 0U;
  bool failed_ = false;
};

class TargetTeeSink final : public FingerprintSink {
 public:
  TargetTeeSink(std::uint64_t maximum,
                IUpgradeMigrationTargetStore& target)
      : FingerprintSink(maximum), target_(target) {}

  bool write(const std::uint8_t* bytes, std::size_t length) override {
    if (!FingerprintSink::write(bytes, length)) return false;
    if (target_.writeTargetBytes(bytes, length) !=
        UpgradeTargetStoreCode::Ok) {
      target_failed_ = true;
      return false;
    }
    return true;
  }

  bool targetFailed() const { return target_failed_; }

 private:
  IUpgradeMigrationTargetStore& target_;
  bool target_failed_ = false;
};

bool presentProbe(RecordProbe probe) {
  return probe == RecordProbe::Valid || probe == RecordProbe::Recoverable;
}

UpgradeExecutorCode observedSourceRecord(
    UpgradeRecordStreamCode code, FingerprintSink& sink,
    FingerprintedUpgradeRecord& output) {
  output = FingerprintedUpgradeRecord{};
  if (sink.failed() || code == UpgradeRecordStreamCode::TooLarge)
    return UpgradeExecutorCode::TooLarge;
  if (code == UpgradeRecordStreamCode::IoError)
    return UpgradeExecutorCode::SourceReadError;
  if (code == UpgradeRecordStreamCode::Missing) {
    if (sink.bytes() != 0U) return UpgradeExecutorCode::SourceChanged;
    output.probe = RecordProbe::Missing;
    return UpgradeExecutorCode::Ok;
  }
  if (code == UpgradeRecordStreamCode::Valid)
    output.probe = RecordProbe::Valid;
  else if (code == UpgradeRecordStreamCode::Recoverable)
    output.probe = RecordProbe::Recoverable;
  else
    return UpgradeExecutorCode::SourceChanged;
  return sink.finish(output) ? UpgradeExecutorCode::Ok
                             : UpgradeExecutorCode::SourceReadError;
}

bool sourceRecordEqual(const FingerprintedUpgradeRecord& expected,
                       const FingerprintedUpgradeRecord& observed) {
  return expected.probe == observed.probe &&
      expected.logical_bytes == observed.logical_bytes &&
      expected.content_fingerprint == observed.content_fingerprint;
}

bool targetEvidenceMatches(const MigrationSlotEvidence& target,
                           const MigrationMarker& marker) {
  return target.probe == MigrationSlotProbe::Valid &&
      target.migration_generation == marker.generation &&
      target.source_fingerprint == marker.source_fingerprint;
}

UpgradeExecutorCode verifyOneTargetRecord(
    const UpgradeEvidenceSnapshot& snapshot,
    const UpgradeExecutionRequest& request,
    IUpgradeMigrationTargetStore& target, UpgradeRecordId record) {
  const FingerprintedUpgradeRecord* expected =
      expectedRecord(snapshot, record);
  if (!expected) return UpgradeExecutorCode::InvalidArgument;
  const std::uint64_t maximum = upgradeRecordMaximumBytes(record);
  FingerprintSink sink(maximum);
  const UpgradeRecordStreamCode code = target.streamTargetRecord(
      request.plan.target_slot, request.group, record, maximum, sink);
  if (code == UpgradeRecordStreamCode::IoError)
    return UpgradeExecutorCode::TargetReadError;
  if (code == UpgradeRecordStreamCode::TooLarge || sink.failed())
    return UpgradeExecutorCode::TargetMismatch;
  if (expected->probe == RecordProbe::Missing)
    return code == UpgradeRecordStreamCode::Missing && sink.bytes() == 0U
        ? UpgradeExecutorCode::Ok
        : UpgradeExecutorCode::TargetMismatch;
  if (!presentProbe(expected->probe) || code != UpgradeRecordStreamCode::Valid)
    return UpgradeExecutorCode::TargetMismatch;
  FingerprintedUpgradeRecord observed;
  if (!sink.finish(observed)) return UpgradeExecutorCode::TargetReadError;
  return observed.logical_bytes == expected->logical_bytes &&
          observed.content_fingerprint == expected->content_fingerprint
      ? UpgradeExecutorCode::Ok
      : UpgradeExecutorCode::TargetMismatch;
}

template <typename Function>
UpgradeExecutorCode eachRecord(Function function) {
  for (std::size_t at = 0U; at < kProtectedNvsNamespaces.size(); ++at) {
    const UpgradeExecutorCode code =
        function(UpgradeRecordId{UpgradeRecordDomain::NvsNamespace, at});
    if (code != UpgradeExecutorCode::Ok) return code;
  }
  for (std::size_t at = 0U; at < kProtectedFilePaths.size(); ++at) {
    const UpgradeExecutorCode code =
        function(UpgradeRecordId{UpgradeRecordDomain::File, at});
    if (code != UpgradeExecutorCode::Ok) return code;
  }
  return UpgradeExecutorCode::Ok;
}

UpgradeExecutorCode verifyTargetGroup(
    const CollectedUpgradeRecovery& collected,
    const UpgradeExecutionRequest& request,
    IUpgradeMigrationTargetStore& target) {
  MigrationSlotEvidence inspected;
  if (target.inspectTargetGroup(request.plan.target_slot, request.group,
                                inspected) != UpgradeTargetStoreCode::Ok)
    return UpgradeExecutorCode::TargetReadError;
  const MigrationMarker& marker = request.marker_present
      ? request.marker
      : collected.evidence.marker;
  MigrationMarker identity = marker;
  if (!request.marker_present) {
    identity.source_layout_schema_version =
        collected.evidence.source_layout_schema_version;
    identity.generation = request.plan.generation;
    identity.source_fingerprint = request.source_fingerprint;
    identity.phase = MigrationPhase::Prepared;
    identity.target_slot = request.plan.target_slot;
    identity.rollback_source = request.plan.rollback_source;
    identity.checksum = migrationMarkerChecksum(identity);
  }
  if (!targetEvidenceMatches(inspected, identity))
    return UpgradeExecutorCode::TargetMismatch;
  return eachRecord([&](UpgradeRecordId record) {
    return verifyOneTargetRecord(collected.snapshot, request, target, record);
  });
}

UpgradeExecutorCode stageTargetGroup(
    const CollectedUpgradeRecovery& collected,
    const UpgradeExecutionRequest& request,
    const IUpgradeSnapshotSource& source,
    IUpgradeMigrationTargetStore& target) {
  // Re-check the target through the target owner immediately before staging;
  // the double-read source snapshot is not used as a lease.
  MigrationSlotEvidence before;
  if (target.inspectTargetGroup(request.plan.target_slot, request.group,
                                before) != UpgradeTargetStoreCode::Ok)
    return UpgradeExecutorCode::TargetReadError;
  if (before.probe != MigrationSlotProbe::Missing ||
      before.migration_generation != 0U ||
      fingerprintPresent(before.source_fingerprint))
    return UpgradeExecutorCode::UnsafeTarget;
  if (target.beginTargetGroup(request.plan.target_slot, request.group,
                              request.plan.generation,
                              request.source_fingerprint) !=
      UpgradeTargetStoreCode::Ok)
    return UpgradeExecutorCode::TargetWriteError;

  const UpgradeExecutorCode staged = eachRecord([&](UpgradeRecordId record) {
    const FingerprintedUpgradeRecord* expected =
        expectedRecord(collected.snapshot, record);
    if (!expected || (!presentProbe(expected->probe) &&
                      expected->probe != RecordProbe::Missing))
      return UpgradeExecutorCode::SourceChanged;
    if (target.beginTargetRecord(record, presentProbe(expected->probe)) !=
        UpgradeTargetStoreCode::Ok)
      return UpgradeExecutorCode::TargetWriteError;
    const std::uint64_t maximum = upgradeRecordMaximumBytes(record);
    TargetTeeSink sink(maximum, target);
    const UpgradeRecordStreamCode streamed =
        source.streamRecord(record, maximum, sink);
    if (sink.targetFailed()) return UpgradeExecutorCode::TargetWriteError;
    FingerprintedUpgradeRecord observed;
    const UpgradeExecutorCode observed_code =
        observedSourceRecord(streamed, sink, observed);
    if (observed_code != UpgradeExecutorCode::Ok) return observed_code;
    if (!sourceRecordEqual(*expected, observed))
      return UpgradeExecutorCode::SourceChanged;
    if (target.finishTargetRecord() != UpgradeTargetStoreCode::Ok)
      return UpgradeExecutorCode::TargetWriteError;
    return UpgradeExecutorCode::Ok;
  });
  if (staged != UpgradeExecutorCode::Ok) return staged;
  if (target.commitTargetGroup() != UpgradeTargetStoreCode::Ok)
    return UpgradeExecutorCode::TargetCommitError;
  return verifyTargetGroup(collected, request, target);
}

MigrationSlotEvidence targetSlotEvidence(
    const UpgradeRecoveryEvidence& evidence, MigrationSlot slot) {
  if (slot == MigrationSlot::SlotA) return evidence.native_slots[0];
  if (slot == MigrationSlot::SlotB) return evidence.native_slots[1];
  return MigrationSlotEvidence{};
}

bool authorityIsTarget(const UpgradeAuthority& authority,
                       const UpgradeExecutionRequest& request) {
  const UpgradeAuthorityKind target =
      request.plan.target_slot == MigrationSlot::SlotA
      ? UpgradeAuthorityKind::NativeSlotA
      : UpgradeAuthorityKind::NativeSlotB;
  return authority.kind == target &&
      authority.generation == request.plan.generation &&
      authority.source_fingerprint == request.source_fingerprint;
}

bool authorityIsRollback(const UpgradeAuthority& authority,
                         const UpgradeExecutionRequest& request,
                         const UpgradeRecoveryEvidence& evidence) {
  if (authority.source_fingerprint != request.source_fingerprint) return false;
  switch (request.plan.rollback_source) {
    case MigrationRollbackSource::LegacySnapshot:
      return authority.kind == UpgradeAuthorityKind::LegacySnapshot &&
          authority.generation == 0U && evidence.legacy_source.durable;
    case MigrationRollbackSource::NativeSlotA:
      return authority.kind == UpgradeAuthorityKind::NativeSlotA &&
          evidence.native_slots[0].probe == MigrationSlotProbe::Valid &&
          authority.generation ==
              evidence.native_slots[0].migration_generation;
    case MigrationRollbackSource::NativeSlotB:
      return authority.kind == UpgradeAuthorityKind::NativeSlotB &&
          evidence.native_slots[1].probe == MigrationSlotProbe::Valid &&
          authority.generation ==
              evidence.native_slots[1].migration_generation;
    case MigrationRollbackSource::None:
      return false;
  }
  return false;
}

UpgradeExecutorCode mapCollector(UpgradeSnapshotCollectCode code) {
  switch (code) {
    case UpgradeSnapshotCollectCode::Ok: return UpgradeExecutorCode::Ok;
    case UpgradeSnapshotCollectCode::Changed:
      return UpgradeExecutorCode::EvidenceChanged;
    case UpgradeSnapshotCollectCode::Ambiguous:
    case UpgradeSnapshotCollectCode::InvalidEvidence:
      return UpgradeExecutorCode::EvidenceAmbiguous;
    case UpgradeSnapshotCollectCode::IoError:
      return UpgradeExecutorCode::EvidenceIoError;
    case UpgradeSnapshotCollectCode::UnsupportedSchema:
      return UpgradeExecutorCode::UnsupportedSchema;
    case UpgradeSnapshotCollectCode::TooLarge:
      return UpgradeExecutorCode::TooLarge;
  }
  return UpgradeExecutorCode::EvidenceAmbiguous;
}

bool requestMatches(const UpgradeExecutionRequest& request,
                    const CollectedUpgradeRecovery& collected) {
  if (!groupValid(request.group) || !planEqual(request.plan, collected.plan) ||
      request.source_fingerprint != collected.evidence.source_fingerprint ||
      !fingerprintPresent(request.source_fingerprint) ||
      request.marker_sequence != collected.snapshot.marker_journal.sequence)
    return false;
  const bool marker_present = collected.snapshot.marker_journal.probe ==
      MigrationMarkerJournalProbe::Valid;
  if (request.marker_present != marker_present) return false;
  return !marker_present || markerEqual(request.marker,
                                        collected.evidence.marker);
}

UpgradeExecutorCode commitPhase(
    const CollectedUpgradeRecovery& collected,
    const UpgradeExecutionRequest& request, MigrationPhase next_phase,
    IMigrationMarkerJournalStore& journal,
    UpgradeExecutionOutcome& outcome) {
  MigrationMarker next;
  if (request.marker_present) {
    next = request.marker;
  } else {
    next.source_layout_schema_version =
        collected.evidence.source_layout_schema_version;
    next.generation = request.plan.generation;
    next.source_fingerprint = request.source_fingerprint;
    next.target_slot = request.plan.target_slot;
    next.rollback_source = request.plan.rollback_source;
  }
  next.phase = next_phase;
  next.checksum = migrationMarkerChecksum(next);
  if (!migrationMarkerValid(next)) return UpgradeExecutorCode::InvalidArgument;
  MigrationMarkerJournalCore core(journal);
  MigrationMarkerJournalInspection committed;
  if (core.commit(next, request.marker_sequence, committed) !=
      MigrationMarkerJournalCode::Ok)
    return UpgradeExecutorCode::MarkerError;
  outcome.durable_phase = committed.marker.phase;
  outcome.marker_sequence = committed.sequence;
  return UpgradeExecutorCode::Ok;
}

}  // namespace

const char* upgradeLogicalGroupName(UpgradeLogicalGroup group) {
  return group == UpgradeLogicalGroup::ProtectedSnapshotV1
      ? "protected-upgrade-snapshot-v1"
      : "unknown";
}

bool bindUpgradeExecutionRequest(
    const CollectedUpgradeRecovery& collected, UpgradeLogicalGroup group,
    UpgradeExecutionRequest& output) {
  output = UpgradeExecutionRequest{};
  if (!groupValid(group) || !collected.plan.authorizesMutation() ||
      !fingerprintPresent(collected.evidence.source_fingerprint))
    return false;
  output.plan = collected.plan;
  output.source_fingerprint = collected.evidence.source_fingerprint;
  output.marker_sequence = collected.snapshot.marker_journal.sequence;
  output.marker_present = collected.snapshot.marker_journal.probe ==
      MigrationMarkerJournalProbe::Valid;
  if (output.marker_present) output.marker = collected.evidence.marker;
  output.group = group;
  return true;
}

UpgradeExecutorCode UpgradeExecutorCore::execute(
    const UpgradeExecutionRequest& request,
    UpgradeExecutionOutcome& outcome) {
  outcome = UpgradeExecutionOutcome{};
  UpgradeSnapshotCollector collector(source_);
  CollectedUpgradeRecovery collected;
  const UpgradeExecutorCode collection_code =
      mapCollector(collector.collect(collected));
  if (collection_code != UpgradeExecutorCode::Ok) {
    outcome.code = collection_code;
    return outcome.code;
  }
  if (!collected.plan.authorizesMutation()) {
    outcome.code = UpgradeExecutorCode::NotAuthorized;
    return outcome.code;
  }
  if (!requestMatches(request, collected)) {
    outcome.code = UpgradeExecutorCode::PlanMismatch;
    return outcome.code;
  }

  UpgradeAuthority authority;
  if (authority_.inspectAuthority(authority) != UpgradeAuthorityCode::Ok) {
    outcome.code = UpgradeExecutorCode::UnsafeAuthority;
    return outcome.code;
  }
  const bool is_rollback = authorityIsRollback(
      authority, request, collected.evidence);
  const bool is_target = authorityIsTarget(authority, request);
  const MigrationSlotEvidence target_evidence = targetSlotEvidence(
      collected.evidence, request.plan.target_slot);

  if (request.plan.decision == UpgradeRecoveryDecision::Start) {
    if (request.marker_present || request.plan.phase != MigrationPhase::None ||
        request.plan.next_phase != MigrationPhase::Prepared || !is_rollback ||
        target_evidence.probe != MigrationSlotProbe::Missing) {
      outcome.code = UpgradeExecutorCode::UnsafeTarget;
      return outcome.code;
    }
    outcome.code = commitPhase(collected, request, MigrationPhase::Prepared,
                               journal_, outcome);
    return outcome.code;
  }

  if (!request.marker_present ||
      request.marker.generation != request.plan.generation ||
      request.marker.source_fingerprint != request.source_fingerprint) {
    outcome.code = UpgradeExecutorCode::PlanMismatch;
    return outcome.code;
  }

  if (request.plan.decision == UpgradeRecoveryDecision::Rollback) {
    if (request.plan.next_phase != MigrationPhase::RollbackRequired) {
      outcome.code = UpgradeExecutorCode::PlanMismatch;
      return outcome.code;
    }
    if (request.plan.phase != MigrationPhase::RollbackRequired) {
      outcome.code = commitPhase(collected, request,
                                 MigrationPhase::RollbackRequired,
                                 journal_, outcome);
      return outcome.code;
    }
    if (!is_rollback && !is_target) {
      outcome.code = UpgradeExecutorCode::UnsafeAuthority;
      return outcome.code;
    }
    if (is_target) {
      if (authority_.activateRollback(request.plan.rollback_source,
                                      request.source_fingerprint) !=
          UpgradeAuthorityCode::Ok) {
        outcome.code = UpgradeExecutorCode::AuthoritySwitchError;
        return outcome.code;
      }
      UpgradeAuthority verified;
      if (authority_.inspectAuthority(verified) != UpgradeAuthorityCode::Ok ||
          !authorityIsRollback(verified, request, collected.evidence)) {
        outcome.code = UpgradeExecutorCode::AuthoritySwitchError;
        return outcome.code;
      }
      outcome.authority_switched = true;
    }
    outcome.durable_phase = MigrationPhase::RollbackRequired;
    outcome.marker_sequence = request.marker_sequence;
    outcome.code = UpgradeExecutorCode::RollbackAppliedAwaitingTerminal;
    return outcome.code;
  }

  if (request.plan.decision != UpgradeRecoveryDecision::Resume) {
    outcome.code = UpgradeExecutorCode::NotAuthorized;
    return outcome.code;
  }

  switch (request.plan.phase) {
    case MigrationPhase::Prepared: {
      if (!is_rollback || request.plan.next_phase !=
              MigrationPhase::TargetWritten) {
        outcome.code = UpgradeExecutorCode::UnsafeAuthority;
        return outcome.code;
      }
      if (target_evidence.probe == MigrationSlotProbe::Missing) {
        outcome.code = stageTargetGroup(collected, request, source_, target_);
        if (outcome.code != UpgradeExecutorCode::Ok) return outcome.code;
        outcome.target_group_committed = true;
      } else if (targetEvidenceMatches(target_evidence, request.marker)) {
        outcome.code = verifyTargetGroup(collected, request, target_);
        if (outcome.code != UpgradeExecutorCode::Ok) return outcome.code;
      } else {
        outcome.code = UpgradeExecutorCode::UnsafeTarget;
        return outcome.code;
      }
      outcome.code = commitPhase(collected, request,
                                 MigrationPhase::TargetWritten,
                                 journal_, outcome);
      return outcome.code;
    }
    case MigrationPhase::TargetWritten:
      if (!is_rollback || request.plan.next_phase !=
              MigrationPhase::TargetVerified ||
          !targetEvidenceMatches(target_evidence, request.marker)) {
        outcome.code = UpgradeExecutorCode::UnsafeTarget;
        return outcome.code;
      }
      outcome.code = verifyTargetGroup(collected, request, target_);
      if (outcome.code == UpgradeExecutorCode::Ok)
        outcome.code = commitPhase(collected, request,
                                   MigrationPhase::TargetVerified,
                                   journal_, outcome);
      return outcome.code;
    case MigrationPhase::TargetVerified:
      if (request.plan.next_phase != MigrationPhase::CommitRecorded ||
          !targetEvidenceMatches(target_evidence, request.marker)) {
        outcome.code = UpgradeExecutorCode::UnsafeTarget;
        return outcome.code;
      }
      outcome.code = verifyTargetGroup(collected, request, target_);
      if (outcome.code != UpgradeExecutorCode::Ok) return outcome.code;
      if (!is_rollback && !is_target) {
        outcome.code = UpgradeExecutorCode::UnsafeAuthority;
        return outcome.code;
      }
      if (is_rollback) {
        if (authority_.activateTarget(request.plan.target_slot,
                                      request.plan.generation,
                                      request.source_fingerprint) !=
            UpgradeAuthorityCode::Ok) {
          outcome.code = UpgradeExecutorCode::AuthoritySwitchError;
          return outcome.code;
        }
        UpgradeAuthority verified;
        if (authority_.inspectAuthority(verified) !=
                UpgradeAuthorityCode::Ok ||
            !authorityIsTarget(verified, request)) {
          outcome.code = UpgradeExecutorCode::AuthoritySwitchError;
          return outcome.code;
        }
        outcome.authority_switched = true;
      }
      outcome.code = commitPhase(collected, request,
                                 MigrationPhase::CommitRecorded,
                                 journal_, outcome);
      return outcome.code;
    case MigrationPhase::CommitRecorded:
      if (!is_target || request.plan.next_phase != MigrationPhase::Complete ||
          !targetEvidenceMatches(target_evidence, request.marker)) {
        outcome.code = UpgradeExecutorCode::UnsafeAuthority;
        return outcome.code;
      }
      outcome.code = verifyTargetGroup(collected, request, target_);
      if (outcome.code == UpgradeExecutorCode::Ok)
        outcome.code = commitPhase(collected, request,
                                   MigrationPhase::Complete,
                                   journal_, outcome);
      return outcome.code;
    case MigrationPhase::None:
    case MigrationPhase::RollbackRequired:
    case MigrationPhase::Complete:
      outcome.code = UpgradeExecutorCode::PlanMismatch;
      return outcome.code;
  }
  outcome.code = UpgradeExecutorCode::InvalidArgument;
  return outcome.code;
}

const char* upgradeTargetStoreCodeName(UpgradeTargetStoreCode code) {
  switch (code) {
    case UpgradeTargetStoreCode::Ok: return "OK";
    case UpgradeTargetStoreCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case UpgradeTargetStoreCode::Conflict: return "CONFLICT";
    case UpgradeTargetStoreCode::IoError: return "IO_ERROR";
  }
  return "UNKNOWN";
}

const char* upgradeAuthorityCodeName(UpgradeAuthorityCode code) {
  switch (code) {
    case UpgradeAuthorityCode::Ok: return "OK";
    case UpgradeAuthorityCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case UpgradeAuthorityCode::Conflict: return "CONFLICT";
    case UpgradeAuthorityCode::IoError: return "IO_ERROR";
  }
  return "UNKNOWN";
}

const char* upgradeExecutorCodeName(UpgradeExecutorCode code) {
  switch (code) {
    case UpgradeExecutorCode::Ok: return "OK";
    case UpgradeExecutorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case UpgradeExecutorCode::EvidenceChanged: return "EVIDENCE_CHANGED";
    case UpgradeExecutorCode::EvidenceAmbiguous:
      return "EVIDENCE_AMBIGUOUS";
    case UpgradeExecutorCode::EvidenceIoError: return "EVIDENCE_IO_ERROR";
    case UpgradeExecutorCode::UnsupportedSchema:
      return "UNSUPPORTED_SCHEMA";
    case UpgradeExecutorCode::TooLarge: return "TOO_LARGE";
    case UpgradeExecutorCode::PlanMismatch: return "PLAN_MISMATCH";
    case UpgradeExecutorCode::NotAuthorized: return "NOT_AUTHORIZED";
    case UpgradeExecutorCode::UnsafeAuthority: return "UNSAFE_AUTHORITY";
    case UpgradeExecutorCode::UnsafeTarget: return "UNSAFE_TARGET";
    case UpgradeExecutorCode::SourceChanged: return "SOURCE_CHANGED";
    case UpgradeExecutorCode::SourceReadError: return "SOURCE_READ_ERROR";
    case UpgradeExecutorCode::TargetWriteError: return "TARGET_WRITE_ERROR";
    case UpgradeExecutorCode::TargetCommitError:
      return "TARGET_COMMIT_ERROR";
    case UpgradeExecutorCode::TargetReadError: return "TARGET_READ_ERROR";
    case UpgradeExecutorCode::TargetMismatch: return "TARGET_MISMATCH";
    case UpgradeExecutorCode::MarkerError: return "MARKER_ERROR";
    case UpgradeExecutorCode::AuthoritySwitchError:
      return "AUTHORITY_SWITCH_ERROR";
    case UpgradeExecutorCode::RollbackAppliedAwaitingTerminal:
      return "ROLLBACK_APPLIED_AWAITING_TERMINAL";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
