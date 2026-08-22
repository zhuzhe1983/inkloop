#include "inkloop/power_policy.hpp"

#include <limits>

namespace inkloop {
namespace {

constexpr uint32_t kHalfUint32Range = 0x80000000UL;

}  // namespace

bool validPowerBlocker(PowerBlocker blocker) {
  const uint8_t value = static_cast<uint8_t>(blocker);
  return value > static_cast<uint8_t>(PowerBlocker::None) &&
         value < static_cast<uint8_t>(PowerBlocker::Count);
}

const char* powerBlockerName(PowerBlocker blocker) {
  switch (blocker) {
    case PowerBlocker::None:
      return "NONE";
    case PowerBlocker::PagePending:
      return "PAGE_PENDING";
    case PowerBlocker::DisplayRefresh:
      return "DISPLAY_REFRESH";
    case PowerBlocker::VoiceSession:
      return "VOICE_SESSION";
    case PowerBlocker::AudioPlayback:
      return "AUDIO_PLAYBACK";
    case PowerBlocker::AigcGeneration:
      return "AIGC_GENERATION";
    case PowerBlocker::AssetDownload:
      return "ASSET_DOWNLOAD";
    case PowerBlocker::ContentConversion:
      return "CONTENT_CONVERSION";
    case PowerBlocker::AlbumUpload:
      return "ALBUM_UPLOAD";
    case PowerBlocker::TaskSync:
      return "TASK_SYNC";
    case PowerBlocker::TaskFinalization:
      return "TASK_FINALIZATION";
    case PowerBlocker::Pairing:
      return "PAIRING";
    case PowerBlocker::PortalSession:
      return "PORTAL_SESSION";
    case PowerBlocker::StorageCommit:
      return "STORAGE_COMMIT";
    case PowerBlocker::Count:
      break;
  }
  return "UNKNOWN_POWER_BLOCKER";
}

uint32_t PowerBlockerSet::bit(PowerBlocker blocker) {
  if (!validPowerBlocker(blocker)) return 0;
  const uint8_t shift = static_cast<uint8_t>(blocker) - 1U;
  return static_cast<uint32_t>(1UL << shift);
}

BlockerUpdate PowerBlockerSet::set(PowerBlocker blocker, bool active_value) {
  const uint32_t blocker_bit = bit(blocker);
  if (blocker_bit == 0) return BlockerUpdate::Invalid;
  const bool was_active = (mask_ & blocker_bit) != 0;
  if (was_active == active_value) return BlockerUpdate::Unchanged;
  if (active_value) {
    mask_ |= blocker_bit;
  } else {
    mask_ &= ~blocker_bit;
  }
  return BlockerUpdate::Changed;
}

bool PowerBlockerSet::active(PowerBlocker blocker) const {
  const uint32_t blocker_bit = bit(blocker);
  return blocker_bit != 0 && (mask_ & blocker_bit) != 0;
}

PowerBlocker PowerBlockerSet::firstActive() const {
  for (uint8_t value = static_cast<uint8_t>(PowerBlocker::None) + 1U;
       value < static_cast<uint8_t>(PowerBlocker::Count); ++value) {
    const PowerBlocker blocker = static_cast<PowerBlocker>(value);
    if (active(blocker)) return blocker;
  }
  return PowerBlocker::None;
}

BlockerUpdate PowerActivityState::setBlocker(PowerBlocker blocker,
                                              bool active_value,
                                              uint32_t now_ms) {
  const BlockerUpdate result = blockers_.set(blocker, active_value);
  if (result == BlockerUpdate::Changed) noteMeaningfulActivity(now_ms);
  return result;
}

bool elapsedAtLeast32(uint32_t now_ms, uint32_t since_ms,
                      uint32_t interval_ms) {
  return interval_ms < kHalfUint32Range &&
         static_cast<uint32_t>(now_ms - since_ms) >= interval_ms;
}

SleepPolicy::SleepPolicy(const SleepPolicyConfig& config) {
  configuration_valid_ = setConfig(config);
}

bool SleepPolicy::validConfig(const SleepPolicyConfig& config) {
  return config.eligible_idle_ms >= 120000U &&
         config.eligible_idle_ms < kHalfUint32Range &&
         config.heartbeat_interval_seconds >= 60U &&
         config.heartbeat_interval_seconds <= 86400U &&
         config.rtc_margin_seconds <= 600U &&
         config.rtc_margin_seconds < config.heartbeat_interval_seconds &&
         config.minimum_useful_sleep_seconds > 0U &&
         config.minimum_useful_sleep_seconds <= 3600U;
}

bool SleepPolicy::setConfig(const SleepPolicyConfig& config) {
  if (!validConfig(config)) {
    configuration_valid_ = false;
    return false;
  }
  config_ = config;
  configuration_valid_ = true;
  return true;
}

uint64_t SleepPolicy::adjustedDeadline(uint64_t deadline,
                                       uint64_t now) const {
  if (deadline <= now) return now;
  const uint64_t margin = config_.rtc_margin_seconds;
  if (deadline <= margin || deadline - margin <= now) return now;
  return deadline - margin;
}

SleepDecision SleepPolicy::evaluate(const PowerInputs& inputs) const {
  SleepDecision decision;
  if (!configuration_valid_) {
    decision.reason = SleepDecisionReason::InvalidConfiguration;
    return decision;
  }
  if (!config_.enabled) {
    decision.reason = SleepDecisionReason::Disabled;
    return decision;
  }
  if (!inputs.rtc_synchronized || inputs.rtc_epoch_seconds == 0) {
    decision.reason = SleepDecisionReason::ClockUnavailable;
    return decision;
  }
  if (inputs.blockers.any()) {
    decision.reason = SleepDecisionReason::Blocked;
    decision.blocker = inputs.blockers.firstActive();
    return decision;
  }
  if (!elapsedAtLeast32(inputs.now_ms, inputs.last_meaningful_activity_ms,
                        config_.eligible_idle_ms)) {
    decision.reason = SleepDecisionReason::IdlePeriodNotReached;
    return decision;
  }
  if (!inputs.wake_buttons_released) {
    decision.reason = SleepDecisionReason::WakeButtonsHeld;
    return decision;
  }

  if (inputs.rtc_epoch_seconds >
      std::numeric_limits<uint64_t>::max() -
          config_.heartbeat_interval_seconds) {
    decision.reason = SleepDecisionReason::ClockUnavailable;
    return decision;
  }
  const uint64_t heartbeat_cap =
      inputs.rtc_epoch_seconds + config_.heartbeat_interval_seconds;
  uint64_t heartbeat = inputs.next_heartbeat_epoch_seconds;
  if (heartbeat == 0 || heartbeat > heartbeat_cap) {
    heartbeat = heartbeat_cap;
  } else if (heartbeat <= inputs.rtc_epoch_seconds) {
    decision.reason = SleepDecisionReason::WakeDeadlineDue;
    return decision;
  }

  uint64_t wake_at = adjustedDeadline(heartbeat, inputs.rtc_epoch_seconds);
  if (inputs.next_task_epoch_seconds != 0) {
    if (inputs.next_task_epoch_seconds <= inputs.rtc_epoch_seconds) {
      decision.reason = SleepDecisionReason::WakeDeadlineDue;
      return decision;
    }
    const uint64_t task_wake = adjustedDeadline(
        inputs.next_task_epoch_seconds, inputs.rtc_epoch_seconds);
    if (task_wake < wake_at) wake_at = task_wake;
  }
  if (wake_at <= inputs.rtc_epoch_seconds) {
    decision.reason = SleepDecisionReason::WakeDeadlineDue;
    return decision;
  }
  const uint64_t delay = wake_at - inputs.rtc_epoch_seconds;
  if (delay < config_.minimum_useful_sleep_seconds) {
    decision.reason = SleepDecisionReason::SleepWindowTooShort;
    return decision;
  }
  decision.should_sleep = true;
  decision.reason = SleepDecisionReason::Eligible;
  decision.timer_delay_seconds = delay;
  return decision;
}

const char* sleepDecisionReasonName(SleepDecisionReason reason) {
  switch (reason) {
    case SleepDecisionReason::Eligible:
      return "ELIGIBLE";
    case SleepDecisionReason::Disabled:
      return "DISABLED";
    case SleepDecisionReason::InvalidConfiguration:
      return "INVALID_CONFIGURATION";
    case SleepDecisionReason::ClockUnavailable:
      return "CLOCK_UNAVAILABLE";
    case SleepDecisionReason::Blocked:
      return "BLOCKED";
    case SleepDecisionReason::IdlePeriodNotReached:
      return "IDLE_PERIOD_NOT_REACHED";
    case SleepDecisionReason::WakeButtonsHeld:
      return "WAKE_BUTTONS_HELD";
    case SleepDecisionReason::WakeDeadlineDue:
      return "WAKE_DEADLINE_DUE";
    case SleepDecisionReason::SleepWindowTooShort:
      return "SLEEP_WINDOW_TOO_SHORT";
  }
  return "UNKNOWN_SLEEP_DECISION_REASON";
}

}  // namespace inkloop

