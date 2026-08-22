import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_storage",
);

const harness = String.raw`
#include <cassert>
#include <cstring>

#include "inkloop/storage/persistence_compatibility.hpp"

using namespace inkloop::storage;

int main() {
  assert(persistenceCompatibilityContractValid());
  const auto& contract = persistenceCompatibilityContract();
  assert(contract.size() == 20U);
  for (const auto& entry : contract) {
    assert(upgradeRecordIdValid(entry.record));
    assert(std::strcmp(upgradeRecordName(entry.record), entry.name) == 0);
    assert(entry.native_consumer && entry.native_consumer[0]);
    assert(persistenceCompatibilityEntry(entry.record) == &entry);
  }
  assert(persistenceCompatibilityEntry({UpgradeRecordDomain::File, 11U}) ==
         nullptr);
  for (std::size_t at = 3U; at <= 5U; ++at) {
    const auto* entry = persistenceCompatibilityEntry(
        {UpgradeRecordDomain::File, at});
    assert(entry && entry->mode ==
        PersistenceCompatibilityMode::ExplicitPhysicalResolution);
  }
  assert(persistenceCompatibilityEntry(
      {UpgradeRecordDomain::NvsNamespace, 3U})->mode ==
      PersistenceCompatibilityMode::ReadOnlyImportRetained);
  assert(persistenceCompatibilityEntry(
      {UpgradeRecordDomain::NvsNamespace, 6U})->mode ==
      PersistenceCompatibilityMode::EspSystemShared);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-persistence-contract-"));
  try {
    const source = join(scratch, "contract.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(component, "include"),
      source,
      join(component, "persistence_compatibility.cpp"),
      join(component, "upgrade_snapshot_collector.cpp"),
      join(component, "upgrade_evidence_composer.cpp"),
      join(component, "upgrade_recovery_planner.cpp"),
      join(component, "upgrade_audit.cpp"),
      join(component, "sha256.cpp"),
      "-o",
      binary,
    ];
    if (sanitized) {
      args.splice(1, 0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer");
    }
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

test("all protected records have an exact rollback compatibility policy", () => {
  buildAndRun(false);
});

test("persistence compatibility contract survives ASan/UBSan", () => {
  buildAndRun(true);
});
