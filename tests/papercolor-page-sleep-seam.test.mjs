import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const moduleSource = new URL(
  "../firmware/m5-papercolor/lib/InkloopDisplayPower/",
  import.meta.url,
);

function compileAndRun(source, output, sanitized) {
  const compiler = process.env.CXX || "c++";
  const args = [
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
  ];
  if (sanitized) {
    args.push(
      "-O1",
      "-g",
      "-fno-omit-frame-pointer",
      "-fsanitize=address,undefined",
    );
  }
  args.push(
    "-I",
    moduleSource.pathname,
    source,
    new URL("ImageProcessing.cpp", moduleSource).pathname,
    new URL("PngAttestation.cpp", moduleSource).pathname,
    new URL("RefreshControl.cpp", moduleSource).pathname,
    new URL("PowerPolicy.cpp", moduleSource).pathname,
    new URL("DisplayPowerRuntime.cpp", moduleSource).pathname,
    "-o",
    output,
  );
  const compiled = spawnSync(compiler, args, { encoding: "utf8" });
  assert.equal(compiled.status, 0, `${compiled.stdout}\n${compiled.stderr}`);
  const run = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitized
      ? {
          ...process.env,
          ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
          UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
        }
      : process.env,
  });
  assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
  assert.match(run.stdout, /page sleep seam checks passed/);
}

