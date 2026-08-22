#include "inkloop/native_power_owner.hpp"

#include <new>

#include <ctime>

#include "esp_log.h"
#include "inkloop/product_opcodes.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-power-own";
constexpr uint32_t kPortalActivityWindowMs = 30000U;
constexpr uint32_t kButtonWakePanelGuardMs = 5000U;

SleepPolicy configuredSleepPolicy() {
  SleepPolicyConfig config;
  config.enabled = true;
  config.eligible_idle_ms = 120000U;
  config.heartbeat_interval_seconds = 300U;
  config.rtc_margin_seconds = 30U;
  config.minimum_useful_sleep_seconds = 10U;
  return SleepPolicy(config);
}

}  // namespace

NativePowerOwner::NativePowerOwner(
    IBoardAdapter& board, RuntimeSupervisor& supervisor,
    EspButtonInputOwner& buttons, EspStatusLedOwner& leds,
    EspWifiStationOwner& wifi, NativeDisplayService& display,
    NativeVoiceService& voice, NativeInkloopService& inkloop,
    NativePortalOwner& portal)
    : board_(board), supervisor_(supervisor), buttons_(buttons), leds_(leds),
      wifi_(wifi), display_(display), voice_(voice), inkloop_(inkloop),
      portal_(portal),
      wake_capabilities_(board),
      deep_sleep_(wake_capabilities_, systemEspSleepFunctions()),
      policy_(configuredSleepPolicy()), recovery_(*this) {}

bool NativePowerOwner::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

esp_err_t NativePowerOwner::initialize(uint32_t now_ms) {
  if (initialized_ || !policy_.configurationValid() ||
      !deep_sleep_.capabilitiesValid()) {
    return ESP_ERR_INVALID_STATE;
  }
  boot_wake_cause_ = deep_sleep_.wakeCause();
  activity_.noteMeaningfulActivity(now_ms);
  initialized_ = true;
  return ESP_OK;
}

esp_err_t NativePowerOwner::afterSupervisorStarted(uint32_t now_ms) {
  if (!initialized_ || !supervisor_.started()) return ESP_ERR_INVALID_STATE;
  if (boot_wake_cause_ == WakeCause::ColdBoot ||
      boot_wake_cause_ == WakeCause::Unknown) {
    return buttons_.arm();
  }
  if (!recovery_.begin(boot_wake_cause_)) return buttons_.arm();
  recovery_active_ = true;
  capture_now_ms_ = now_ms;
  ESP_LOGI(kTag, "wake recovery started cause=%s panel=preserved",
           wakeCauseName(boot_wake_cause_));
  return ESP_OK;
}

void NativePowerOwner::shutdown() {
  portENTER_CRITICAL(&mux_);
  activity_ = PowerActivityState();
  request_sequence_ = 0U;
  next_heartbeat_epoch_ = 0U;
  capture_now_ms_ = 0U;
  preserve_panel_until_ms_ = 0U;
  recovery_active_ = false;
  network_quiesced_ = false;
  initialized_ = false;
  portEXIT_CRITICAL(&mux_);
  attempts_ = SleepAttemptRuntime();
  recovery_.~WakeRecoveryRuntime();
  new (&recovery_) WakeRecoveryRuntime(*this);
  boot_wake_cause_ = WakeCause::Unknown;
}

void NativePowerOwner::noteButtonActivity(uint32_t now_ms) {
  portENTER_CRITICAL(&mux_);
  activity_.noteMeaningfulActivity(now_ms);
  preserve_panel_until_ms_ = 0;
  portEXIT_CRITICAL(&mux_);
}

bool NativePowerOwner::recovering() const {
  return recovery_active_ && !recovery_.ready() &&
         recovery_.stage() != WakeRecoveryStage::Fault;
}

bool NativePowerOwner::deferBackgroundPanel(uint32_t now_ms) const {
  portENTER_CRITICAL(&mux_);
  const uint32_t deadline = preserve_panel_until_ms_;
  portEXIT_CRITICAL(&mux_);
  return deadline != 0U && !due(now_ms, deadline);
}

