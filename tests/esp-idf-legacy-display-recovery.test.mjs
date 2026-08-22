import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const cjson = join(
  repo,
  "firmware/inkloop-idf/managed_components/espressif__cjson/cJSON",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <string>

#include "inkloop/storage/legacy_display_recovery.hpp"

using namespace inkloop::storage;

class FakeSource final : public ILegacyDisplayRecoverySource {
 public:
  LegacyDisplayRecoverySnapshot snapshot;
  LegacyDisplayRecoveryProbe inspect(
      LegacyDisplayRecoverySnapshot& output) const override {
    output = snapshot;
    return snapshot.probe;
  }
};

class FakeAdapter final : public ILegacyDisplayResolutionAdapter {
 public:
  LegacyDisplayResolutionAdapterCode target =
      LegacyDisplayResolutionAdapterCode::Ok;
  LegacyDisplayResolutionAdapterCode task =
      LegacyDisplayResolutionAdapterCode::Ok;
  LegacyDisplayResolutionAdapterCode clear =
      LegacyDisplayResolutionAdapterCode::Ok;
  std::string order;

  LegacyDisplayResolutionAdapterCode applyTargetCurrent(
      const LegacyDisplayJournal&) override {
    order.push_back('A');
    return target;
  }
  LegacyDisplayResolutionAdapterCode acknowledgeTask(
      const LegacyDisplayJournal&) override {
    order.push_back('T');
    return task;
  }
  LegacyDisplayResolutionAdapterCode clearJournalSet() override {
    order.push_back('C');
    return clear;
  }
};

std::string journal(unsigned stage, bool task, char asset = 'a') {
  const std::string id(64, asset);
  const std::string previous(64, 'b');
  std::string value =
      "{\"schema\":1,\"stage\":" + std::to_string(stage) +
      ",\"backend\":\"sd\",\"assetId\":\"" + id +
      "\",\"assetPath\":\"/inkloop-album/" + id +
      ".png\",\"previousCurrent\":\"" + previous +
      "\",\"operation\":\"" + (task ? "task" : "page") +
      "\",\"bytes\":363024,\"landscape\":false,\"page\":2,"
      "\"hasTask\":" + (task ? "true" : "false") +
      ",\"runAt\":" + (task ? "1787440000" : "0") +
      ",\"runDay\":2026234";
  if (task) {
    value += ",\"taskId\":\"dtask-one\",\"taskRevision\":7,"
        "\"taskFrameUrl\":\"https://inkloop.mess.host/frame/one\","
        "\"taskFrameHash\":\"" + id + "\"";
  }
  return value + "}";
}

int main() {
  std::array<RawLegacyDisplayRecord, 3> records{};
  LegacyDisplayRecoverySnapshot snapshot;
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::Empty);

  records[1].present = true;
  records[1].bytes = journal(1, true, 'c');
  records[2].present = true;
  records[2].bytes = journal(2, false, 'd');
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::Recoverable);
  assert(snapshot.selected_slot == LegacyDisplayRecordSlot::Next);
  assert(snapshot.journal.asset_id == std::string(64, 'c'));
  assert(snapshot.journal.has_task && snapshot.journal.task_revision == 7U);

  FakeSource source;
  source.snapshot = snapshot;
  FakeAdapter adapter;
  assert(executeLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, source, adapter) ==
      LegacyDisplayResolutionCode::Ok);
  assert(adapter.order == "ATC");
  adapter = FakeAdapter{};
  adapter.task = LegacyDisplayResolutionAdapterCode::IoError;
  assert(executeLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, source, adapter) ==
      LegacyDisplayResolutionCode::TaskCommitFailed);
  assert(adapter.order == "AT");
  adapter = FakeAdapter{};
  source.snapshot.journal.run_day++;
  assert(executeLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, source, adapter) ==
      LegacyDisplayResolutionCode::SourceChanged);
  assert(adapter.order.empty());
  source.snapshot = snapshot;

  // A valid current record is authoritative even with a corrupt candidate.
  records[0].present = true;
  records[0].bytes = journal(3, false, 'e');
  records[1].bytes = "bad";
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::Recoverable);
  assert(snapshot.selected_slot == LegacyDisplayRecordSlot::Current);
  assert(snapshot.journal.asset_id == std::string(64, 'e'));

  LegacyDisplayResolutionPlan plan;
  assert(planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, plan));
  assert(plan.apply_target_current && !plan.acknowledge_task &&
         plan.clear_journal_set);
  assert(planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Previous, plan));
  assert(!plan.apply_target_current && !plan.acknowledge_task &&
         plan.clear_journal_set);

  records[0].bytes = journal(5, false);
  records[1] = RawLegacyDisplayRecord{};
  records[2] = RawLegacyDisplayRecord{};
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::Recoverable);
  assert(!planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, plan));
  assert(planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Previous, plan));

  records[0].bytes = "{}";
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::Corrupt);
  assert(!planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Target, plan));
  assert(planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Previous, plan));

  records[0].io_error = true;
  assert(inspectLegacyDisplayRecovery(records, snapshot) ==
         LegacyDisplayRecoveryProbe::IoError);
  assert(!planLegacyDisplayResolution(
      snapshot, LegacyDisplayResolutionChoice::Previous, plan));

  const std::string malformed[] = {
      journal(0, false), journal(6, false),
      std::string("{\"schema\":1,\"stage\":1}"),
      journal(1, true).substr(0, journal(1, true).size() - 1U),
  };
  for (const std::string& value : malformed) {
    records = {};
    records[0].present = true;
    records[0].bytes = value;
    assert(inspectLegacyDisplayRecovery(records, snapshot) ==
           LegacyDisplayRecoveryProbe::Corrupt);
  }
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-display-recovery-"));
  try {
    const source = join(scratch, "display_recovery.cpp");
    const cObject = join(scratch, "cJSON.o");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-Wno-deprecated-declarations", ...sanitizer,
      "-I", cjson, "-c", join(cjson, "cJSON.c"), "-o", cObject,
    ], { stdio: "pipe" });
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer, "-I", join(storage, "include"), "-I", cjson,
      source, join(storage, "legacy_display_recovery.cpp"), cObject,
      "-lm", "-o", binary,
    ], { stdio: "pipe" });
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

test("legacy display journal requires an explicit bounded resolution", () => {
  buildAndRun(false);
});

test("legacy display recovery parser survives ASan/UBSan", () => {
  buildAndRun(true);
});
