import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);

function compileAndRun(source, output, sanitizer) {
  const args = [
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-I",
    sourceRoot.pathname,
    source,
    "-o",
    output,
  ];
  if (sanitizer) {
    args.unshift(
      "-fno-omit-frame-pointer",
      "-fsanitize=address,undefined",
      "-O1",
      "-g",
    );
  }
  const compiled = spawnSync("c++", args, { encoding: "utf8" });
  assert.equal(compiled.status, 0, compiled.stderr || compiled.stdout);
  const executed = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitizer
      ? {
          ...process.env,
          ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
          UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
        }
      : process.env,
  });
  assert.equal(executed.status, 0, executed.stderr || executed.stdout);
}

test("Portal NVS classification and MyAI ingress caps fail closed under adversarial sizes", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-portal-remediation-"));
  try {
    const source = join(directory, "remediation.cpp");
    const executable = join(directory, "remediation");
    await writeFile(source, String.raw`
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "MyAiIngressPrimitives.h"
#include "MyAiCredentialPersistencePrimitives.h"
#include "PortalPersistencePrimitives.h"

using namespace inkloop;

PortalStorageProbe validProbe(bool marker) {
  PortalStorageProbe probe;
  probe.namespaceAvailable = true;
  probe.markerPresent = marker;
  probe.markerValid = marker;
  probe.headPresent = true;
  probe.headValid = true;
  probe.slotAPresent = true;
  probe.headSlotValid = true;
  return probe;
}

int main() {
  MyAiCredentialStorageProbe credentials;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Unavailable);
  credentials.namespaceAvailable = true;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Absent);
  credentials.slot1Present = true;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Corrupt);
  credentials.headPresent = true;
  credentials.headValid = true;
  credentials.committedSlotValid = true;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::LoadedLegacy);
  credentials.markerPresent = true;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Corrupt);
  credentials.markerValid = true;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Loaded);
  credentials.committedSlotValid = false;
  assert(classifyMyAiCredentialStorage(credentials) ==
      MyAiCredentialLoadResult::Corrupt);

  PortalStorageProbe probe;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Unavailable);
  assert(!portalStorageMayInitializeFresh(
      PortalSnapshotLoadResult::Unavailable,
      PortalIdentityState::Unconfigured));

  probe.namespaceAvailable = true;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Absent);
  assert(portalStorageMayInitializeFresh(
      PortalSnapshotLoadResult::Absent,
      PortalIdentityState::Unconfigured));
  assert(!portalStorageMayInitializeFresh(
      PortalSnapshotLoadResult::Absent,
      PortalIdentityState::BoundActive));

  probe.markerPresent = true;
  probe.markerValid = true;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Corrupt);
  probe.markerValid = false;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Corrupt);

  probe = validProbe(false);
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::LoadedLegacy);
  assert(!portalStorageMayInitializeFresh(
      PortalSnapshotLoadResult::LoadedLegacy,
      PortalIdentityState::Unconfigured));

  assert(portalIdentityMatchesSnapshot(
      PortalIdentityState::Unconfigured, false, false, false, false));
  assert(!portalIdentityMatchesSnapshot(
      PortalIdentityState::Unconfigured, false, false, true, true));
  assert(portalIdentityMatchesSnapshot(
      PortalIdentityState::Pairing, false, false, true, true));
  assert(portalIdentityMatchesSnapshot(
      PortalIdentityState::Pairing, true, false, false, false));
  assert(portalIdentityMatchesSnapshot(
      PortalIdentityState::BoundActive, true, false, true, true));
  assert(!portalIdentityMatchesSnapshot(
      PortalIdentityState::BoundActive, false, false, true, true));

  probe = validProbe(true);
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Loaded);
  probe.headSlotValid = false;
  probe.slotBPresent = true;
  probe.fallbackSlotValid = true;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Loaded);
  probe.fallbackSlotValid = false;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Corrupt);
  probe.headValid = false;
  assert(classifyPortalStorage(probe) == PortalSnapshotLoadResult::Corrupt);

  const uint8_t byte = 0;
  assert(!acceptMyAiIngressFrame(MyAiIngressFrameKind::Text, nullptr, 0));
  assert(!acceptMyAiIngressFrame(MyAiIngressFrameKind::Audio, nullptr, 1));
  assert(acceptMyAiIngressFrame(MyAiIngressFrameKind::Text, &byte, 1));
  assert(acceptMyAiIngressFrame(
      MyAiIngressFrameKind::Text, &byte,
      kMaximumMyAiWebSocketTextFrameBytes));
  assert(!acceptMyAiIngressFrame(
      MyAiIngressFrameKind::Text, &byte,
      kMaximumMyAiWebSocketTextFrameBytes + 1));
  assert(acceptMyAiIngressFrame(
      MyAiIngressFrameKind::Audio, &byte,
      kMaximumMyAiWebSocketAudioFrameBytes));
  assert(!acceptMyAiIngressFrame(
      MyAiIngressFrameKind::Audio, &byte,
      kMaximumMyAiWebSocketAudioFrameBytes + 1));
  assert(!acceptMyAiIngressFrame(
      MyAiIngressFrameKind::Audio, &byte,
      std::numeric_limits<size_t>::max()));
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-sanitized`, true);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("concrete Portal/MyAI adapters enforce typed recovery, terminal scrub, and pre-allocation frame checks", async () => {
  const [portal, application, adapters, header, main, statusPng, portalCore] = await Promise.all([
    readFile(new URL("PaperColorPortalRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorMyAiAdapters.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorPortalRuntime.h", sourceRoot), "utf8"),
    readFile(new URL("main.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorStatusPng.cpp", sourceRoot), "utf8"),
    readFile(new URL("../lib/InkloopPortal/InkloopPortal.cpp", sourceRoot), "utf8"),
  ]);

  assert.match(header, /PortalSnapshotLoadResult loadSnapshot/);
  assert.match(portal, /preferences\.begin\(kPortalNvsNamespace, false\)/);
  assert.match(portal, /preferences\.isKey\(kPortalInitialized\)/);
  assert.match(portal, /PortalSnapshotLoadResult::LoadedLegacy/);
  assert.match(portal, /EXISTING_IDENTITY_WITHOUT_PORTAL/);
  assert.match(portal, /SNAPSHOT_CORRUPT/);
  assert.match(portal, /verifyInkloopBinding[\s\S]*BOUND_RECONCILIATION_REQUIRED/);
  assert.match(portal, /BOUND_RECONCILIATION_COMMIT_FAILED/);
  assert.match(portal, /portal-recover-bound/);
  assert.match(portal, /kPortalInitializedValue/);

  const initialization = application.slice(
    application.indexOf("bool PaperColorApplicationRuntime::begin"),
    application.indexOf("bool PaperColorApplicationRuntime::activateDisplayOwner"),
  );
  assert.ok(initialization.indexOf("myAi_.initialize()") >= 0);
  assert.ok(initialization.indexOf("myAi_.initialize()") < initialization.indexOf("portal_.begin("));
  assert.match(initialization, /PortalIdentityState::Unconfigured/);
  assert.match(initialization, /PortalIdentityState::BoundInactive/);
  assert.match(application, /recoverPortalBoundState[\s\S]*ActivationState::Bound[\s\S]*ActivationState::PaymentRequired[\s\S]*syncTasks\(\)[\s\S]*inkloopClient_\.paired\(\)/);
  assert.match(application, /%02X:%02X:%02X:%02X:%02X:%02X/);
  assert.match(application, /config\.macAddress = macAddress/);
  assert.match(main, /portal-recover-bound/);
  assert.match(main, /snapshot\.pairingCode = client\.paired\(\) \? String\(\) : client\.pairingCode\(\)/);
  assert.match(main, /client\.paired\(\)[\s\S]*BOUND_NO_CODE/);

  const onEvent = adapters.slice(
    adapters.indexOf("void Esp32MyAiWebSocket::onEvent"),
    adapters.indexOf("void Esp32MyAiWebSocket::rejectIngress"),
  );
  assert.ok(onEvent.indexOf("acceptMyAiIngressFrame") >= 0);
  assert.ok(onEvent.indexOf("acceptMyAiIngressFrame") < onEvent.indexOf("std::string("));
  assert.ok(onEvent.indexOf("acceptMyAiIngressFrame") < onEvent.indexOf("onWebSocketBinary(payload, length)"));
  assert.match(onEvent, /rejectIngress\(listener, 1009, "text_frame_too_large"\)/);
  assert.match(onEvent, /rejectIngress\(listener, 1009, "audio_frame_too_large"\)/);
  assert.match(onEvent, /WStype_FRAGMENT_TEXT_START[\s\S]*fragmented_frame_rejected/);
  assert.match(adapters, /PaperColorStreamingAudio::write[\s\S]*length > kMaximumMyAiWebSocketAudioFrameBytes/);

  const credentialLoad = adapters.slice(
    adapters.indexOf("Status NvsMyAiCredentialStore::load"),
    adapters.indexOf("Status NvsMyAiCredentialStore::store"),
  );
  assert.match(credentialLoad, /preferences\.begin\(kMyAiCredentialNamespace, false\)/);
  assert.match(credentialLoad, /preferences\.isKey\(kMyAiCredentialInitialized\)/);
  assert.match(credentialLoad, /classifyMyAiCredentialStorage/);
  assert.match(credentialLoad, /MyAiCredentialLoadResult::Absent[\s\S]*Status::success\(\)/);
  assert.doesNotMatch(credentialLoad, /preferences\.clear\(/);
  const credentialStore = adapters.slice(
    adapters.indexOf("Status NvsMyAiCredentialStore::store"),
    adapters.indexOf("Status NvsMyAiCredentialStore::initializeFingerprintAtomically"),
  );
  assert.match(credentialStore, /putUInt\("head"[\s\S]*putUChar\([\s\S]*kMyAiCredentialInitialized/);

  const pairingCallback = application.slice(
    application.indexOf("if (pairingCallbackPending_"),
    application.indexOf("pollPairing();"),
  );
  assert.doesNotMatch(pairingCallback, /cancelPairing\(/);
  assert.match(pairingCallback, /pairingCallbackPending_ = true/);
  assert.match(pairingCallback, /storePairingScreenScrubbed\(false\)/);
  assert.ok(
    pairingCallback.indexOf("storePairingScreenScrubbed(false)") <
      pairingCallback.indexOf("refreshFrame("),
  );
  assert.match(application, /clearPendingPairingMaterial[\s\S]*secureClear\(pendingPairing_\.onboardingCode\)[\s\S]*secureClear\(pendingPairing_\.bindingUrl\)/);
  assert.match(application, /tryTerminalDisplayScrub[\s\S]*makeBoundStatusPng[\s\S]*"binding-complete"/);
  assert.match(application, /"myai-pairing"[\s\S]*pairingScreen\.length,[\s\S]*true\)/);
  assert.match(application, /"myai-service-unavailable"[\s\S]*unavailable\.length, true\)/);
  assert.match(application, /"binding-complete"[\s\S]*readyScreen\.length, true\)/);
  assert.match(statusPng, /constexpr uint32_t kWidth = 400/);
  assert.match(statusPng, /constexpr uint32_t kHeight = 600/);
  assert.match(statusPng, /pairing code must stay inside the physical safe area/);
  assert.match(statusPng, /makeBoundStatusPng[\s\S]*No digit glyphs or QR encoder/);
  const boundScreen = statusPng.slice(statusPng.indexOf("bool makeBoundStatusPng"));
  assert.doesNotMatch(boundScreen, /drawDigit|lgfx_qrcode|bindingUrl|sixDigitCode/);
  assert.match(portalCore, /onboarding_\.terminalBindingComplete\(\)[\s\S]*MyAI 与 Inkloop 已绑定/);
});
