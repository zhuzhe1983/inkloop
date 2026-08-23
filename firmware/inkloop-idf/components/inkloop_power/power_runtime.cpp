#include "inkloop/power_runtime.hpp"

#include <limits>

namespace inkloop {
namespace {

constexpr uint32_t kHalfUint32Range = 0x80000000UL;

void restoreAfterAbort(ISleepPreparation& preparation,
                       SleepAttemptOutcome& outcome) {
  outcome.restore_attempted = true;
  outcome.restore_succeeded = preparation.restoreAwakeServices();
}

}  // namespace

const char* deepSleepResultName(DeepSleepResult result) {
  switch (result) {
    case DeepSleepResult::Entered:
      return "ENTERED";
    case DeepSleepResult::ButtonsHeld:
      return "BUTTONS_HELD";
    case DeepSleepResult::InvalidCapabilities:
      return "INVALID_CAPABILITIES";
    case DeepSleepResult::InvalidTimer:
      return "INVALID_TIMER";
    case DeepSleepResult::WakeResetFailed:
      return "WAKE_RESET_FAILED";
    case DeepSleepResult::TimerConfigurationFailed:
      return "TIMER_CONFIGURATION_FAILED";
    case DeepSleepResult::ButtonConfigurationFailed:
      return "BUTTON_CONFIGURATION_FAILED";
    case DeepSleepResult::EnterReturned:
      return "ENTER_RETURNED";
  }
  return "UNKNOWN_DEEP_SLEEP_RESULT";
}

const char* powerSnapshotResultName(PowerSnapshotResult result) {
  switch (result) {
    case PowerSnapshotResult::Captured:
      return "CAPTURED";
    case PowerSnapshotResult::InvalidTarget:
      return "INVALID_TARGET";
    case PowerSnapshotResult::ClockUnavailable:
      return "CLOCK_UNAVAILABLE";
    case PowerSnapshotResult::TaskStoreUnavailable:
      return "TASK_STORE_UNAVAILABLE";
    case PowerSnapshotResult::TaskScheduleInvalid:
      return "TASK_SCHEDULE_INVALID";
    case PowerSnapshotResult::UnknownFailure:
      return "UNKNOWN_FAILURE";
  }
  return "UNKNOWN_POWER_SNAPSHOT_RESULT";
}

const char* sleepAttemptResultName(SleepAttemptResult result) {
  switch (result) {
    case SleepAttemptResult::Entered:
      return "ENTERED";
    case SleepAttemptResult::NotEligible:
      return "NOT_ELIGIBLE";
    case SleepAttemptResult::InitialSnapshotFailed:
      return "INITIAL_SNAPSHOT_FAILED";
    case SleepAttemptResult::TaskSettleFailed:
      return "TASK_SETTLE_FAILED";
    case SleepAttemptResult::DisplayQuiescenceFailed:
      return "DISPLAY_QUIESCENCE_FAILED";
    case SleepAttemptResult::VoiceQuiescenceFailed:
      return "VOICE_QUIESCENCE_FAILED";
    case SleepAttemptResult::NetworkQuiescenceFailed:
      return "NETWORK_QUIESCENCE_FAILED";
    case SleepAttemptResult::FinalSnapshotFailed:
      return "FINAL_SNAPSHOT_FAILED";
    case SleepAttemptResult::RecheckNotEligible:
      return "RECHECK_NOT_ELIGIBLE";
    case SleepAttemptResult::AdmissionFreezeFailed:
      return "ADMISSION_FREEZE_FAILED";
    case SleepAttemptResult::HardwareQuiescenceFailed:
      return "HARDWARE_QUIESCENCE_FAILED";
    case SleepAttemptResult::FinalAdmissionFailed:
      return "FINAL_ADMISSION_FAILED";
    case SleepAttemptResult::DeepSleepRejected:
      return "DEEP_SLEEP_REJECTED";
  }
  return "UNKNOWN_SLEEP_ATTEMPT_RESULT";
}

SleepAttemptOutcome executeSleepAttempt(const SleepPolicy& policy,
                                        ISleepPreparation& preparation,
                                        IDeepSleepDriver& driver) {
  SleepAttemptOutcome outcome;
  if (!policy.configurationValid() || !policy.config().enabled) {
    outcome.result = SleepAttemptResult::NotEligible;
    outcome.snapshot_result = PowerSnapshotResult::Captured;
    outcome.decision.reason = policy.configurationValid()
                                  ? SleepDecisionReason::Disabled
                                  : SleepDecisionReason::InvalidConfiguration;
    return outcome;
  }

  PowerInputs initial;
  outcome.snapshot_result = preparation.capturePowerInputs(initial);
  if (outcome.snapshot_result != PowerSnapshotResult::Captured) {
    outcome.result = SleepAttemptResult::InitialSnapshotFailed;
    return outcome;
  }
  outcome.decision = policy.evaluate(initial);
  if (!outcome.decision.should_sleep) {
    outcome.result = SleepAttemptResult::NotEligible;
    return outcome;
  }

  if (!preparation.settleTasksAndSync()) {
    outcome.result = SleepAttemptResult::TaskSettleFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }
  if (!preparation.quiesceDisplay()) {
    outcome.result = SleepAttemptResult::DisplayQuiescenceFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }
  if (!preparation.quiesceVoiceAndAudio()) {
    outcome.result = SleepAttemptResult::VoiceQuiescenceFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }
  if (!preparation.quiesceNetwork()) {
    outcome.result = SleepAttemptResult::NetworkQuiescenceFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }

  PowerInputs final_inputs;
  outcome.snapshot_result = preparation.capturePowerInputs(final_inputs);
  if (outcome.snapshot_result != PowerSnapshotResult::Captured) {
    outcome.result = SleepAttemptResult::FinalSnapshotFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }
  outcome.decision = policy.evaluate(final_inputs);
  if (!outcome.decision.should_sleep) {
    outcome.result = SleepAttemptResult::RecheckNotEligible;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }

  if (!preparation.freezeWorkAdmission()) {
    outcome.result = SleepAttemptResult::AdmissionFreezeFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }

  // Panel sleep, LEDs, codecs and board rails are deliberately deferred until
  // after the live recheck and admission freeze. If wake configuration or
  // deep-sleep entry returns, restoreAfterAbort() reverses this final hardware
  // transaction as well as the earlier network stop and admission freeze.
  if (!preparation.quiesceBoardHardware()) {
    outcome.result = SleepAttemptResult::HardwareQuiescenceFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }

  // A button interrupt during board quiescence is latched by the frozen
  // supervisor. Recheck that latch and physical wake inputs in the smallest
  // practical window before configuring wake sources and entering sleep.
  if (!preparation.sleepAdmissionStillSafe()) {
    outcome.result = SleepAttemptResult::FinalAdmissionFailed;
    restoreAfterAbort(preparation, outcome);
    return outcome;
  }

  outcome.deep_sleep_result =
      driver.enterAfterSeconds(outcome.decision.timer_delay_seconds);
  if (outcome.deep_sleep_result == DeepSleepResult::Entered) {
    outcome.result = SleepAttemptResult::Entered;
    return outcome;
  }
  outcome.result = SleepAttemptResult::DeepSleepRejected;
  restoreAfterAbort(preparation, outcome);
  return outcome;
}

const char* sleepLogDispositionName(SleepLogDisposition disposition) {
  switch (disposition) {
    case SleepLogDisposition::None:
      return "NONE";
    case SleepLogDisposition::Transition:
      return "TRANSITION";
    case SleepLogDisposition::Summary:
      return "SUMMARY";
  }
  return "UNKNOWN_SLEEP_LOG_DISPOSITION";
}

SleepAttemptRuntime::SleepAttemptRuntime(
    const SleepAttemptRuntimeConfig& config)
    : config_(config),
      configuration_valid_(validConfig(config)),
      current_retry_ms_(config.blocker_recheck_ms) {}

bool SleepAttemptRuntime::validConfig(
    const SleepAttemptRuntimeConfig& config) {
  return config.blocker_recheck_ms > 0 &&
         config.blocker_recheck_ms < kHalfUint32Range &&
         config.minimum_failure_retry_ms >= 5000U &&
         config.minimum_failure_retry_ms <= 60000U &&
         config.maximum_failure_retry_ms >=
             config.minimum_failure_retry_ms &&
         config.maximum_failure_retry_ms <= 60000U &&
         config.summary_interval_ms >= 60000U &&
         config.summary_interval_ms < kHalfUint32Range;
}

bool SleepAttemptRuntime::sameOutcome(const SleepAttemptOutcome& left,
                                      const SleepAttemptOutcome& right) {
  return left.result == right.result &&
         left.snapshot_result == right.snapshot_result &&
         left.decision.reason == right.decision.reason &&
         left.decision.blocker == right.decision.blocker &&
         left.deep_sleep_result == right.deep_sleep_result &&
         left.restore_attempted == right.restore_attempted &&
         left.restore_succeeded == right.restore_succeeded;
}

bool SleepAttemptRuntime::operationalFailure(
    const SleepAttemptOutcome& outcome) {
  return !outcome.restore_succeeded ||
         (outcome.result != SleepAttemptResult::Entered &&
          outcome.result != SleepAttemptResult::NotEligible &&
          outcome.result != SleepAttemptResult::RecheckNotEligible);
}

SleepAttemptObservation SleepAttemptRuntime::poll(
    uint32_t now_ms, const SleepPolicy& policy,
    ISleepPreparation& preparation, IDeepSleepDriver& driver) {
  SleepAttemptObservation observation;
  if (!configuration_valid_) return observation;
  if (has_attempt_ &&
      !elapsedAtLeast32(now_ms, last_attempt_ms_, current_retry_ms_)) {
    return observation;
  }

  observation.attempted = true;
  observation.outcome = executeSleepAttempt(policy, preparation, driver);
  const bool repeated =
      has_outcome_ && sameOutcome(last_outcome_, observation.outcome);
  has_attempt_ = true;
  last_attempt_ms_ = now_ms;

  if (operationalFailure(observation.outcome)) {
    if (!repeated) {
      current_retry_ms_ = config_.minimum_failure_retry_ms;
    } else {
      const uint32_t remaining =
          config_.maximum_failure_retry_ms - current_retry_ms_;
      current_retry_ms_ +=
          current_retry_ms_ > remaining ? remaining : current_retry_ms_;
    }
  } else {
    current_retry_ms_ = config_.blocker_recheck_ms;
  }

  if (!repeated) {
    observation.log = SleepLogDisposition::Transition;
    suppressed_attempts_ = 0;
    last_report_ms_ = now_ms;
  } else {
    if (suppressed_attempts_ != std::numeric_limits<uint32_t>::max()) {
      ++suppressed_attempts_;
    }
    if (elapsedAtLeast32(now_ms, last_report_ms_,
                         config_.summary_interval_ms)) {
      observation.log = SleepLogDisposition::Summary;
      observation.suppressed_attempts = suppressed_attempts_;
      suppressed_attempts_ = 0;
      last_report_ms_ = now_ms;
    }
  }
  has_outcome_ = true;
  last_outcome_ = observation.outcome;
  return observation;
}

bool validWakeCause(WakeCause cause) {
  return cause == WakeCause::ColdBoot || cause == WakeCause::Timer ||
         cause == WakeCause::VoiceButton ||
         cause == WakeCause::PreviousButton ||
         cause == WakeCause::NextButton ||
         cause == WakeCause::MultipleButtons ||
         cause == WakeCause::MultipleSources || cause == WakeCause::Unknown;
}

bool isButtonWakeCause(WakeCause cause) {
  return cause == WakeCause::VoiceButton ||
         cause == WakeCause::PreviousButton ||
         cause == WakeCause::NextButton ||
         cause == WakeCause::MultipleButtons ||
         cause == WakeCause::MultipleSources;
}

const char* wakeCauseName(WakeCause cause) {
  switch (cause) {
    case WakeCause::ColdBoot:
      return "COLD_BOOT";
    case WakeCause::Timer:
      return "TIMER";
    case WakeCause::VoiceButton:
      return "VOICE_BUTTON";
    case WakeCause::PreviousButton:
      return "PREVIOUS_BUTTON";
    case WakeCause::NextButton:
      return "NEXT_BUTTON";
    case WakeCause::MultipleButtons:
      return "MULTIPLE_BUTTONS";
    case WakeCause::MultipleSources:
      return "MULTIPLE_SOURCES";
    case WakeCause::Unknown:
      return "UNKNOWN";
  }
  return "INVALID_WAKE_CAUSE";
}

WakeFeedbackContract wakeFeedbackFor(WakeCause cause,
                                     bool prompt_on_button_wake) {
  WakeFeedbackContract feedback;
  if (!validWakeCause(cause)) return feedback;
  const bool button_wake = isButtonWakeCause(cause);
  feedback.consume_wake_press = button_wake;
  feedback.request_awake_indicator = button_wake;
  feedback.request_device_restored_prompt =
      button_wake && prompt_on_button_wake;
  return feedback;
}

const char* wakeRecoveryStageName(WakeRecoveryStage stage) {
  switch (stage) {
    case WakeRecoveryStage::NotStarted:
      return "NOT_STARTED";
    case WakeRecoveryStage::RequestIndicator:
      return "REQUEST_INDICATOR";
    case WakeRecoveryStage::RestorePeripherals:
      return "RESTORE_PERIPHERALS";
    case WakeRecoveryStage::ReconnectNetwork:
      return "RECONNECT_NETWORK";
    case WakeRecoveryStage::SyncDeviceState:
      return "SYNC_DEVICE_STATE";
    case WakeRecoveryStage::AwaitButtonRelease:
      return "AWAIT_BUTTON_RELEASE";
    case WakeRecoveryStage::RearmInput:
      return "REARM_INPUT";
    case WakeRecoveryStage::RequestPrompt:
      return "REQUEST_PROMPT";
    case WakeRecoveryStage::Ready:
      return "READY";
    case WakeRecoveryStage::Fault:
      return "FAULT";
  }
  return "UNKNOWN_WAKE_RECOVERY_STAGE";
}

WakeRecoveryRuntime::WakeRecoveryRuntime(IWakeRecoverySeam& seam,
                                         const WakeRecoveryConfig& config)
    : seam_(seam),
      config_(config),
      configuration_valid_(validConfig(config)) {}

bool WakeRecoveryRuntime::validConfig(const WakeRecoveryConfig& config) {
  return config.retry_interval_ms > 0 &&
         config.retry_interval_ms < kHalfUint32Range &&
         config.release_debounce_ms > 0 &&
         config.release_debounce_ms < kHalfUint32Range &&
         config.maximum_attempts_per_stage > 0;
}

bool WakeRecoveryRuntime::begin(WakeCause cause) {
  if (stage_ != WakeRecoveryStage::NotStarted &&
      stage_ != WakeRecoveryStage::Ready &&
      stage_ != WakeRecoveryStage::Fault) {
    return false;
  }
  cause_ = cause;
  feedback_ = wakeFeedbackFor(cause, config_.prompt_on_button_wake);
  stage_attempts_ = 0;
  has_callback_attempt_ = false;
  release_candidate_active_ = false;
  if (!configuration_valid_ || !validWakeCause(cause) ||
      cause == WakeCause::ColdBoot || cause == WakeCause::Unknown) {
    stage_ = WakeRecoveryStage::Fault;
    return false;
  }
  stage_ = feedback_.request_awake_indicator
               ? WakeRecoveryStage::RequestIndicator
               : WakeRecoveryStage::RestorePeripherals;
  return true;
}

bool WakeRecoveryRuntime::retryDue(uint32_t now_ms) const {
  return !has_callback_attempt_ ||
         elapsedAtLeast32(now_ms, last_callback_attempt_ms_,
                          config_.retry_interval_ms);
}

void WakeRecoveryRuntime::transitionTo(
    WakeRecoveryStage stage, WakeRecoveryObservation& observation) {
  stage_ = stage;
  stage_attempts_ = 0;
  has_callback_attempt_ = false;
  observation.transition = true;
  observation.stage = stage_;
}

void WakeRecoveryRuntime::recordCallback(
    uint32_t now_ms, bool succeeded, WakeRecoveryStage next,
    WakeRecoveryObservation& observation) {
  observation.attempted = true;
  has_callback_attempt_ = true;
  last_callback_attempt_ms_ = now_ms;
  if (succeeded) {
    transitionTo(next, observation);
    return;
  }
  observation.callback_failed = true;
  if (stage_attempts_ != std::numeric_limits<uint16_t>::max()) {
    ++stage_attempts_;
  }
  if (stage_attempts_ >= config_.maximum_attempts_per_stage) {
    transitionTo(WakeRecoveryStage::Fault, observation);
  }
}

WakeRecoveryObservation WakeRecoveryRuntime::poll(uint32_t now_ms) {
  WakeRecoveryObservation observation;
  observation.stage = stage_;
  switch (stage_) {
    case WakeRecoveryStage::RequestIndicator:
      if (retryDue(now_ms)) {
        recordCallback(now_ms, seam_.requestAwakeIndicator(),
                       WakeRecoveryStage::RestorePeripherals, observation);
      }
      break;
    case WakeRecoveryStage::RestorePeripherals:
      if (retryDue(now_ms)) {
        recordCallback(now_ms, seam_.restorePeripheralsPreservingPanel(),
                       WakeRecoveryStage::ReconnectNetwork, observation);
      }
      break;
    case WakeRecoveryStage::ReconnectNetwork:
      if (retryDue(now_ms)) {
        recordCallback(now_ms, seam_.reconnectNetwork(),
                       WakeRecoveryStage::SyncDeviceState, observation);
      }
      break;
    case WakeRecoveryStage::SyncDeviceState:
      if (retryDue(now_ms)) {
        recordCallback(
            now_ms, seam_.syncMetadataPreservingPanel(),
            feedback_.consume_wake_press
                ? WakeRecoveryStage::AwaitButtonRelease
                : WakeRecoveryStage::RearmInput,
            observation);
      }
      break;
    case WakeRecoveryStage::AwaitButtonRelease:
      observation.attempted = true;
      if (!seam_.allWakeButtonsReleased()) {
        release_candidate_active_ = false;
        break;
      }
      if (!release_candidate_active_) {
        release_candidate_active_ = true;
        release_candidate_ms_ = now_ms;
        break;
      }
      if (elapsedAtLeast32(now_ms, release_candidate_ms_,
                           config_.release_debounce_ms)) {
        transitionTo(WakeRecoveryStage::RearmInput, observation);
      }
      break;
    case WakeRecoveryStage::RearmInput:
      if (retryDue(now_ms)) {
        recordCallback(
            now_ms, seam_.rearmButtonInput(),
            feedback_.request_device_restored_prompt
                ? WakeRecoveryStage::RequestPrompt
                : WakeRecoveryStage::Ready,
            observation);
      }
      break;
    case WakeRecoveryStage::RequestPrompt:
      observation.attempted = true;
      seam_.requestDeviceRestoredPrompt();
      transitionTo(WakeRecoveryStage::Ready, observation);
      break;
    case WakeRecoveryStage::Ready:
    case WakeRecoveryStage::Fault:
    case WakeRecoveryStage::NotStarted:
      break;
  }
  observation.stage = stage_;
  return observation;
}

}  // namespace inkloop
