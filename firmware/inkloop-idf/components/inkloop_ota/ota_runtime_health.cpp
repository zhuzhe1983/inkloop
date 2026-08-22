#include "inkloop/ota_runtime_health.hpp"

#include <limits>

#include "inkloop/work_contracts.hpp"

namespace inkloop {
namespace {

std::uint32_t progressed(const RuntimeLaneTelemetry& lane) {
  const std::uint64_t value = 1ULL + lane.handler_count + lane.tick_count;
  return value > std::numeric_limits<std::uint32_t>::max()
      ? std::numeric_limits<std::uint32_t>::max()
      : static_cast<std::uint32_t>(value);
}

}  // namespace

OtaBootHealthConfig productionOtaBootHealthConfig() {
  static_assert(kTaskLaneCount <= kMaximumOtaSupervisorLanes,
                "OTA evidence must represent every supervisor lane");
  static_assert(kTaskLaneCount < 16U,
                "mandatory lane mask is a 16-bit value");
  OtaBootHealthConfig config;
  config.soak_window_ms = 30000U;
  config.pending_deadline_ms = 120000U;
  config.telemetry_max_age_ms = 15000U;
  config.mandatory_lane_mask = static_cast<std::uint16_t>(
      (1UL << kTaskLaneCount) - 1UL);
  return config;
}

OtaBootHealthEvidence composeOtaBootHealthEvidence(
    std::uint32_t now_ms, const OtaBootStageState& stage,
    const RuntimeTelemetrySnapshot& runtime) {
  OtaBootHealthEvidence evidence;
  evidence.now_ms = now_ms;
  evidence.storage_upgrade_gate = {
      stage.storage_gate_observed, stage.storage_gate_healthy, now_ms};
  evidence.board_initialized = {
      stage.board_observed, stage.board_healthy, now_ms};
  evidence.product_runtime_started = {
      stage.runtime_observed, stage.runtime_healthy, now_ms};
  evidence.fatal_status_clear = {
      stage.fatal_status_observed, stage.fatal_status_clear, now_ms};
  evidence.explicit_fatal_health_failure =
      stage.explicit_fatal_health_failure;

  static_assert(kTaskLaneCount <=
                    std::tuple_size<decltype(runtime.lanes)>::value,
                "runtime telemetry lost a supervisor lane");
  for (std::size_t at = 0U; at < kTaskLaneCount; ++at) {
    const RuntimeLaneTelemetry& lane = runtime.lanes[at];
    const bool placed = lane.configured_core == lane.observed_core &&
        lane.configured_priority == lane.observed_priority;
    OtaSupervisorLaneTelemetry& output = evidence.supervisor_lanes[at];
    output.present = lane.task_running && lane.stack_sampled && placed;
    output.progress_count = output.present ? progressed(lane) : 0U;
    output.observed_ms = lane.last_progress_ms;
  }
  return evidence;
}

}  // namespace inkloop
