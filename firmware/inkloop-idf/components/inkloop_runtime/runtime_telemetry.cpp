#include "inkloop/runtime_telemetry.hpp"

#include <limits>

namespace inkloop {
namespace {

RuntimeLaneTelemetry* laneAt(RuntimeTelemetrySnapshot& state, TaskLane lane) {
  const size_t index = taskLaneIndex(lane);
  return index < state.lanes.size() ? &state.lanes[index] : nullptr;
}

}  // namespace

uint32_t RuntimeTelemetryAccumulator::boundedSize(size_t value) {
  constexpr size_t limit = std::numeric_limits<uint32_t>::max();
  return value > limit ? std::numeric_limits<uint32_t>::max()
                       : static_cast<uint32_t>(value);
}

uint32_t RuntimeTelemetryAccumulator::boundedU64(uint64_t value) {
  return value > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(value);
}

uint32_t RuntimeTelemetryAccumulator::ticksToUs(uint64_t ticks,
                                                uint32_t tick_rate_hz) {
  if (tick_rate_hz == 0 || ticks == 0) return 0;
  constexpr uint64_t scale = 1000000ULL;
  if (ticks > std::numeric_limits<uint64_t>::max() / scale) {
    return std::numeric_limits<uint32_t>::max();
  }
  return boundedU64((ticks * scale) / tick_rate_hz);
}

void RuntimeTelemetryAccumulator::changed() {
  state_.sequence = saturatingAdd(state_.sequence, 1);
}

void RuntimeTelemetryAccumulator::configureLane(
    TaskLane lane, size_t queue_capacity, int8_t configured_core,
    uint8_t configured_priority) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  value->queue_capacity = boundedSize(queue_capacity);
  value->configured_core = configured_core;
  value->configured_priority = configured_priority;
  changed();
}

void RuntimeTelemetryAccumulator::recordTaskStarted(
    TaskLane lane, int8_t observed_core, uint8_t observed_priority,
    uint32_t now_ms) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  value->task_running = true;
  value->observed_core = observed_core;
  value->observed_priority = observed_priority;
  value->last_progress_ms = now_ms;
  state_.last_managed_update_ms = now_ms;
  changed();
}

void RuntimeTelemetryAccumulator::recordTaskStopped(TaskLane lane) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  value->task_running = false;
  value->queue_depth = 0;
  changed();
}

void RuntimeTelemetryAccumulator::recordQueueDepth(TaskLane lane,
                                                   size_t depth) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  value->queue_depth = boundedSize(depth);
  if (value->queue_depth > value->queue_high_water) {
    value->queue_high_water = value->queue_depth;
  }
  changed();
}

void RuntimeTelemetryAccumulator::recordHandler(TaskLane lane,
                                                uint32_t duration_us,
                                                uint32_t now_ms) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  value->handler_count = saturatingAdd(value->handler_count, 1);
  if (duration_us > value->handler_max_us) value->handler_max_us = duration_us;
  value->last_progress_ms = now_ms;
  state_.last_managed_update_ms = now_ms;
  changed();
}

void RuntimeTelemetryAccumulator::recordTick(
    TaskLane lane, uint32_t duration_us, uint64_t elapsed_ticks,
    uint64_t interval_ticks, uint32_t tick_rate_hz, uint32_t now_ms) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value || interval_ticks == 0) return;
  value->tick_count = saturatingAdd(value->tick_count, 1);
  if (duration_us > value->tick_max_us) value->tick_max_us = duration_us;
  if (elapsed_ticks > interval_ticks) {
    value->tick_late_count = saturatingAdd(value->tick_late_count, 1);
    const uint64_t missed = (elapsed_ticks / interval_ticks) - 1;
    value->tick_missed = saturatingAdd(value->tick_missed, missed);
    const uint32_t late_us =
        ticksToUs(elapsed_ticks - interval_ticks, tick_rate_hz);
    if (late_us > value->tick_late_max_us) {
      value->tick_late_max_us = late_us;
    }
  }
  value->last_progress_ms = now_ms;
  state_.last_managed_update_ms = now_ms;
  changed();
}

void RuntimeTelemetryAccumulator::recordResourceSample(
    TaskLane lane, uint32_t stack_low_water_bytes,
    bool heap_sampled, uint32_t internal_heap_free_bytes,
    bool psram_available,
    uint32_t psram_free_bytes, int8_t observed_core,
    uint8_t observed_priority, uint32_t now_ms) {
  RuntimeLaneTelemetry* value = laneAt(state_, lane);
  if (!value) return;
  if (!value->stack_sampled ||
      stack_low_water_bytes < value->stack_low_water_bytes) {
    value->stack_low_water_bytes = stack_low_water_bytes;
  }
  value->stack_sampled = true;
  value->observed_core = observed_core;
  value->observed_priority = observed_priority;
  value->last_progress_ms = now_ms;

  if (heap_sampled) {
    if (!state_.internal_heap_sampled ||
        internal_heap_free_bytes < state_.internal_heap_min_free_bytes) {
      state_.internal_heap_min_free_bytes = internal_heap_free_bytes;
    }
    state_.internal_heap_sampled = true;
    if (psram_available) {
      if (!state_.psram_available ||
          psram_free_bytes < state_.psram_min_free_bytes) {
        state_.psram_min_free_bytes = psram_free_bytes;
      }
      state_.psram_available = true;
    }
  }
  state_.resource_sample_count =
      saturatingAdd(state_.resource_sample_count, 1);
  state_.last_managed_update_ms = now_ms;
  changed();
}

RuntimeTelemetrySnapshot RuntimeTelemetryAccumulator::snapshot() const {
  return state_;
}

}  // namespace inkloop
