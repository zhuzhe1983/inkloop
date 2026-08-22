#pragma once

#include <cstdint>

#include "inkloop/power_policy.hpp"

namespace inkloop {

enum class DeepSleepResult : uint8_t {
  Entered,
  ButtonsHeld,
  InvalidCapabilities,
  InvalidTimer,
  WakeResetFailed,
  TimerConfigurationFailed,
  ButtonConfigurationFailed,
  EnterReturned,
};

const char* deepSleepResultName(DeepSleepResult result);

class IDeepSleepDriver {
 public:
  virtual ~IDeepSleepDriver() = default;
  virtual DeepSleepResult enterAfterSeconds(uint64_t timer_delay_seconds) = 0;
};

enum class PowerSnapshotResult : uint8_t {
  Captured,
  InvalidTarget,
  ClockUnavailable,
  TaskStoreUnavailable,
  TaskScheduleInvalid,
  UnknownFailure,
};

const char* powerSnapshotResultName(PowerSnapshotResult result);

class ISleepPreparation {
 public:
  virtual ~ISleepPreparation() = default;
  virtual PowerSnapshotResult capturePowerInputs(PowerInputs& inputs) = 0;
  virtual bool settleTasksAndSync() = 0;
  virtual bool quiesceDisplay() = 0;
  virtual bool quiesceVoiceAndAudio() = 0;
  virtual bool quiesceNetwork() = 0;
  // Called whenever preparation has changed runtime state but sleep cannot be
  // entered. Implementations must make the device usable again.
  virtual bool restoreAwakeServices() = 0;
};

enum class SleepAttemptResult : uint8_t {
  Entered,
  NotEligible,
  InitialSnapshotFailed,
  TaskSettleFailed,
  DisplayQuiescenceFailed,
  VoiceQuiescenceFailed,
  NetworkQuiescenceFailed,
  FinalSnapshotFailed,
  RecheckNotEligible,
  DeepSleepRejected,
};

const char* sleepAttemptResultName(SleepAttemptResult result);

struct SleepAttemptOutcome {
  SleepAttemptResult result = SleepAttemptResult::InitialSnapshotFailed;
  PowerSnapshotResult snapshot_result = PowerSnapshotResult::UnknownFailure;
  SleepDecision decision{};
  DeepSleepResult deep_sleep_result = DeepSleepResult::InvalidCapabilities;
  bool restore_attempted = false;
  bool restore_succeeded = true;
};

SleepAttemptOutcome executeSleepAttempt(const SleepPolicy& policy,
                                        ISleepPreparation& preparation,
                                        IDeepSleepDriver& driver);

struct SleepAttemptRuntimeConfig {
  uint32_t blocker_recheck_ms = 1000;
  uint32_t minimum_failure_retry_ms = 5000;
  uint32_t maximum_failure_retry_ms = 60000;
  uint32_t summary_interval_ms = 60000;
};

enum class SleepLogDisposition : uint8_t {
  None,
  Transition,
  Summary,
};

const char* sleepLogDispositionName(SleepLogDisposition disposition);

struct SleepAttemptObservation {
  bool attempted = false;
  SleepLogDisposition log = SleepLogDisposition::None;
  uint32_t suppressed_attempts = 0;
  SleepAttemptOutcome outcome{};
};

class SleepAttemptRuntime final {
 public:
  SleepAttemptRuntime() = default;
  explicit SleepAttemptRuntime(const SleepAttemptRuntimeConfig& config);

  SleepAttemptObservation poll(uint32_t now_ms, const SleepPolicy& policy,
                               ISleepPreparation& preparation,
                               IDeepSleepDriver& driver);

  bool configurationValid() const { return configuration_valid_; }
  uint32_t currentRetryMs() const { return current_retry_ms_; }

 private:
  static bool validConfig(const SleepAttemptRuntimeConfig& config);
  static bool sameOutcome(const SleepAttemptOutcome& left,
                          const SleepAttemptOutcome& right);
  static bool operationalFailure(const SleepAttemptOutcome& outcome);

