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
#include <string>

#include "inkloop/storage/upgrade_executor.hpp"

using namespace inkloop::storage;

class FakeJournal final : public IMigrationMarkerJournalStore {
 public:
  MigrationJournalStoreCode inspectRaw(
      RawMigrationMarkerJournal& output) const override {
    ++inspect_calls;
    if (fail_inspect_call != 0 && inspect_calls == fail_inspect_call) {
      return MigrationJournalStoreCode::IoError;
    }
    output = raw;
    return MigrationJournalStoreCode::Ok;
  }

  MigrationJournalStoreCode writeSlotAndCommit(
      std::uint8_t slot,
      const EncodedMigrationJournalSlot& encoded) override {
    ++slot_writes;
    if (fail_next_slot || slot > 1U) {
      fail_next_slot = false;
      return MigrationJournalStoreCode::IoError;
    }
    raw.namespace_available = true;
    raw.slots[slot].present = true;
    raw.slots[slot].length = encoded.size();
    raw.slots[slot].bytes = encoded;
    return MigrationJournalStoreCode::Ok;
  }

  MigrationJournalStoreCode writeHeadAndMarkerAndCommit(
      std::uint64_t sequence) override {
    ++head_writes;
    if (fail_next_head) {
      fail_next_head = false;
      return MigrationJournalStoreCode::IoError;
    }
    raw.namespace_available = true;
    raw.initialized_present = true;
    raw.initialized = kMigrationJournalInitializedMarker;
    raw.head_present = true;
    raw.head_sequence = sequence;
    return MigrationJournalStoreCode::Ok;
  }

  MigrationMarkerJournalInspection inspection() const {
    MigrationMarkerJournalCore core(const_cast<FakeJournal&>(*this));
    MigrationMarkerJournalInspection output;
    const MigrationMarkerJournalCode code = core.inspect(output);
    assert(code == MigrationMarkerJournalCode::Ok ||
           code == MigrationMarkerJournalCode::Torn ||
           code == MigrationMarkerJournalCode::Corrupt);
    return output;
  }

  RawMigrationMarkerJournal raw{true};
  mutable int inspect_calls = 0;
  int fail_inspect_call = 0;
  bool fail_next_slot = false;
  bool fail_next_head = false;
  int slot_writes = 0;
  int head_writes = 0;
};

struct FakeRecord {
  UpgradeRecordStreamCode code = UpgradeRecordStreamCode::Missing;
  std::string bytes;
};

static std::size_t flat(UpgradeRecordId record) {
  assert(upgradeRecordIdValid(record));
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? record.index
      : kProtectedNvsNamespaces.size() + record.index;
}

static constexpr std::size_t kRecordCount =
    kProtectedNvsNamespaces.size() + kProtectedFilePaths.size();

class FakeSource final : public IUpgradeSnapshotSource {
 public:
  explicit FakeSource(FakeJournal& journal) : journal(journal) {
    metadata.internal_mounted = true;
    metadata.source_layout = UpgradeSourceLayout::Legacy;
    metadata.source_layout_schema_version =
        kSupportedLegacyLayoutSchemaVersion;
    metadata.legacy_source_durable = true;
    for (MigrationSlotEvidence& slot : metadata.native_slots)
      slot.probe = MigrationSlotProbe::Missing;
    records[0].code = UpgradeRecordStreamCode::Valid;
    records[0].bytes = "settings-v1";
  }

  bool inspectMetadata(UpgradeSnapshotMetadata& output) const override {
    ++metadata_calls;
    if (fail_metadata_call == metadata_calls) return false;
    output = metadata;
    const MigrationMarkerJournalInspection inspected = journal.inspection();
    output.marker_journal = inspected;
    if (change_metadata_on_second_pass && metadata_calls % 2 == 0)
      output.legacy_source_durable = !output.legacy_source_durable;
    return true;
  }

