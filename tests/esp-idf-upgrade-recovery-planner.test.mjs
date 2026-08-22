import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "inkloop/storage/upgrade_recovery_planner.hpp"

using namespace inkloop::storage;

static UpgradeAuditReport audit(UpgradeAuditResult result) {
  UpgradeAuditReport output;
  output.result = result;
  if (result == UpgradeAuditResult::Fresh) {
    output.tasks = TransactionAudit::Empty;
    output.album = TransactionAudit::Empty;
    output.protected_records_present = 0U;
  } else {
    output.tasks = TransactionAudit::Clean;
    output.album = TransactionAudit::Clean;
    output.protected_records_present = 1U;
  }
  return output;
}

static MigrationFingerprint fingerprint(std::uint8_t seed) {
  MigrationFingerprint output{};
  for (std::size_t at = 0; at < output.size(); ++at)
    output[at] = static_cast<std::uint8_t>(seed + at);
  return output;
}

static MigrationMarker marker(MigrationPhase phase,
                              MigrationRollbackSource rollback =
                                  MigrationRollbackSource::LegacySnapshot) {
  MigrationMarker output;
  output.source_layout_schema_version =
      kSupportedLegacyLayoutSchemaVersion;
  output.generation = 7U;
  output.source_fingerprint = fingerprint(1U);
  output.phase = phase;
  output.target_slot = MigrationSlot::SlotA;
  output.rollback_source = rollback;
  output.checksum = migrationMarkerChecksum(output);
  assert(migrationMarkerValid(output));
  return output;
}

static UpgradeRecoveryEvidence legacy(MigrationPhase phase,
                                      MigrationSlotProbe target_probe) {
  UpgradeRecoveryEvidence output;
  output.audit = audit(UpgradeAuditResult::Compatible);
  output.source_layout = UpgradeSourceLayout::Legacy;
  output.source_layout_schema_version =
      kSupportedLegacyLayoutSchemaVersion;
  output.source_fingerprint = fingerprint(1U);
  output.marker_probe = MigrationMarkerProbe::Valid;
  output.marker = marker(phase);
  output.legacy_source.durable = true;
  output.legacy_source.fingerprint = fingerprint(1U);
  output.native_slots[0].probe = target_probe;
  if (target_probe == MigrationSlotProbe::Valid) {
    output.native_slots[0].migration_generation = output.marker.generation;
    output.native_slots[0].source_fingerprint = output.marker.source_fingerprint;
  }
  output.native_slots[1].probe = MigrationSlotProbe::Missing;
  return output;
}

static bool samePlan(const UpgradeRecoveryPlan& left,
                     const UpgradeRecoveryPlan& right) {
  return left.decision == right.decision && left.reason == right.reason &&
      left.generation == right.generation && left.phase == right.phase &&
      left.next_phase == right.next_phase &&
      left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source;
}

static bool rollbackSourceIsProvenDurable(
    const UpgradeRecoveryEvidence& evidence,
    const UpgradeRecoveryPlan& plan) {
  if (plan.rollback_source == MigrationRollbackSource::LegacySnapshot) {
    return evidence.legacy_source.durable &&
        evidence.legacy_source.fingerprint == evidence.marker.source_fingerprint;
  }
  std::size_t at = 0U;
  if (plan.rollback_source == MigrationRollbackSource::NativeSlotA) at = 0U;
  else if (plan.rollback_source == MigrationRollbackSource::NativeSlotB) at = 1U;
  else return false;
  const MigrationSlotEvidence& slot = evidence.native_slots[at];
  return slot.probe == MigrationSlotProbe::Valid &&
      slot.migration_generation != 0U &&
      slot.source_fingerprint == evidence.marker.source_fingerprint;
}

static void assertNonAuthorizing(const UpgradeRecoveryPlan& plan) {
  assert(!plan.authorizesMutation());
  assert(plan.decision == UpgradeRecoveryDecision::ReadOnlyRecovery ||
         plan.decision == UpgradeRecoveryDecision::Refuse);
}

