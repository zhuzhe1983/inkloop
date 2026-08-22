#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "inkloop/work_contracts.hpp"

namespace inkloop {

// Fixed-size, credential-free health state. The snapshot deliberately contains
// no names, pointers, strings or dynamically sized storage, so it can be copied
// coherently under the supervisor's existing cross-core lock.
struct RuntimeLaneTelemetry {
  uint32_t queue_capacity = 0;
  uint32_t queue_depth = 0;
  uint32_t queue_high_water = 0;
  uint32_t stack_low_water_bytes = 0;
  uint32_t handler_count = 0;
  uint32_t handler_max_us = 0;
  uint32_t tick_count = 0;
  uint32_t tick_max_us = 0;
  uint32_t tick_late_count = 0;
  uint32_t tick_missed = 0;
  uint32_t tick_late_max_us = 0;
  uint32_t last_progress_ms = 0;
  int8_t configured_core = -1;
  int8_t observed_core = -1;
  uint8_t configured_priority = 0;
  uint8_t observed_priority = 0;
  bool task_running = false;
  bool stack_sampled = false;
};

struct RuntimeTelemetrySnapshot {
  std::array<RuntimeLaneTelemetry, kTaskLaneCount> lanes{};
  uint32_t sequence = 0;
  uint32_t last_managed_update_ms = 0;
  uint32_t internal_heap_min_free_bytes = 0;
  uint32_t psram_min_free_bytes = 0;
  uint32_t resource_sample_count = 0;
  bool internal_heap_sampled = false;
  bool psram_available = false;
};

// Portable accumulator. RuntimeSupervisor supplies serialization; none of the
// methods allocate, block, log, or inspect an envelope payload.
class RuntimeTelemetryAccumulator final {
 public:
  RuntimeTelemetryAccumulator() = default;

  void configureLane(TaskLane lane, size_t queue_capacity,
                     int8_t configured_core, uint8_t configured_priority);
  void recordTaskStarted(TaskLane lane, int8_t observed_core,
                         uint8_t observed_priority, uint32_t now_ms);
  void recordTaskStopped(TaskLane lane);
  void recordQueueDepth(TaskLane lane, size_t depth);
  void recordHandler(TaskLane lane, uint32_t duration_us, uint32_t now_ms);
  void recordTick(TaskLane lane, uint32_t duration_us,
                  uint64_t elapsed_ticks, uint64_t interval_ticks,
                  uint32_t tick_rate_hz, uint32_t now_ms);
  void recordResourceSample(TaskLane lane, uint32_t stack_low_water_bytes,
                            bool heap_sampled,
                            uint32_t internal_heap_free_bytes,
                            bool psram_available,
                            uint32_t psram_free_bytes,
                            int8_t observed_core,
                            uint8_t observed_priority,
                            uint32_t now_ms);

  RuntimeTelemetrySnapshot snapshot() const;

  // Public constexpr helpers make saturation and wrap semantics independently
  // testable without FreeRTOS. elapsed32 is valid for intervals < 2^31 ms.
  static constexpr uint32_t saturatingAdd(uint32_t value, uint64_t increment) {
    return increment >=
                   static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) -
                       value
               ? std::numeric_limits<uint32_t>::max()
               : static_cast<uint32_t>(value + increment);
  }
  static constexpr uint32_t elapsed32(uint32_t now, uint32_t then) {
    return now - then;
  }

 private:
  static uint32_t boundedSize(size_t value);
  static uint32_t boundedU64(uint64_t value);
  static uint32_t ticksToUs(uint64_t ticks, uint32_t tick_rate_hz);
  void changed();

  RuntimeTelemetrySnapshot state_{};
};

}  // namespace inkloop