  UpgradeRecordStreamCode streamRecord(
      UpgradeRecordId record, std::uint64_t maximum,
      IUpgradeByteSink& sink) const override {
    const std::size_t at = flat(record);
    ++record_calls[at];
    ++total_record_calls;
    if (fail_stream_call == total_record_calls) {
      const std::uint8_t partial[] = {0xA5U, 0x5AU};
      (void)sink.write(partial, sizeof(partial));
      return UpgradeRecordStreamCode::IoError;
    }
    if (too_large_record == static_cast<int>(at)) {
      if (force_sink_overflow) {
        std::array<std::uint8_t, 4096> chunk{};
        std::uint64_t written = 0U;
        while (written <= maximum) {
          const std::size_t length = static_cast<std::size_t>(
              std::min<std::uint64_t>(chunk.size(), maximum + 1U - written));
          if (!sink.write(chunk.data(), length)) break;
          written += length;
        }
        return UpgradeRecordStreamCode::Valid;
      }
      return UpgradeRecordStreamCode::TooLarge;
    }
    FakeRecord selected = records[at];
    if (change_record == static_cast<int>(at) &&
        record_calls[at] % 2 == 0) {
      selected.code = UpgradeRecordStreamCode::Valid;
      selected.bytes += "-changed";
    }
    if (change_on_absolute_call == total_record_calls) {
      selected.code = UpgradeRecordStreamCode::Valid;
      selected.bytes += "-changed-now";
    }
    if (!selected.bytes.empty() &&
        !sink.write(reinterpret_cast<const std::uint8_t*>(
                        selected.bytes.data()), selected.bytes.size())) {
      return selected.code;
    }
    return selected.code;
  }

  void clearFaults() {
    fail_metadata_call = -1;
    change_metadata_on_second_pass = false;
    fail_stream_call = -1;
    too_large_record = -1;
    force_sink_overflow = false;
    change_record = -1;
    change_on_absolute_call = -1;
  }

  FakeJournal& journal;
  UpgradeSnapshotMetadata metadata;
  std::array<FakeRecord, kRecordCount> records{};
  mutable std::array<int, kRecordCount> record_calls{};
  mutable int total_record_calls = 0;
  mutable int metadata_calls = 0;
  int fail_metadata_call = -1;
  bool change_metadata_on_second_pass = false;
  int fail_stream_call = -1;
  int too_large_record = -1;
  bool force_sink_overflow = false;
  int change_record = -1;
  int change_on_absolute_call = -1;
};

class FakeTarget final : public IUpgradeMigrationTargetStore {
 public:
  explicit FakeTarget(FakeSource& source) : source(source) {
    evidence.probe = MigrationSlotProbe::Missing;
  }

