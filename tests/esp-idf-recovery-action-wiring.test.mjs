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

test("only audit-refusal branches receive the mutation owner", () => {
  const storageActionCalls = appMain.match(/\), &storage\);/g) ?? [];
  assert.equal(storageActionCalls.length, 2);
  assert.match(appMain, /upgrade state needs explicit recovery; refusing all writers/);
  assert.match(appMain, /selected removable backend needs explicit recovery/);
  assert.match(appMain, /RecoveryNetworkModeOwner recovery\(\s*cache, actions\.get\(\),\s*actions\.get\(\)\)/);
});