test("queued external pages block sleep and restart the full idle interval", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-page-sleep-"));
  const source = join(directory, "page_sleep.cpp");
  const output = join(directory, "page_sleep");
  const sanitized = join(directory, "page_sleep_sanitized");
  const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "DisplayPowerRuntime.h"

using namespace inkloop::displaypower;

class FakePlatform final : public IDeepSleepPlatform {
 public:
  int enters = 0;
  int timers = 0;
  int buttons = 0;
  uint64_t mask = 0;
  bool resetWakeSources() override { return true; }
  bool enableTimerWakeAfterSeconds(uint64_t) override { ++timers; return true; }
  bool enableAnyLowWake(uint64_t value) override {
    ++buttons; mask = value; return true;
  }
  bool enterDeepSleep() override { ++enters; return true; }
};

class FakeHooks final : public IPreSleepQuiescenceHooks {
 public:
  PowerInputs inputs;
  int captures = 0;
  int finalizes = 0;
  bool capturePowerInputs(PowerInputs* output) override {
    if (!output) return false;
    *output = inputs;
    ++captures;
    return true;
  }
  bool finalizeTaskAndDisplay() override { ++finalizes; return true; }
  bool stopAudio() override { return true; }
  bool stopImageRgb() override { return true; }
  bool closeNetwork() override { return true; }
};

PowerInputs capturedAt(const RuntimeActivityState& activity, uint32_t now) {
  PowerInputs inputs;
  inputs.nowMilliseconds = now;
  inputs.lastMeaningfulActivityMilliseconds =
      activity.lastMeaningfulActivityMilliseconds();
  inputs.rtcSynchronized = true;
  inputs.rtcNowEpochSeconds = 1000U;
  inputs.nextHeartbeatEpochSeconds = 1300U;
  inputs.wakeButtonsReleased = true;
  inputs.blockers.externalPagePending = activity.externalPagePending();
  return inputs;
}

int main() {
  PowerPolicyConfig config;
  config.mode = PowerMode::BatteryOptIn;
  PowerPolicy policy(config);
  RuntimeActivityState activity;
  FakeHooks hooks;
  FakePlatform platform;

  // The button/voice queue callback runs before the application's sleep poll.
  activity.noteMeaningfulActivity(0U);
  activity.setExternalPagePending(true, 120000U);
  hooks.inputs = capturedAt(activity, 120000U);
  PrepareSleepOutcome outcome = prepareAndExecuteSleepDetailed(
      policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::NotEligible);
  assert(outcome.decisionReason == SleepDecisionReason::ExternalPagePending);
  assert(hooks.finalizes == 0 && platform.enters == 0 &&
      platform.timers == 0 && platform.buttons == 0);
  assert(std::strcmp(
      sleepDecisionReasonName(outcome.decisionReason),
      "EXTERNAL_PAGE_PENDING") == 0);

  // A delayed processor may not age out the pending blocker.
  hooks.inputs = capturedAt(activity, 300000U);
  outcome = prepareAndExecuteSleepDetailed(policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::NotEligible);
  assert(outcome.decisionReason == SleepDecisionReason::ExternalPagePending);
  assert(platform.enters == 0);

  // Success and every terminal failure clear through the same seam. Clearing
  // is meaningful activity, so the complete two-minute interval starts here.
  activity.setExternalPagePending(false, 300700U);
  hooks.inputs = capturedAt(activity, 420699U);
  outcome = prepareAndExecuteSleepDetailed(policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::NotEligible);
  assert(outcome.decisionReason == SleepDecisionReason::IdlePeriodNotReached);
  assert(platform.enters == 0);
  hooks.inputs = capturedAt(activity, 420700U);
  outcome = prepareAndExecuteSleepDetailed(policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::Entered);
  assert(platform.enters == 1 && platform.timers == 1 && platform.buttons == 1);
  assert(platform.mask == 0x602ULL);

  // Activity and pending state remain correct over uint32 millisecond wrap.
  RuntimeActivityState wrapped;
  wrapped.setExternalPagePending(true, 0xfffffff0U);
  assert(wrapped.externalPagePending());
  wrapped.setExternalPagePending(false, 0x20U);
  hooks.inputs = capturedAt(wrapped, static_cast<uint32_t>(0x20U + 119999U));
  outcome = prepareAndExecuteSleepDetailed(policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::NotEligible);
  assert(outcome.decisionReason == SleepDecisionReason::IdlePeriodNotReached);

  std::cout << "page sleep seam checks passed\n";
  return 0;
}
`;
  await writeFile(source, harness);
  compileAndRun(source, output, false);
  compileAndRun(source, sanitized, true);
  await rm(directory, { recursive: true, force: true });
});

test("physical and voice page queues share the pending lifecycle seam", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const [main, runtime, runtimeHeader] = await Promise.all([
    readFile(new URL("main.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.h", sourceRoot), "utf8"),
  ]);

  assert.match(runtimeHeader, /void noteExternalActivity\(uint32_t nowMilliseconds\)/);
  assert.match(runtimeHeader, /void setExternalPagePending\(bool pending, uint32_t nowMilliseconds\)/);
  assert.match(runtime, /blockers\.externalPagePending\s*=\s*[\s\S]*activityState_\.externalPagePending\(\)/);

  const acceptStart = main.indexOf("void acceptPendingPage(", main.indexOf("void acceptPendingPage(") + 1);
  const clearStart = main.indexOf("void clearPendingPage(", main.indexOf("void clearPendingPage(") + 1);
  const buttonStart = main.indexOf("void onButton(");
  const voiceQueueStart = main.indexOf("bool queueRuntimePage(", main.indexOf("bool queueRuntimePage(") + 1);
  const processStart = main.indexOf("bool processPendingPage()");
  const diagnosticsStart = main.indexOf("void printDiagnosticStatus()", processStart);
  assert(acceptStart > 0 && clearStart > acceptStart && buttonStart > clearStart);
  assert(voiceQueueStart > buttonStart && processStart > voiceQueueStart);

  const acceptBody = main.slice(acceptStart, clearStart);
  const clearBody = main.slice(clearStart, buttonStart);
  const buttonBody = main.slice(buttonStart, voiceQueueStart);
  const voiceBody = main.slice(voiceQueueStart, main.indexOf("bool safeShowStatus", voiceQueueStart));
  const processBody = main.slice(processStart, diagnosticsStart);
  assert.match(acceptBody, /setExternalPagePending\(true, nowMilliseconds\)/);
  assert.match(clearBody, /setExternalPagePending\(false, nowMilliseconds\)/);
  assert.match(buttonBody, /acceptPendingPage\(selection\.page, state\.backend, millis\(\)\)/);
  assert.match(voiceBody, /acceptPendingPage\(page, backend, millis\(\)\)/);
  assert.doesNotMatch(processBody, /pendingPageReady\s*=\s*false/);
  assert((processBody.match(/clearPendingPage\(millis\(\)\)/g) || []).length >= 5);
  assert(processBody.indexOf("clearPendingPage(millis())", processBody.indexOf("onPageDisplayCommitted")) > 0);

  const loopStart = main.indexOf("void loop()");
  const applicationPoll = main.indexOf("applicationRuntime.loop()", loopStart);
  const pageProcess = main.indexOf("processPendingPage()", applicationPoll);
  assert(applicationPoll > loopStart && pageProcess > applicationPoll);
  assert.match(runtime, /if \(!queuePage_ \|\| !queuePage_\(queuePageContext_, selected, backend\)\)/);
});
