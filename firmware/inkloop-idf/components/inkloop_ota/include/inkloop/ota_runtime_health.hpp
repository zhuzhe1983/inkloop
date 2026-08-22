#pragma once

#include <cstdint>

#include "inkloop/ota_boot_health.hpp"
#include "inkloop/runtime_telemetry.hpp"

namespace inkloop {

struct OtaBootStageState {
  bool storage_gate_observed = false;
  bool storage_gate_healthy = false;
  bool board_observed = false;
  bool board_healthy = false;
  bool runtime_observed = false;
  bool runtime_healthy = false;
  bool fatal_status_observed = false;
  bool fatal_status_clear = false;
  bool explicit_fatal_health_failure = false;
};

// Product policy for a staged image. All eight supervisor lanes are mandatory;
// Wi-Fi, MyAI and cloud availability are intentionally absent.
OtaBootHealthConfig productionOtaBootHealthConfig();

// Converts fixed-size runtime telemetry into the board-neutral OTA evidence
// contract. A lane is present only after its task has run, sampled its stack,
// and proved the configured core/priority placement.
OtaBootHealthEvidence composeOtaBootHealthEvidence(
    std::uint32_t now_ms, const OtaBootStageState& stage,
    const RuntimeTelemetrySnapshot& runtime);

}  // namespace inkloop
