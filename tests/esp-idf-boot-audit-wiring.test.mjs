import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = join(process.cwd(), "firmware", "inkloop-idf");

test("product boot is guarded by a no-erase read-only upgrade inventory", () => {
  const main = readFileSync(join(root, "main", "app_main.cpp"), "utf8");
  const guard = readFileSync(
    join(root, "components", "inkloop_storage", "esp_upgrade_boot_audit.cpp"),
    "utf8",
  );
  assert.match(
    main,
    /runReadOnlyUpgradeBootAudit\(storage, nvs_boot_mount\)/,
  );
  assert.match(main, /if \(!upgrade\.allowsStartup\(\)\)/);
  assert.ok(main.indexOf("runReadOnlyUpgradeBootAudit") < main.indexOf("board_initialize"));
  assert.match(main, /runReadOnlyMountedFileUpgradeAudit\([\s\S]*selected_asset_root/);
  assert.ok(
    main.indexOf("runReadOnlyMountedFileUpgradeAudit") <
      main.indexOf("EspProductRuntime runtime"),
  );
  assert.match(main, /persistenceCompatibilityContractValid\(\)/);
  assert.match(guard, /nvs_flash_init_partition_ptr\(&read_only_partition_\)/);
  assert.match(guard, /read_only_partition_\.readonly = true/);
  assert.match(guard, /mountInternalReadOnly\(\)/);
  assert.match(guard, /nvs\.mountReadOnlyAudit\(\)/);
  assert.match(guard, /EspNvsUpgradeInventory/);
  assert.match(guard, /PosixUpgradeInventory/);
  assert.match(guard, /auditUpgrade/);
  assert.match(guard, /runReadOnlyMountedFileUpgradeAudit/);
  const audit = guard.slice(
    guard.indexOf("EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit"),
    guard.indexOf("UpgradeAuditReport runReadOnlyMountedFileUpgradeAudit"),
  );
  assert.doesNotMatch(
    audit,
    /nvs_flash_init\(\)|nvs_flash_erase|format|remove|unlink|rename|nvs_set|nvs_commit/,
  );
});

test("every post-audit Recovery entry demotes Product RW NVS back to physical readonly", () => {
  const main = readFileSync(join(root, "main", "app_main.cpp"), "utf8");
  const header = readFileSync(
    join(root, "components", "inkloop_storage", "include", "inkloop", "storage", "esp_upgrade_boot_audit.hpp"),
    "utf8",
  );
  const owner = readFileSync(
    join(root, "components", "inkloop_storage", "esp_upgrade_boot_audit.cpp"),
    "utf8",
  );
  assert.match(header, /prepareRecoveryReadOnlyOrDeinit\(\)/);
  assert.match(header, /recoveryReadOnlyReady\(\)/);
  assert.match(header, /recoveryWritesRevoked\(\)/);
  assert.match(
    main,
    /runRecoveryNetwork\([\s\S]*prepareRecoveryReadOnlyOrDeinit\(\)[\s\S]*recoveryReadOnlyReady\(\)/,
  );
  const demote = owner.slice(
    owner.indexOf("EspNvsBootMountOwner::prepareRecoveryReadOnlyOrDeinit"),
    owner.indexOf("EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit"),
  );
  assert.match(demote, /nvs_flash_deinit_partition\(kDefaultNvsLabel\)/);
  assert.match(demote, /nvs_initialized_ = false/);
  assert.match(demote, /mountReadOnlyAudit\(\)/);
  assert.match(demote, /NvsBootMountAccess::RecoveryRequired/);
  assert.doesNotMatch(demote, /nvs_flash_init\(\)|nvs_flash_erase|nvs_set|nvs_commit/);
});

test("failed NVS deinit preserves the live Product writer truth", () => {
  const header = readFileSync(
    join(root, "components", "inkloop_storage", "include", "inkloop", "storage", "esp_upgrade_boot_audit.hpp"),
    "utf8",
  );
  const owner = readFileSync(
    join(root, "components", "inkloop_storage", "esp_upgrade_boot_audit.cpp"),
    "utf8",
  );
  const revoked = header.slice(
    header.indexOf("bool recoveryWritesRevoked() const"),
    header.indexOf("private:", header.indexOf("bool recoveryWritesRevoked() const")),
  );
  assert.match(
    revoked,
    /access_ == NvsBootMountAccess::ReadWriteProduct\) return false/,
  );
  assert.match(
    revoked,
    /access_ == NvsBootMountAccess::ReadOnlyAudit \|\| !nvs_initialized_/,
  );

  const demote = owner.slice(
    owner.indexOf("EspNvsBootMountOwner::prepareRecoveryReadOnlyOrDeinit"),
    owner.indexOf("EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit"),
  );
  assert.match(
    demote,
    /const esp_err_t unmounted =[\s\S]*nvs_flash_deinit_partition\(kDefaultNvsLabel\);\s*if \(unmounted != ESP_OK\) \{[\s\S]*return unmounted;\s*\}\s*nvs_initialized_ = false;/,
  );
  const failedDeinit = demote.slice(
    demote.indexOf("if (unmounted != ESP_OK)"),
    demote.indexOf("nvs_initialized_ = false"),
  );
  assert.doesNotMatch(failedDeinit, /nvs_initialized_ = false/);
  assert.doesNotMatch(failedDeinit, /access_\s*=/);
  assert.doesNotMatch(failedDeinit, /memset/);
});

test("every post-audit Recovery entry revokes Product LittleFS writes independently of actions", () => {
  const main = readFileSync(join(root, "main", "app_main.cpp"), "utf8");
  const storageHeader = readFileSync(
    join(root, "components", "inkloop_storage", "include", "inkloop", "storage", "esp_storage_mount.hpp"),
    "utf8",
  );
  const storageSource = readFileSync(
    join(root, "components", "inkloop_storage", "esp_storage_mount.cpp"),
    "utf8",
  );
  assert.match(storageHeader, /prepareRecoveryReadOnly\(\)/);
  assert.match(storageHeader, /recoveryWritesRevoked\(\)/);
  assert.match(storageHeader, /recoveryReadOnlyReady\(\)/);
  assert.match(main,
    /storage->prepareRecoveryReadOnly\(\)[\s\S]*storage->recoveryWritesRevoked\(\)/);
  assert.match(main,
    /nvs_writes_revoked && storage_writes_revoked/);
  assert.match(main,
    /all_product_writes_revoked \? ESP_OK : ESP_ERR_INVALID_STATE/);
  assert.match(main,
    /recovery_actions_requested[\s\S]*recovery_actions_available \? storage : nullptr/);
  const afterStorageOwner = main.slice(
    main.indexOf("static inkloop::storage::EspStorageMountOwner storage"),
  );
  assert.doesNotMatch(afterStorageOwner,
    /runRecoveryNetwork\([\s\S]{0,420}&nvs_boot_mount\s*\);/);
  assert.match(afterStorageOwner,
    /runRecoveryAfterProductFailure\([\s\S]{0,500}nvs_boot_mount,[\s\S]{0,80}storage,/);
  const demote = storageSource.slice(
    storageSource.indexOf("EspStorageMountOwner::prepareRecoveryReadOnly"),
    storageSource.indexOf("EspStorageMountOwner::beginRecoveryMutation"),
  );
  assert.match(demote, /recovery_mode_ = true/);
  assert.match(demote, /unregisterInternal\(\)/);
  assert.match(demote, /mountInternalWithAccess\(InternalMountAccess::ReadOnly\)/);
});

test("PaperColor TF storage joins the board-owned shared SPI2 bus", () => {
  const header = readFileSync(
    join(root, "components", "inkloop_storage", "include", "inkloop", "storage", "esp_storage_mount.hpp"),
    "utf8",
  );
  const source = readFileSync(
    join(root, "components", "inkloop_storage", "esp_storage_mount.cpp"),
    "utf8",
  );
  assert.match(header, /sd_spi_host = SPI2_HOST/);
  assert.match(header, /sd_bus_already_initialized = true/);
  assert.match(source, /if \(!config_\.sd_bus_already_initialized\)/);
  assert.match(source, /if \(sd_bus_owned_\) spi_bus_free/);
  assert.doesNotMatch(header, /SPI3_HOST/);
});