  UpgradeTargetStoreCode inspectTargetGroup(
      MigrationSlot slot, UpgradeLogicalGroup group,
      MigrationSlotEvidence& output) const override {
    ++inspect_calls;
    if ((fail_inspect_call != 0 && inspect_calls == fail_inspect_call) ||
        slot != MigrationSlot::SlotA ||
        group != UpgradeLogicalGroup::ProtectedSnapshotV1)
      return UpgradeTargetStoreCode::IoError;
    output = evidence;
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeTargetStoreCode beginTargetGroup(
      MigrationSlot slot, UpgradeLogicalGroup group,
      std::uint64_t generation,
      const MigrationFingerprint& fingerprint) override {
    ++begin_group_calls;
    if (fail_begin_group || slot != MigrationSlot::SlotA ||
        group != UpgradeLogicalGroup::ProtectedSnapshotV1 ||
        evidence.probe != MigrationSlotProbe::Missing)
      return UpgradeTargetStoreCode::IoError;
    staging = {};
    staging_present.fill(false);
    stage_generation = generation;
    stage_fingerprint = fingerprint;
    in_group = true;
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeTargetStoreCode beginTargetRecord(
      UpgradeRecordId record, bool present) override {
    ++begin_record_calls;
    if (!in_group || in_record ||
        (fail_begin_record_call != 0 &&
         begin_record_calls == fail_begin_record_call))
      return UpgradeTargetStoreCode::IoError;
    current = flat(record);
    staging_present[current] = present;
    staging[current].clear();
    in_record = true;
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeTargetStoreCode writeTargetBytes(
      const std::uint8_t* bytes, std::size_t length) override {
    ++write_calls;
    if (!in_record || (fail_write_call != 0 && write_calls == fail_write_call))
      return UpgradeTargetStoreCode::IoError;
    if (length != 0U)
      staging[current].append(reinterpret_cast<const char*>(bytes), length);
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeTargetStoreCode finishTargetRecord() override {
    ++finish_record_calls;
    if (!in_record || (fail_finish_record_call != 0 &&
                       finish_record_calls == fail_finish_record_call))
      return UpgradeTargetStoreCode::IoError;
    in_record = false;
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeTargetStoreCode commitTargetGroup() override {
    ++commit_calls;
    if (!in_group || in_record || fail_commit)
      return UpgradeTargetStoreCode::IoError;
    committed = staging;
    committed_present = staging_present;
    evidence.probe = MigrationSlotProbe::Valid;
    evidence.migration_generation = stage_generation;
    evidence.source_fingerprint = stage_fingerprint;
    source.metadata.native_slots[0] = evidence;
    in_group = false;
    return UpgradeTargetStoreCode::Ok;
  }

  UpgradeRecordStreamCode streamTargetRecord(
      MigrationSlot slot, UpgradeLogicalGroup group, UpgradeRecordId record,
      std::uint64_t, IUpgradeByteSink& sink) const override {
    ++read_calls;
    if (fail_read_call != 0 && read_calls == fail_read_call)
      return UpgradeRecordStreamCode::IoError;
    if (slot != MigrationSlot::SlotA ||
        group != UpgradeLogicalGroup::ProtectedSnapshotV1 ||
        evidence.probe != MigrationSlotProbe::Valid)
      return UpgradeRecordStreamCode::IoError;
    const std::size_t at = flat(record);
    if (!committed_present[at]) return UpgradeRecordStreamCode::Missing;
    std::string bytes = committed[at];
    if (corrupt_read_call != 0 && read_calls == corrupt_read_call)
      bytes += "corrupt";
    if (!bytes.empty() &&
        !sink.write(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                    bytes.size()))
      return UpgradeRecordStreamCode::Valid;
    return UpgradeRecordStreamCode::Valid;
  }

  void loseTarget() {
    evidence = MigrationSlotEvidence{};
    evidence.probe = MigrationSlotProbe::Missing;
    source.metadata.native_slots[0] = evidence;
    committed = {};
    committed_present.fill(false);
  }

  FakeSource& source;
  MigrationSlotEvidence evidence;
  std::array<std::string, kRecordCount> staging{};
  std::array<std::string, kRecordCount> committed{};
  std::array<bool, kRecordCount> staging_present{};
  std::array<bool, kRecordCount> committed_present{};
  MigrationFingerprint stage_fingerprint{};
  std::uint64_t stage_generation = 0U;
  std::size_t current = 0U;
  bool in_group = false;
  bool in_record = false;
  bool fail_begin_group = false;
  int fail_begin_record_call = 0;
  int fail_write_call = 0;
  int fail_finish_record_call = 0;
  bool fail_commit = false;
  mutable int fail_inspect_call = 0;
  mutable int fail_read_call = 0;
  mutable int corrupt_read_call = 0;
  int begin_group_calls = 0;
  int begin_record_calls = 0;
  int write_calls = 0;
  int finish_record_calls = 0;
  int commit_calls = 0;
  mutable int inspect_calls = 0;
  mutable int read_calls = 0;
};

class FakeAuthority final : public IUpgradeAuthorityHead {
 public:
  UpgradeAuthorityCode inspectAuthority(
      UpgradeAuthority& output) const override {
    ++inspect_calls;
    if (fail_inspect_call != 0 && inspect_calls == fail_inspect_call) {
      return UpgradeAuthorityCode::IoError;
    }
    output = state;
    return UpgradeAuthorityCode::Ok;
  }

  UpgradeAuthorityCode activateTarget(
      MigrationSlot target, std::uint64_t generation,
      const MigrationFingerprint& fingerprint) override {
    ++target_switches;
    if (fail_target_switch || target != MigrationSlot::SlotA)
      return UpgradeAuthorityCode::IoError;
    state.kind = UpgradeAuthorityKind::NativeSlotA;
    state.generation = generation;
    state.source_fingerprint = fingerprint;
    return UpgradeAuthorityCode::Ok;
  }

  UpgradeAuthorityCode activateRollback(
      MigrationRollbackSource rollback,
      const MigrationFingerprint& fingerprint) override {
    ++rollback_switches;
    if (fail_rollback_switch ||
        rollback != MigrationRollbackSource::LegacySnapshot)
      return UpgradeAuthorityCode::IoError;
    state.kind = UpgradeAuthorityKind::LegacySnapshot;
    state.generation = 0U;
    state.source_fingerprint = fingerprint;
    return UpgradeAuthorityCode::Ok;
  }

  UpgradeAuthority state;
  mutable int inspect_calls = 0;
  int fail_inspect_call = 0;
  bool fail_target_switch = false;
  bool fail_rollback_switch = false;
  int target_switches = 0;
  int rollback_switches = 0;
};

struct Fixture {
  FakeJournal journal;
  FakeSource source;
  FakeTarget target;
  FakeAuthority authority;

  Fixture() : source(journal), target(source) {}

  CollectedUpgradeRecovery collect() {
    UpgradeSnapshotCollector collector(source);
    CollectedUpgradeRecovery output;
    assert(collector.collect(output) == UpgradeSnapshotCollectCode::Ok);
    return output;
  }

  UpgradeExecutionRequest request() {
    CollectedUpgradeRecovery collected = collect();
    UpgradeExecutionRequest output;
    assert(bindUpgradeExecutionRequest(
        collected, UpgradeLogicalGroup::ProtectedSnapshotV1, output));
    return output;
  }

  UpgradeExecutorCode execute(const UpgradeExecutionRequest& request,
                              UpgradeExecutionOutcome& output) {
    UpgradeExecutorCore core(source, target, authority, journal);
    return core.execute(request, output);
  }

  void initializeAuthority() {
    CollectedUpgradeRecovery initial = collect();
    authority.state.kind = UpgradeAuthorityKind::LegacySnapshot;
    authority.state.generation = 0U;
    authority.state.source_fingerprint = initial.evidence.source_fingerprint;
  }

  void prepare() {
    initializeAuthority();
    UpgradeExecutionRequest start = request();
    assert(start.plan.decision == UpgradeRecoveryDecision::Start);
    UpgradeExecutionOutcome output;
    assert(execute(start, output) == UpgradeExecutorCode::Ok);
    assert(output.durable_phase == MigrationPhase::Prepared);
  }

  void advanceTo(MigrationPhase phase) {
    if (journal.inspection().probe == MigrationMarkerJournalProbe::Missing)
      prepare();
    while (journal.inspection().marker.phase != phase) {
      UpgradeExecutionRequest next = request();
      UpgradeExecutionOutcome output;
      assert(execute(next, output) == UpgradeExecutorCode::Ok);
    }
  }
};

static void collectorMatrix() {
  {
    Fixture fixture;
    CollectedUpgradeRecovery collected = fixture.collect();
    assert(collected.plan.decision == UpgradeRecoveryDecision::Start);
    for (int calls : fixture.source.record_calls) assert(calls == 2);
    assert(collected.snapshot.nvs[0].logical_bytes == 11U);
    assert(collected.snapshot.nvs[0].content_fingerprint !=
           MigrationFingerprint{});
  }
  {
    Fixture fixture;
    fixture.source.change_record = 0;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) == UpgradeSnapshotCollectCode::Changed);
    for (int calls : fixture.source.record_calls) assert(calls == 2);
  }
  {
    Fixture fixture;
    fixture.source.change_metadata_on_second_pass = true;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) == UpgradeSnapshotCollectCode::Changed);
  }
  for (bool sink_overflow : {false, true}) {
    Fixture fixture;
    fixture.source.too_large_record = 0;
    fixture.source.force_sink_overflow = sink_overflow;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) == UpgradeSnapshotCollectCode::TooLarge);
    for (int calls : fixture.source.record_calls) assert(calls == 2);
  }
  {
    Fixture fixture;
    fixture.source.fail_stream_call = 1;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) == UpgradeSnapshotCollectCode::IoError);
    for (int calls : fixture.source.record_calls) assert(calls == 2);
  }
  {
    Fixture fixture;
    fixture.source.fail_metadata_call = 1;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) == UpgradeSnapshotCollectCode::IoError);
    for (int calls : fixture.source.record_calls) assert(calls == 2);
  }
  for (UpgradeRecordStreamCode code : {
           UpgradeRecordStreamCode::Ambiguous,
           UpgradeRecordStreamCode::Invalid,
           UpgradeRecordStreamCode::Unvalidated}) {
    Fixture fixture;
    fixture.source.records[0].code = code;
    fixture.source.records[0].bytes.clear();
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) ==
           UpgradeSnapshotCollectCode::Ambiguous);
  }
  {
    Fixture fixture;
    fixture.source.metadata.source_layout = UpgradeSourceLayout::Unsupported;
    fixture.source.metadata.source_layout_schema_version = 99U;
    CollectedUpgradeRecovery ignored;
    UpgradeSnapshotCollector collector(fixture.source);
    assert(collector.collect(ignored) ==
           UpgradeSnapshotCollectCode::UnsupportedSchema);
  }
  assert(upgradeRecordMaximumBytes(
             {UpgradeRecordDomain::NvsNamespace, 0U}) ==
         kMaximumUpgradeNvsNamespaceBytes);
  assert(upgradeRecordName({UpgradeRecordDomain::File, 0U}) ==
         kProtectedFilePaths[0]);
  assert(!upgradeRecordIdValid({UpgradeRecordDomain::File, 99U}));
}

