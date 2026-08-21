import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);

function compileAndRun(source, output, sanitized) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", sourceRoot.pathname, source, "-o", output,
  ];
  if (sanitized) args.unshift(
    "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
  );
  const built = spawnSync(process.env.CXX || "c++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, built.stderr || built.stdout);
  const ran = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitized ? {
      ...process.env,
      ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    } : process.env,
  });
  assert.equal(ran.status, 0, ran.stderr || ran.stdout);
}

test("quiescent MyAI failures permit sleep but live transactions fail closed", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-myai-sleep-"));
  try {
    const source = join(temporary, "sleep.cpp");
    const executable = join(temporary, "sleep");
    await writeFile(source, String.raw`
#include <cassert>
#include <string>
#include "MyAiSleepPrimitives.h"

int main() {
  using namespace inkloop;
  assert(!myAiPairingTransactionActive(false, false, false));
  assert(myAiPairingTransactionActive(true, false, false));
  assert(myAiPairingTransactionActive(false, true, false));
  assert(myAiPairingTransactionActive(false, false, true));
  const char* unavailable[] = {
      "app_not_registered", "offline", "error", "unconfigured",
      "credential_recovery"};
  for (const char* state : unavailable) {
    assert(myAiServiceQuiescentAndUnavailable(state, false));
    assert(!myAiServiceQuiescentAndUnavailable(state, true));
    assert(!onboardingBlocksSleep(false, false, false, false, true));
  }
  assert(!myAiServiceQuiescentAndUnavailable("pairing", false));
  assert(!myAiServiceQuiescentAndUnavailable("active", false));
  assert(onboardingBlocksSleep(false, false, false, false, false));
  assert(onboardingBlocksSleep(true, true, true, false, false));
  assert(!onboardingBlocksSleep(true, true, true, true, false));
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-sanitized`, true);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("production capture blocks upload, display, portal request, and true pairing only", async () => {
  const application = await readFile(
    new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8",
  );
  const capture = application.slice(
    application.indexOf("PowerSnapshotResult PaperColorApplicationRuntime::capture"),
    application.indexOf("void PaperColorApplicationRuntime::prepareForSleep"),
  );
  assert.match(capture, /const bool uploadActive = runtime->album_\.userUploadActive\(\)/);
  assert.match(capture, /writeActive = runtime->displayBusy\(\) \|\| uploadActive/);
  assert.match(capture, /downloadActive =[\s\S]*uploadActive/);
  assert.match(capture, /myAiPairingTransactionActive\([\s\S]*pairingPollActive_[\s\S]*pairingCallbackPending_[\s\S]*ActivationState::Pairing/);
  assert.doesNotMatch(capture, /ActivationState::Unconfigured\).*pairingActive/);
  assert.match(capture, /myAiServiceQuiescentAndUnavailable/);
  assert.match(capture, /portalRequestActive = runtime->portal_\.requestActive\(\)/);
  assert.match(capture, /externalPagePending =\n      runtime->activityState_\.externalPagePending\(\)/);
});
