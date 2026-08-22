import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const main = join(repo, "firmware/inkloop-idf/main");
const ota = join(repo, "firmware/inkloop-idf/components/inkloop_ota");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

#include "ota_outcome_journal.hpp"

using namespace inkloop;

static OtaTextView text(const std::string& value) {
  return {value.data(), value.size()};
}

struct Store final : IOtaOutcomeJournalStore {
  std::array<RawOtaOutcomeSlot, 2U> slots{};
  bool fail_inspect = false;
  bool fail_write_before = false;
  bool fail_write_after = false;
  bool tear_write = false;
  bool fail_clear = false;
  unsigned writes = 0U;
  unsigned clears = 0U;

  OtaOutcomeStoreCode inspectRaw(
      std::array<RawOtaOutcomeSlot, 2U>& output) const override {
    if (fail_inspect) return OtaOutcomeStoreCode::IoError;
    output = slots;
    return OtaOutcomeStoreCode::Ok;
  }

  OtaOutcomeStoreCode writeSlotAndCommit(
      std::uint8_t slot, const EncodedOtaOutcomeSlot& encoded) override {
    ++writes;
    if (slot > 1U) return OtaOutcomeStoreCode::InvalidArgument;
    if (fail_write_before) return OtaOutcomeStoreCode::IoError;
    slots[slot].present = true;
    slots[slot].length = encoded.size();
    slots[slot].bytes = encoded;
    if (tear_write) slots[slot].bytes[12] ^= 0x80U;
    return fail_write_after ? OtaOutcomeStoreCode::IoError
                            : OtaOutcomeStoreCode::Ok;
  }

  OtaOutcomeStoreCode clearAndCommit() override {
    ++clears;
    if (fail_clear) return OtaOutcomeStoreCode::IoError;
    slots = {};
    return OtaOutcomeStoreCode::Ok;
  }
};

static void assertSnapshot(const OtaOutcomeJournal& journal,
                           OtaOutcomeKind kind, OtaUpdateCode code,
                           std::uint64_t request_id) {
  const OtaOutcomeSnapshot value = journal.snapshot();
  assert(value.available);
  assert(value.kind == kind);
  assert(value.code == code);
  assert(value.request_id == request_id);
}

