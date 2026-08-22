#pragma once

#include <cstdint>

#include "inkloop/ota_boot_health.hpp"

namespace inkloop {

enum class EspOtaStateReadCode : std::uint8_t {
  Ok,
  RunningPartitionUnavailable,
  StateUnavailable,
};

struct EspOtaSystemFunctions {
  EspOtaStateReadCode (*read_running_image_state)(
      OtaRunningImageState& output) = nullptr;
  int (*mark_app_valid_cancel_rollback)() = nullptr;
  int (*mark_app_invalid_rollback_and_reboot)() = nullptr;
};

const EspOtaSystemFunctions& systemEspOtaFunctions();

enum class EspOtaBootHealthCode : std::uint8_t {
  NoAction,
  ConfirmationSucceeded,
  RollbackInvoked,
  InvalidFunctions,
  StateReadFailed,
  PendingStateChanged,
  ConfirmationFailed,
  RollbackFailed,
};

struct EspOtaBootHealthObservation {
  EspOtaBootHealthCode code = EspOtaBootHealthCode::NoAction;
  OtaRunningImageState observed_image = OtaRunningImageState::Unknown;
  OtaRunningImageState action_image = OtaRunningImageState::Unknown;
  OtaBootHealthDecision decision{};
  bool action_attempted = false;
  int system_status = 0;
};

class EspOtaBootHealthAdapter final {
 public:
  EspOtaBootHealthAdapter(OtaBootHealthCore& core,
                          const EspOtaSystemFunctions& functions)
      : core_(core), functions_(functions) {}

  // The adapter owns image-state observation. Callers provide health evidence
  // only; any running_image value in the input is overwritten.
  EspOtaBootHealthObservation tick(OtaBootHealthEvidence evidence);

  bool confirmationAttempted() const { return confirmation_attempted_; }
  bool rollbackAttempted() const { return rollback_attempted_; }

 private:
  bool functionsValid() const;
  bool rereadPending(OtaRunningImageState& output) const;

  OtaBootHealthCore& core_;
  const EspOtaSystemFunctions& functions_;
  bool confirmation_attempted_ = false;
  bool rollback_attempted_ = false;
};

const char* espOtaStateReadCodeName(EspOtaStateReadCode code);
const char* espOtaBootHealthCodeName(EspOtaBootHealthCode code);

}  // namespace inkloop