static void happyExecutor() {
  Fixture fixture;
  fixture.initializeAuthority();
  UpgradeExecutionRequest stale = fixture.request();
  assert(stale.plan.decision == UpgradeRecoveryDecision::Start);
  fixture.source.records[0].bytes += "-new";
  UpgradeExecutionOutcome output;
  assert(fixture.execute(stale, output) == UpgradeExecutorCode::PlanMismatch);
  assert(fixture.journal.inspection().probe ==
         MigrationMarkerJournalProbe::Missing);
  fixture.source.records[0].bytes = "settings-v1";

  UpgradeExecutionRequest start = fixture.request();
  assert(fixture.execute(start, output) == UpgradeExecutorCode::Ok);
  assert(output.durable_phase == MigrationPhase::Prepared);
  assert(fixture.target.begin_group_calls == 0);
  assert(fixture.authority.target_switches == 0);

  UpgradeExecutionRequest prepared = fixture.request();
  assert(prepared.plan.phase == MigrationPhase::Prepared);
  assert(fixture.execute(prepared, output) == UpgradeExecutorCode::Ok);
  assert(output.target_group_committed);
  assert(output.durable_phase == MigrationPhase::TargetWritten);
  assert(fixture.target.begin_group_calls == 1);
  assert(fixture.target.begin_record_calls ==
         static_cast<int>(kRecordCount));
  assert(fixture.target.finish_record_calls ==
         static_cast<int>(kRecordCount));
  assert(fixture.target.commit_calls == 1);
  assert(fixture.authority.state.kind ==
         UpgradeAuthorityKind::LegacySnapshot);

  UpgradeExecutionRequest written = fixture.request();
  assert(written.plan.phase == MigrationPhase::TargetWritten);
  assert(fixture.execute(written, output) == UpgradeExecutorCode::Ok);
  assert(output.durable_phase == MigrationPhase::TargetVerified);
  assert(fixture.target.begin_group_calls == 1);

  UpgradeExecutionRequest verified = fixture.request();
  assert(verified.plan.phase == MigrationPhase::TargetVerified);
  assert(fixture.execute(verified, output) == UpgradeExecutorCode::Ok);
  assert(output.authority_switched);
  assert(output.durable_phase == MigrationPhase::CommitRecorded);
  assert(fixture.authority.state.kind == UpgradeAuthorityKind::NativeSlotA);

  UpgradeExecutionRequest committed = fixture.request();
  assert(committed.plan.phase == MigrationPhase::CommitRecorded);
  assert(fixture.execute(committed, output) == UpgradeExecutorCode::Ok);
  assert(output.durable_phase == MigrationPhase::Complete);
  assert(fixture.target.begin_group_calls == 1);
  assert(fixture.authority.target_switches == 1);
}

