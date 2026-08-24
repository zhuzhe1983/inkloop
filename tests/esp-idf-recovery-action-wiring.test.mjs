import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";

const root = process.cwd();
const ownerHeader = fs.readFileSync(path.join(
  root, "firmware/inkloop-idf/main/recovery_action_owner.hpp"), "utf8");
const ownerSource = fs.readFileSync(path.join(
  root, "firmware/inkloop-idf/main/recovery_action_owner.cpp"), "utf8");
const appMain = fs.readFileSync(path.join(
  root, "firmware/inkloop-idf/main/app_main.cpp"), "utf8");
const storageHeader = fs.readFileSync(path.join(
  root,
  "firmware/inkloop-idf/components/inkloop_storage/include/inkloop/storage/esp_storage_mount.hpp"),
"utf8");
const storageSource = fs.readFileSync(path.join(
  root,
  "firmware/inkloop-idf/components/inkloop_storage/esp_storage_mount.cpp"),
"utf8");

test("recovery action composition exposes only fixed typed domains and binds full snapshots", () => {
  assert.match(ownerHeader, /public recovery::IRecoveryActionOwner/);
  assert.match(ownerSource, /LegacyFileTransactionDomain::Tasks/);
  assert.match(ownerSource, /LegacyFileTransactionDomain::Album/);
  assert.match(ownerSource, /RecoveryActionBackend::Internal/);
  assert.match(ownerSource, /RecoveryActionBackend::Removable/);
  assert.match(ownerSource, /request\.choice != recovery::RecoveryActionChoice::Next/);
  assert.match(ownerSource, /removable_album != request\.external_backup_confirmed/);
  assert.match(ownerSource, /LegacyDisplayResolutionChoice::Target/);
  assert.match(ownerSource, /LegacyDisplayResolutionChoice::Previous/);
  assert.match(ownerSource, /hash\.text\(journal\.task_frame_url\)/);
  assert.match(ownerSource, /hash\.u32\(journal\.task_revision\)/);
  assert.match(ownerSource, /LegacyFileTransactionSnapshot/);
  assert.doesNotMatch(ownerSource, /formatSdCardConfirmed|nvs_flash_erase|esp_littlefs_format/);
});

test("successful recovery re-audits exact protected state and delays reboot until after response", () => {
  assert.match(ownerSource, /EspNvsUpgradeInventory/);
  assert.match(ownerSource, /PosixUpgradeInventory/);
  assert.match(ownerSource, /auditUpgrade\(files\.inspect\(nvs\.inspect\(\)\)\)/);
  assert.match(ownerSource, /runReadOnlyMountedFileUpgradeAudit/);
  assert.match(ownerSource, /kRestartResponseGraceMs = 2000U/);
  assert.match(appMain, /actions && actions->restartReady\(now\)/);
  assert.match(appMain, /recovery\.stop\(\)/);
  assert.match(appMain, /esp_restart\(\)/);
  assert.ok(appMain.indexOf("recovery.stop()") < appMain.indexOf("esp_restart()"));
});

