import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");

const harness = String.raw`
#include <cassert>
#include "inkloop/storage/upgrade_audit.hpp"

using namespace inkloop::storage;

int main() {
  UpgradeAuditInput input;
  assert(auditUpgrade(input).result == UpgradeAuditResult::SourceUnavailable);
  input.internal_mounted = true;
  UpgradeAuditReport report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::Fresh);
  assert(report.allowsInitialization());

  input.application_nvs[0] = RecordProbe::Valid;
  report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::Compatible);
  assert(report.protected_records_present == 1);

  input.tasks.current = RecordProbe::Valid;
  input.tasks.previous = RecordProbe::Valid;
  assert(classifyTransaction(input.tasks) == TransactionAudit::Clean);
  input.tasks.next = RecordProbe::Valid;
  report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::RecoveryRequired);
  assert(!report.allowsInitialization());

  input.tasks.current = RecordProbe::Invalid;
  input.tasks.next = RecordProbe::Valid;
  input.tasks.previous = RecordProbe::Valid;
  report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::Ambiguous);

  input.tasks = TransactionProbe{};
  input.display_transaction = RecordProbe::Valid;
  report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::DisplayResolutionRequired);
  input.display_transaction = RecordProbe::Ambiguous;
  assert(auditUpgrade(input).result ==
         UpgradeAuditResult::DisplayResolutionRequired);

  input.display_transaction = RecordProbe::Missing;
  input.application_nvs[5] = RecordProbe::IoError;
  assert(auditUpgrade(input).result == UpgradeAuditResult::SourceUnavailable);
  input.application_nvs[5] = RecordProbe::Missing;
  input.chat_current = RecordProbe::Unvalidated;
  assert(auditUpgrade(input).result == UpgradeAuditResult::RecoveryRequired);

  static_assert(kProtectedNvsNamespaces.size() == 9);
  static_assert(kProtectedFilePaths.size() == 11);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-audit-"));
  try {
    const source = join(scratch, "audit.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(storage, "include"),
      source, join(storage, "upgrade_audit.cpp"), "-o", binary,
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

test("upgrade audit classifies protected legacy state under strict C++17", () => {
  buildAndRun(false);
});

test("upgrade audit fails closed under ASan/UBSan", () => {
  buildAndRun(true);
});

test("native mount preserves by default and exposes only confirmed TF format", () => {
  const source = readFileSync(join(storage, "esp_storage_mount.cpp"), "utf8");
  const header = readFileSync(join(
    storage, "include/inkloop/storage/esp_storage_mount.hpp"), "utf8");
  const manifest = readFileSync(join(storage, "idf_component.yml"), "utf8");
  const lock = readFileSync(join(repo, "firmware/inkloop-idf/dependencies.lock"), "utf8");
  assert.match(source, /ESP_PARTITION_SUBTYPE_ANY/);
  assert.match(source, /partition->address != config_\.internal_partition_address/);
  assert.match(source, /partition->size != config_\.internal_partition_size/);
  assert.match(source, /mount_config\.format_if_mount_failed = false/);
  assert.match(source, /mount_config\.grow_on_mount = false/);
  assert.match(source, /\.format_if_mount_failed = false/);
  assert.doesNotMatch(source, /esp_littlefs_format|f_mkfs|esp_partition_erase/);
  assert.match(header, /formatSdCardConfirmed\(\)/);
  assert.match(source, /formatSdCardConfirmed\(\)[\s\S]*sd_album_\.active\(\)/);
  assert.match(source, /esp_vfs_fat_sdcard_format\(config_\.sd_base_path, sd_card_\)/);
  assert.match(
    source,
    /esp_vfs_fat_sdcard_format[\s\S]*!updateCapacity\(config_\.sd_base_path,[\s\S]*sd_registered_ = false;[\s\S]*sd_card_ = nullptr;/,
  );
  assert.doesNotMatch(source, /esp_vfs_fat_sdcard_format\([^)]*internal/);
  assert.match(header, /internal_partition_address = 0x00c90000U/);
  assert.match(header, /internal_partition_size = 0x00360000U/);
  assert.match(header, /sd_sclk_gpio = 15/);
  assert.match(header, /sd_miso_gpio = 14/);
  assert.match(header, /sd_mosi_gpio = 13/);
  assert.match(header, /sd_cs_gpio = 47/);
  assert.match(manifest, /==1\.22\.3/);
  assert.match(lock, /joltwallet\/littlefs:/);
  assert.match(lock, /version: 1\.22\.3/);
});

test("native NVS inventory reads all protected namespaces without writes", () => {
  const source = readFileSync(join(storage, "esp_nvs_upgrade_inventory.cpp"), "utf8");
  const header = readFileSync(join(
    storage, "include/inkloop/storage/esp_nvs_upgrade_inventory.hpp"), "utf8");
  const auditHeader = readFileSync(join(
    storage, "include/inkloop/storage/upgrade_audit.hpp"), "utf8");
  const manifest = readFileSync(join(storage, "idf_component.yml"), "utf8");
  const lock = readFileSync(join(repo, "firmware/inkloop-idf/dependencies.lock"), "utf8");
  assert.match(source, /nvs_open\(name, NVS_READONLY/);
  for (const name of [
    "inkloop-v2", "inkloop", "ink-myai-v1", "ink-portal",
    "ink-album-meta", "ink-pair-ui", "nvs.net80211", "phy", "cal_data",
  ]) assert.match(source + auditHeader, new RegExp(name.replace(".", "\\.")));
  assert.match(source, /CredentialPersistenceCore/);
  assert.match(source, /portalEnvelopeValid/);
  assert.match(source, /Sha256/);
  assert.match(header, /kProtectedNvsNamespaces\.size\(\)/);
  assert.doesNotMatch(source, /NVS_READWRITE|nvs_set_|nvs_erase|nvs_commit/);
  assert.match(manifest, /espressif\/cjson:[\s\S]*==1\.7\.19~2/);
  assert.match(lock, /espressif\/cjson:[\s\S]*version: 1\.7\.19~2/);
});