void NativePowerOwner::refreshBlockers(uint32_t now_ms) {
  const StatusLedCore led = leds_.snapshot();
  const ImageLedMode image = led.imageMode();
  const NativeMyAiOnboardingSnapshot onboarding = voice_.onboardingSnapshot();
  const bool voice_busy = voice_.interactiveAudioBusy();
  const uint32_t portal_access = portal_.lastAccessMs();
  const bool portal_recent = portal_access != 0U &&
      static_cast<uint32_t>(now_ms - portal_access) <
          kPortalActivityWindowMs;
  portENTER_CRITICAL(&mux_);
  activity_.setBlocker(PowerBlocker::DisplayRefresh, display_.busy(), now_ms);
  activity_.setBlocker(PowerBlocker::VoiceSession, voice_busy, now_ms);
  activity_.setBlocker(PowerBlocker::AigcGeneration,
                       image == ImageLedMode::Generating, now_ms);
  activity_.setBlocker(PowerBlocker::AssetDownload,
                       image == ImageLedMode::Downloading, now_ms);
  activity_.setBlocker(PowerBlocker::ContentConversion,
                       image == ImageLedMode::Converting, now_ms);
  activity_.setBlocker(PowerBlocker::TaskFinalization,
                       image == ImageLedMode::Writing, now_ms);
  activity_.setBlocker(PowerBlocker::TaskSync, inkloop_.busy(), now_ms);
  activity_.setBlocker(PowerBlocker::AlbumUpload, portal_.mutationBusy(),
                       now_ms);
  activity_.setBlocker(
      PowerBlocker::Pairing,
      onboarding.activation_state == myai::ActivationState::Pairing, now_ms);
  activity_.setBlocker(PowerBlocker::PortalSession,
                       portal_recent || wifi_.provisioningActive(), now_ms);
  portEXIT_CRITICAL(&mux_);
}

void NativePowerOwner::tick(uint32_t now_ms) {
  if (!initialized_) return;
  capture_now_ms_ = now_ms;
  if (recovery_active_) {
    const WakeRecoveryObservation observation = recovery_.poll(now_ms);
    if (observation.transition) {
      ESP_LOGI(kTag, "wake recovery stage=%s",
               wakeRecoveryStageName(observation.stage));
    }
    if (recovery_.ready()) {
      recovery_active_ = false;
      noteButtonActivity(now_ms);
      ESP_LOGI(kTag, "wake recovery complete; input rearmed");
    } else if (recovery_.stage() == WakeRecoveryStage::Fault) {
      recovery_active_ = false;
      const esp_err_t armed = buttons_.arm();
      ESP_LOGE(kTag, "wake recovery failed; input fallback=%s",
               esp_err_to_name(armed));
    }
    return;
  }

  refreshBlockers(now_ms);
  const SleepAttemptObservation observation =
      attempts_.poll(now_ms, policy_, *this, deep_sleep_);
  if (observation.log != SleepLogDisposition::None) {
    ESP_LOGI(kTag,
             "sleep result=%s reason=%s blocker=%s retry_ms=%lu suppressed=%lu",
             sleepAttemptResultName(observation.outcome.result),
             sleepDecisionReasonName(observation.outcome.decision.reason),
             powerBlockerName(observation.outcome.decision.blocker),
             static_cast<unsigned long>(attempts_.currentRetryMs()),
             static_cast<unsigned long>(observation.suppressed_attempts));
  }
}

PowerSnapshotResult NativePowerOwner::capturePowerInputs(PowerInputs& inputs) {
  inputs = PowerInputs();
  inputs.now_ms = capture_now_ms_;
  portENTER_CRITICAL(&mux_);
  inputs.last_meaningful_activity_ms = activity_.lastMeaningfulActivityMs();
  inputs.blockers = activity_.blockers();
  portEXIT_CRITICAL(&mux_);
  inputs.wake_buttons_released = deep_sleep_.wakeButtonsReleased();
  const EspWifiStationSnapshot wifi = wifi_.snapshot();
  const std::time_t now = std::time(nullptr);
  inputs.rtc_synchronized = wifi.clock_synchronized && now >= 1700000000;
  if (!inputs.rtc_synchronized) return PowerSnapshotResult::Captured;
  inputs.rtc_epoch_seconds = static_cast<uint64_t>(now);
  if (next_heartbeat_epoch_ <= inputs.rtc_epoch_seconds) {
    next_heartbeat_epoch_ = inputs.rtc_epoch_seconds +
                            policy_.config().heartbeat_interval_seconds;
  }
  inputs.next_heartbeat_epoch_seconds = next_heartbeat_epoch_;
  if (inputs.blockers.any() || !inputs.wake_buttons_released ||
      !elapsedAtLeast32(inputs.now_ms,
                        inputs.last_meaningful_activity_ms,
                        policy_.config().eligible_idle_ms)) {
    return PowerSnapshotResult::Captured;
  }
  uint64_t next_task = 0;
  const storage::TaskStoreCode task_status =
      inkloop_.nextTaskEpoch(now, next_task);
  if (task_status != storage::TaskStoreCode::Ok) {
    return task_status == storage::TaskStoreCode::InvalidRecord
               ? PowerSnapshotResult::TaskScheduleInvalid
               : PowerSnapshotResult::TaskStoreUnavailable;
  }
  inputs.next_task_epoch_seconds = next_task;
  return PowerSnapshotResult::Captured;
}