test("failed read-only remount permanently revokes actions and latches a forced restart", () => {
  assert.match(ownerHeader, /bool forcedRestartReady\(std::uint32_t now_ms\) const/);
  assert.match(ownerHeader, /std::atomic<bool> actions_available_\{true\}/);
  assert.match(ownerHeader, /std::atomic<bool> forced_restart_latched_\{false\}/);
  assert.match(ownerHeader,
    /std::atomic<std::uint32_t> forced_restart_not_before_ms_\{0U\}/);

  const remountFailure = ownerSource.slice(
    ownerSource.indexOf("const esp_err_t revoked"),
    ownerSource.indexOf("unlock();", ownerSource.indexOf("const esp_err_t revoked")),
  );
  assert.match(remountFailure,
    /revoked != ESP_OK[\s\S]*invalidateActionsAndLatchForcedRestartLocked\(nowMs\(\)\)[\s\S]*Result::IoError/);

  const invalidation = ownerSource.slice(
    ownerSource.indexOf("void EspRecoveryActionOwner::invalidateActionsAndLatchForcedRestartLocked"),
    ownerSource.indexOf("bool EspRecoveryActionOwner::exportSessionMatches"),
  );
  assert.match(invalidation,
    /actions_available_\.store\(false[\s\S]*resetExportLocked\(\)[\s\S]*cached_inventory_ = recovery::RecoveryActionInventory\{\}/);
  assert.match(invalidation,
    /file_snapshot_valid_\.fill\(false\)[\s\S]*display_snapshot_valid_ = false/);
  assert.match(invalidation,
    /restart_not_before_ms_\.store\(0U[\s\S]*forced_restart_not_before_ms_\.store\(now_ms \+ kRestartResponseGraceMs[\s\S]*forced_restart_latched_\.store\(true/);
  assert.match(ownerSource,
    /bool EspRecoveryActionOwner::lockActions\(\)[\s\S]*actions_available_\.load[\s\S]*lock\(\)[\s\S]*actions_available_\.load/);

  const forcedTruth = ownerSource.slice(
    ownerSource.indexOf("bool EspRecoveryActionOwner::forcedRestartReady"),
  );
  assert.match(forcedTruth,
    /forced_restart_latched_\.load[\s\S]*now_ms - deadline[\s\S]*>= 0/);
  assert.match(appMain,
    /actions && actions->forcedRestartReady\(now\)/);
  assert.match(appMain,
    /forced_restart \|\| clean_restart[\s\S]*recovery\.stop\(\)/);
  assert.match(appMain,
    /kMaximumForcedRestartStopAttempts = 8U[\s\S]*forced_restart_stop_attempts[\s\S]*esp_restart\(\)/);

  // The separate latch makes deadline zero valid across uint32 wraparound.
  const ready = (latched, now, deadline) => latched &&
    ((now - deadline) << 0) >= 0;
  assert.equal(ready(false, 2_000, 2_000), false);
  assert.equal(ready(true, 1_999, 2_000), false);
  assert.equal(ready(true, 2_000, 2_000), true);
  assert.equal(ready(true, 0, 0), true);
});

test("typed recovery mutation is one-shot and Product roots stay revoked", () => {
  assert.match(storageHeader,
    /friend class ::inkloop::EspRecoveryActionOwner/);
  assert.match(storageHeader,
    /enum class RecoveryMutationDomain[\s\S]*Display[\s\S]*Tasks[\s\S]*InternalAlbum[\s\S]*RemovableAlbum/);
  assert.match(ownerSource,
    /prepareRecoveryReadOnly\(\) == ESP_OK/);
  assert.ok(ownerSource.indexOf("prepareRecoveryReadOnly() == ESP_OK") <
    ownerSource.indexOf("recoveryReadInternalRoot()"));
  assert.match(ownerSource,
    /sameId\(cached->inspection_id, request\.inspection_id\)/);
  assert.ok(ownerSource.indexOf(
    "sameId(cached->inspection_id, request.inspection_id)") <
    ownerSource.indexOf("beginRecoveryMutation(mutation)"));
  assert.match(ownerSource,
    /beginRecoveryMutation\(mutation\)[\s\S]*endRecoveryMutationAndRemountReadOnly\(\)[\s\S]*postActionAuditClean\(\)/);
  assert.match(storageSource,
    /prepareRecoveryReadOnly[\s\S]*recovery_mode_ = true[\s\S]*internal_registered_[\s\S]*unregisterInternal\(\)[\s\S]*InternalMountAccess::ReadOnly/);
  assert.match(storageHeader,
    /recoveryWritesRevoked[\s\S]*ReadWriteProduct[\s\S]*ReadWriteRecovery/);
  assert.match(storageSource,
    /beginRecoveryMutation[\s\S]*InternalMountAccess::ReadWriteRecovery/);
  assert.match(storageSource,
    /endRecoveryMutationAndRemountReadOnly[\s\S]*unregisterInternal\(\)[\s\S]*InternalMountAccess::ReadOnly/);
  assert.match(storageSource,
    /taskRoot[\s\S]*!recovery_mode_[\s\S]*ReadWriteProduct/);
  assert.match(storageSource,
    /removableRoot[\s\S]*!recovery_mode_/);
});

test("only audit-refusal branches receive the mutation owner", () => {
  const upgradeRefusal = appMain.slice(
    appMain.indexOf("if (!upgrade.allowsStartup())"),
    appMain.indexOf("static inkloop::NativeSettingsMigrationGate"),
  );
  const settingsRefusal = appMain.slice(
    appMain.indexOf("if (settings_audit !="),
    appMain.indexOf("const esp_err_t internal_promotion"),
  );
  const removableRefusal = appMain.slice(
    appMain.indexOf("if (!asset_upgrade.allowsInitialization())"),
    appMain.indexOf("ota_stage.storage_gate_healthy = true",
      appMain.indexOf("if (!asset_upgrade.allowsInitialization())")),
  );
  assert.match(upgradeRefusal,
    /runRecoveryNetwork[\s\S]*&nvs_boot_mount, &storage, true/);
  assert.match(removableRefusal,
    /runRecoveryNetwork[\s\S]*&nvs_boot_mount, &storage, true/);
  assert.match(settingsRefusal, /&nvs_boot_mount, &storage/);
  assert.doesNotMatch(settingsRefusal, /&storage, true/);
  assert.match(settingsRefusal, /&nvs_boot_mount/);
  assert.match(upgradeRefusal,
    /upgrade state needs explicit recovery; refusing RW promotion/);
  assert.match(removableRefusal,
    /selected removable backend needs explicit recovery/);
  assert.match(appMain, /RecoveryNetworkModeOwner recovery\(\s*cache, actions\.get\(\),\s*actions\.get\(\), wifi_storage\)/);
  assert.match(appMain,
    /recovery_actions_available = recovery_actions_requested[\s\S]*nvs_inventory_available[\s\S]*storage_inventory_available/);
});
