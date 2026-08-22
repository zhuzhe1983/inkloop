import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const main = readFileSync(
  join(repo, "firmware/inkloop-idf/main/app_main.cpp"),
  "utf8",
);
const journal = readFileSync(
  join(repo, "firmware/inkloop-idf/main/ota_outcome_journal.cpp"),
  "utf8",
);

test("normal Portal OTA is gated by boot health and a root-owned request latch", () => {
  assert.match(main, /class PortalOtaUpdateBridge/);
  assert.match(main, /std::atomic<bool> ota_update_allowed\{!ota_pending\}/);
  assert.match(main, /attachPortalFirmwareUpdateOwner\([\s\S]*ota_portal_bridge/);
  assert.match(main, /ota_update_allowed\.store\(true, std::memory_order_release\)/);
  assert.match(main, /ota_update\.take\(ota_request\)/);
  assert.match(main, /PortalFirmwareUpdatePhase::AcceptedOffline/);
  assert.match(
    main,
    /source\.state == OtaUpdateState::Disabled \|\|[\s\S]*source\.state == OtaUpdateState::Idle[\s\S]*outcomes_\.snapshot\(\)/,
  );
  assert.match(main, /PortalFirmwareUpdateCode::UpdateConfirmed/);
  assert.match(main, /PortalFirmwareUpdateCode::UpdateRolledBack/);
  assert.doesNotMatch(
    main,
    /PortalFirmwareUpdatePhase::(?:Checking|Downloading|Staging|RebootPending)/,
  );
});

test("OTA uses two-phase Product quiesce and always closes retained STA before reboot", () => {
  const request = main.slice(main.indexOf("inkloop::OtaUpdateRequest ota_request"));
  const take = request.indexOf("ota_update.take(ota_request)");
  const quiesce = request.indexOf("runtime.shutdownForOtaAcquisition()");
  const acquire = request.indexOf("ota_update.acquire(");
  const journalTerminal = request.indexOf("ota_outcomes.recordTerminal(");
  const close = request.indexOf("runtime.shutdownForRecovery()");
  const reboot = request.indexOf("esp_restart()");
  assert.ok(take >= 0 && quiesce > take && acquire > quiesce);
  assert.ok(journalTerminal > acquire && close > journalTerminal && reboot > close);
  assert.match(request, /ota_update\.fail\([\s\S]*OtaUpdateCode::QuiesceFailed/);
  assert.match(request, /kOtaShutdownAttempts = 8U/);
  assert.match(request, /kOtaOutcomeJournalAttempts = 3U/);
  assert.doesNotMatch(
    request.slice(quiesce, acquire),
    /esp_ota|OtaHttps|systemEspOtaWriterFunctions/,
  );
});

test("OTA outcome mutation begins only after read-only storage gates", () => {
  const boot = main.slice(main.indexOf('extern "C" void app_main'));
  const pending = boot.indexOf("ota_health.tick(");
  const upgradeAudit = boot.indexOf("runReadOnlyUpgradeBootAudit(storage)");
  const compatibility = boot.indexOf("persistenceCompatibilityContractValid()");
  const startupGate = boot.indexOf("upgrade.allowsStartup()");
  const removableAudit = boot.indexOf("runReadOnlyMountedFileUpgradeAudit(");
  const healthy = boot.indexOf("ota_stage.storage_gate_healthy = true");
  const begin = boot.indexOf("ota_outcomes.beginBoot(");
  const product = boot.indexOf("static inkloop::EspProductRuntime runtime(");
  assert.ok(pending >= 0 && upgradeAudit > pending);
  assert.ok(compatibility > upgradeAudit && startupGate > compatibility);
  assert.ok(removableAudit > startupGate && healthy > removableAudit);
  assert.ok(begin > healthy && product > begin);

  const confirmation = boot.indexOf("ota_outcomes.recordConfirmed()");
  const allowed = boot.indexOf(
    "ota_update_allowed.store(true, std::memory_order_release)",
  );
  assert.ok(confirmation > product && allowed > confirmation);
});

test("OTA outcome persistence remains bounded and owns no network path", () => {
  assert.match(journal, /kNvsNamespace\[\] = "ink-ota-out-v1"/);
  assert.match(journal, /kSlotKeys\[2\]\[6\]/);
  assert.doesNotMatch(
    journal,
    /esp_http|httpd_|esp_restart|https?:\/\/|Authorization|Bearer|ESP_LOG/i,
  );
  assert.equal((main.match(/attachPortalFirmwareUpdateOwner/g) ?? []).length, 1);
});

test("Portal OTA state never exposes endpoint, key, token, signature or manifest", () => {
  const bridge = main.slice(
    main.indexOf("class PortalOtaUpdateBridge"),
    main.indexOf("[[noreturn]] void runRecoveryNetwork"),
  );
  assert.doesNotMatch(bridge, /manifest|public.?key|private.?key|signature|token|url/i);
});
