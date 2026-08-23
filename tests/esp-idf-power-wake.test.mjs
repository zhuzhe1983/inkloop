import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const firmware = join(repo, "firmware/inkloop-idf");
const power = join(firmware, "components/inkloop_power");
const native = join(firmware, "components/inkloop_power_idf");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "inkloop/esp_deep_sleep_adapter.hpp"
#include "inkloop/power_runtime.hpp"

using namespace inkloop;

class FakePins final : public IWakePinCapabilities {
 public:
  uint8_t supported = 0x07U;
  int voice = 1;
  int previous = 10;
  int next = 9;
  bool voice_pressed = false;
  bool previous_pressed = false;
  bool next_pressed = false;
  mutable int pin_reads[3]{};
  mutable int pressed_reads[3]{};

  static unsigned index(WakeButton button) {
    return static_cast<unsigned>(button);
  }
  bool supportsWakeButton(WakeButton button) const override {
    return (supported & static_cast<uint8_t>(1U << index(button))) != 0U;
  }

  int wakePin(WakeButton button) const override {
    assert(supportsWakeButton(button));
    ++pin_reads[index(button)];
    switch (button) {
      case WakeButton::Voice: return voice;
      case WakeButton::Previous: return previous;
      case WakeButton::Next: return next;
    }
    return -1;
  }
  bool wakeButtonPressed(WakeButton button) const override {
    assert(supportsWakeButton(button));
    ++pressed_reads[index(button)];
    switch (button) {
      case WakeButton::Voice: return voice_pressed;
      case WakeButton::Previous: return previous_pressed;
      case WakeButton::Next: return next_pressed;
    }
    return true;
  }
};

int reset_result = 0;
int timer_result = 0;
int ext1_result = 0;
int resets = 0;
int timers = 0;
int ext1_calls = 0;
int starts = 0;
uint64_t timer_us = 0;
uint64_t ext1_mask = 0;
uint32_t wake_flags = 0;
uint64_t wake_mask = 0;

int resetWake() { ++resets; return reset_result; }
int timerWake(uint64_t value) { ++timers; timer_us = value; return timer_result; }
int ext1Wake(uint64_t value) { ++ext1_calls; ext1_mask = value; return ext1_result; }
uint32_t wakeFlags() { return wake_flags; }
uint64_t wakeMask() { return wake_mask; }
void startSleep() { ++starts; }

void resetFakes() {
  reset_result = 0;
  timer_result = 0;
  ext1_result = 0;
  resets = timers = ext1_calls = starts = 0;
  timer_us = ext1_mask = wake_mask = 0;
  wake_flags = 0;
}

const EspSleepFunctions functions{
    &resetWake, &timerWake, &ext1Wake, &wakeFlags, &wakeMask, &startSleep};

class FakeWakeSeam final : public IWakeRecoverySeam {
 public:
  bool indicator_ok = true;
  bool peripherals_ok = true;
  bool network_ok = true;
  bool sync_ok = true;
  bool released = false;
  bool rearm_ok = true;
  int network_failures_before_success = 0;
  int indicators = 0;
  int peripherals = 0;
  int networks = 0;
  int syncs = 0;
  int release_samples = 0;
  int rearms = 0;
  int prompts = 0;

  bool requestAwakeIndicator() override {
    ++indicators;
    return indicator_ok;
  }
  bool restorePeripheralsPreservingPanel() override {
    ++peripherals;
    return peripherals_ok;
  }
  bool reconnectNetwork() override {
    ++networks;
    if (network_failures_before_success > 0) {
      --network_failures_before_success;
      return false;
    }
    return network_ok;
  }
  bool syncMetadataPreservingPanel() override { ++syncs; return sync_ok; }
  bool allWakeButtonsReleased() override {
    ++release_samples;
    return released;
  }
  bool rearmButtonInput() override { ++rearms; return rearm_ok; }
  void requestDeviceRestoredPrompt() override { ++prompts; }
};

