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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "inkloop/storage/upgrade_evidence_composer.hpp"

using namespace inkloop::storage;

static MigrationFingerprint fingerprint(std::uint8_t seed) {
  MigrationFingerprint output{};
  for (std::size_t at = 0U; at < output.size(); ++at)
    output[at] = static_cast<std::uint8_t>(seed + at);
  return output;
}

static MigrationMarker marker(MigrationPhase phase,
                              std::uint64_t generation = 1U,
                              MigrationFingerprint source = fingerprint(1U)) {
  MigrationMarker output;
  output.source_layout_schema_version =
      kSupportedLegacyLayoutSchemaVersion;
  output.generation = generation;
  output.source_fingerprint = source;
  output.phase = phase;
  output.target_slot = MigrationSlot::SlotA;
  output.rollback_source = MigrationRollbackSource::LegacySnapshot;
  output.checksum = migrationMarkerChecksum(output);
  assert(migrationMarkerValid(output));
  return output;
}

static bool sameMarker(const MigrationMarker& left,
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

static std::string hex(const std::uint8_t* bytes, std::size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string output(length * 2U, '0');
  for (std::size_t at = 0U; at < length; ++at) {
    output[at * 2U] = digits[bytes[at] >> 4U];
    output[at * 2U + 1U] = digits[bytes[at] & 0x0FU];
  }
  return output;
}

class FakeJournalStore final : public IMigrationMarkerJournalStore {
 public:
  MigrationJournalStoreCode inspectRaw(
      RawMigrationMarkerJournal& output) const override {
    ++inspect_calls;
    if (fail_inspect_call != 0 && inspect_calls == fail_inspect_call)
      return MigrationJournalStoreCode::IoError;
    output = state;
    return MigrationJournalStoreCode::Ok;
  }

  MigrationJournalStoreCode writeSlotAndCommit(
      std::uint8_t slot,
      const EncodedMigrationJournalSlot& encoded) override {
    ++slot_writes;
    if (reject_slot_write || slot > 1U)
      return MigrationJournalStoreCode::IoError;
    state.namespace_available = true;
    state.slots[slot].present = true;
    state.slots[slot].length = encoded.size();
    state.slots[slot].bytes = encoded;
    if (truncate_slot_after_write)
      state.slots[slot].length = encoded.size() - 1U;
    if (corrupt_slot_after_write) state.slots[slot].bytes[20] ^= 0x80U;
    return MigrationJournalStoreCode::Ok;
  }

  MigrationJournalStoreCode writeHeadAndMarkerAndCommit(
      std::uint64_t sequence) override {
    ++head_writes;
    if (reject_head_write) return MigrationJournalStoreCode::IoError;
    state.namespace_available = true;
    state.head_present = true;
    state.head_sequence = corrupt_head_after_write ? sequence + 1U : sequence;
    state.initialized_present = !omit_initialized_after_write;
    state.initialized = kMigrationJournalInitializedMarker;
    return MigrationJournalStoreCode::Ok;
  }

  RawMigrationMarkerJournal state{true};
  mutable int inspect_calls = 0;
  int fail_inspect_call = 0;
  int slot_writes = 0;
  int head_writes = 0;
  bool reject_slot_write = false;
  bool truncate_slot_after_write = false;
  bool corrupt_slot_after_write = false;
  bool reject_head_write = false;
  bool corrupt_head_after_write = false;
  bool omit_initialized_after_write = false;
};

static MigrationMarkerJournalInspection inspect(FakeJournalStore& store) {
  store.fail_inspect_call = 0;
  MigrationMarkerJournalCore core(store);
  MigrationMarkerJournalInspection output;
  const MigrationMarkerJournalCode code = core.inspect(output);
  assert(code == MigrationMarkerJournalCode::Ok ||
         code == MigrationMarkerJournalCode::Torn ||
         code == MigrationMarkerJournalCode::Corrupt);
  return output;
}

static FakeJournalStore committedPrepared() {
  FakeJournalStore store;
  MigrationMarkerJournalCore core(store);
  MigrationMarkerJournalInspection committed;
  assert(core.commit(marker(MigrationPhase::Prepared), 0U, committed) ==
         MigrationMarkerJournalCode::Ok);
  assert(committed.probe == MigrationMarkerJournalProbe::Valid);
  assert(committed.sequence == 1U);
  return store;
}

static FingerprintedUpgradeRecord missingRecord() {
  FingerprintedUpgradeRecord record;
  record.probe = RecordProbe::Missing;
  return record;
}

static UpgradeEvidenceSnapshot freshSnapshot() {
  UpgradeEvidenceSnapshot snapshot;
  snapshot.internal_mounted = true;
  snapshot.source_layout = UpgradeSourceLayout::Fresh;
  for (FingerprintedUpgradeRecord& record : snapshot.nvs)
    record = missingRecord();
  for (FingerprintedUpgradeRecord& record : snapshot.files)
    record = missingRecord();
  for (MigrationSlotEvidence& slot : snapshot.native_slots)
    slot.probe = MigrationSlotProbe::Missing;
  snapshot.marker_journal.probe = MigrationMarkerJournalProbe::Missing;
  return snapshot;
}

static UpgradeEvidenceSnapshot legacySnapshot() {
  UpgradeEvidenceSnapshot snapshot = freshSnapshot();
  snapshot.source_layout = UpgradeSourceLayout::Legacy;
  snapshot.source_layout_schema_version =
      kSupportedLegacyLayoutSchemaVersion;
  snapshot.legacy_source_durable = true;
  snapshot.nvs[0].probe = RecordProbe::Valid;
  snapshot.nvs[0].logical_bytes = 19U;
  snapshot.nvs[0].content_fingerprint = fingerprint(40U);
  return snapshot;
}

static void assertComposerRejects(
    UpgradeEvidenceComposeCode expected,
    const UpgradeEvidenceSnapshot& first,
    const UpgradeEvidenceSnapshot& second) {
  UpgradeRecoveryEvidence output;
  const UpgradeEvidenceComposeCode code =
      composeUpgradeRecoveryEvidence(first, second, output);
  assert(code == expected);
  assert(!planUpgradeRecovery(output).authorizesMutation());
}

int main() {
  static_assert(kEncodedMigrationMarkerBytes == 56U);
  static_assert(kMigrationMarkerJournalSlotBytes == 72U);

  // Canonical v1 marker encoding is fixed-length, little-endian, and
  // independent of MigrationMarker padding.
  const MigrationMarker prepared = marker(MigrationPhase::Prepared);
  assert(prepared.checksum == 0x7466E443U);
  EncodedMigrationMarker encoded{};
  assert(encodeMigrationMarkerV1(prepared, encoded) ==
         MigrationMarkerCodecCode::Ok);
  assert(hex(encoded.data(), encoded.size()) ==
         "494e4b4d010001000100000000000000"
         "0102030405060708090a0b0c0d0e0f10"
         "1112131415161718191a1b1c1d1e1f20"
         "0101010043e46674");
  MigrationMarker decoded;
  assert(decodeMigrationMarkerV1(encoded.data(), encoded.size(), decoded) ==
         MigrationMarkerCodecCode::Ok);
  assert(sameMarker(prepared, decoded));

  for (std::size_t length = 0U; length < encoded.size(); ++length) {
    assert(decodeMigrationMarkerV1(encoded.data(), length, decoded) ==
           MigrationMarkerCodecCode::WrongSize);
  }
  assert(decodeMigrationMarkerV1(encoded.data(), encoded.size() + 1U,
                                 decoded) ==
         MigrationMarkerCodecCode::WrongSize);
  for (std::size_t at = 0U; at < encoded.size(); ++at) {
    EncodedMigrationMarker corrupt = encoded;
    corrupt[at] ^= 0x80U;
    assert(decodeMigrationMarkerV1(corrupt.data(), corrupt.size(), decoded) !=
           MigrationMarkerCodecCode::Ok);
  }
  EncodedMigrationMarker unsupported = encoded;
  unsupported[4] = 2U;
  assert(decodeMigrationMarkerV1(unsupported.data(), unsupported.size(),
                                 decoded) ==
         MigrationMarkerCodecCode::UnsupportedSchema);

  // Empty, partial metadata, invalid marker/head, missing selected slot,
  // truncated selected slot, CRC corruption, and head/slot disagreement have
  // explicit classifications.
  FakeJournalStore empty;
  MigrationMarkerJournalInspection journal = inspect(empty);
  assert(journal.probe == MigrationMarkerJournalProbe::Missing);

  FakeJournalStore partial = empty;
  partial.state.initialized_present = true;
  partial.state.initialized = kMigrationJournalInitializedMarker;
  assert(inspect(partial).probe == MigrationMarkerJournalProbe::Torn);
  partial = empty;
  partial.state.head_present = true;
  partial.state.head_sequence = 1U;
  assert(inspect(partial).probe == MigrationMarkerJournalProbe::Torn);

  FakeJournalStore base = committedPrepared();
  assert(inspect(base).probe == MigrationMarkerJournalProbe::Valid);
  FakeJournalStore damaged = base;
  damaged.state.initialized = 0U;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Corrupt);
  damaged = base;
  damaged.state.head_sequence = 0U;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Corrupt);
  damaged = base;
  damaged.state.slots[1].present = false;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Torn);
  damaged = base;
  damaged.state.slots[1].length--;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Torn);
  damaged = base;
  damaged.state.slots[1].bytes[30] ^= 1U;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Corrupt);
  damaged = base;
  damaged.state.head_sequence = 3U;
  assert(inspect(damaged).probe == MigrationMarkerJournalProbe::Corrupt);

  // An interrupted write in the inactive slot never outranks the committed
  // head, even if its bytes are torn or corrupt.
  damaged = base;
  damaged.state.slots[0].present = true;
  damaged.state.slots[0].length = 9U;
  damaged.state.slots[0].bytes.fill(0xA5U);
  journal = inspect(damaged);
  assert(journal.probe == MigrationMarkerJournalProbe::Valid);
  assert(journal.sequence == 1U);
  assert(journal.marker.phase == MigrationPhase::Prepared);

  // The core alternates journal slots by its own sequence while the migration
  // generation remains stable across phase commits.
  MigrationMarkerJournalCore base_core(base);
  MigrationMarkerJournalInspection committed;
  const MigrationMarker written = marker(MigrationPhase::TargetWritten);
  assert(base_core.commit(written, 1U, committed) ==
         MigrationMarkerJournalCode::Ok);
  assert(committed.sequence == 2U);
  assert(committed.marker.generation == 1U);
  assert(committed.marker.phase == MigrationPhase::TargetWritten);
  assert(base.state.head_sequence == 2U);
  assert(base.state.slots[0].present && base.state.slots[1].present);

  const int slot_writes = base.slot_writes;
  assert(base_core.commit(written, 2U, committed) ==
         MigrationMarkerJournalCode::Ok);
  assert(base.slot_writes == slot_writes);
  assert(base_core.commit(marker(MigrationPhase::TargetVerified), 1U,
                          committed) ==
         MigrationMarkerJournalCode::Conflict);
  assert(base_core.commit(marker(MigrationPhase::Prepared), 2U, committed) ==
         MigrationMarkerJournalCode::InvalidArgument);

  // Crash/fault injection at every store boundary. Before the head switch an
  // existing committed authority always survives; after a durable head switch
  // a final-read failure reports failure but restart recovers the new record.
  const MigrationMarker next = marker(MigrationPhase::TargetWritten);
  for (int mode = 0; mode < 5; ++mode) {
    FakeJournalStore fault = committedPrepared();
    fault.inspect_calls = 0;
    if (mode == 0) fault.reject_slot_write = true;
    if (mode == 1) fault.truncate_slot_after_write = true;
    if (mode == 2) fault.corrupt_slot_after_write = true;
    if (mode == 3) fault.fail_inspect_call = 2;
    if (mode == 4) fault.reject_head_write = true;
    MigrationMarkerJournalCore core(fault);
    MigrationMarkerJournalInspection ignored;
    assert(core.commit(next, 1U, ignored) != MigrationMarkerJournalCode::Ok);
    fault.fail_inspect_call = 0;
    fault.inspect_calls = 0;
    journal = inspect(fault);
    assert(journal.probe == MigrationMarkerJournalProbe::Valid);
    assert(journal.sequence == 1U);
    assert(journal.marker.phase == MigrationPhase::Prepared);
  }

  FakeJournalStore final_read = committedPrepared();
  final_read.inspect_calls = 0;
  final_read.fail_inspect_call = 3;
  MigrationMarkerJournalCore final_core(final_read);
  assert(final_core.commit(next, 1U, committed) ==
         MigrationMarkerJournalCode::ReadBackFailed);
  final_read.fail_inspect_call = 0;
  final_read.inspect_calls = 0;
  journal = inspect(final_read);
  assert(journal.probe == MigrationMarkerJournalProbe::Valid);
  assert(journal.sequence == 2U);
  assert(journal.marker.phase == MigrationPhase::TargetWritten);

  FakeJournalStore corrupt_head = committedPrepared();
  corrupt_head.inspect_calls = 0;
  corrupt_head.corrupt_head_after_write = true;
  MigrationMarkerJournalCore corrupt_head_core(corrupt_head);
  assert(corrupt_head_core.commit(next, 1U, committed) ==
         MigrationMarkerJournalCode::ReadBackFailed);
  assert(inspect(corrupt_head).probe == MigrationMarkerJournalProbe::Corrupt);

  FakeJournalStore torn_head = committedPrepared();
  torn_head.inspect_calls = 0;
  torn_head.omit_initialized_after_write = true;
  MigrationMarkerJournalCore torn_head_core(torn_head);
  assert(torn_head_core.commit(next, 1U, committed) ==
         MigrationMarkerJournalCode::ReadBackFailed);
  assert(inspect(torn_head).probe == MigrationMarkerJournalProbe::Torn);

  FakeJournalStore first_head_failure;
  first_head_failure.reject_head_write = true;
  MigrationMarkerJournalCore first_failure_core(first_head_failure);
  assert(first_failure_core.commit(prepared, 0U, committed) ==
         MigrationMarkerJournalCode::IoError);
  assert(inspect(first_head_failure).probe ==
         MigrationMarkerJournalProbe::Torn);

  // Double-read composition produces fresh and eligible legacy planner input
  // only from fully classified, identical snapshots.
  UpgradeEvidenceSnapshot fresh = freshSnapshot();
  UpgradeRecoveryEvidence evidence;
  assert(composeUpgradeRecoveryEvidence(fresh, fresh, evidence) ==
         UpgradeEvidenceComposeCode::Ok);
  assert(evidence.audit.result == UpgradeAuditResult::Fresh);
  assert(planUpgradeRecovery(evidence).noMigrationNeeded());

  UpgradeEvidenceSnapshot legacy = legacySnapshot();
  assert(composeUpgradeRecoveryEvidence(legacy, legacy, evidence) ==
         UpgradeEvidenceComposeCode::Ok);
  assert(evidence.audit.result == UpgradeAuditResult::Compatible);
  assert(evidence.source_layout == UpgradeSourceLayout::Legacy);
  assert(evidence.source_fingerprint == evidence.legacy_source.fingerprint);
  assert(evidence.legacy_source.durable);
  assert(hex(evidence.source_fingerprint.data(),
             evidence.source_fingerprint.size()) ==
         "9f07f4b422c684b0d10b10f832fd017f"
         "3a4d65bc2d2b71e2fa73b7c17a078e73");
  assert(planUpgradeRecovery(evidence).decision ==
         UpgradeRecoveryDecision::Start);
  const MigrationFingerprint stable_fingerprint = evidence.source_fingerprint;

  // Every input dimension participates in the two-read coherence check.
  UpgradeEvidenceSnapshot changed = legacy;
  changed.nvs[0].content_fingerprint[0] ^= 1U;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);
  changed = legacy;
  ++changed.nvs[0].logical_bytes;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);
  changed = legacy;
  changed.nvs[0].probe = RecordProbe::Recoverable;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);
  changed = legacy;
  changed.source_layout_schema_version = 2U;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);
  changed = legacy;
  changed.legacy_source_durable = false;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);
  changed = legacy;
  changed.marker_journal.probe = MigrationMarkerJournalProbe::Torn;
  assertComposerRejects(UpgradeEvidenceComposeCode::Changed, legacy, changed);

  // Unknown, ambiguous, invalid, unvalidated, I/O, and unsupported evidence
  // is never promoted into Missing/Valid planner input.
  for (RecordProbe probe : {RecordProbe::Ambiguous, RecordProbe::Invalid,
                            RecordProbe::Unvalidated}) {
    changed = legacy;
    changed.files[0].probe = probe;
    assertComposerRejects(UpgradeEvidenceComposeCode::Ambiguous,
                          changed, changed);
  }
  changed = legacy;
  changed.files[0].probe = RecordProbe::IoError;
  assertComposerRejects(UpgradeEvidenceComposeCode::IoError, changed, changed);
  changed = legacy;
  changed.files[3].probe = RecordProbe::Valid;
  changed.files[3].logical_bytes = 5U;
  changed.files[3].content_fingerprint = fingerprint(90U);
  assertComposerRejects(UpgradeEvidenceComposeCode::Ambiguous,
                        changed, changed);
  for (MigrationSlotProbe probe : {MigrationSlotProbe::Unknown,
                                   MigrationSlotProbe::Invalid}) {
    changed = legacy;
    changed.native_slots[0].probe = probe;
    assertComposerRejects(UpgradeEvidenceComposeCode::Ambiguous,
                          changed, changed);
  }
  changed = legacy;
  changed.native_slots[0].probe = MigrationSlotProbe::IoError;
  assertComposerRejects(UpgradeEvidenceComposeCode::IoError, changed, changed);
  changed = legacy;
  changed.source_layout_schema_version = 2U;
  assertComposerRejects(UpgradeEvidenceComposeCode::UnsupportedSchema,
                        changed, changed);

  // Journal corruption classifications are preserved for the planner, while
  // journal I/O prevents evidence production entirely.
  for (MigrationMarkerJournalProbe probe : {
           MigrationMarkerJournalProbe::Torn,
           MigrationMarkerJournalProbe::Corrupt}) {
    changed = legacy;
    changed.marker_journal.probe = probe;
    assert(composeUpgradeRecoveryEvidence(changed, changed, evidence) ==
           UpgradeEvidenceComposeCode::Ok);
    assert(!planUpgradeRecovery(evidence).authorizesMutation());
    assert(evidence.marker_probe ==
           (probe == MigrationMarkerJournalProbe::Torn
                ? MigrationMarkerProbe::Torn
                : MigrationMarkerProbe::Corrupt));
  }
  changed = legacy;
  changed.marker_journal.probe = MigrationMarkerJournalProbe::IoError;
  assertComposerRejects(UpgradeEvidenceComposeCode::IoError, changed, changed);

  // A valid journal marker can authorize resume only when its source
  // fingerprint is the composer's stable logical digest.
  changed = legacy;
  changed.marker_journal.probe = MigrationMarkerJournalProbe::Valid;
  changed.marker_journal.sequence = 9U;
  changed.marker_journal.marker =
      marker(MigrationPhase::Prepared, 1U, stable_fingerprint);
  assert(composeUpgradeRecoveryEvidence(changed, changed, evidence) ==
         UpgradeEvidenceComposeCode::Ok);
  assert(evidence.marker_probe == MigrationMarkerProbe::Valid);
  assert(planUpgradeRecovery(evidence).decision ==
         UpgradeRecoveryDecision::Resume);

  assert(std::strcmp(migrationMarkerCodecCodeName(
      MigrationMarkerCodecCode::WrongSize), "WRONG_SIZE") == 0);
  assert(std::strcmp(migrationMarkerJournalProbeName(
      MigrationMarkerJournalProbe::Torn), "TORN") == 0);
  assert(std::strcmp(upgradeEvidenceComposeCodeName(
      UpgradeEvidenceComposeCode::Changed), "CHANGED") == 0);
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-marker-"));
  try {
    const source = join(scratch, "marker.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(storage, "include"), source,
      join(storage, "upgrade_marker_journal.cpp"),
      join(storage, "upgrade_evidence_composer.cpp"),
      join(storage, "upgrade_recovery_planner.cpp"),
      join(storage, "upgrade_audit.cpp"), join(storage, "sha256.cpp"),
      "-o", binary,
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

test("marker journal and evidence composer pass strict C++17 fault matrix", () => {
  run(false);
});

test("marker journal and evidence composer pass ASan/UBSan fault matrix", () => {
  run(true);
});

test("ESP-IDF marker adapter is isolated to its dedicated namespace", () => {
  const adapter = readFileSync(
    join(storage, "esp_nvs_upgrade_marker_journal.cpp"), "utf8",
  );
  const header = readFileSync(
    join(storage, "include/inkloop/storage/esp_nvs_upgrade_marker_journal.hpp"),
    "utf8",
  );
  const portable = [
    "upgrade_marker_journal.cpp", "upgrade_evidence_composer.cpp",
  ].map((name) => readFileSync(join(storage, name), "utf8")).join("\n");
  assert.match(adapter, /kNamespace\[\] = "ink-migrate-v1"/);
  assert.match(adapter, /nvs_open\(kNamespace, NVS_READONLY/);
  assert.match(adapter, /nvs_open\(kNamespace, NVS_READWRITE/);
  assert.match(adapter, /nvs_set_blob/);
  assert.match(adapter, /nvs_set_u64/);
  assert.match(adapter, /nvs_commit/);
  assert.doesNotMatch(adapter, /nvs_erase|format|unlink|remove\s*\(|rename\s*\(/);
  for (const protectedName of [
    "inkloop-v2", "inkloop", "ink-myai-v1", "ink-portal",
    "ink-album-meta", "ink-pair-ui", "nvs.net80211", "phy", "cal_data",
  ]) assert.doesNotMatch(
    adapter + header,
    new RegExp(`["']${protectedName.replaceAll(".", "\\.")}["']`),
  );
  assert.doesNotMatch(portable, /#include\s*[<"](?:Arduino|esp_|nvs|freertos)/);
  const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");
  for (const source of [
    "esp_nvs_upgrade_marker_journal.cpp", "upgrade_evidence_composer.cpp",
    "upgrade_marker_journal.cpp",
  ]) assert.match(cmake, new RegExp(`"${source.replaceAll(".", "\\.")}"`));
});
