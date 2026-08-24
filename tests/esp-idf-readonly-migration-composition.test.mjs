import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf");
const main = readFileSync(join(idf, "main/app_main.cpp"), "utf8");
const storage = readFileSync(join(
  idf, "components/inkloop_storage/esp_storage_mount.cpp"), "utf8");
const audit = readFileSync(join(
  idf, "components/inkloop_storage/esp_upgrade_boot_audit.cpp"), "utf8");
const runner = readFileSync(join(
  idf, "components/inkloop_product/native_settings_migration_gate.cpp"),
"utf8");

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `${signature} missing`);
  const open = source.indexOf("{", start);
  let depth = 0;
  for (let at = open; at < source.length; at += 1) {
    if (source[at] === "{") depth += 1;
    if (source[at] === "}") depth -= 1;
    if (depth === 0) return source.slice(open + 1, at);
  }
  assert.fail(`${signature} unterminated`);
}

test("boot selects settings migration entirely read-only before either RW promotion", () => {
  const boot = main.slice(main.indexOf('extern "C" void app_main'));
  const upgrade = boot.indexOf(
    "runReadOnlyUpgradeBootAudit(storage, nvs_boot_mount)");
  const startupSelection = boot.indexOf("if (!upgrade.allowsStartup())");
  const settingsSelection = boot.indexOf(
    "settings_migration.auditReadOnly(settings_migration_plan)");
  const littlefsPromotion = boot.indexOf("storage.promoteInternalReadWrite()");
  const nvsPromotion = boot.indexOf("nvs_boot_mount.promoteReadWriteProduct()");
  const settingsExecution = boot.indexOf("settings_migration.execute(");
  const authorizationCheck = boot.indexOf("!settings_authorization.valid()");
  const deviceInitialize = boot.indexOf(
    "device_state.initialize(settings_authorization)");
  const product = boot.indexOf("static inkloop::EspProductRuntime runtime(");
  assert.ok(upgrade >= 0 && startupSelection > upgrade);
  assert.ok(settingsSelection > startupSelection);
  assert.ok(littlefsPromotion > settingsSelection);
  assert.ok(nvsPromotion > littlefsPromotion);
  assert.ok(settingsExecution > nvsPromotion);
  assert.ok(authorizationCheck > settingsExecution);
  assert.ok(deviceInitialize > authorizationCheck);
  assert.ok(product > deviceInitialize);
  for (const stage of [
    "settings_migration_audit", "internal_rw_promotion",
    "nvs_rw_promotion", "settings_migration_execution",
  ]) assert.match(boot, new RegExp(`failPendingBoot\\(\"${stage}\"\\)`));
});

test("NVS and LittleFS audit mounts are physically read-only and hide writer roots", () => {
  const nvsMount = body(audit, "EspNvsBootMountOwner::mountReadOnlyAudit");
  assert.match(nvsMount, /read_only_partition_ = \*partition/);
  assert.match(nvsMount, /read_only_partition_\.readonly = true/);
  assert.match(nvsMount, /partitionIsBlank/);
  assert.match(nvsMount, /nvs_flash_init_partition_ptr/);
  assert.doesNotMatch(
    nvsMount,
    /nvs_flash_init\(\)|nvs_set_|nvs_commit|nvs_flash_erase|esp_partition_(?:write|erase)/,
  );
  const nvsPromote = body(audit,
    "EspNvsBootMountOwner::promoteReadWriteProduct");
  assert.match(
    nvsPromote,
    /nvs_flash_deinit_partition[\s\S]*nvs_flash_init\(\)/,
  );

  const fsMount = body(storage, "EspStorageMountOwner::mountInternalWithAccess");
  assert.match(
    fsMount,
    /mount_config\.read_only = access == InternalMountAccess::ReadOnly/,
  );
  assert.match(fsMount, /format_if_mount_failed = false/);
  assert.match(fsMount, /grow_on_mount = false/);
  assert.match(fsMount, /internal_readonly_partition_ = \*partition/);
  assert.match(fsMount, /internal_readonly_partition_\.readonly = true/);
  assert.match(fsMount, /mount_config\.partition_label = nullptr/);
  assert.match(fsMount,
    /mount_config\.partition = &internal_readonly_partition_/);
  const fsPromote = body(storage,
    "EspStorageMountOwner::promoteInternalReadWrite");
  assert.match(
    fsPromote,
    /unregisterInternal\(\)[\s\S]*mountInternalWithAccess\(InternalMountAccess::ReadWriteProduct\)/,
  );
  assert.match(body(storage, "EspStorageMountOwner::taskRoot"),
    /!recovery_mode_[\s\S]*InternalMountAccess::ReadWriteProduct[\s\S]*snapshot_\.internal\.healthy\(\)/);
  assert.match(body(storage, "EspStorageMountOwner::auditInternalRoot"),
    /InternalMountAccess::ReadOnly[\s\S]*!snapshot_\.internal\.writable/);
});