static void argumentAndFailureMatrix() {
  Store store;
  OtaOutcomeJournal journal(store);
  assert(!journal.snapshot().available);
  assert(journal.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Missing);
  assert(journal.beginBoot({}, false) ==
         OtaOutcomeJournalCode::InvalidArgument);
  assert(journal.recordTerminal({0U}, OtaUpdateCode::QuiesceFailed,
                                text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::InvalidArgument);
  assert(journal.recordTerminal({1U}, OtaUpdateCode::Ready,
                                text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::InvalidArgument);

  const std::array<OtaUpdateCode, 12U> failures{
      OtaUpdateCode::QuiesceFailed,
      OtaUpdateCode::PlatformUnavailable,
      OtaUpdateCode::VerifierUnavailable,
      OtaUpdateCode::AcquisitionInvalidState,
      OtaUpdateCode::AcquisitionConfigurationRejected,
      OtaUpdateCode::DeadlineExceeded,
      OtaUpdateCode::ManifestFetchFailed,
      OtaUpdateCode::ManifestRejected,
      OtaUpdateCode::ImageOriginMismatch,
      OtaUpdateCode::StagingBeginFailed,
      OtaUpdateCode::ImageFetchFailed,
      OtaUpdateCode::StagingFinishFailed};
  std::uint64_t request_id = 0xFEDCBA9876543000ULL;
  for (const OtaUpdateCode failure : failures) {
    assert(journal.recordTerminal({request_id}, failure,
                                  text("0.4.0-beta.1")) ==
           OtaOutcomeJournalCode::Ok);
    assertSnapshot(journal, OtaOutcomeKind::AcquisitionFailed,
                   failure, request_id);
    ++request_id;
  }
}

static void selectedImageLifecycle() {
  Store pending_store;
  OtaOutcomeJournal pending(pending_store);
  assert(pending.recordTerminal({41U}, OtaUpdateCode::ImageSelected,
                                text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assert(pending.beginBoot(text("0.4.0-beta.2"), true) ==
         OtaOutcomeJournalCode::Ok);
  assertSnapshot(pending, OtaOutcomeKind::ImageSelected,
                 OtaUpdateCode::ImageSelected, 41U);
  assert(pending.recordConfirmed() == OtaOutcomeJournalCode::Ok);
  assertSnapshot(pending, OtaOutcomeKind::Confirmed,
                 OtaUpdateCode::ImageSelected, 41U);
  assert(pending.recordConfirmed() == OtaOutcomeJournalCode::Ok);

  Store rollback_store;
  OtaOutcomeJournal rollback(rollback_store);
  assert(rollback.recordTerminal({42U}, OtaUpdateCode::ImageSelected,
                                 text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assert(rollback.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Ok);
  assertSnapshot(rollback, OtaOutcomeKind::RollbackObserved,
                 OtaUpdateCode::ImageSelected, 42U);

  Store inferred_store;
  OtaOutcomeJournal inferred(inferred_store);
  assert(inferred.recordTerminal({43U}, OtaUpdateCode::ImageSelected,
                                 text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assert(inferred.beginBoot(text("0.4.0-beta.2"), false) ==
         OtaOutcomeJournalCode::Ok);
  assertSnapshot(inferred, OtaOutcomeKind::Confirmed,
                 OtaUpdateCode::ImageSelected, 43U);
}

static void boundedRetentionAndCorruption() {
  Store store;
  OtaOutcomeJournal journal(store);
  assert(journal.recordTerminal({51U}, OtaUpdateCode::ManifestRejected,
                                text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assert(journal.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Ok);
  assert(journal.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Ok);
  assert(journal.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Stale);
  assert(!journal.snapshot().available);
  assert(!store.slots[0].present && !store.slots[1].present);

  Store torn_store;
  OtaOutcomeJournal torn(torn_store);
  assert(torn.recordTerminal({61U}, OtaUpdateCode::ManifestFetchFailed,
                             text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  torn_store.tear_write = true;
  assert(torn.recordTerminal({62U}, OtaUpdateCode::ImageFetchFailed,
                             text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::ReadBackFailed);
  assertSnapshot(torn, OtaOutcomeKind::AcquisitionFailed,
                 OtaUpdateCode::ManifestFetchFailed, 61U);
  torn_store.tear_write = false;
  assert(torn.recordTerminal({62U}, OtaUpdateCode::ImageFetchFailed,
                             text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assertSnapshot(torn, OtaOutcomeKind::AcquisitionFailed,
                 OtaUpdateCode::ImageFetchFailed, 62U);

  // No valid slot is ever published. beginBoot clears a wholly corrupt pair
  // only after the read-only boot audits have admitted journal mutation.
  Store corrupt_store;
  corrupt_store.slots[0].present = true;
  corrupt_store.slots[0].length = kEncodedOtaOutcomeSlotBytes;
  corrupt_store.slots[1] = corrupt_store.slots[0];
  corrupt_store.slots[1].bytes[3] = 1U;
  OtaOutcomeJournal corrupt(corrupt_store);
  assert(corrupt.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Corrupt);
  assert(!corrupt.snapshot().available);
  assert(corrupt_store.clears == 1U);

  Store short_store;
  short_store.slots[0].present = true;
  short_store.slots[0].length = kEncodedOtaOutcomeSlotBytes - 1U;
  OtaOutcomeJournal short_record(short_store);
  assert(short_record.recordTerminal({63U}, OtaUpdateCode::QuiesceFailed,
                                     text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::Ok);
  assert(short_store.clears == 1U);
}

static void ioFailureNeverPublishesInventedOutcome() {
  Store before_store;
  before_store.fail_write_before = true;
  OtaOutcomeJournal before(before_store);
  assert(before.recordTerminal({71U}, OtaUpdateCode::QuiesceFailed,
                               text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::IoError);
  assert(!before.snapshot().available);

  // A commit that reports failure may nevertheless be durable. It remains
  // unpublished in this boot, and only a later authoritative read consumes it.
  Store uncertain_store;
  uncertain_store.fail_write_after = true;
  OtaOutcomeJournal uncertain(uncertain_store);
  assert(uncertain.recordTerminal({72U}, OtaUpdateCode::ManifestFetchFailed,
                                  text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::IoError);
  assert(!uncertain.snapshot().available);
  uncertain_store.fail_write_after = false;
  OtaOutcomeJournal recovered(uncertain_store);
  assert(recovered.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::Ok);
  assertSnapshot(recovered, OtaOutcomeKind::AcquisitionFailed,
                 OtaUpdateCode::ManifestFetchFailed, 72U);

  Store inspect_store;
  inspect_store.fail_inspect = true;
  OtaOutcomeJournal inspect(inspect_store);
  assert(inspect.recordTerminal({73U}, OtaUpdateCode::QuiesceFailed,
                                text("0.4.0-beta.1")) ==
         OtaOutcomeJournalCode::IoError);
  assert(inspect.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::IoError);
  assert(!inspect.snapshot().available);

  Store clear_store;
  clear_store.slots[0].present = true;
  clear_store.slots[0].length = kEncodedOtaOutcomeSlotBytes;
  clear_store.fail_clear = true;
  OtaOutcomeJournal clear(clear_store);
  assert(clear.beginBoot(text("0.4.0-beta.1"), false) ==
         OtaOutcomeJournalCode::IoError);
  assert(!clear.snapshot().available);
}

int main() {
  static_assert(kEncodedOtaOutcomeSlotBytes == 40U);
  static_assert(kMaximumOtaOutcomeBootAge == 2U);
  argumentAndFailureMatrix();
  selectedImageLifecycle();
  boundedRetentionAndCorruption();
  ioFailureNeverPublishesInventedOutcome();
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-outcome-"));
  try {
    const source = join(scratch, "outcome.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-pthread", "-I", main, "-I", join(ota, "include"), source,
      join(main, "ota_outcome_journal.cpp"), "-o", binary,
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

test("OTA outcome journal passes strict lifecycle and failure matrix", () => {
  run(false);
});

test("OTA outcome journal passes ASan/UBSan lifecycle and failure matrix", () => {
  run(true);
});

test("OTA outcome journal is fixed, bounded, credential-free NVS data", () => {
  const header = readFileSync(join(main, "ota_outcome_journal.hpp"), "utf8");
  const source = readFileSync(join(main, "ota_outcome_journal.cpp"), "utf8");
  const combined = header + source;
  assert.match(header, /kEncodedOtaOutcomeSlotBytes = 40U/);
  assert.match(header, /kMaximumOtaOutcomeBootAge = 2U/);
  assert.match(header, /std::array<RawOtaOutcomeSlot, 2U>/);
  assert.match(source, /constexpr char kNvsNamespace\[\] = "ink-ota-out-v1"/);
  assert.match(source, /slot0/);
  assert.match(source, /slot1/);
  assert.match(source, /crc32/);
  assert.match(source, /nvs_commit/);
  const recordStart = header.indexOf("struct PersistentRecord");
  const recordEnd = header.indexOf("private:", recordStart);
  assert.ok(recordStart >= 0 && recordEnd > recordStart);
  const record = header.slice(recordStart, recordEnd);
  assert.match(record, /sequence/);
  assert.match(record, /request_id/);
  assert.match(record, /source_version_fingerprint/);
  assert.match(record, /kind/);
  assert.match(record, /code/);
  assert.match(record, /boot_age/);
  assert.doesNotMatch(
    record + source,
    /https?:|signature|private[_ -]?key|token|authorization|bearer|remote response|ESP_LOG|printf|fprintf/i,
  );
});
