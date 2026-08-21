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

function compileAndRun(harnessPath, executablePath, sanitized) {
  const compiler = process.env.CXX || "c++";
  const args = [
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-DINKLOOP_DISPLAYPOWER_TESTING=1",
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
    harnessPath,
    new URL("ImageProcessing.cpp", moduleSource).pathname,
    new URL("PngAttestation.cpp", moduleSource).pathname,
    new URL("RefreshControl.cpp", moduleSource).pathname,
    new URL("PowerPolicy.cpp", moduleSource).pathname,
    new URL("DisplayPowerRuntime.cpp", moduleSource).pathname,
    "-o",
    executablePath,
  );
  const compiled = spawnSync(compiler, args, { encoding: "utf8" });
  assert.equal(compiled.status, 0, `${compiled.stdout}\n${compiled.stderr}`);
  const executed = spawnSync(executablePath, [], {
    encoding: "utf8",
    env: sanitized
      ? {
          ...process.env,
          ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
          UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
        }
      : process.env,
  });
  assert.equal(executed.status, 0, `${executed.stdout}\n${executed.stderr}`);
  assert.match(executed.stdout, /sleep remediation checks passed/);
}

test("sleep failure remediation is named, bounded, quiet, and fail closed", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-sleep-remediation-"));
  const harnessPath = join(directory, "sleep_remediation.cpp");
  const executablePath = join(directory, "sleep_remediation");
  const sanitizedPath = join(directory, "sleep_remediation_sanitized");

  const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include "DisplayPowerRuntime.h"

using namespace inkloop::displaypower;

class FakePlatform final : public IDeepSleepPlatform {
 public:
  int resets = 0;
  int timers = 0;
  int buttons = 0;
  int enters = 0;
  uint64_t seconds = 0;
  uint64_t mask = 0;
  bool enterResult = true;
  bool resetWakeSources() override { ++resets; return true; }
  bool enableTimerWakeAfterSeconds(uint64_t value) override {
    ++timers; seconds = value; return true;
  }
  bool enableAnyLowWake(uint64_t value) override {
    ++buttons; mask = value; return true;
  }
  bool enterDeepSleep() override { ++enters; return enterResult; }
};

PowerInputs idleInputs(uint32_t now = 120000U, uint64_t epoch = 1000U) {
  PowerInputs inputs;
  inputs.nowMilliseconds = now;
  inputs.lastMeaningfulActivityMilliseconds = 0;
  inputs.rtcNowEpochSeconds = epoch;
  inputs.nextHeartbeatEpochSeconds = epoch + 300U;
  inputs.rtcSynchronized = true;
  inputs.wakeButtonsReleased = true;
  return inputs;
}

class FakeHooks final : public IPreSleepQuiescenceHooks {
 public:
  PowerInputs initial = idleInputs();
  PowerInputs final = idleInputs(121000U, 1001U);
  std::vector<PowerSnapshotResult> snapshotResults;
  PowerSnapshotResult persistentResult = PowerSnapshotResult::Captured;
  int captures = 0;
  int finalizes = 0;
  int audioStops = 0;
  int rgbStops = 0;
  int networkStops = 0;

  bool capturePowerInputs(PowerInputs* inputs) override {
    return capturePowerInputsDetailed(inputs) == PowerSnapshotResult::Captured;
  }
  PowerSnapshotResult capturePowerInputsDetailed(PowerInputs* inputs) override {
    const int index = captures++;
    const PowerSnapshotResult result = index < static_cast<int>(snapshotResults.size())
        ? snapshotResults[static_cast<size_t>(index)]
        : persistentResult;
    if (result != PowerSnapshotResult::Captured || !inputs) return result;
    *inputs = index == 0 ? initial : final;
    return PowerSnapshotResult::Captured;
  }
  bool finalizeTaskAndDisplay() override { ++finalizes; return true; }
  bool stopAudio() override { ++audioStops; return true; }
  bool stopImageRgb() override { ++rgbStops; return true; }
  bool closeNetwork() override { ++networkStops; return true; }
};

PowerPolicy batteryPolicy() {
  PowerPolicyConfig config;
  config.mode = PowerMode::BatteryOptIn;
  return PowerPolicy(config);
}

void assertBlocker(
    const PowerInputs& blocked,
    SleepDecisionReason expected,
    const PowerPolicy& policy) {
  FakeHooks hooks;
  hooks.initial = blocked;
  FakePlatform platform;
  const PrepareSleepOutcome outcome =
      prepareAndExecuteSleepDetailed(policy, hooks, platform);
  assert(outcome.result == PrepareSleepResult::NotEligible);
  assert(outcome.decisionReason == expected);
  assert(hooks.finalizes == 0 && hooks.audioStops == 0 &&
      hooks.rgbStops == 0 && hooks.networkStops == 0);
  assert(platform.resets == 0 && platform.timers == 0 &&
      platform.buttons == 0 && platform.enters == 0);
}

int main() {
  assert(std::strcmp(prepareSleepResultName(PrepareSleepResult::SnapshotFailed),
                     "SNAPSHOT_FAILED") == 0);
  assert(std::strcmp(powerSnapshotResultName(
                         PowerSnapshotResult::TaskStoreUnavailable),
                     "TASK_STORE_UNAVAILABLE") == 0);
  assert(std::strcmp(powerSnapshotResultName(
                         PowerSnapshotResult::TaskScheduleInvalid),
                     "TASK_SCHEDULE_INVALID") == 0);
  assert(std::strcmp(sleepDecisionReasonName(SleepDecisionReason::PairingActive),
                     "PAIRING_ACTIVE") == 0);
  assert(std::strcmp(sleepDecisionReasonName(
                         SleepDecisionReason::OnboardingActive),
                     "ONBOARDING_ACTIVE") == 0);
  assert(std::strcmp(sleepDecisionReasonName(
                         SleepDecisionReason::PortalRequestActive),
                     "PORTAL_REQUEST_ACTIVE") == 0);
  assert(std::strcmp(sleepDecisionReasonName(SleepDecisionReason::TutorialActive),
                     "TUTORIAL_ACTIVE") == 0);
  assert(std::strcmp(powerSnapshotPhaseName(PowerSnapshotPhase::Final),
                     "FINAL") == 0);

  for (int value = 0; value < 256; ++value) {
    const char* prepare = prepareSleepResultName(
        static_cast<PrepareSleepResult>(value));
    const char* snapshot = powerSnapshotResultName(
        static_cast<PowerSnapshotResult>(value));
    const char* phase = powerSnapshotPhaseName(
        static_cast<PowerSnapshotPhase>(value));
    const char* deep = deepSleepExecutionResultName(
        static_cast<DeepSleepExecutionResult>(value));
    const char* decision = sleepDecisionReasonName(
        static_cast<SleepDecisionReason>(value));
    assert(prepare && snapshot && phase && deep && decision);
    if (value > static_cast<int>(PrepareSleepResult::DeepSleepRejected))
      assert(std::strstr(prepare, "UNKNOWN_") == prepare);
    if (value > static_cast<int>(PowerSnapshotResult::UnknownFailure))
      assert(std::strstr(snapshot, "UNKNOWN_") == snapshot);
    if (value > static_cast<int>(PowerSnapshotPhase::Final))
      assert(std::strstr(phase, "UNKNOWN_") == phase);
    if (value > static_cast<int>(DeepSleepExecutionResult::EnterReturned))
      assert(std::strstr(deep, "UNKNOWN_") == deep);
    if (value > static_cast<int>(SleepDecisionReason::SleepWindowTooShort))
      assert(std::strstr(decision, "UNKNOWN_") == decision);
  }

  const PowerPolicy policy = batteryPolicy();
  FakeHooks persistentFailure;
  persistentFailure.persistentResult = PowerSnapshotResult::TaskStoreUnavailable;
  FakePlatform failurePlatform;
  SleepAttemptRuntime attempts;
  assert(attempts.configurationValid());
  int actualAttempts = 0;
  int transitions = 0;
  std::vector<uint32_t> summaries;
  uint32_t suppressed = 0;
  for (uint32_t now = 0; now <= 200000U; now += 50U) {
    const SleepAttemptObservation observation = attempts.poll(
        now, policy, persistentFailure, failurePlatform);
    if (!observation.attempted) continue;
    ++actualAttempts;
    assert(observation.outcome.result == PrepareSleepResult::SnapshotFailed);
    assert(observation.outcome.snapshotPhase == PowerSnapshotPhase::Initial);
    assert(observation.outcome.snapshotResult ==
        PowerSnapshotResult::TaskStoreUnavailable);
    transitions += observation.transition ? 1 : 0;
    if (observation.summary) {
      summaries.push_back(now);
      suppressed += observation.suppressedAttempts;
    }
  }
  assert(actualAttempts == 7);
  assert(persistentFailure.captures == actualAttempts);
  assert(transitions == 1);
  assert(summaries.size() == 3 && suppressed > 0);
  for (size_t index = 1; index < summaries.size(); ++index)
    assert(summaries[index] - summaries[index - 1] >= 60000U);
  assert(attempts.currentRetryMilliseconds() == 60000U);
  assert(failurePlatform.enters == 0 && failurePlatform.timers == 0 &&
      failurePlatform.buttons == 0);

  persistentFailure.persistentResult = PowerSnapshotResult::TaskScheduleInvalid;
  SleepAttemptObservation changed = attempts.poll(
      255000U, policy, persistentFailure, failurePlatform);
  assert(changed.attempted && changed.transition && !changed.summary);
  assert(changed.outcome.snapshotResult == PowerSnapshotResult::TaskScheduleInvalid);
  assert(attempts.currentRetryMilliseconds() == 5000U);
  changed = attempts.poll(259999U, policy, persistentFailure, failurePlatform);
  assert(!changed.attempted);
  changed = attempts.poll(260000U, policy, persistentFailure, failurePlatform);
  assert(changed.attempted && !changed.transition);
  assert(attempts.currentRetryMilliseconds() == 10000U);

  persistentFailure.persistentResult = PowerSnapshotResult::Captured;
  persistentFailure.initial = idleInputs(270000U);
  persistentFailure.initial.blockers.pairingActive = true;
  persistentFailure.final = persistentFailure.initial;
  changed = attempts.poll(270000U, policy, persistentFailure, failurePlatform);
  assert(changed.attempted && changed.transition);
  assert(changed.outcome.result == PrepareSleepResult::NotEligible);
  assert(changed.outcome.decisionReason == SleepDecisionReason::PairingActive);
  assert(persistentFailure.finalizes == 0 && failurePlatform.enters == 0);
  assert(attempts.currentRetryMilliseconds() == 1000U);

  FakeHooks initialFailure;
  initialFailure.persistentResult = PowerSnapshotResult::ClockUnsynchronized;
  FakePlatform initialFailurePlatform;
  PrepareSleepOutcome detailed = prepareAndExecuteSleepDetailed(
      policy, initialFailure, initialFailurePlatform);
  assert(detailed.result == PrepareSleepResult::SnapshotFailed);
  assert(detailed.snapshotPhase == PowerSnapshotPhase::Initial);
  assert(detailed.snapshotResult == PowerSnapshotResult::ClockUnsynchronized);

  FakeHooks finalFailure;
  finalFailure.snapshotResults.push_back(PowerSnapshotResult::Captured);
  finalFailure.snapshotResults.push_back(PowerSnapshotResult::TaskScheduleInvalid);
  FakePlatform finalFailurePlatform;
  detailed = prepareAndExecuteSleepDetailed(policy, finalFailure, finalFailurePlatform);
  assert(detailed.result == PrepareSleepResult::SnapshotFailed);
  assert(detailed.snapshotPhase == PowerSnapshotPhase::Final);
  assert(detailed.snapshotResult == PowerSnapshotResult::TaskScheduleInvalid);
  assert(finalFailure.finalizes == 1 && finalFailure.audioStops == 1 &&
      finalFailure.rgbStops == 1 && finalFailure.networkStops == 1);
  assert(finalFailurePlatform.enters == 0);

  PowerInputs blocked = idleInputs(); blocked.blockers.audioActive = true;
  assertBlocker(blocked, SleepDecisionReason::AudioActive, policy);
  blocked = idleInputs(); blocked.blockers.generationActive = true;
  assertBlocker(blocked, SleepDecisionReason::GenerationActive, policy);
  blocked = idleInputs(); blocked.blockers.downloadActive = true;
  assertBlocker(blocked, SleepDecisionReason::DownloadActive, policy);
  blocked = idleInputs(); blocked.blockers.conversionActive = true;
  assertBlocker(blocked, SleepDecisionReason::ConversionActive, policy);
  blocked = idleInputs(); blocked.blockers.writeActive = true;
  assertBlocker(blocked, SleepDecisionReason::WriteActive, policy);
  blocked = idleInputs(); blocked.blockers.displayActive = true;
  assertBlocker(blocked, SleepDecisionReason::DisplayActive, policy);
  blocked = idleInputs(); blocked.blockers.voiceActive = true;
  assertBlocker(blocked, SleepDecisionReason::VoiceActive, policy);
  blocked = idleInputs(); blocked.blockers.taskFinalizationActive = true;
  assertBlocker(blocked, SleepDecisionReason::TaskFinalizationActive, policy);
  blocked = idleInputs(); blocked.blockers.pendingJournal = true;
  assertBlocker(blocked, SleepDecisionReason::PendingJournal, policy);
  blocked = idleInputs(); blocked.blockers.unacknowledgedTask = true;
  assertBlocker(blocked, SleepDecisionReason::UnacknowledgedTask, policy);
  blocked = idleInputs(); blocked.blockers.pairingActive = true;
  assertBlocker(blocked, SleepDecisionReason::PairingActive, policy);
  blocked = idleInputs(); blocked.blockers.portalRequestActive = true;
  assertBlocker(blocked, SleepDecisionReason::PortalRequestActive, policy);
  blocked = idleInputs(); blocked.blockers.tutorialActive = true;
  assertBlocker(blocked, SleepDecisionReason::TutorialActive, policy);
  blocked = idleInputs(); blocked.blockers.onboardingActive = true;
  assertBlocker(blocked, SleepDecisionReason::OnboardingActive, policy);
  blocked = idleInputs(); blocked.blockers.portalActive = true;
  assertBlocker(blocked, SleepDecisionReason::PortalActive, policy);
  blocked = idleInputs(); blocked.blockers.externalPagePending = true;
  assertBlocker(blocked, SleepDecisionReason::ExternalPagePending, policy);
  blocked = idleInputs(); blocked.wakeButtonsReleased = false;
  assertBlocker(blocked, SleepDecisionReason::WakeButtonsHeld, policy);

  FakeHooks eligible;
  FakePlatform eligiblePlatform;
  detailed = prepareAndExecuteSleepDetailed(policy, eligible, eligiblePlatform);
  assert(detailed.result == PrepareSleepResult::Entered);
  assert(eligiblePlatform.enters == 1 && eligiblePlatform.timers == 1 &&
      eligiblePlatform.buttons == 1);
  assert(eligiblePlatform.mask == 0x602ULL);
  assert(eligiblePlatform.seconds == 270U);

  FakeHooks wrapFailure;
  wrapFailure.persistentResult = PowerSnapshotResult::TaskScheduleInvalid;
  FakePlatform wrapPlatform;
  SleepAttemptRuntime wrapAttempts;
  const uint32_t nearWrap = 0xfffff000U;
  assert(wrapAttempts.poll(nearWrap, policy, wrapFailure, wrapPlatform).attempted);
  assert(!wrapAttempts.poll(
      static_cast<uint32_t>(nearWrap + 4999U),
      policy, wrapFailure, wrapPlatform).attempted);
  assert(wrapAttempts.poll(
      static_cast<uint32_t>(nearWrap + 5000U),
      policy, wrapFailure, wrapPlatform).attempted);
  assert(wrapFailure.captures == 2);

  SleepAttemptRuntimeConfig invalidConfig;
  invalidConfig.minimumFailureRetryMilliseconds = 4999U;
  SleepAttemptRuntime invalidRuntime(invalidConfig);
  assert(!invalidRuntime.configurationValid());
  assert(!invalidRuntime.poll(0, policy, wrapFailure, wrapPlatform).attempted);

  PowerPolicy alwaysAwake;
  FakeHooks noCapture;
  noCapture.persistentResult = PowerSnapshotResult::TaskStoreUnavailable;
  FakePlatform alwaysAwakePlatform;
  detailed = prepareAndExecuteSleepDetailed(
      alwaysAwake, noCapture, alwaysAwakePlatform);
  assert(detailed.result == PrepareSleepResult::NotEligible);
  assert(detailed.decisionReason == SleepDecisionReason::AlwaysAwakeMode);
  assert(noCapture.captures == 0 && alwaysAwakePlatform.enters == 0);

  assert(PowerPolicy::paperColorExt1AnyLowMask() == 0x602ULL);
  assert(wakeReasonFromExt1Mask(1ULL << 1U) == WakeReason::TopButton);
  assert(wakeReasonFromExt1Mask(1ULL << 10U) == WakeReason::PreviousButton);
  assert(wakeReasonFromExt1Mask(1ULL << 9U) == WakeReason::NextButton);

  std::cout << "sleep remediation checks passed\n";
  return 0;
}
`;

  await writeFile(harnessPath, harness);
  compileAndRun(harnessPath, executablePath, false);
  compileAndRun(harnessPath, sanitizedPath, true);
  await rm(directory, { recursive: true, force: true });
});

test("PaperColor runtime uses named bounded sleep integration", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const [application, applicationHeader, adapterHeader, adapter, main] = await Promise.all([
    readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.h", sourceRoot), "utf8"),
    readFile(new URL("DisplayPowerAdapters.h", sourceRoot), "utf8"),
    readFile(new URL("DisplayPowerAdapters.cpp", sourceRoot), "utf8"),
    readFile(new URL("main.cpp", sourceRoot), "utf8"),
  ]);

  assert.match(applicationHeader, /displaypower::SleepAttemptRuntime sleepAttempt_/);
  assert.match(applicationHeader, /displaypower::PowerSnapshotResult capturePowerHook/);
  assert.match(adapterHeader, /displaypower::PowerSnapshotResult \(\*\)/);
  assert.match(adapter, /capturePowerInputsDetailed/);
  assert.match(application, /sleepAttempt_\.poll\(/);
  assert.match(application, /Diagnostics::event\("SLEEP_STATE"/);
  assert.match(application, /Diagnostics::event\("SLEEP_SUMMARY"/);
  assert.doesNotMatch(application, /Diagnostics::event\("SLEEP_RESULT"/);
  assert.match(application, /PowerSnapshotResult::TaskStoreUnavailable/);
  assert.match(application, /PowerSnapshotResult::TaskScheduleInvalid/);
  assert.match(application, /PowerSnapshotResult::ClockUnsynchronized/);
  for (const blocker of [
    "audioActive",
    "generationActive",
    "downloadActive",
    "conversionActive",
    "writeActive",
    "displayActive",
    "voiceActive",
    "taskFinalizationActive",
    "pendingJournal",
    "unacknowledgedTask",
    "pairingActive",
    "onboardingActive",
    "portalRequestActive",
    "tutorialActive",
    "externalPagePending",
  ]) {
    assert.match(application, new RegExp(`blockers\\.${blocker}`));
  }
  const blockerShortCircuit = application.indexOf("inputs->blockers.any()");
  const taskStoreCheck = application.indexOf("runtime->tasks_.ready()", blockerShortCircuit);
  const taskScheduleRead = application.indexOf("runtime->tasks_.nextDueEpoch", taskStoreCheck);
  assert(blockerShortCircuit > 0 && taskStoreCheck > blockerShortCircuit);
  assert(taskScheduleRead > taskStoreCheck);
  assert.match(main, /PaperColorApplicationRuntime applicationRuntime/);
});