test("settings marker runner binds source, target generation and every durable phase", () => {
  const selection = body(runner, "NativeSettingsMigrationGate::compose");
  assert.match(selection, /decodeFingerprint/);
  assert.match(selection, /marker\.source_fingerprint != fingerprint/);
  assert.match(selection, /marker\.generation/);
  assert.match(selection, /MarkerCorrupt/);
  assert.match(selection, /RollbackRequired/);
  assert.match(selection, /RecoverPreparedHead/);
  assert.match(selection, /matchesHistoricalIncompleteImport/);
  assert.match(selection, /nativeRollbackFor/);
  assert.match(selection, /decodeMigrationJournalSlotV1/);

  const execute = body(runner, "NativeSettingsMigrationGate::execute");
  assert.match(execute, /samePlan\(authorized_plan, fresh\)/);
  assert.match(execute, /recoverPreparedHead/);
  assert.match(execute, /MigrationPhase::Prepared/);
  const advance = body(runner, "NativeSettingsMigrationGate::advance");
  for (const phase of [
    "Prepared", "TargetWritten", "TargetVerified", "CommitRecorded",
    "Complete", "RollbackRequired",
  ]) assert.match(advance, new RegExp(`MigrationPhase::${phase}`));
  assert.match(
    advance,
    /saveRollbackCompatibleSettings\([\s\S]*settings_store_, settings_extension_store_[\s\S]*!main_projection_written/,
  );
  assert.match(advance, /MigrationMarkerJournalCore\(marker_store_\)\.commit/);
});

test("post-audit Recovery is reachable without NVS and never repairs or promotes", () => {
  const recovery = main.slice(
    main.indexOf("[[noreturn]] void runRecoveryNetwork("),
    main.indexOf("[[noreturn]] void runEarlyOtaRecoveryNetwork("),
  );
  assert.match(recovery,
    /nvs_boot_mount->prepareRecoveryReadOnlyOrDeinit\(\)/);
  assert.match(recovery, /nvs_boot_mount->recoveryReadOnlyReady\(\)/);
  assert.match(recovery,
    /storage->prepareRecoveryReadOnly\(\)/);
  assert.match(recovery, /storage->recoveryWritesRevoked\(\)/);
  assert.match(recovery, /storage->recoveryReadOnlyReady\(\)/);
  assert.match(recovery,
    /recovery_actions_available \? storage : nullptr/);
  assert.doesNotMatch(recovery,
    /nvs_flash_init|nvs_flash_erase|promoteReadWrite|mountInternal/);
  assert.match(recovery,
    /RecoveryWifiStoragePolicy::VolatileRam/);

  const early = body(main, "void runEarlyOtaRecoveryNetwork(");
  assert.doesNotMatch(early,
    /nvs_flash_init|nvs_flash_erase|nvs_set_|nvs_commit|PersistentFlash/);
  assert.match(early, /diagnostic, ESP_OK, nullptr/);
  assert.match(early, /RecoveryWifiStoragePolicy::VolatileRam/);
  const boot = main.slice(main.indexOf('extern "C" void app_main'));
  assert.match(boot,
    /OtaHealthRefused[\s\S]*runEarlyOtaRecoveryNetwork|runEarlyOtaRecoveryNetwork[\s\S]*OtaHealthRefused/);
  assert.doesNotMatch(boot.slice(boot.indexOf("runReadOnlyUpgradeBootAudit")),
    /runEarlyOtaRecoveryNetwork/);
});

test("read-only preflight contains no settings or marker commit capability", () => {
  const preflight = body(runner,
    "NativeSettingsMigrationGate::auditReadOnly");
  const collect = body(runner, "NativeSettingsMigrationGate::collect");
  const compose = body(runner, "NativeSettingsMigrationGate::compose");
  for (const source of [preflight, collect, compose]) {
    assert.doesNotMatch(
      source,
      /\.save\(|\.commit\(|writeSlotAndCommit|writeHeadAndMarkerAndCommit|nvs_set_|nvs_commit/,
    );
  }
});