static void preparedFaultMatrix() {
  for (int mode = 0; mode < 9; ++mode) {
    Fixture fixture;
    fixture.prepare();
    UpgradeExecutionRequest prepared = fixture.request();
    const int calls = fixture.source.total_record_calls;
    if (mode == 0) fixture.source.fail_stream_call = calls + 41;
    if (mode == 1) fixture.source.change_on_absolute_call = calls + 41;
    if (mode == 2) fixture.target.fail_begin_group = true;
    if (mode == 3) fixture.target.fail_begin_record_call = 1;
    if (mode == 4) fixture.target.fail_write_call = 1;
    if (mode == 5) fixture.target.fail_finish_record_call = 1;
    if (mode == 6) fixture.target.fail_commit = true;
    if (mode == 7) fixture.target.fail_inspect_call = 2;
    if (mode == 8) fixture.target.fail_read_call = 1;
    UpgradeExecutionOutcome output;
    const UpgradeExecutorCode code = fixture.execute(prepared, output);
    assert(code != UpgradeExecutorCode::Ok);
    assert(fixture.authority.state.kind ==
           UpgradeAuthorityKind::LegacySnapshot);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::Prepared);
    if (mode <= 6)
      assert(fixture.source.metadata.native_slots[0].probe ==
             MigrationSlotProbe::Missing);
  }
  {
    Fixture fixture;
    fixture.prepare();
    UpgradeExecutionRequest prepared = fixture.request();
    fixture.target.corrupt_read_call = 1;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(prepared, output) ==
           UpgradeExecutorCode::TargetMismatch);
    assert(fixture.authority.state.kind ==
           UpgradeAuthorityKind::LegacySnapshot);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::Prepared);
  }
  {
    Fixture fixture;
    fixture.prepare();
    UpgradeExecutionRequest prepared = fixture.request();
    fixture.journal.fail_next_slot = true;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(prepared, output) ==
           UpgradeExecutorCode::MarkerError);
    assert(fixture.target.evidence.probe == MigrationSlotProbe::Valid);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::Prepared);
    const int begin_calls = fixture.target.begin_group_calls;
    UpgradeExecutionRequest restart = fixture.request();
    assert(restart.plan.phase == MigrationPhase::Prepared);
    assert(fixture.execute(restart, output) == UpgradeExecutorCode::Ok);
    assert(output.durable_phase == MigrationPhase::TargetWritten);
    assert(fixture.target.begin_group_calls == begin_calls);
  }
}

