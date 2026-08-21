import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const firmware = new URL("../firmware/m5-papercolor/", import.meta.url);

test("button wake preserves the existing e-paper frame before status-page branches", async () => {
  const runtime = await readFile(
    new URL("src/PaperColorApplicationRuntime.cpp", firmware), "utf8");
  const main = await readFile(new URL("src/main.cpp", firmware), "utf8");
  const policyStart = runtime.indexOf(
    "StableStartupDisplay PaperColorApplicationRuntime::stableStartupDisplay() const");
  const policyEnd = runtime.indexOf(
    "void PaperColorApplicationRuntime::noteExternalActivity", policyStart);
  const policy = runtime.slice(policyStart, policyEnd);
  assert.ok(policy.indexOf("wakeAnnouncementPending_") >= 0);
  assert.ok(policy.indexOf("wakeAnnouncementPending_") < policy.indexOf("myAiAuthorized_"));

  const startupStart = main.indexOf("const StableStartupDisplay startupDisplay");
  const ownerStart = main.indexOf("activateDisplayOwner", startupStart);
  const startup = main.slice(startupStart, ownerStart);
  assert.ok(startup.indexOf("StableStartupDisplay::PreserveExisting") >= 0);
  assert.ok(startup.indexOf("StableStartupDisplay::PreserveExisting") <
    startup.indexOf("storageRecovery.internalRecoveryRequired"));
  assert.ok(startup.indexOf("StableStartupDisplay::PreserveExisting") <
    startup.indexOf("safeShowSettingsPortal"));
  assert.doesNotMatch(startup.slice(0, startup.indexOf("storageRecovery.internalRecoveryRequired")),
    /safeShow(?:Status|SettingsPortal|WifiSetup)/);
});

test("button wake acknowledgement uses only LED and optional local audio", async () => {
  const runtime = await readFile(
    new URL("src/PaperColorApplicationRuntime.cpp", firmware), "utf8");
  const wakeStart = runtime.indexOf(
    "if (wakeAnnouncementPending_ && wakeRecovery_.state().readyForUserInput())");
  const wakeEnd = runtime.indexOf("if (wakeIndicatorOffAt_", wakeStart);
  const wake = runtime.slice(wakeStart, wakeEnd);
  assert.match(wake, /LedRole::Voice/);
  assert.match(wake, /device\.restored/);
  assert.doesNotMatch(wake, /refreshFrame|showStatus|showSettingsPortal|showWifiSetup/);
});