int main() {
  // Fresh media and an already-native layout are explicit no-migration cases.
  UpgradeRecoveryEvidence evidence;
  evidence.audit = audit(UpgradeAuditResult::Fresh);
  evidence.source_layout = UpgradeSourceLayout::Fresh;
  UpgradeRecoveryPlan plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Refuse);
  assert(plan.reason == UpgradeRecoveryReason::FreshNoMigration);
  assert(plan.noMigrationNeeded());
  assertNonAuthorizing(plan);

  evidence = UpgradeRecoveryEvidence{};
  evidence.audit = audit(UpgradeAuditResult::Compatible);
  evidence.source_layout = UpgradeSourceLayout::Native;
  evidence.source_layout_schema_version =
      kSupportedNativeLayoutSchemaVersion;
  evidence.source_fingerprint = fingerprint(1U);
  plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Refuse);
  assert(plan.reason == UpgradeRecoveryReason::NativeNoMigration);
  assert(plan.noMigrationNeeded());
  assertNonAuthorizing(plan);

  // Only a durable eligible legacy source with two known-empty target slots
  // may begin generation 1. The first target selection is deterministic.
  evidence = UpgradeRecoveryEvidence{};
  evidence.audit = audit(UpgradeAuditResult::Compatible);
  evidence.source_layout = UpgradeSourceLayout::Legacy;
  evidence.source_layout_schema_version =
      kSupportedLegacyLayoutSchemaVersion;
  evidence.source_fingerprint = fingerprint(1U);
  evidence.legacy_source = {true, fingerprint(1U)};
  evidence.native_slots[0].probe = MigrationSlotProbe::Missing;
  evidence.native_slots[1].probe = MigrationSlotProbe::Missing;
  plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Start);
  assert(plan.authorizesMutation());
  assert(plan.generation == 1U);
  assert(plan.phase == MigrationPhase::None);
  assert(plan.next_phase == MigrationPhase::Prepared);
  assert(plan.target_slot == MigrationSlot::SlotA);
  assert(plan.rollback_source ==
         MigrationRollbackSource::LegacySnapshot);
  assert(samePlan(plan, planUpgradeRecovery(evidence)));

  UpgradeRecoveryEvidence ambiguous_start = evidence;
  ambiguous_start.native_slots[1].probe = MigrationSlotProbe::Unknown;
  plan = planUpgradeRecovery(ambiguous_start);
  assert(plan.reason == UpgradeRecoveryReason::TargetAmbiguous);
  assertNonAuthorizing(plan);

  UpgradeRecoveryEvidence incoherent_audit = evidence;
  incoherent_audit.audit.tasks = TransactionAudit::SourceUnavailable;
  assertNonAuthorizing(planUpgradeRecovery(incoherent_audit));

  // Every forward phase has one deterministic next durable checkpoint.
  struct Transition {
    MigrationPhase from;
    MigrationSlotProbe target;
    MigrationPhase to;
  };
  constexpr std::array<Transition, 5> transitions{{
      {MigrationPhase::Prepared, MigrationSlotProbe::Missing,
       MigrationPhase::TargetWritten},
      {MigrationPhase::Prepared, MigrationSlotProbe::Valid,
       MigrationPhase::TargetWritten},
      {MigrationPhase::TargetWritten, MigrationSlotProbe::Valid,
       MigrationPhase::TargetVerified},
      {MigrationPhase::TargetVerified, MigrationSlotProbe::Valid,
       MigrationPhase::CommitRecorded},
      {MigrationPhase::CommitRecorded, MigrationSlotProbe::Valid,
       MigrationPhase::Complete},
  }};
  for (const Transition& transition : transitions) {
    evidence = legacy(transition.from, transition.target);
    plan = planUpgradeRecovery(evidence);
    assert(plan.decision == UpgradeRecoveryDecision::Resume);
    assert(plan.reason == UpgradeRecoveryReason::MigrationInProgress);
    assert(plan.authorizesMutation());
    assert(plan.phase == transition.from);
    assert(plan.next_phase == transition.to);
    assert(samePlan(plan, planUpgradeRecovery(evidence)));
  }
  evidence = legacy(MigrationPhase::TargetWritten,
                    MigrationSlotProbe::Valid);
  evidence.legacy_source.durable = false;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::RollbackSourceUnavailable);
  assertNonAuthorizing(plan);
  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.native_slots[1].probe = MigrationSlotProbe::Unknown;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::TargetAmbiguous);
  assertNonAuthorizing(plan);
  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.native_slots[1].probe = MigrationSlotProbe::Valid;
  evidence.native_slots[1].migration_generation = 2U;
  evidence.native_slots[1].source_fingerprint = evidence.marker.source_fingerprint;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::TargetAmbiguous);
  assertNonAuthorizing(plan);

  // RecoveryRequired is explainable only by a valid in-progress marker.
  evidence = legacy(MigrationPhase::TargetWritten,
                    MigrationSlotProbe::Valid);
  evidence.audit = audit(UpgradeAuditResult::RecoveryRequired);
  assert(planUpgradeRecovery(evidence).decision ==
         UpgradeRecoveryDecision::Resume);
  evidence.marker_probe = MigrationMarkerProbe::Missing;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::AuditRecoveryWithoutMarker);
  assertNonAuthorizing(plan);

  // Completion is accepted as a no-op only after native layout evidence and a
  // matching durable target agree with the sealed marker.
  evidence = legacy(MigrationPhase::Complete, MigrationSlotProbe::Valid);
  evidence.source_layout = UpgradeSourceLayout::Native;
  evidence.source_layout_schema_version =
      kSupportedNativeLayoutSchemaVersion;
  plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Refuse);
  assert(plan.reason == UpgradeRecoveryReason::NativeNoMigration);
  assert(plan.phase == MigrationPhase::Complete);
  assert(plan.noMigrationNeeded());
  assertNonAuthorizing(plan);

  // Explicit rollback and loss-after-write both name the already-durable
  // legacy source. If that proof disappears, rollback is not authorized.
  for (MigrationPhase phase : {MigrationPhase::TargetWritten,
                               MigrationPhase::TargetVerified,
                               MigrationPhase::CommitRecorded}) {
    evidence = legacy(phase, MigrationSlotProbe::Missing);
    plan = planUpgradeRecovery(evidence);
    assert(plan.decision == UpgradeRecoveryDecision::Rollback);
    assert(plan.reason == UpgradeRecoveryReason::TargetLostAfterWrite);
    assert(plan.authorizesMutation());
    assert(rollbackSourceIsProvenDurable(evidence, plan));
  }
  evidence = legacy(MigrationPhase::RollbackRequired,
                    MigrationSlotProbe::Valid);
  plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Rollback);
  assert(plan.reason == UpgradeRecoveryReason::MarkerRequestsRollback);
  assert(rollbackSourceIsProvenDurable(evidence, plan));

  evidence.legacy_source.durable = false;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::RollbackSourceUnavailable);
  assertNonAuthorizing(plan);

  // A prior native slot can be the rollback source only when it is different
  // from the target and carries the source fingerprint in durable evidence.
  evidence = legacy(MigrationPhase::RollbackRequired,
                    MigrationSlotProbe::Valid);
  evidence.marker.rollback_source = MigrationRollbackSource::NativeSlotB;
  evidence.marker.checksum = migrationMarkerChecksum(evidence.marker);
  evidence.native_slots[1].probe = MigrationSlotProbe::Valid;
  evidence.native_slots[1].migration_generation = 3U;
  evidence.native_slots[1].source_fingerprint = evidence.marker.source_fingerprint;
  plan = planUpgradeRecovery(evidence);
  assert(plan.decision == UpgradeRecoveryDecision::Rollback);
  assert(rollbackSourceIsProvenDurable(evidence, plan));
  evidence.native_slots[1].source_fingerprint = fingerprint(99U);
  assertNonAuthorizing(planUpgradeRecovery(evidence));

  // Display ambiguity, audit ambiguity, unavailable sources, unsupported
  // schemas and unknown enum values never authorize a mutation.
  for (UpgradeAuditResult result : {
           UpgradeAuditResult::DisplayResolutionRequired,
           UpgradeAuditResult::Ambiguous,
           UpgradeAuditResult::SourceUnavailable}) {
    for (MigrationMarkerProbe probe : {
             MigrationMarkerProbe::Missing, MigrationMarkerProbe::Valid,
             MigrationMarkerProbe::Torn, MigrationMarkerProbe::Corrupt,
             MigrationMarkerProbe::IoError}) {
      evidence = legacy(MigrationPhase::TargetWritten,
                        MigrationSlotProbe::Valid);
      evidence.audit = audit(result);
      evidence.marker_probe = probe;
      assertNonAuthorizing(planUpgradeRecovery(evidence));
    }
  }
  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.evidence_schema_version = 2U;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::UnsupportedEvidenceSchema);
  assertNonAuthorizing(plan);

  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.source_layout_schema_version = 2U;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::UnsupportedSourceSchema);
  assertNonAuthorizing(plan);

  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.source_layout = UpgradeSourceLayout::Unsupported;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::UnsupportedSourceSchema);
  assertNonAuthorizing(plan);

  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.source_layout = static_cast<UpgradeSourceLayout>(0xFEU);
  assertNonAuthorizing(planUpgradeRecovery(evidence));
  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.audit.result = static_cast<UpgradeAuditResult>(0xFEU);
  assertNonAuthorizing(planUpgradeRecovery(evidence));

  // Torn and corrupt markers, unsupported marker versions, changed sources,
  // and orphaned markers are all fail-closed.
  for (MigrationMarkerProbe probe : {MigrationMarkerProbe::Torn,
                                     MigrationMarkerProbe::Corrupt}) {
    evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
    evidence.marker_probe = probe;
    plan = planUpgradeRecovery(evidence);
    assert(plan.reason == UpgradeRecoveryReason::CorruptMarker);
    assertNonAuthorizing(plan);
  }
  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.marker.checksum ^= 1U;
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::CorruptMarker);
  assertNonAuthorizing(plan);

  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.marker.schema_version = 2U;
  evidence.marker.checksum = migrationMarkerChecksum(evidence.marker);
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::UnsupportedMarkerSchema);
  assertNonAuthorizing(plan);

  evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
  evidence.source_fingerprint = fingerprint(42U);
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::SourceFingerprintChanged);
  assertNonAuthorizing(plan);

  evidence = UpgradeRecoveryEvidence{};
  evidence.audit = audit(UpgradeAuditResult::Fresh);
  evidence.source_layout = UpgradeSourceLayout::Fresh;
  evidence.marker_probe = MigrationMarkerProbe::Valid;
  evidence.marker = marker(MigrationPhase::Prepared);
  plan = planUpgradeRecovery(evidence);
  assert(plan.reason == UpgradeRecoveryReason::OrphanedMarker);
  assertNonAuthorizing(plan);

  // Exercise structural marker corruption and prove the canonical CRC covers
  // all marker fields (including every fingerprint byte).
  MigrationMarker sealed = marker(MigrationPhase::Prepared);
  assert(migrationMarkerValid(sealed));
  assert(sealed.checksum == 0x3D7FD418U);
  for (std::size_t at = 0; at < sealed.source_fingerprint.size(); ++at) {
    MigrationMarker changed = sealed;
    changed.source_fingerprint[at] ^= 0x80U;
    assert(!migrationMarkerValid(changed));
  }
  for (MigrationMarker changed : {
           [&] { MigrationMarker v = sealed; ++v.schema_version; return v; }(),
           [&] { MigrationMarker v = sealed;
                 ++v.source_layout_schema_version; return v; }(),
           [&] { MigrationMarker v = sealed; ++v.generation; return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.phase = MigrationPhase::TargetWritten; return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.target_slot = MigrationSlot::SlotB; return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.rollback_source = MigrationRollbackSource::NativeSlotB;
                 return v; }(),
      }) {
    assert(!migrationMarkerValid(changed));
  }
  for (MigrationMarker malformed : {
           [&] { MigrationMarker v = sealed; v.generation = 0U;
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed; v.source_fingerprint = {};
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed; v.phase = MigrationPhase::None;
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed; v.target_slot = MigrationSlot::None;
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.rollback_source = MigrationRollbackSource::None;
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.rollback_source = MigrationRollbackSource::NativeSlotA;
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
           [&] { MigrationMarker v = sealed;
                 v.phase = static_cast<MigrationPhase>(0xFEU);
                 v.checksum = migrationMarkerChecksum(v); return v; }(),
      }) {
    assert(!migrationMarkerValid(malformed));
    evidence = legacy(MigrationPhase::Prepared, MigrationSlotProbe::Missing);
    evidence.marker = malformed;
    assertNonAuthorizing(planUpgradeRecovery(evidence));
  }

  // Unknown, invalid, I/O-failed, wrong-generation and wrong-fingerprint
  // target evidence cannot authorize resume or rollback in any active phase.
  for (MigrationPhase phase : {
           MigrationPhase::Prepared, MigrationPhase::TargetWritten,
           MigrationPhase::TargetVerified, MigrationPhase::CommitRecorded,
           MigrationPhase::RollbackRequired}) {
    for (MigrationSlotProbe probe : {MigrationSlotProbe::Unknown,
                                     MigrationSlotProbe::Invalid,
                                     MigrationSlotProbe::IoError}) {
      evidence = legacy(phase, probe);
      plan = planUpgradeRecovery(evidence);
      assert(plan.reason == UpgradeRecoveryReason::TargetAmbiguous);
      assertNonAuthorizing(plan);
    }
    evidence = legacy(phase, MigrationSlotProbe::Valid);
    evidence.native_slots[0].migration_generation++;
    assertNonAuthorizing(planUpgradeRecovery(evidence));
    evidence = legacy(phase, MigrationSlotProbe::Valid);
    evidence.native_slots[0].source_fingerprint = fingerprint(88U);
    assertNonAuthorizing(planUpgradeRecovery(evidence));
  }

  // Diagnostic names are stable, and invalid enum values still have bounded
  // output rather than triggering undefined behavior.
  assert(std::strcmp(upgradeRecoveryDecisionName(
      UpgradeRecoveryDecision::ReadOnlyRecovery), "READ_ONLY_RECOVERY") == 0);
  assert(std::strcmp(upgradeRecoveryReasonName(
      UpgradeRecoveryReason::CorruptMarker), "CORRUPT_MARKER") == 0);
  assert(std::strcmp(migrationPhaseName(MigrationPhase::TargetVerified),
                     "TARGET_VERIFIED") == 0);
  assert(std::strcmp(upgradeRecoveryDecisionName(
      static_cast<UpgradeRecoveryDecision>(0xFFU)), "UNKNOWN") == 0);
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-planner-"));
  try {
    const source = join(scratch, "planner.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(storage, "include"), source,
      join(storage, "upgrade_recovery_planner.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("upgrade/recovery planner covers every phase under strict C++17", () => {
  run(false);
});

test("upgrade/recovery planner is memory-safe under ASan/UBSan", () => {
  run(true);
});

test("portable planner has no storage or device mutation primitive", () => {
  const source = readFileSync(
    join(storage, "upgrade_recovery_planner.cpp"), "utf8",
  );
  const header = readFileSync(
    join(storage, "include/inkloop/storage/upgrade_recovery_planner.hpp"),
    "utf8",
  );
  for (const text of [source, header]) {
    assert.doesNotMatch(
      text,
      /nvs_|fopen|ofstream|filesystem|rename\s*\(|unlink\s*\(|remove\s*\(|erase\s*\(|format\s*\(|write\s*\(/,
    );
    assert.doesNotMatch(text, /#include\s*[<"](?:Arduino|esp_|nvs|freertos)/);
  }
  const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");
  assert.match(cmake, /"upgrade_recovery_planner\.cpp"/);
});