static void perRecordFaultMatrix() {
  for (std::size_t at = 0U; at < kRecordCount; ++at) {
    {
      Fixture fixture;
      fixture.prepare();
      UpgradeExecutionRequest prepared = fixture.request();
      fixture.source.fail_stream_call =
          fixture.source.total_record_calls + 41 + static_cast<int>(at);
      UpgradeExecutionOutcome output;
      assert(fixture.execute(prepared, output) ==
             UpgradeExecutorCode::SourceReadError);
      assert(fixture.authority.state.kind ==
             UpgradeAuthorityKind::LegacySnapshot);
      assert(fixture.journal.inspection().marker.phase ==
             MigrationPhase::Prepared);
    }
    {
      Fixture fixture;
      fixture.prepare();
      UpgradeExecutionRequest prepared = fixture.request();
      fixture.target.fail_begin_record_call = static_cast<int>(at) + 1;
      UpgradeExecutionOutcome output;
      assert(fixture.execute(prepared, output) ==
             UpgradeExecutorCode::TargetWriteError);
      assert(fixture.journal.inspection().marker.phase ==
             MigrationPhase::Prepared);
    }
    {
      Fixture fixture;
      fixture.prepare();
      UpgradeExecutionRequest prepared = fixture.request();
      fixture.target.fail_finish_record_call = static_cast<int>(at) + 1;
      UpgradeExecutionOutcome output;
      assert(fixture.execute(prepared, output) ==
             UpgradeExecutorCode::TargetWriteError);
      assert(fixture.journal.inspection().marker.phase ==
             MigrationPhase::Prepared);
    }
    {
      Fixture fixture;
      fixture.prepare();
      UpgradeExecutionRequest prepared = fixture.request();
      fixture.target.fail_read_call = static_cast<int>(at) + 1;
      UpgradeExecutionOutcome output;
      assert(fixture.execute(prepared, output) ==
             UpgradeExecutorCode::TargetReadError);
      assert(fixture.authority.state.kind ==
             UpgradeAuthorityKind::LegacySnapshot);
      assert(fixture.journal.inspection().marker.phase ==
             MigrationPhase::Prepared);
    }
  }
}