int main() {
  FakePins pins;
  EspDeepSleepAdapter adapter(pins, functions);
  assert(adapter.capabilitiesValid());
  assert(adapter.wakeButtonMask() == 0x602ULL);
  assert(adapter.wakeButtonsReleased());

  wake_flags = 0;
  assert(adapter.wakeCause() == WakeCause::ColdBoot);
  wake_flags = kSystemWakeFlagTimer;
  assert(adapter.wakeCause() == WakeCause::Timer);
  wake_flags = kSystemWakeFlagExt1;
  wake_mask = 1ULL << 1U;
  assert(adapter.wakeCause() == WakeCause::VoiceButton);
  wake_mask = 1ULL << 10U;
  assert(adapter.wakeCause() == WakeCause::PreviousButton);
  wake_mask = 1ULL << 9U;
  assert(adapter.wakeCause() == WakeCause::NextButton);
  wake_mask = (1ULL << 1U) | (1ULL << 9U);
  assert(adapter.wakeCause() == WakeCause::MultipleButtons);
  wake_mask = 1ULL << 8U;
  assert(adapter.wakeCause() == WakeCause::Unknown);
  wake_flags = kSystemWakeFlagTimer | kSystemWakeFlagExt1;
  assert(adapter.wakeCause() == WakeCause::MultipleSources);
  wake_flags = 1UL << 31U;
  assert(adapter.wakeCause() == WakeCause::Unknown);

  resetFakes();
  assert(adapter.enterAfterSeconds(270U) == DeepSleepResult::EnterReturned);
  assert(timer_us == 270000000ULL && ext1_mask == 0x602ULL);
  assert(timers == 1 && ext1_calls == 1 && starts == 1 && resets == 2);

  resetFakes();
  pins.voice_pressed = true;
  assert(adapter.enterAfterSeconds(270U) == DeepSleepResult::ButtonsHeld);
  assert(resets == 0 && timers == 0 && ext1_calls == 0 && starts == 0);
  pins.voice_pressed = false;

  assert(adapter.enterAfterSeconds(0) == DeepSleepResult::InvalidTimer);
  assert(adapter.enterAfterSeconds(
             std::numeric_limits<uint64_t>::max()) ==
         DeepSleepResult::InvalidTimer);

  resetFakes();
  reset_result = -1;
  assert(adapter.enterAfterSeconds(1) == DeepSleepResult::WakeResetFailed);
  resetFakes();
  timer_result = -1;
  assert(adapter.enterAfterSeconds(1) ==
         DeepSleepResult::TimerConfigurationFailed);
  assert(resets == 2 && ext1_calls == 0 && starts == 0);
  resetFakes();
  ext1_result = -1;
  assert(adapter.enterAfterSeconds(1) ==
         DeepSleepResult::ButtonConfigurationFailed);
  assert(resets == 2 && timers == 1 && starts == 0);

  resetFakes();
  FakePins duplicate;
  duplicate.next = 10;
  EspDeepSleepAdapter invalid_adapter(duplicate, functions);
  assert(!invalid_adapter.capabilitiesValid());
  assert(invalid_adapter.wakeButtonMask() == 0);
  assert(invalid_adapter.enterAfterSeconds(1) ==
         DeepSleepResult::InvalidCapabilities);

  // Only descriptor-supported buttons may be read or armed.
  resetFakes();
  FakePins next_only;
  next_only.supported = 1U << FakePins::index(WakeButton::Next);
  next_only.voice = -20;
  next_only.previous = -30;
  EspDeepSleepAdapter one_button(next_only, functions);
  assert(one_button.capabilitiesValid());
  assert(one_button.wakeButtonMask() == (1ULL << 9U));
  assert(next_only.pin_reads[0] == 0 && next_only.pin_reads[1] == 0);
  assert(one_button.wakeButtonsReleased());
  assert(next_only.pressed_reads[0] == 0 && next_only.pressed_reads[1] == 0);
  assert(one_button.enterAfterSeconds(2) == DeepSleepResult::EnterReturned);
  assert(timer_us == 2000000ULL && ext1_mask == (1ULL << 9U));
  assert(ext1_calls == 1 && starts == 1);
  wake_flags = kSystemWakeFlagExt1;
  wake_mask = 1ULL << 9U;
  assert(one_button.wakeCause() == WakeCause::NextButton);
  wake_mask = 1ULL << 1U;
  assert(one_button.wakeCause() == WakeCause::Unknown);
  assert(next_only.pin_reads[0] == 0 && next_only.pin_reads[1] == 0);

  // Zero wake buttons is a valid timer-only SKU and does not require or call
  // EXT1 functions.
  resetFakes();
  FakePins timer_only;
  timer_only.supported = 0U;
  const EspSleepFunctions timer_functions{
      &resetWake, &timerWake, nullptr, &wakeFlags, nullptr, &startSleep};
  EspDeepSleepAdapter no_buttons(timer_only, timer_functions);
  assert(no_buttons.capabilitiesValid());
  assert(no_buttons.wakeButtonMask() == 0U);
  assert(no_buttons.wakeButtonsReleased());
  assert(no_buttons.enterAfterSeconds(3) == DeepSleepResult::EnterReturned);
  assert(timer_us == 3000000ULL && ext1_calls == 0 && starts == 1);
  for (int reads : timer_only.pin_reads) assert(reads == 0);
  for (int reads : timer_only.pressed_reads) assert(reads == 0);
  wake_flags = kSystemWakeFlagTimer;
  assert(no_buttons.wakeCause() == WakeCause::Timer);
  wake_flags = kSystemWakeFlagExt1;
  assert(no_buttons.wakeCause() == WakeCause::Unknown);

  FakePins invalid_supported;
  invalid_supported.supported = 1U << FakePins::index(WakeButton::Next);
  invalid_supported.next = -1;
  EspDeepSleepAdapter invalid_supported_adapter(invalid_supported, functions);
  assert(!invalid_supported_adapter.capabilitiesValid());

  const EspSleepFunctions missing{};
  EspDeepSleepAdapter missing_adapter(pins, missing);
  assert(missing_adapter.enterAfterSeconds(1) ==
         DeepSleepResult::InvalidCapabilities);
  assert(missing_adapter.wakeCause() == WakeCause::Unknown);

  // A button wake can only request an indicator and optional local prompt.
  // It restores/syncs without panel mutation and consumes the held wake press
  // until every key has been released for the debounce interval.
  FakeWakeSeam seam;
  seam.network_failures_before_success = 1;
  WakeRecoveryRuntime recovery(seam);
  assert(recovery.begin(WakeCause::VoiceButton));
  assert(recovery.feedback().preserve_panel);
  assert(recovery.feedback().consume_wake_press);
  assert(recovery.feedback().request_awake_indicator);
  assert(recovery.feedback().request_device_restored_prompt);
  assert(recovery.stage() == WakeRecoveryStage::RequestIndicator);
  assert(!recovery.begin(WakeCause::PreviousButton));
  assert(recovery.poll(0).stage == WakeRecoveryStage::RestorePeripherals);
  assert(recovery.poll(1).stage == WakeRecoveryStage::ReconnectNetwork);
  WakeRecoveryObservation observed = recovery.poll(2);
  assert(observed.callback_failed &&
         recovery.stage() == WakeRecoveryStage::ReconnectNetwork);
  assert(!recovery.poll(1001U).attempted);
  assert(recovery.poll(1002U).stage == WakeRecoveryStage::SyncDeviceState);
  assert(recovery.poll(1003U).stage == WakeRecoveryStage::AwaitButtonRelease);
  assert(recovery.poll(1004U).stage == WakeRecoveryStage::AwaitButtonRelease);
  seam.released = true;
  assert(recovery.poll(1005U).stage == WakeRecoveryStage::AwaitButtonRelease);
  assert(recovery.poll(1054U).stage == WakeRecoveryStage::AwaitButtonRelease);
  assert(recovery.poll(1055U).stage == WakeRecoveryStage::RearmInput);
  assert(recovery.poll(1056U).stage == WakeRecoveryStage::RequestPrompt);
  assert(recovery.poll(1057U).stage == WakeRecoveryStage::Ready);
  assert(recovery.ready());
  assert(seam.indicators == 1 && seam.peripherals == 1 &&
         seam.networks == 2 && seam.syncs == 1 && seam.rearms == 1 &&
         seam.prompts == 1);

  // Timer wake restores services silently and never requests presentation.
  FakeWakeSeam timer_seam;
  WakeRecoveryRuntime timer_recovery(timer_seam);
  assert(timer_recovery.begin(WakeCause::Timer));
  assert(timer_recovery.feedback().preserve_panel);
  assert(!timer_recovery.feedback().consume_wake_press);
  assert(!timer_recovery.feedback().request_awake_indicator);
  timer_recovery.poll(0);
  timer_recovery.poll(1);
  timer_recovery.poll(2);
  timer_recovery.poll(3);
  assert(timer_recovery.ready());
  assert(timer_seam.indicators == 0 && timer_seam.prompts == 0 &&
         timer_seam.release_samples == 0);

  // Callback retry timing survives uint32 wrap and faults after the bounded
  // number of attempts rather than spinning forever.
  WakeRecoveryConfig bounded;
  bounded.retry_interval_ms = 1000U;
  bounded.maximum_attempts_per_stage = 2;
  FakeWakeSeam failing;
  failing.indicator_ok = false;
  WakeRecoveryRuntime bounded_recovery(failing, bounded);
  assert(bounded_recovery.begin(WakeCause::NextButton));
  const uint32_t near_wrap = 0xfffffff0U;
  assert(bounded_recovery.poll(near_wrap).callback_failed);
  assert(!bounded_recovery.poll(
      static_cast<uint32_t>(near_wrap + 999U)).attempted);
  observed = bounded_recovery.poll(
      static_cast<uint32_t>(near_wrap + 1000U));
  assert(observed.callback_failed &&
         bounded_recovery.stage() == WakeRecoveryStage::Fault);

  assert(!recovery.begin(WakeCause::ColdBoot));
  assert(recovery.stage() == WakeRecoveryStage::Fault);
  assert(!recovery.begin(static_cast<WakeCause>(255)));
  for (int value = 0; value < 256; ++value) {
    assert(wakeCauseName(static_cast<WakeCause>(value)) != nullptr);
    assert(wakeRecoveryStageName(
               static_cast<WakeRecoveryStage>(value)) != nullptr);
  }
  assert(std::strcmp(wakeCauseName(WakeCause::VoiceButton),
                     "VOICE_BUTTON") == 0);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-idf-power-wake-"));
  try {
    const source = join(scratch, "power_wake.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(power, "include"), "-I", join(native, "include"),
      source, join(power, "power_policy.cpp"),
      join(power, "power_runtime.cpp"),
      join(native, "esp_deep_sleep_adapter.cpp"), "-o", binary,
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

test("GPIO 1/9/10 and timer wake adapter pass strict C++17", () => {
  buildAndRun(false);
});

test("wake decode, retry and release gates pass ASan/UBSan", () => {
  buildAndRun(true);
});

test("native wake composition is capability-injected and preserves the panel", () => {
  const adapter = readFileSync(join(native, "esp_deep_sleep_adapter.cpp"), "utf8");
  const system = readFileSync(join(native, "esp_sleep_system_api.cpp"), "utf8");
  const bridge = readFileSync(join(native, "esp_board_wake_capabilities.cpp"), "utf8");
  const wakeHeader = readFileSync(
    join(power, "include/inkloop/power_runtime.hpp"), "utf8");
  const board = readFileSync(
    join(firmware, "boards/m5_papercolor_c151/board.cpp"), "utf8");
  const cmake = readFileSync(join(native, "CMakeLists.txt"), "utf8");
  const nativePower = readFileSync(
    join(firmware, "components/inkloop_product/native_power_owner.cpp"),
    "utf8",
  );
  const nativeVoice = readFileSync(
    join(firmware, "components/inkloop_product/native_voice_service.cpp"),
    "utf8",
  );
  const nativeVoiceHeader = readFileSync(
    join(
      firmware,
      "components/inkloop_product/include/inkloop/native_voice_service.hpp",
    ),
    "utf8",
  );
  const nativeInkloop = readFileSync(
    join(firmware, "components/inkloop_product/native_inkloop_service.cpp"),
    "utf8",
  );
  const productRuntime = readFileSync(
    join(firmware, "components/inkloop_product/product_runtime.cpp"),
    "utf8",
  );

  assert.match(board, /kPreviousButton\s*=\s*GPIO_NUM_10/);
  assert.match(board, /kNextButton\s*=\s*GPIO_NUM_9/);
  assert.match(board, /kVoiceButton\s*=\s*GPIO_NUM_1/);
  assert.match(bridge, /board_\.buttonGpio/);
  assert.match(bridge, /board_\.buttonPressed/);
  assert.match(bridge, /board_\.descriptor\(\)\.supportsButton/);
  assert.match(system, /esp_sleep_enable_timer_wakeup/);
  assert.match(system, /esp_sleep_enable_ext1_wakeup_io\([\s\S]*ESP_EXT1_WAKEUP_ANY_LOW/);
  assert.match(system, /esp_sleep_get_wakeup_causes/);
  assert.match(system, /esp_sleep_get_ext1_wakeup_status/);
  assert.match(system, /esp_deep_sleep_start/);
  assert.match(adapter, /supportsWakeButton\(button\)[\s\S]*continue/);
  assert.match(adapter, /button_mask != 0U[\s\S]*enable_ext1_any_low\(button_mask\)/);
  assert.doesNotMatch(adapter, /pins\[0\].*pins\[1\]|pins\[1\].*pins\[2\]/s);
  assert.doesNotMatch(adapter, /std::array<WakeButton,\s*3>/);
  assert.match(cmake, /REQUIRES inkloop_board inkloop_power esp_hw_support/);

  const forbiddenWakePresentation =
    /writeFullFrame|showStatus|systemPage|system_page|refreshPanel|renderPage/;
  assert.doesNotMatch(`${adapter}\n${wakeHeader}`, forbiddenWakePresentation);
  assert.match(wakeHeader, /requestAwakeIndicator/);
  assert.match(wakeHeader, /requestDeviceRestoredPrompt/);
  assert.match(wakeHeader, /restorePeripheralsPreservingPanel/);
  assert.match(wakeHeader, /syncMetadataPreservingPanel/);
  assert.match(
    nativePower,
    /syncMetadataPreservingPanel\(\)[\s\S]{0,500}inkloop_\.requestImmediateSync\(\)/,
  );
  assert.match(
    nativeInkloop,
    /requestImmediateSync\(\)[\s\S]{0,300}next_sync_ms_\s*=\s*now/,
  );
  assert.doesNotMatch(
    nativePower.match(/syncMetadataPreservingPanel\(\)[\s\S]*?\n\}/)?.[0] ?? "",
    /writeFullFrame|showStatus|refreshPanel|reloadCatalog/,
  );
  assert.match(
    productRuntime,
    /inkloop_\.portalTick\(wifi_\.online\(\),[\s\S]{0,220}!power_\.deferBackgroundPanel\(now\), onboarding\)/,
  );

  // Power admission follows actual work, never a presentation color. An idle
  // proactive MyAI connection preserves the false interaction authority,
  // while a button-raised turn remains active through Connecting.
  assert.match(nativeVoiceHeader, /bool interactiveAudioBusy\(\) const/);
  assert.match(nativeVoiceHeader, /bool aigcBusy\(\) const/);
  assert.match(
    nativeVoice,
    /bool NativeVoiceService::aigcBusy\(\) const[\s\S]{0,260}aigc_admission_pending_[\s\S]{0,100}aigc_phase_ != AigcPhase::Idle \|\| aigc_exclusive_/,
  );
  assert.match(
    nativeVoice,
    /enqueueImageGenerationImpl[\s\S]{0,1800}aigc_admission_pending_ = true[\s\S]{0,120}aigc_admission_ticket_ = ticket[\s\S]{0,1600}supervisor_\.post\(envelope\)/,
  );
  assert.match(
    nativeVoice,
    /enqueueImageGenerationImpl[\s\S]{0,3200}envelope\.deadline_ms\s*=\s*0/,
    "durable AIGC admission must not expire behind a blocking gateway tick",
  );
  assert.match(
    nativeVoice,
    /handoffAigcIfReady\(\)[\s\S]{0,1800}post\(\s*WorkClass::Portal,\s*ProductOpcode::PortalRunAigc,\s*0,\s*0\)/,
    "the accepted Network-to-Portal AIGC handoff must also be durable",
  );
  assert.match(
    nativeVoice,
    /acceptAigcPrompt\(std::string prompt,[\s\S]{0,900}aigc_admission_ticket_ == queued_ticket[\s\S]{0,500}aigc_admission_pending_ = false[\s\S]{0,160}aigc_phase_ = AigcPhase::PendingHandoff/,
  );
  assert.match(
    nativeVoice,
    /handleControlResult[\s\S]{0,2200}NetworkQueueAigc[\s\S]{0,500}disposition != WorkDisposition::Complete[\s\S]{0,180}text_pool_\.release\(envelope\.request_id\)[\s\S]{0,160}cancelQueuedAigcAdmission\(envelope\.request_id\)/,
  );
  assert.match(
    nativeVoice,
    /handleControlResult[\s\S]{0,2600}NetworkQueueAigc[\s\S]{0,700}envelope\.flags == 1U[\s\S]{0,200}SerialDiagnosticEventKind::AigcError/,
    "a diagnostic request must report terminal admission failure",
  );
  assert.match(
    nativePower,
    /PowerBlocker::AigcGeneration,[\s\S]{0,100}aigc_busy \|\| image == ImageLedMode::Generating/,
  );
  assert.match(
    nativePower,
    /capturePowerInputs\(PowerInputs& inputs\)[\s\S]{0,220}refreshBlockers\(inputs\.now_ms\)[\s\S]{0,180}activity_\.blockers\(\)/,
  );
  assert.match(
    nativePower,
    /quiesceNetwork\(\)[\s\S]{0,280}voice_\.portalBusy\(\)[\s\S]{0,280}wifi_\.prepareForSleep\(\)/,
  );
  assert.match(
    nativePower,
    /PowerBlocker::VoiceSession, voice_busy, now_ms/,
  );
  assert.match(
    nativePower,
    /quiesceVoiceAndAudio\(\)[\s\S]{0,180}!voice_\.interactiveAudioBusy\(\)/,
  );
  assert.doesNotMatch(nativePower, /voiceActive\(led\.voiceMode\(\)\)/);
  assert.match(
    nativeVoice,
    /if \(state != myai::VoiceState::Connecting\)[\s\S]{0,260}noteVoiceTurnActive/,
  );
  assert.match(
    nativeVoice,
    /case myai::VoiceState::Connecting:[\s\S]{0,420}voiceTurnActive\(\)[\s\S]{0,100}VoiceLedMode::Blocked/,
  );
});
