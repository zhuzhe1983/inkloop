import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const power = join(repo, "firmware/inkloop-idf/components/inkloop_power");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "inkloop/power_policy.hpp"
#include "inkloop/power_runtime.hpp"

using namespace inkloop;

SleepPolicy enabledPolicy() {
  SleepPolicyConfig config;
  config.enabled = true;
  return SleepPolicy(config);
}

PowerInputs idleInputs(uint32_t now = 120000U, uint64_t epoch = 1000U) {
  PowerInputs inputs;
  inputs.now_ms = now;
  inputs.last_meaningful_activity_ms = 0;
  inputs.rtc_epoch_seconds = epoch;
  inputs.next_heartbeat_epoch_seconds = epoch + 300U;
  inputs.rtc_synchronized = true;
  inputs.wake_buttons_released = true;
  return inputs;
}

class FakeDriver final : public IDeepSleepDriver {
 public:
  DeepSleepResult result = DeepSleepResult::Entered;
  uint64_t seconds = 0;
  int calls = 0;
  DeepSleepResult enterAfterSeconds(uint64_t value) override {
    ++calls;
    seconds = value;
    return result;
  }
};

class FakePreparation final : public ISleepPreparation {
 public:
  PowerInputs initial = idleInputs();
  PowerInputs final = idleInputs(121000U, 1001U);
  PowerSnapshotResult persistent_snapshot = PowerSnapshotResult::Captured;
  PowerSnapshotResult final_snapshot = PowerSnapshotResult::Captured;
  bool tasks = true;
  bool display = true;
  bool voice = true;
  bool network = true;
  bool restore = true;
  bool inject_aigc_during_network = false;
  int captures = 0;
  int task_calls = 0;
  int display_calls = 0;
  int voice_calls = 0;
  int network_calls = 0;
  int restore_calls = 0;

  PowerSnapshotResult capturePowerInputs(PowerInputs& output) override {
    const bool initial_capture = (captures++ & 1) == 0;
    const PowerSnapshotResult status =
        persistent_snapshot != PowerSnapshotResult::Captured
            ? persistent_snapshot
            : (initial_capture ? persistent_snapshot : final_snapshot);
    if (status == PowerSnapshotResult::Captured) {
      output = initial_capture ? initial : final;
    }
    return status;
  }
  bool settleTasksAndSync() override { ++task_calls; return tasks; }
  bool quiesceDisplay() override { ++display_calls; return display; }
  bool quiesceVoiceAndAudio() override { ++voice_calls; return voice; }
  bool quiesceNetwork() override {
    ++network_calls;
    if (inject_aigc_during_network) {
      final.blockers.set(PowerBlocker::AigcGeneration, true);
    }
    return network;
  }
  bool restoreAwakeServices() override { ++restore_calls; return restore; }
};

void assertAllNamesFailClosed() {
  for (int value = 0; value < 256; ++value) {
    assert(powerBlockerName(static_cast<PowerBlocker>(value)) != nullptr);
    assert(sleepDecisionReasonName(
               static_cast<SleepDecisionReason>(value)) != nullptr);
    assert(deepSleepResultName(static_cast<DeepSleepResult>(value)) != nullptr);
    assert(powerSnapshotResultName(
               static_cast<PowerSnapshotResult>(value)) != nullptr);
    assert(sleepAttemptResultName(
               static_cast<SleepAttemptResult>(value)) != nullptr);
    assert(sleepLogDispositionName(
               static_cast<SleepLogDisposition>(value)) != nullptr);
  }
  assert(std::strcmp(powerBlockerName(PowerBlocker::AigcGeneration),
                     "AIGC_GENERATION") == 0);
  assert(std::strcmp(powerBlockerName(PowerBlocker::AlbumUpload),
                     "ALBUM_UPLOAD") == 0);
}