static void markerAndAuthorityFaults() {
  for (int mode = 0; mode < 2; ++mode) {
    Fixture fixture;
    fixture.initializeAuthority();
    UpgradeExecutionRequest start = fixture.request();
    if (mode == 0) fixture.journal.fail_next_slot = true;
    else fixture.journal.fail_next_head = true;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(start, output) == UpgradeExecutorCode::MarkerError);
    assert(fixture.target.begin_group_calls == 0);
    assert(fixture.authority.state.kind ==
           UpgradeAuthorityKind::LegacySnapshot);
    const MigrationMarkerJournalInspection inspected =
        fixture.journal.inspection();
    assert(mode == 0
        ? inspected.probe == MigrationMarkerJournalProbe::Missing
        : inspected.probe == MigrationMarkerJournalProbe::Torn);
  }
  // Once the target is durable, every marker-store boundary may fail without
  // changing the old authority. A final-inspect failure may hide a successful
  // head commit, which restart recognizes as TargetWritten.
  for (int mode = 0; mode < 5; ++mode) {
    Fixture fixture;
    fixture.prepare();
    UpgradeExecutionRequest prepared = fixture.request();
    const int base_inspects = fixture.journal.inspect_calls;
    if (mode == 0) fixture.journal.fail_next_slot = true;
    if (mode == 1) fixture.journal.fail_next_head = true;
    if (mode == 2) fixture.journal.fail_inspect_call = base_inspects + 3;
    if (mode == 3) fixture.journal.fail_inspect_call = base_inspects + 4;
    if (mode == 4) fixture.journal.fail_inspect_call = base_inspects + 5;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(prepared, output) ==
           UpgradeExecutorCode::MarkerError);
    assert(fixture.target.evidence.probe == MigrationSlotProbe::Valid);
    assert(fixture.authority.state.kind ==
           UpgradeAuthorityKind::LegacySnapshot);
    UpgradeExecutionRequest restart = fixture.request();
    assert(restart.plan.phase == (mode == 4
        ? MigrationPhase::TargetWritten
        : MigrationPhase::Prepared));
    assert(fixture.execute(restart, output) == UpgradeExecutorCode::Ok);
  }
  {
    Fixture fixture;
    fixture.advanceTo(MigrationPhase::TargetVerified);
    UpgradeExecutionRequest verified = fixture.request();
    fixture.authority.fail_target_switch = true;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(verified, output) ==
           UpgradeExecutorCode::AuthoritySwitchError);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::TargetVerified);
    assert(fixture.authority.state.kind ==
           UpgradeAuthorityKind::LegacySnapshot);
  }
  {
    Fixture fixture;
    fixture.advanceTo(MigrationPhase::TargetVerified);
    UpgradeExecutionRequest verified = fixture.request();
    fixture.authority.fail_inspect_call =
        fixture.authority.inspect_calls + 2;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(verified, output) ==
           UpgradeExecutorCode::AuthoritySwitchError);
    assert(fixture.authority.state.kind == UpgradeAuthorityKind::NativeSlotA);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::TargetVerified);
    UpgradeExecutionRequest restart = fixture.request();
    assert(fixture.execute(restart, output) == UpgradeExecutorCode::Ok);
    assert(output.durable_phase == MigrationPhase::CommitRecorded);
  }
  {
    Fixture fixture;
    fixture.advanceTo(MigrationPhase::TargetVerified);
    UpgradeExecutionRequest verified = fixture.request();
    fixture.journal.fail_next_slot = true;
    UpgradeExecutionOutcome output;
    assert(fixture.execute(verified, output) ==
           UpgradeExecutorCode::MarkerError);
    assert(fixture.authority.state.kind == UpgradeAuthorityKind::NativeSlotA);
    assert(fixture.journal.inspection().marker.phase ==
           MigrationPhase::TargetVerified);
    UpgradeExecutionRequest restart = fixture.request();
    assert(restart.plan.phase == MigrationPhase::TargetVerified);
    const int switches = fixture.authority.target_switches;
    assert(fixture.execute(restart, output) == UpgradeExecutorCode::Ok);
    assert(output.durable_phase == MigrationPhase::CommitRecorded);
    assert(fixture.authority.target_switches == switches);
  }
}