bool NativePowerOwner::settleTasksAndSync() {
  return !display_.busy() && !voice_.portalBusy() && !inkloop_.busy() &&
         !portal_.mutationBusy();
}

bool NativePowerOwner::quiesceDisplay() {
  IBoardDisplay* panel = board_.display();
  return panel && !display_.busy() && !panel->busy();
}

bool NativePowerOwner::quiesceVoiceAndAudio() {
  const StatusLedCore led = leds_.snapshot();
  return !voice_.portalBusy() && !voice_.interactiveAudioBusy() &&
         led.imageMode() != ImageLedMode::Generating &&
         led.imageMode() != ImageLedMode::Downloading &&
         led.imageMode() != ImageLedMode::Converting &&
         led.imageMode() != ImageLedMode::Writing;
}

bool NativePowerOwner::quiesceNetwork() {
  if (wifi_.provisioningActive()) return false;
  network_quiesced_ = wifi_.prepareForSleep() == ESP_OK;
  return network_quiesced_;
}

bool NativePowerOwner::restoreAwakeServices() {
  if (!network_quiesced_) return true;
  const esp_err_t restored = wifi_.restoreAfterSleep(capture_now_ms_);
  if (restored == ESP_OK) network_quiesced_ = false;
  return restored == ESP_OK;
}

uint64_t NativePowerOwner::nextRequestId() {
  portENTER_CRITICAL(&mux_);
  const uint64_t value = ++request_sequence_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool NativePowerOwner::postVoiceLed(VoiceLedMode mode) {
  WorkEnvelope command{};
  command.generation = 1;
  command.request_id = nextRequestId();
  command.opcode = productOpcode(ProductOpcode::SetVoiceLed);
  command.work_class = WorkClass::LedStatus;
  command.kind = EnvelopeKind::Command;
  command.disposition = WorkDisposition::Accepted;
  command.flags = static_cast<uint8_t>(mode);
  return supervisor_.post(command) == AdmissionResult::Admitted;
}

bool NativePowerOwner::requestAwakeIndicator() {
  return postVoiceLed(VoiceLedMode::Ready);
}

bool NativePowerOwner::restorePeripheralsPreservingPanel() {
  // A deep-sleep wake is a fresh boot. Board/display initialization already
  // happened in app_main; deliberately do not draw or sleep/wake the panel.
  return board_.display() != nullptr;
}

bool NativePowerOwner::reconnectNetwork() {
  // EspWifiStationOwner has already started its non-blocking saved-credential
  // connection during composition. Recovery must not wait for association.
  return true;
}

bool NativePowerOwner::syncMetadataPreservingPanel() {
  if (isButtonWakeCause(boot_wake_cause_)) {
    portENTER_CRITICAL(&mux_);
    preserve_panel_until_ms_ = capture_now_ms_ + kButtonWakePanelGuardMs;
    portEXIT_CRITICAL(&mux_);
  }
  // This only advances the Portal-owned deadline. Wi-Fi association remains
  // non-blocking and the existing panel is never redrawn during recovery.
  return inkloop_.requestImmediateSync();
}

bool NativePowerOwner::allWakeButtonsReleased() {
  return deep_sleep_.wakeButtonsReleased();
}

bool NativePowerOwner::rearmButtonInput() {
  return buttons_.arm() == ESP_OK;
}

void NativePowerOwner::requestDeviceRestoredPrompt() {
  voice_.enqueueLocalPrompt(LocalPrompt::DeviceRestored);
}

}  // namespace inkloop
