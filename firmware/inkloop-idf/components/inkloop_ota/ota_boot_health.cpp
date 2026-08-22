#include "inkloop/ota_boot_health.hpp"

namespace inkloop {
namespace {

enum class TimedFlagStatus : std::uint8_t {
  Healthy,
  Missing,
  Unhealthy,
  Stale,
};

TimedFlagStatus timedFlagStatus(const TimedOtaHealthFlag& flag,
                                std::uint32_t now_ms,
                                std::uint32_t maximum_age_ms) {
  if (!flag.present) return TimedFlagStatus::Missing;
  if (!flag.healthy) return TimedFlagStatus::Unhealthy;
  if (!otaTelemetryFresh32(now_ms, flag.observed_ms, maximum_age_ms))
    return TimedFlagStatus::Stale;
  return TimedFlagStatus::Healthy;
}

}  // namespace

bool otaElapsedAtLeast32(std::uint32_t now_ms, std::uint32_t since_ms,
                         std::uint32_t interval_ms) {
  return interval_ms < kOtaHalfTimeRange &&
      static_cast<std::uint32_t>(now_ms - since_ms) >= interval_ms;
}

bool otaTelemetryFresh32(std::uint32_t now_ms, std::uint32_t observed_ms,
                         std::uint32_t maximum_age_ms) {
  return maximum_age_ms < kOtaHalfTimeRange &&
      static_cast<std::uint32_t>(now_ms - observed_ms) <= maximum_age_ms;
}

OtaBootHealthCore::OtaBootHealthCore(const OtaBootHealthConfig& config)
    : config_(config), configuration_valid_(validConfig(config)) {}

bool OtaBootHealthCore::validConfig(const OtaBootHealthConfig& config) {
  constexpr std::uint32_t kAllLanes =
      (1UL << kMaximumOtaSupervisorLanes) - 1UL;
  return config.soak_window_ms > 0U &&
      config.soak_window_ms < kOtaHalfTimeRange &&
      config.pending_deadline_ms > config.soak_window_ms &&
      config.pending_deadline_ms < kOtaHalfTimeRange &&
      config.telemetry_max_age_ms > 0U &&
      config.telemetry_max_age_ms < kOtaHalfTimeRange &&
      config.mandatory_lane_mask != 0U &&
      (static_cast<std::uint32_t>(config.mandatory_lane_mask) &
       ~kAllLanes) == 0U;
}

void OtaBootHealthCore::beginPending(std::uint32_t now_ms) {
  pending_active_ = true;
  pending_since_ms_ = now_ms;
  has_pending_observation_ = true;
  last_pending_observation_ms_ = now_ms;
  healthy_candidate_active_ = false;
  confirmation_requested_ = false;
  rollback_requested_ = false;
}

void OtaBootHealthCore::leavePending() {
  pending_active_ = false;
  has_pending_observation_ = false;
  healthy_candidate_active_ = false;
  confirmation_requested_ = false;
  rollback_requested_ = false;
}

OtaBootHealthDecision OtaBootHealthCore::decision(
    OtaBootHealthState state, OtaBootHealthAction action,
    OtaBootHealthReason reason, std::uint32_t soak_elapsed) {
  state_ = state;
  OtaBootHealthDecision output;
  output.state = state;
  output.action = action;
  output.reason = reason;
  output.healthy_soak_elapsed_ms = soak_elapsed;
  return output;
}

bool OtaBootHealthCore::mandatoryEvidenceHealthy(
    const OtaBootHealthEvidence& evidence,
    OtaBootHealthReason& reason) const {
  const TimedOtaHealthFlag flags[] = {
      evidence.storage_upgrade_gate,
      evidence.board_initialized,
      evidence.product_runtime_started,
      evidence.fatal_status_clear,
  };
  for (const TimedOtaHealthFlag& flag : flags) {
    switch (timedFlagStatus(flag, evidence.now_ms,
                            config_.telemetry_max_age_ms)) {
      case TimedFlagStatus::Healthy:
        break;
      case TimedFlagStatus::Missing:
        reason = OtaBootHealthReason::AwaitingMandatoryEvidence;
        return false;
      case TimedFlagStatus::Unhealthy:
        reason = OtaBootHealthReason::MandatoryEvidenceUnhealthy;
        return false;
      case TimedFlagStatus::Stale:
        reason = OtaBootHealthReason::MandatoryTelemetryStale;
        return false;
    }
  }

  for (std::size_t at = 0U; at < evidence.supervisor_lanes.size(); ++at) {
    const std::uint16_t bit = static_cast<std::uint16_t>(1U << at);
    if ((config_.mandatory_lane_mask & bit) == 0U) continue;
    const OtaSupervisorLaneTelemetry& lane = evidence.supervisor_lanes[at];
    if (!lane.present) {
      reason = OtaBootHealthReason::MandatoryLaneMissing;
      return false;
    }
    if (lane.progress_count == 0U) {
      reason = OtaBootHealthReason::MandatoryLaneNotProgressed;
      return false;
    }
    if (!otaTelemetryFresh32(evidence.now_ms, lane.observed_ms,
                             config_.telemetry_max_age_ms)) {
      reason = OtaBootHealthReason::MandatoryLaneStale;
      return false;
    }
  }
  return true;
}

OtaBootHealthDecision OtaBootHealthCore::evaluate(
    const OtaBootHealthEvidence& evidence) {
  if (!configuration_valid_) {
    return decision(OtaBootHealthState::Refused,
                    OtaBootHealthAction::None,
                    OtaBootHealthReason::InvalidConfiguration);
  }

  switch (evidence.running_image) {
    case OtaRunningImageState::Ordinary:
      leavePending();
      return decision(OtaBootHealthState::OrdinaryBoot,
                      OtaBootHealthAction::None,
                      OtaBootHealthReason::OrdinaryImage);
    case OtaRunningImageState::Confirmed:
      leavePending();
      return decision(OtaBootHealthState::ConfirmedBoot,
                      OtaBootHealthAction::None,
                      OtaBootHealthReason::AlreadyConfirmed);
    case OtaRunningImageState::Invalid:
    case OtaRunningImageState::Unknown:
      leavePending();
      return decision(OtaBootHealthState::Refused,
                      OtaBootHealthAction::None,
                      OtaBootHealthReason::ImageStateUnavailable);
    case OtaRunningImageState::PendingVerify:
      break;
    default:
      leavePending();
      return decision(OtaBootHealthState::Refused,
                      OtaBootHealthAction::None,
                      OtaBootHealthReason::ImageStateUnavailable);
  }

  if (!pending_active_) beginPending(evidence.now_ms);
  if (has_pending_observation_ && healthy_candidate_active_ &&
      !otaTelemetryFresh32(evidence.now_ms,
                           last_pending_observation_ms_,
                           config_.telemetry_max_age_ms)) {
    // A polling/telemetry gap cannot count toward a continuous healthy soak,
    // even if every source happens to publish a fresh sample on this tick.
    healthy_candidate_active_ = false;
  }
  has_pending_observation_ = true;
  last_pending_observation_ms_ = evidence.now_ms;
  if (rollback_requested_) {
    return decision(OtaBootHealthState::RollbackRequested,
                    OtaBootHealthAction::None,
                    OtaBootHealthReason::ActionAlreadyRequested);
  }
  if (otaElapsedAtLeast32(evidence.now_ms, pending_since_ms_,
                          config_.pending_deadline_ms)) {
    rollback_requested_ = true;
    healthy_candidate_active_ = false;
    return decision(OtaBootHealthState::RollbackRequested,
                    OtaBootHealthAction::Rollback,
                    OtaBootHealthReason::PendingDeadlineExpired);
  }
  if (evidence.explicit_fatal_health_failure) {
    rollback_requested_ = true;
    healthy_candidate_active_ = false;
    return decision(OtaBootHealthState::RollbackRequested,
                    OtaBootHealthAction::Rollback,
                    OtaBootHealthReason::ExplicitFatalHealthFailure);
  }
  if (evidence.fatal_status_clear.present &&
      !evidence.fatal_status_clear.healthy) {
    rollback_requested_ = true;
    healthy_candidate_active_ = false;
    return decision(OtaBootHealthState::RollbackRequested,
                    OtaBootHealthAction::Rollback,
                    OtaBootHealthReason::FatalResetCondition);
  }
  if (confirmation_requested_) {
    return decision(OtaBootHealthState::ConfirmationRequested,
                    OtaBootHealthAction::None,
                    OtaBootHealthReason::ActionAlreadyRequested);
  }

  OtaBootHealthReason evidence_reason =
      OtaBootHealthReason::AwaitingMandatoryEvidence;
  if (!mandatoryEvidenceHealthy(evidence, evidence_reason)) {
    healthy_candidate_active_ = false;
    return decision(OtaBootHealthState::AwaitingEvidence,
                    OtaBootHealthAction::None, evidence_reason);
  }
  if (!healthy_candidate_active_) {
    healthy_candidate_active_ = true;
    healthy_since_ms_ = evidence.now_ms;
  }
  const std::uint32_t soak_elapsed =
      static_cast<std::uint32_t>(evidence.now_ms - healthy_since_ms_);
  if (!otaElapsedAtLeast32(evidence.now_ms, healthy_since_ms_,
                           config_.soak_window_ms)) {
    return decision(OtaBootHealthState::Soaking,
                    OtaBootHealthAction::None,
                    OtaBootHealthReason::SoakInProgress, soak_elapsed);
  }
  confirmation_requested_ = true;
  return decision(OtaBootHealthState::ConfirmationRequested,
                  OtaBootHealthAction::Confirm,
                  OtaBootHealthReason::HealthySoakComplete, soak_elapsed);
}

const char* otaRunningImageStateName(OtaRunningImageState state) {
  switch (state) {
    case OtaRunningImageState::Ordinary: return "ORDINARY";
    case OtaRunningImageState::PendingVerify: return "PENDING_VERIFY";
    case OtaRunningImageState::Confirmed: return "CONFIRMED";
    case OtaRunningImageState::Invalid: return "INVALID";
    case OtaRunningImageState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* otaBootHealthStateName(OtaBootHealthState state) {
  switch (state) {
    case OtaBootHealthState::Uninitialized: return "UNINITIALIZED";
    case OtaBootHealthState::OrdinaryBoot: return "ORDINARY_BOOT";
    case OtaBootHealthState::AwaitingEvidence: return "AWAITING_EVIDENCE";
    case OtaBootHealthState::Soaking: return "SOAKING";
    case OtaBootHealthState::ConfirmationRequested:
      return "CONFIRMATION_REQUESTED";
    case OtaBootHealthState::ConfirmedBoot: return "CONFIRMED_BOOT";
    case OtaBootHealthState::RollbackRequested:
      return "ROLLBACK_REQUESTED";
    case OtaBootHealthState::Refused: return "REFUSED";
  }
  return "UNKNOWN";
}

const char* otaBootHealthActionName(OtaBootHealthAction action) {
  switch (action) {
    case OtaBootHealthAction::None: return "NONE";
    case OtaBootHealthAction::Confirm: return "CONFIRM";
    case OtaBootHealthAction::Rollback: return "ROLLBACK";
  }
  return "UNKNOWN";
}

const char* otaBootHealthReasonName(OtaBootHealthReason reason) {
  switch (reason) {
    case OtaBootHealthReason::OrdinaryImage: return "ORDINARY_IMAGE";
    case OtaBootHealthReason::AlreadyConfirmed: return "ALREADY_CONFIRMED";
    case OtaBootHealthReason::InvalidConfiguration:
      return "INVALID_CONFIGURATION";
    case OtaBootHealthReason::ImageStateUnavailable:
      return "IMAGE_STATE_UNAVAILABLE";
    case OtaBootHealthReason::AwaitingMandatoryEvidence:
      return "AWAITING_MANDATORY_EVIDENCE";
    case OtaBootHealthReason::MandatoryEvidenceUnhealthy:
      return "MANDATORY_EVIDENCE_UNHEALTHY";
    case OtaBootHealthReason::MandatoryTelemetryStale:
      return "MANDATORY_TELEMETRY_STALE";
    case OtaBootHealthReason::MandatoryLaneMissing:
      return "MANDATORY_LANE_MISSING";
    case OtaBootHealthReason::MandatoryLaneStale:
      return "MANDATORY_LANE_STALE";
    case OtaBootHealthReason::MandatoryLaneNotProgressed:
      return "MANDATORY_LANE_NOT_PROGRESSED";
    case OtaBootHealthReason::SoakInProgress: return "SOAK_IN_PROGRESS";
    case OtaBootHealthReason::HealthySoakComplete:
      return "HEALTHY_SOAK_COMPLETE";
    case OtaBootHealthReason::PendingDeadlineExpired:
      return "PENDING_DEADLINE_EXPIRED";
    case OtaBootHealthReason::ExplicitFatalHealthFailure:
      return "EXPLICIT_FATAL_HEALTH_FAILURE";
    case OtaBootHealthReason::FatalResetCondition:
      return "FATAL_RESET_CONDITION";
    case OtaBootHealthReason::ActionAlreadyRequested:
      return "ACTION_ALREADY_REQUESTED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