  SleepAttemptRuntimeConfig config_{};
  bool configuration_valid_ = true;
  bool has_attempt_ = false;
  uint32_t last_attempt_ms_ = 0;
  uint32_t current_retry_ms_ = 1000;
  bool has_outcome_ = false;
  SleepAttemptOutcome last_outcome_{};
  uint32_t last_report_ms_ = 0;
  uint32_t suppressed_attempts_ = 0;
};

enum class WakeCause : uint8_t {
  ColdBoot,
  Timer,
  VoiceButton,
  PreviousButton,
  NextButton,
  MultipleButtons,
  MultipleSources,
  Unknown,
};

bool validWakeCause(WakeCause cause);
bool isButtonWakeCause(WakeCause cause);
const char* wakeCauseName(WakeCause cause);

struct WakeFeedbackContract {
  bool preserve_panel = true;
  bool consume_wake_press = false;
  bool request_awake_indicator = false;
  bool request_device_restored_prompt = false;
};

WakeFeedbackContract wakeFeedbackFor(WakeCause cause,
                                     bool prompt_on_button_wake);

class IWakeRecoverySeam {
 public:
  virtual ~IWakeRecoverySeam() = default;
  virtual bool requestAwakeIndicator() = 0;
  virtual bool restorePeripheralsPreservingPanel() = 0;
  virtual bool reconnectNetwork() = 0;
  // Reconcile schedules/metadata only. Due work is dispatched later through
  // the normal product loop, never as a side effect of a button wake.
  virtual bool syncMetadataPreservingPanel() = 0;
  virtual bool allWakeButtonsReleased() = 0;
  virtual bool rearmButtonInput() = 0;
  // This only queues an optional local prompt; it must not block recovery.
  virtual void requestDeviceRestoredPrompt() = 0;
};

enum class WakeRecoveryStage : uint8_t {
  NotStarted,
  RequestIndicator,
  RestorePeripherals,
  ReconnectNetwork,
  SyncDeviceState,
  AwaitButtonRelease,
  RearmInput,
  RequestPrompt,
  Ready,
  Fault,
};

const char* wakeRecoveryStageName(WakeRecoveryStage stage);

struct WakeRecoveryConfig {
  uint32_t retry_interval_ms = 1000;
  uint32_t release_debounce_ms = 50;
  uint16_t maximum_attempts_per_stage = 120;
  bool prompt_on_button_wake = true;
};

struct WakeRecoveryObservation {
  bool attempted = false;
  bool transition = false;
  bool callback_failed = false;
  WakeRecoveryStage stage = WakeRecoveryStage::NotStarted;
};

class WakeRecoveryRuntime final {
 public:
  WakeRecoveryRuntime(IWakeRecoverySeam& seam,
                      const WakeRecoveryConfig& config = {});

  bool begin(WakeCause cause);
  WakeRecoveryObservation poll(uint32_t now_ms);

  bool configurationValid() const { return configuration_valid_; }
  WakeCause cause() const { return cause_; }
  WakeRecoveryStage stage() const { return stage_; }
  const WakeFeedbackContract& feedback() const { return feedback_; }
  bool ready() const { return stage_ == WakeRecoveryStage::Ready; }

 private:
  static bool validConfig(const WakeRecoveryConfig& config);
  bool retryDue(uint32_t now_ms) const;
  void transitionTo(WakeRecoveryStage stage,
                    WakeRecoveryObservation& observation);
  void recordCallback(uint32_t now_ms, bool succeeded,
                      WakeRecoveryStage next,
                      WakeRecoveryObservation& observation);

  IWakeRecoverySeam& seam_;
  WakeRecoveryConfig config_{};
  bool configuration_valid_ = true;
  WakeCause cause_ = WakeCause::ColdBoot;
  WakeRecoveryStage stage_ = WakeRecoveryStage::NotStarted;
  WakeFeedbackContract feedback_{};
  uint16_t stage_attempts_ = 0;
  bool has_callback_attempt_ = false;
  uint32_t last_callback_attempt_ms_ = 0;
  bool release_candidate_active_ = false;
  uint32_t release_candidate_ms_ = 0;
};

}  // namespace inkloop