int main() {
  assertAllNamesFailClosed();
  const SleepPolicy policy = enabledPolicy();
  assert(policy.configurationValid());
  assert(policy.config().eligible_idle_ms == 120000U);

  // The typed finite blocker set covers every migrated owner and rejects
  // sentinel/unknown values without shifting by an invalid amount.
  for (uint8_t value = static_cast<uint8_t>(PowerBlocker::None) + 1U;
       value < static_cast<uint8_t>(PowerBlocker::Count); ++value) {
    PowerInputs blocked = idleInputs();
    const PowerBlocker blocker = static_cast<PowerBlocker>(value);
    assert(blocked.blockers.set(blocker, true) == BlockerUpdate::Changed);
    assert(blocked.blockers.active(blocker));
    const SleepDecision decision = policy.evaluate(blocked);
    assert(!decision.should_sleep);
    assert(decision.reason == SleepDecisionReason::Blocked);
    assert(decision.blocker == blocker);
  }
  PowerBlockerSet invalid;
  assert(invalid.set(PowerBlocker::None, true) == BlockerUpdate::Invalid);
  assert(invalid.set(PowerBlocker::Count, true) == BlockerUpdate::Invalid);
  assert(invalid.set(static_cast<PowerBlocker>(255), true) ==
         BlockerUpdate::Invalid);
  assert(!invalid.any());

  // A page can remain pending for arbitrarily longer than two minutes. Its
  // clear transition starts a new full idle interval.
  PowerActivityState activity(0);
  assert(activity.setBlocker(PowerBlocker::PagePending, true, 120000U) ==
         BlockerUpdate::Changed);
  PowerInputs page = idleInputs(400000U);
  page.last_meaningful_activity_ms = activity.lastMeaningfulActivityMs();
  page.blockers = activity.blockers();
  SleepDecision decision = policy.evaluate(page);
  assert(decision.reason == SleepDecisionReason::Blocked);
  assert(decision.blocker == PowerBlocker::PagePending);
  assert(activity.setBlocker(PowerBlocker::PagePending, false, 400700U) ==
         BlockerUpdate::Changed);
  page.now_ms = 520699U;
  page.last_meaningful_activity_ms = activity.lastMeaningfulActivityMs();
  page.blockers = activity.blockers();
  assert(policy.evaluate(page).reason ==
         SleepDecisionReason::IdlePeriodNotReached);
  page.now_ms = 520700U;
  decision = policy.evaluate(page);
  assert(decision.should_sleep && decision.timer_delay_seconds == 270U);
  assert(activity.setBlocker(PowerBlocker::PagePending, false, 520701U) ==
         BlockerUpdate::Unchanged);
  assert(activity.lastMeaningfulActivityMs() == 400700U);

  // Idle and blocker lifecycle math remains valid through uint32 wrap.
  PowerActivityState wrapped(0xfffffff0U);
  assert(wrapped.setBlocker(PowerBlocker::PagePending, true, 0xfffffff0U) ==
         BlockerUpdate::Changed);
  assert(wrapped.setBlocker(PowerBlocker::PagePending, false, 0x20U) ==
         BlockerUpdate::Changed);
  PowerInputs wrap_input = idleInputs();
  wrap_input.last_meaningful_activity_ms =
      wrapped.lastMeaningfulActivityMs();
  wrap_input.now_ms = static_cast<uint32_t>(0x20U + 119999U);
  assert(policy.evaluate(wrap_input).reason ==
         SleepDecisionReason::IdlePeriodNotReached);
  wrap_input.now_ms = static_cast<uint32_t>(0x20U + 120000U);
  assert(policy.evaluate(wrap_input).should_sleep);

  // Explicit heartbeat values cannot extend the configured five-minute cap;
  // task deadlines take precedence and the RTC connection margin is applied.
  PowerInputs deadlines = idleInputs();
  deadlines.next_heartbeat_epoch_seconds = 1000000U;
  assert(policy.evaluate(deadlines).timer_delay_seconds == 270U);
  deadlines.next_task_epoch_seconds = 1100U;
  assert(policy.evaluate(deadlines).timer_delay_seconds == 70U);
  deadlines.next_task_epoch_seconds = 1000U;
  assert(policy.evaluate(deadlines).reason ==
         SleepDecisionReason::WakeDeadlineDue);
  deadlines = idleInputs();
  deadlines.wake_buttons_released = false;
  assert(policy.evaluate(deadlines).reason ==
         SleepDecisionReason::WakeButtonsHeld);
  deadlines = idleInputs();
  deadlines.rtc_epoch_seconds = std::numeric_limits<uint64_t>::max() - 1U;
  deadlines.next_heartbeat_epoch_seconds = 0;
  assert(policy.evaluate(deadlines).reason ==
         SleepDecisionReason::ClockUnavailable);

  SleepPolicy disabled;
  FakePreparation disabled_prep;
  FakeDriver disabled_driver;
  SleepAttemptOutcome outcome = executeSleepAttempt(
      disabled, disabled_prep, disabled_driver);
  assert(outcome.result == SleepAttemptResult::NotEligible);
  assert(outcome.decision.reason == SleepDecisionReason::Disabled);
  assert(disabled_prep.captures == 0 && disabled_driver.calls == 0);

  SleepPolicyConfig invalid_config;
  invalid_config.enabled = true;
  invalid_config.eligible_idle_ms = 119999U;
  SleepPolicy invalid_policy(invalid_config);
  assert(!invalid_policy.configurationValid());
  assert(invalid_policy.evaluate(idleInputs()).reason ==
         SleepDecisionReason::InvalidConfiguration);

  // Sleep admission is checked once before and once after ordered quiescence.
  FakePreparation ready;
  FakeDriver driver;
  outcome = executeSleepAttempt(policy, ready, driver);
  assert(outcome.result == SleepAttemptResult::Entered);
  assert(ready.captures == 2 && ready.task_calls == 1 &&
         ready.display_calls == 1 && ready.voice_calls == 1 &&
         ready.network_calls == 1 && ready.restore_calls == 0);
  assert(driver.calls == 1 && driver.seconds == 270U);

  FakePreparation appeared;
  appeared.final.blockers.set(PowerBlocker::AlbumUpload, true);
  FakeDriver not_entered;
  outcome = executeSleepAttempt(policy, appeared, not_entered);
  assert(outcome.result == SleepAttemptResult::RecheckNotEligible);
  assert(outcome.decision.blocker == PowerBlocker::AlbumUpload);
  assert(outcome.restore_attempted && outcome.restore_succeeded);
  assert(appeared.restore_calls == 1 && not_entered.calls == 0);

  // Work accepted during network quiescence must be visible in the live final
  // snapshot, restore networking and cancel the sleep entry.
  FakePreparation queued_aigc;
  queued_aigc.inject_aigc_during_network = true;
  outcome = executeSleepAttempt(policy, queued_aigc, not_entered);
  assert(outcome.result == SleepAttemptResult::RecheckNotEligible);
  assert(outcome.decision.reason == SleepDecisionReason::Blocked);
  assert(outcome.decision.blocker == PowerBlocker::AigcGeneration);
  assert(queued_aigc.captures == 2 && queued_aigc.network_calls == 1);
  assert(outcome.restore_attempted && outcome.restore_succeeded);
  assert(queued_aigc.restore_calls == 1 && not_entered.calls == 0);

  FakePreparation failed_quiescence;
  failed_quiescence.voice = false;
  outcome = executeSleepAttempt(policy, failed_quiescence, not_entered);
  assert(outcome.result == SleepAttemptResult::VoiceQuiescenceFailed);
  assert(failed_quiescence.network_calls == 0);
  assert(failed_quiescence.restore_calls == 1);

  // Repeated operational failures back off 5, 10, 20, 40, then 60 seconds.
  // Logs are emitted only on transition or once-per-minute summary.
  FakePreparation unavailable;
  unavailable.persistent_snapshot = PowerSnapshotResult::TaskStoreUnavailable;
  FakeDriver never;
  SleepAttemptRuntime attempts;
  int actual_attempts = 0;
  int transitions = 0;
  int summaries = 0;
  uint32_t suppressed = 0;
  for (uint32_t now = 0; now <= 200000U; now += 50U) {
    const SleepAttemptObservation observed =
        attempts.poll(now, policy, unavailable, never);
    if (!observed.attempted) continue;
    ++actual_attempts;
    if (observed.log == SleepLogDisposition::Transition) ++transitions;
    if (observed.log == SleepLogDisposition::Summary) {
      ++summaries;
      suppressed += observed.suppressed_attempts;
    }
  }
  assert(actual_attempts == 7 && transitions == 1 && summaries == 3);
  assert(suppressed > 0 && attempts.currentRetryMs() == 60000U);
  assert(never.calls == 0);

  // Scheduler deadlines are also uint32-wrap safe.
  FakePreparation wrap_failure;
  wrap_failure.persistent_snapshot = PowerSnapshotResult::TaskScheduleInvalid;
  SleepAttemptRuntime wrap_attempts;
  const uint32_t near_wrap = 0xfffff000U;
  assert(wrap_attempts.poll(near_wrap, policy, wrap_failure, never).attempted);
  assert(!wrap_attempts.poll(static_cast<uint32_t>(near_wrap + 4999U),
                             policy, wrap_failure, never).attempted);
  assert(wrap_attempts.poll(static_cast<uint32_t>(near_wrap + 5000U),
                            policy, wrap_failure, never).attempted);

  SleepAttemptRuntimeConfig bad_runtime_config;
  bad_runtime_config.minimum_failure_retry_ms = 4999U;
  SleepAttemptRuntime bad_runtime(bad_runtime_config);
  assert(!bad_runtime.configurationValid());
  assert(!bad_runtime.poll(0, policy, ready, driver).attempted);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-idf-power-core-"));
  try {
    const source = join(scratch, "power_core.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(power, "include"), source,
      join(power, "power_policy.cpp"), join(power, "power_runtime.cpp"),
      "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-O1", "-g", "-fsanitize=address,undefined",
      "-fno-omit-frame-pointer",
    );
    execFileSync(process.env.CXX || "c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? {
            ...process.env,
            ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
            UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
          }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("native power policy, typed blockers and retry logs pass strict C++17", () => {
  buildAndRun(false);
});

test("native power policy is wrap-safe under ASan/UBSan", () => {
  buildAndRun(true);
});
