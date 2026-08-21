import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);

function compileAndRun(source, output, sanitizer = false) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", sourceRoot.pathname, source, "-o", output,
  ];
  if (sanitizer) args.unshift("-fno-omit-frame-pointer", "-fsanitize=address,undefined");
  const compiled = spawnSync("c++", args, { encoding: "utf8" });
  assert.equal(compiled.status, 0, compiled.stderr || compiled.stdout);
  const executed = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitizer
      ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS: "halt_on_error=1" }
      : process.env,
  });
  assert.equal(executed.status, 0, executed.stderr || executed.stdout);
}

test("cooperative provisioning preserves legacy timing and remains wrap-safe", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-provisioning-"));
  try {
    const source = join(directory, "provisioning.cpp");
    const output = join(directory, "provisioning");
    await writeFile(source, String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include "WifiProvisioningPrimitives.h"
#include "StorageRecoveryPrimitives.h"

using namespace inkloop;

int main() {
  static_assert(kSavedWifiAttemptMs == 25000, "0.2 saved-network policy changed");
  static_assert(kWifiPortalTimeoutMs == 300000, "0.2 portal policy changed");
  static_assert(kProvisioningSerialLatencyBudgetMs <= 1500, "status latency unbounded");

  WifiProvisioningState state;
  WifiProvisioningActions action = state.start(UINT32_MAX - 10000U, false, true);
  assert(!action.startPortal && state.provisioning());
  action = state.tick(14998U, false, false);
  assert(!action.startPortal);
  action = state.tick(14999U, false, false);
  assert(action.startPortal && state.portalShown());
  assert(action.portalReason == WifiPortalReason::SavedConnectTimeout);
  assert(std::string(wifiPortalReasonName(action.portalReason)) == "SAVED_CONNECT_TIMEOUT");
  action = state.tick(314998U, false, false);
  assert(!action.reportTimeout);
  action = state.tick(314999U, false, false);
  assert(action.stopPortal && action.reportTimeout && !state.provisioning());
  assert(state.phase() == WifiProvisioningPhase::TimedOutDegraded);

  WifiProvisioningState unconfigured;
  action = unconfigured.start(50U, false, false);
  assert(action.startPortal && unconfigured.portalShown());
  assert(action.portalReason == WifiPortalReason::NoCredentials);
  assert(std::string(wifiPortalReasonName(action.portalReason)) == "NO_CREDENTIALS");

  WifiProvisioningState connected;
  action = connected.start(100U, false, true);
  assert(!action.startPortal && !connected.portalShown());
  action = connected.tick(200U, true, false);
  assert(action.startClockSync && !action.finalizeOnline && !action.startPortal);
  assert(!connected.portalShown());
  action = connected.tick(15199U, true, false);
  assert(!action.finalizeOnline);
  action = connected.tick(15200U, true, false);
  assert(action.finalizeOnline && connected.onlineReady());
  action = connected.tick(99999U, true, true);
  assert(!action.finalizeOnline);

  WifiProvisioningState alreadyOnline;
  action = alreadyOnline.start(7U, true, true);
  assert(action.startClockSync && !action.startPortal);
  assert(!alreadyOnline.portalShown());

  const StorageRecoveryState healthy = storageRecoveryState(true, true);
  assert(taskControlAllowed(healthy));
  assert(healthy.assetMode == RecoveryAssetMode::Internal);
  const StorageRecoveryState degraded = storageRecoveryState(false, true);
  assert(!taskControlAllowed(degraded));
  assert(degraded.internalRecoveryRequired && degraded.dataPreserved);
  assert(degraded.assetMode == RecoveryAssetMode::SdAssetsOnly);
  const StorageRecoveryState unavailable = storageRecoveryState(false, false);
  assert(!taskControlAllowed(unavailable));
  assert(unavailable.assetMode == RecoveryAssetMode::Unavailable);
  return 0;
}
`, "utf8");
    compileAndRun(source, output, false);
    compileAndRun(source, `${output}-san`, true);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("main loop services serial around one bounded WiFiManager iteration", async () => {
  const main = await readFile(new URL("main.cpp", sourceRoot), "utf8");
  const provisioning = await readFile(new URL("WifiProvisioningPrimitives.h", sourceRoot), "utf8");
  assert.doesNotMatch(main, /\.autoConnect\s*\(/);
  const pollStart = main.indexOf("void pollWifiProvisioning() {");
  const pollEnd = main.indexOf("\n}", pollStart);
  const body = main.slice(pollStart, pollEnd);
  const firstSerial = body.indexOf("pollSerialConsole();");
  const process = body.indexOf("wifiManager.process();");
  const secondSerial = body.indexOf("pollSerialConsole();", firstSerial + 1);
  assert.ok(firstSerial >= 0 && firstSerial < process && process < secondSerial);
  assert.doesNotMatch(body, /xTaskCreate|std::thread|delay\s*\(/);
  assert.match(main, /setConnectTimeout\(kWifiManagerConnectCallBoundMs \/ 1000U\)/);
  assert.match(main, /setSaveConnectTimeout\(kWifiManagerConnectCallBoundMs \/ 1000U\)/);
  assert.match(main, /getWiFiSSID\(true\)\.length\(\) > 0/);
  assert.match(main, /WiFi\.begin\(\)/);
  assert.match(main, /COMMAND_UNAVAILABLE[\s\S]*WIFI_PROVISIONING/);
  assert.match(main, /readOnlyDuringBoot = command == "help" \|\| command == "status" \|\| command == "diag"/);
  const beginStart = main.indexOf("void beginWifiProvisioning() {");
  const beginEnd = main.indexOf("\nvoid pollWifiProvisioning()", beginStart);
  const beginBody = main.slice(beginStart, beginEnd);
  assert.doesNotMatch(beginBody, /safeShowWifiSetup\s*\(/);
  assert.doesNotMatch(beginBody, /Diagnostics::event\("WIFI_AP"/);
  assert.doesNotMatch(beginBody, /Starting Inkloop|Connecting to saved Wi-Fi|safeShowStatus\s*\(/);
  assert.match(beginBody, /PRESERVED_DURING_WIFI_CONNECT/);
  assert.match(beginBody, /setSaveConfigCallback/);
  assert.match(beginBody, /WIFI_CREDENTIALS[\s\S]*PRESENT[\s\S]*ABSENT/);
  const applyStart = main.indexOf("void applyWifiProvisioningActions(");
  const applyEnd = main.indexOf("\nvoid beginWifiProvisioning()", applyStart);
  const applyBody = main.slice(applyStart, applyEnd);
  assert.ok(applyBody.indexOf("startConfigPortal(") < applyBody.indexOf("showWifiPortal("));
  assert.match(main, /WIFI_PROVISIONING_REASON/);
  assert.match(provisioning, /NO_CREDENTIALS/);
  assert.match(provisioning, /SAVED_CONNECT_TIMEOUT/);
});

test("LittleFS degradation is typed, non-destructive, and gates task control", async () => {
  const [main, storage, taskStore, client, diagnostics, portal, application] = await Promise.all([
    readFile(new URL("main.cpp", sourceRoot), "utf8"),
    readFile(new URL("Storage.cpp", sourceRoot), "utf8"),
    readFile(new URL("TaskStore.cpp", sourceRoot), "utf8"),
    readFile(new URL("InkloopClient.cpp", sourceRoot), "utf8"),
    readFile(new URL("Diagnostics.cpp", sourceRoot), "utf8"),
    readFile(new URL("../lib/InkloopPortal/InkloopPortal.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
  ]);
  const all = [main, storage, taskStore, client].join("\n");
  assert.match(storage, /baseline_\.begin\(false\)/);
  assert.doesNotMatch(all, /LittleFS\.begin\(true\)|LittleFS\.format\s*\(/);
  assert.match(main, /STORAGE_RECOVERY_REQUIRED", "LITTLEFS_MOUNT_FAILED"/);
  assert.match(main, /"SD_ASSETS_ONLY"[\s\S]*"NONE"/);
  assert.match(main, /"TASK_STORE", "UNAVAILABLE_DATA_PRESERVED"/);
  assert.match(main, /Data not erased; SD is images only\. Run serial diag for recovery\./);
  assert.match(main, /if \(storageRecovery\.taskStoreReady\) \{[\s\S]*displayTransaction\.recoverAtBoot\(\)/);
  assert.match(client, /SyncResult InkloopClient::syncTasks\(\) \{[\s\S]*if \(!tasks_\.ready\(\)\)[\s\S]*TASK_SYNC_BLOCKED/);
  for (const signature of [
    "TaskStore::load(", "TaskStore::replace(", "TaskStore::firstDue(",
    "TaskStore::nextDueEpoch(", "TaskStore::markRunWithDay(",
    "TaskStore::acknowledgementPayloadSize(", "TaskStore::isRunAcknowledged(",
  ]) {
    const start = taskStore.indexOf(signature);
    assert.ok(start >= 0, signature);
    assert.match(taskStore.slice(start, start + 500), /if \(!ready\(\)\) return false;/, signature);
  }
  for (const field of [
    "internalMounted", "internalRecoveryRequired", "taskStoreReady",
    "assetBackend", "dataPreserved",
  ]) assert.match(diagnostics, new RegExp(`status\\["${field}"\\]`));
  for (const field of ["internalMounted", "internalRecoveryRequired", "taskStoreReady"]) {
    assert.match(portal, new RegExp(`\\\\"${field}\\\\"`));
    assert.match(application, new RegExp(`status\\.${field} =`));
  }
});
