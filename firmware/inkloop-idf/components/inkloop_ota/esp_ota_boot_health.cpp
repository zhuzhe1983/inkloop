#include "inkloop/esp_ota_boot_health.hpp"

namespace inkloop {

bool EspOtaBootHealthAdapter::functionsValid() const {
  return functions_.read_running_image_state &&
      functions_.mark_app_valid_cancel_rollback &&
      functions_.mark_app_invalid_rollback_and_reboot;
}

bool EspOtaBootHealthAdapter::rereadPending(
    OtaRunningImageState& output) const {
  output = OtaRunningImageState::Unknown;
  return functions_.read_running_image_state(output) ==
             EspOtaStateReadCode::Ok &&
      output == OtaRunningImageState::PendingVerify;
}

EspOtaBootHealthObservation EspOtaBootHealthAdapter::tick(
    OtaBootHealthEvidence evidence) {
  EspOtaBootHealthObservation observation;
  if (!functionsValid()) {
    observation.code = EspOtaBootHealthCode::InvalidFunctions;
    return observation;
  }
  OtaRunningImageState running = OtaRunningImageState::Unknown;
  if (functions_.read_running_image_state(running) !=
      EspOtaStateReadCode::Ok) {
    observation.code = EspOtaBootHealthCode::StateReadFailed;
    return observation;
  }
  evidence.running_image = running;
  observation.observed_image = running;
  observation.decision = core_.evaluate(evidence);
  if (observation.decision.action == OtaBootHealthAction::None)
    return observation;

  OtaRunningImageState action_image = OtaRunningImageState::Unknown;
  if (!rereadPending(action_image)) {
    observation.code = EspOtaBootHealthCode::PendingStateChanged;
    observation.action_image = action_image;
    return observation;
  }
  observation.action_image = action_image;

  if (observation.decision.action == OtaBootHealthAction::Confirm) {
    if (confirmation_attempted_) return observation;
    confirmation_attempted_ = true;
    observation.action_attempted = true;
    observation.system_status =
        functions_.mark_app_valid_cancel_rollback();
    observation.code = observation.system_status == 0
        ? EspOtaBootHealthCode::ConfirmationSucceeded
        : EspOtaBootHealthCode::ConfirmationFailed;
    return observation;
  }
  if (observation.decision.action == OtaBootHealthAction::Rollback) {
    if (rollback_attempted_) return observation;
    rollback_attempted_ = true;
    observation.action_attempted = true;
    observation.system_status =
        functions_.mark_app_invalid_rollback_and_reboot();
    observation.code = observation.system_status == 0
        ? EspOtaBootHealthCode::RollbackInvoked
        : EspOtaBootHealthCode::RollbackFailed;
  }
  return observation;
}

const char* espOtaStateReadCodeName(EspOtaStateReadCode code) {
  switch (code) {
    case EspOtaStateReadCode::Ok: return "OK";
    case EspOtaStateReadCode::RunningPartitionUnavailable:
      return "RUNNING_PARTITION_UNAVAILABLE";
    case EspOtaStateReadCode::StateUnavailable:
      return "STATE_UNAVAILABLE";
  }
  return "UNKNOWN";
}

const char* espOtaBootHealthCodeName(EspOtaBootHealthCode code) {
  switch (code) {
    case EspOtaBootHealthCode::NoAction: return "NO_ACTION";
    case EspOtaBootHealthCode::ConfirmationSucceeded:
      return "CONFIRMATION_SUCCEEDED";
    case EspOtaBootHealthCode::RollbackInvoked: return "ROLLBACK_INVOKED";
    case EspOtaBootHealthCode::InvalidFunctions: return "INVALID_FUNCTIONS";
    case EspOtaBootHealthCode::StateReadFailed: return "STATE_READ_FAILED";
    case EspOtaBootHealthCode::PendingStateChanged:
      return "PENDING_STATE_CHANGED";
    case EspOtaBootHealthCode::ConfirmationFailed:
      return "CONFIRMATION_FAILED";
    case EspOtaBootHealthCode::RollbackFailed: return "ROLLBACK_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
