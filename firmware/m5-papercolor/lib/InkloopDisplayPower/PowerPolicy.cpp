#include "PowerPolicy.h"

#include <algorithm>
#include <limits>

namespace inkloop {
namespace displaypower {

bool SleepBlockers::any() const {
  return audioActive || generationActive || downloadActive || conversionActive ||
      writeActive || portalActive || taskFinalizationActive || voiceActive ||
      displayActive || pendingJournal || unacknowledgedTask || pairingActive ||
      onboardingActive || portalRequestActive || tutorialActive ||
      externalPagePending;
}

const char* sleepDecisionReasonName(SleepDecisionReason reason) {
  switch (reason) {
    case SleepDecisionReason::Eligible: return "ELIGIBLE";
    case SleepDecisionReason::AlwaysAwakeMode: return "ALWAYS_AWAKE_MODE";
    case SleepDecisionReason::InvalidClock: return "INVALID_CLOCK";
    case SleepDecisionReason::IdlePeriodNotReached: return "IDLE_PERIOD_NOT_REACHED";
    case SleepDecisionReason::AudioActive: return "AUDIO_ACTIVE";
    case SleepDecisionReason::GenerationActive: return "GENERATION_ACTIVE";
    case SleepDecisionReason::ConversionActive: return "CONVERSION_ACTIVE";
    case SleepDecisionReason::WriteActive: return "WRITE_ACTIVE";
    case SleepDecisionReason::TaskFinalizationActive:
      return "TASK_FINALIZATION_ACTIVE";
    case SleepDecisionReason::VoiceActive: return "VOICE_ACTIVE";
    case SleepDecisionReason::DisplayActive: return "DISPLAY_ACTIVE";
    case SleepDecisionReason::DownloadActive: return "DOWNLOAD_ACTIVE";
    case SleepDecisionReason::PendingJournal: return "PENDING_JOURNAL";
    case SleepDecisionReason::PortalActive: return "PORTAL_ACTIVE";
    case SleepDecisionReason::UnacknowledgedTask:
      return "UNACKNOWLEDGED_TASK";
    case SleepDecisionReason::PairingActive: return "PAIRING_ACTIVE";
    case SleepDecisionReason::OnboardingActive: return "ONBOARDING_ACTIVE";
    case SleepDecisionReason::PortalRequestActive: return "PORTAL_REQUEST_ACTIVE";
    case SleepDecisionReason::TutorialActive: return "TUTORIAL_ACTIVE";
    case SleepDecisionReason::ExternalPagePending:
      return "EXTERNAL_PAGE_PENDING";
    case SleepDecisionReason::WakeButtonsHeld: return "WAKE_BUTTONS_HELD";
    case SleepDecisionReason::WakeDeadlineDue: return "WAKE_DEADLINE_DUE";
    case SleepDecisionReason::SleepWindowTooShort: return "SLEEP_WINDOW_TOO_SHORT";
  }
  return "UNKNOWN_DECISION_REASON";
}

PowerPolicy::PowerPolicy() : config_() {}

PowerPolicy::PowerPolicy(const PowerPolicyConfig& config) : config_() {
  setConfig(config);
}

bool PowerPolicy::validConfig(const PowerPolicyConfig& config) {
  return validPowerMode(config.mode) &&
      config.eligibleIdleMilliseconds >= 120000U &&
      config.eligibleIdleMilliseconds < 0x80000000UL &&
      config.heartbeatWakeIntervalSeconds >= 60U &&
      config.heartbeatWakeIntervalSeconds <= 86400U &&
      config.rtcConnectionMarginSeconds <= 600U &&
      config.rtcConnectionMarginSeconds < config.heartbeatWakeIntervalSeconds &&
      config.minimumUsefulSleepSeconds <= 3600U;
}

bool PowerPolicy::validPowerMode(PowerMode mode) {
  return mode == PowerMode::AlwaysAwake || mode == PowerMode::BatteryOptIn;
}

bool PowerPolicy::setConfig(const PowerPolicyConfig& config) {
  if (!validConfig(config)) return false;
  config_ = config;
  return true;
}

uint64_t PowerPolicy::paperColorExt1AnyLowMask() {
  return (1ULL << 1U) | (1ULL << 9U) | (1ULL << 10U);
}

SleepDecisionReason PowerPolicy::blockerReason(const SleepBlockers& blockers) {
  if (blockers.audioActive) return SleepDecisionReason::AudioActive;
  if (blockers.generationActive) return SleepDecisionReason::GenerationActive;
  if (blockers.conversionActive) return SleepDecisionReason::ConversionActive;
  if (blockers.writeActive) return SleepDecisionReason::WriteActive;
  if (blockers.taskFinalizationActive) {
    return SleepDecisionReason::TaskFinalizationActive;
  }
  if (blockers.voiceActive) return SleepDecisionReason::VoiceActive;
  if (blockers.displayActive) return SleepDecisionReason::DisplayActive;
  if (blockers.downloadActive) return SleepDecisionReason::DownloadActive;
  if (blockers.pendingJournal) return SleepDecisionReason::PendingJournal;
  if (blockers.pairingActive) return SleepDecisionReason::PairingActive;
  if (blockers.portalRequestActive) return SleepDecisionReason::PortalRequestActive;
  if (blockers.tutorialActive) return SleepDecisionReason::TutorialActive;
  if (blockers.onboardingActive) return SleepDecisionReason::OnboardingActive;
  if (blockers.externalPagePending) {
    return SleepDecisionReason::ExternalPagePending;
  }
  if (blockers.portalActive) return SleepDecisionReason::PortalActive;
  if (blockers.unacknowledgedTask) return SleepDecisionReason::UnacknowledgedTask;
  return SleepDecisionReason::Eligible;
}

uint64_t PowerPolicy::adjustedWakeDeadline(
    uint64_t deadline,
    uint64_t nowEpochSeconds) const {
  if (deadline <= nowEpochSeconds) return nowEpochSeconds;
  const uint64_t margin = config_.rtcConnectionMarginSeconds;
  if (deadline <= margin || deadline - margin <= nowEpochSeconds) {
    return nowEpochSeconds;
  }
  return deadline - margin;
}

SleepDecision PowerPolicy::evaluate(const PowerInputs& inputs) const {
  SleepDecision decision;
  decision.wake.ext1AnyLowMask = paperColorExt1AnyLowMask();
  if (!validPowerMode(config_.mode) || config_.mode != PowerMode::BatteryOptIn) {
    decision.reason = SleepDecisionReason::AlwaysAwakeMode;
    return decision;
  }
  if (!inputs.rtcSynchronized || inputs.rtcNowEpochSeconds == 0) {
    decision.reason = SleepDecisionReason::InvalidClock;
    return decision;
  }
  if (inputs.blockers.any()) {
    decision.reason = blockerReason(inputs.blockers);
    return decision;
  }
  if (!elapsedAtLeast32(
          inputs.nowMilliseconds,
          inputs.lastMeaningfulActivityMilliseconds,
          config_.eligibleIdleMilliseconds)) {
    decision.reason = SleepDecisionReason::IdlePeriodNotReached;
    return decision;
  }
  if (!inputs.wakeButtonsReleased) {
    decision.reason = SleepDecisionReason::WakeButtonsHeld;
    return decision;
  }

  if (inputs.rtcNowEpochSeconds >
      std::numeric_limits<uint64_t>::max() - config_.heartbeatWakeIntervalSeconds) {
    decision.reason = SleepDecisionReason::InvalidClock;
    return decision;
  }
  const uint64_t configuredHeartbeat =
      inputs.rtcNowEpochSeconds + config_.heartbeatWakeIntervalSeconds;
  uint64_t heartbeat = inputs.nextHeartbeatEpochSeconds;
  if (heartbeat == 0) {
    heartbeat = configuredHeartbeat;
  } else if (heartbeat <= inputs.rtcNowEpochSeconds) {
    decision.reason = SleepDecisionReason::WakeDeadlineDue;
    return decision;
  } else if (heartbeat > configuredHeartbeat) {
    heartbeat = configuredHeartbeat;
  }
  uint64_t wakeAt = adjustedWakeDeadline(heartbeat, inputs.rtcNowEpochSeconds);

  if (inputs.nextLocalTaskEpochSeconds != 0) {
    if (inputs.nextLocalTaskEpochSeconds <= inputs.rtcNowEpochSeconds) {
      decision.reason = SleepDecisionReason::WakeDeadlineDue;
      return decision;
    }
    const uint64_t taskWake = adjustedWakeDeadline(
        inputs.nextLocalTaskEpochSeconds, inputs.rtcNowEpochSeconds);
    if (taskWake < wakeAt) wakeAt = taskWake;
  }
  if (wakeAt <= inputs.rtcNowEpochSeconds) {
    decision.reason = SleepDecisionReason::WakeDeadlineDue;
    return decision;
  }
  if (wakeAt - inputs.rtcNowEpochSeconds < config_.minimumUsefulSleepSeconds) {
    decision.reason = SleepDecisionReason::SleepWindowTooShort;
    return decision;
  }
  decision.shouldSleep = true;
  decision.reason = SleepDecisionReason::Eligible;
  decision.wake.timerEnabled = true;
  decision.wake.timerWakeEpochSeconds = wakeAt;
  return decision;
}

WakeReason wakeReasonFromExt1Mask(uint64_t ext1Mask) {
  if ((ext1Mask & ~PowerPolicy::paperColorExt1AnyLowMask()) != 0) {
    return WakeReason::Unknown;
  }
  const uint64_t supported = ext1Mask & PowerPolicy::paperColorExt1AnyLowMask();
  if (supported == 0) return WakeReason::Unknown;
  if ((supported & (supported - 1ULL)) != 0) return WakeReason::MultipleButtons;
  if (supported == (1ULL << 1U)) return WakeReason::TopButton;
  if (supported == (1ULL << 10U)) return WakeReason::PreviousButton;
  if (supported == (1ULL << 9U)) return WakeReason::NextButton;
  return WakeReason::Unknown;
}

bool validWakeReason(WakeReason reason) {
  return reason == WakeReason::ColdBoot || reason == WakeReason::TopButton ||
      reason == WakeReason::PreviousButton || reason == WakeReason::NextButton ||
      reason == WakeReason::MultipleButtons || reason == WakeReason::RtcTimer ||
      reason == WakeReason::Unknown;
}

WakeReconnectState::WakeReconnectState()
    : wakeReason_(WakeReason::ColdBoot), stage_(ReconnectStage::NotStarted) {}

bool WakeReconnectState::begin(WakeReason reason) {
  if (!validWakeReason(reason)) {
    wakeReason_ = WakeReason::Unknown;
    stage_ = ReconnectStage::Fault;
    return false;
  }
  if (stage_ != ReconnectStage::NotStarted && stage_ != ReconnectStage::Ready) {
    return false;
  }
  wakeReason_ = reason;
  stage_ = ReconnectStage::ReinitializeHardware;
  return true;
}

bool WakeReconnectState::markHardwareReady() {
  if (stage_ != ReconnectStage::ReinitializeHardware) return false;
  stage_ = ReconnectStage::ReconnectWifi;
  return true;
}

bool WakeReconnectState::markWifiConnected() {
  if (stage_ != ReconnectStage::ReconnectWifi) return false;
  stage_ = ReconnectStage::SyncInkloop;
  return true;
}

bool WakeReconnectState::markInkloopSynced() {
  if (stage_ != ReconnectStage::SyncInkloop) return false;
  stage_ = ReconnectStage::AwaitWakeButtonsReleased;
  return true;
}

bool WakeReconnectState::markWakeButtonsReleasedDebounced(
    bool topReleased,
    bool previousReleased,
    bool nextReleased) {
  if (stage_ != ReconnectStage::AwaitWakeButtonsReleased || !topReleased ||
      !previousReleased || !nextReleased) {
    return false;
  }
  stage_ = ReconnectStage::ArmInput;
  return true;
}

bool WakeReconnectState::markInputRearmed() {
  if (stage_ != ReconnectStage::ArmInput) return false;
  stage_ = ReconnectStage::Ready;
  return true;
}

void WakeReconnectState::markFault() {
  stage_ = ReconnectStage::Fault;
}

bool WakeReconnectState::wifiReconnectRequired() const {
  return stage_ == ReconnectStage::ReconnectWifi ||
      stage_ == ReconnectStage::SyncInkloop;
}

}  // namespace displaypower
}  // namespace inkloop