static void rollbackBoundary() {
  Fixture fixture;
  fixture.advanceTo(MigrationPhase::TargetWritten);
  fixture.target.loseTarget();
  UpgradeExecutionRequest rollback = fixture.request();
  assert(rollback.plan.decision == UpgradeRecoveryDecision::Rollback);
  assert(rollback.plan.phase == MigrationPhase::TargetWritten);
  UpgradeExecutionOutcome output;
  assert(fixture.execute(rollback, output) == UpgradeExecutorCode::Ok);
  assert(output.durable_phase == MigrationPhase::RollbackRequired);
  assert(fixture.authority.rollback_switches == 0);

  fixture.authority.state.kind = UpgradeAuthorityKind::NativeSlotA;
  fixture.authority.state.generation = rollback.plan.generation;
  fixture.authority.state.source_fingerprint = rollback.source_fingerprint;
  UpgradeExecutionRequest apply = fixture.request();
  assert(apply.plan.decision == UpgradeRecoveryDecision::Rollback);
  assert(apply.plan.phase == MigrationPhase::RollbackRequired);
  assert(fixture.execute(apply, output) ==
         UpgradeExecutorCode::RollbackAppliedAwaitingTerminal);
  assert(output.authority_switched);
  assert(fixture.authority.state.kind ==
         UpgradeAuthorityKind::LegacySnapshot);
  assert(fixture.journal.inspection().marker.phase ==
         MigrationPhase::RollbackRequired);

  UpgradeExecutionRequest again = fixture.request();
  assert(fixture.execute(again, output) ==
         UpgradeExecutorCode::RollbackAppliedAwaitingTerminal);
  assert(!output.authority_switched);
}

int main() {
  static_assert(kRecordCount == 20U);
  assert(std::string(upgradeLogicalGroupName(
             UpgradeLogicalGroup::ProtectedSnapshotV1)) ==
         "protected-upgrade-snapshot-v1");
  collectorMatrix();
  happyExecutor();
  preparedFaultMatrix();
  perRecordFaultMatrix();
  markerAndAuthorityFaults();
  rollbackBoundary();
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-executor-"));
  try {
    const source = join(scratch, "executor.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(storage, "include"), source,
      join(storage, "upgrade_snapshot_collector.cpp"),
      join(storage, "upgrade_executor.cpp"),
      join(storage, "upgrade_evidence_composer.cpp"),
      join(storage, "upgrade_marker_journal.cpp"),
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

test("double-read collector and executor fault matrix pass strict C++17", () => {
  run(false);
});

test("collector and executor crash matrix pass ASan/UBSan", () => {
  run(true);
});

test("portable upgrade core has no live adapter or destructive primitive", () => {
  const files = [
    "upgrade_snapshot_collector.cpp", "upgrade_executor.cpp",
    "include/inkloop/storage/upgrade_snapshot_collector.hpp",
    "include/inkloop/storage/upgrade_executor.hpp",
  ];
  const portable = files.map((name) =>
    readFileSync(join(storage, name), "utf8")).join("\n");
  assert.doesNotMatch(portable, /#include\s*[<"](?:Arduino|esp_|nvs|freertos)/);
  assert.doesNotMatch(
    portable,
    /nvs_set|nvs_commit|nvs_erase|format|unlink|remove\s*\(|rename\s*\(/,
  );
  assert.match(portable, /protected-upgrade-snapshot-v1/);
  assert.match(portable, /RollbackAppliedAwaitingTerminal/);
  const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");
  assert.match(cmake, /"upgrade_snapshot_collector\.cpp"/);
  assert.match(cmake, /"upgrade_executor\.cpp"/);
});
