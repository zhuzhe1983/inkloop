#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

inline constexpr std::size_t kMaximumOtaSupervisorLanes = 16U;
inline constexpr std::uint32_t kOtaHalfTimeRange = 0x80000000UL;

enum class OtaRunningImageState : std::uint8_t {
  Ordinary,
  PendingVerify,
  Confirmed,
  Invalid,
  Unknown,
};

struct TimedOtaHealthFlag {
  bool present = false;
  bool healthy = false;
  std::uint32_t observed_ms = 0U;
};

struct OtaSupervisorLaneTelemetry {
  bool present = false;
  std::uint32_t progress_count = 0U;
  std::uint32_t observed_ms = 0U;
};

struct OtaBootHealthEvidence {
  std::uint32_t now_ms = 0U;
  OtaRunningImageState running_image = OtaRunningImageState::Unknown;
  TimedOtaHealthFlag storage_upgrade_gate{};
  TimedOtaHealthFlag board_initialized{};
  TimedOtaHealthFlag product_runtime_started{};
  // present+healthy means no watchdog/reset fatal condition was observed.
  TimedOtaHealthFlag fatal_status_clear{};
  std::array<OtaSupervisorLaneTelemetry,
             kMaximumOtaSupervisorLanes> supervisor_lanes{};
  // An explicit fatal event is never softened by missing/stale telemetry.
  bool explicit_fatal_health_failure = false;
};

struct OtaBootHealthConfig {
  std::uint32_t soak_window_ms = 30000U;
  std::uint32_t pending_deadline_ms = 120000U;
  std::uint32_t telemetry_max_age_ms = 5000U;
  std::uint16_t mandatory_lane_mask = 0U;
};

enum class OtaBootHealthState : std::uint8_t {
  Uninitialized,
  OrdinaryBoot,
  AwaitingEvidence,
  Soaking,
  ConfirmationRequested,
  ConfirmedBoot,
  RollbackRequested,
  Refused,
};

enum class OtaBootHealthAction : std::uint8_t {
  None,
  Confirm,
  Rollback,
};

enum class OtaBootHealthReason : std::uint8_t {
  OrdinaryImage,
  AlreadyConfirmed,
  InvalidConfiguration,
  ImageStateUnavailable,
  AwaitingMandatoryEvidence,
  MandatoryEvidenceUnhealthy,
  MandatoryTelemetryStale,
  MandatoryLaneMissing,
  MandatoryLaneStale,
  MandatoryLaneNotProgressed,
  SoakInProgress,
  HealthySoakComplete,
  PendingDeadlineExpired,
  ExplicitFatalHealthFailure,
  FatalResetCondition,
  ActionAlreadyRequested,
};

struct OtaBootHealthDecision {
  OtaBootHealthState state = OtaBootHealthState::Uninitialized;
  OtaBootHealthAction action = OtaBootHealthAction::None;
  OtaBootHealthReason reason =
      OtaBootHealthReason::ImageStateUnavailable;
  std::uint32_t healthy_soak_elapsed_ms = 0U;
};

bool otaElapsedAtLeast32(std::uint32_t now_ms, std::uint32_t since_ms,
                         std::uint32_t interval_ms);
bool otaTelemetryFresh32(std::uint32_t now_ms, std::uint32_t observed_ms,
                         std::uint32_t maximum_age_ms);

class OtaBootHealthCore final {
 public:
  explicit OtaBootHealthCore(const OtaBootHealthConfig& config);

  OtaBootHealthDecision evaluate(const OtaBootHealthEvidence& evidence);

  bool configurationValid() const { return configuration_valid_; }
  OtaBootHealthState state() const { return state_; }

 private:
  static bool validConfig(const OtaBootHealthConfig& config);
  bool mandatoryEvidenceHealthy(const OtaBootHealthEvidence& evidence,
                                OtaBootHealthReason& reason) const;
  OtaBootHealthDecision decision(OtaBootHealthState state,
                                 OtaBootHealthAction action,
                                 OtaBootHealthReason reason,
                                 std::uint32_t soak_elapsed = 0U);
  void beginPending(std::uint32_t now_ms);
  void leavePending();

  OtaBootHealthConfig config_{};
  bool configuration_valid_ = false;
  OtaBootHealthState state_ = OtaBootHealthState::Uninitialized;
  bool pending_active_ = false;
  std::uint32_t pending_since_ms_ = 0U;
  bool has_pending_observation_ = false;
  std::uint32_t last_pending_observation_ms_ = 0U;
  bool healthy_candidate_active_ = false;
  std::uint32_t healthy_since_ms_ = 0U;
  bool confirmation_requested_ = false;
  bool rollback_requested_ = false;
};

const char* otaRunningImageStateName(OtaRunningImageState state);
const char* otaBootHealthStateName(OtaBootHealthState state);
const char* otaBootHealthActionName(OtaBootHealthAction action);
const char* otaBootHealthReasonName(OtaBootHealthReason reason);

}  // namespace inkloop
