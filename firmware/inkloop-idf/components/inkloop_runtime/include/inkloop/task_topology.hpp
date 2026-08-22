#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "inkloop/work_contracts.hpp"

namespace inkloop {

struct TaskSpec {
  const char* name;
  int8_t core;
  uint8_t priority;
  uint32_t stack_bytes;
  size_t queue_depth;
  TaskLane lane;
};

// Core 1 is the responsive lane. Core 0 is the slow/event-driven lane and also
// hosts the ESP-IDF Wi-Fi stack. LED work is intentionally below control/voice.
inline constexpr std::array<TaskSpec, 8> kTaskTopology{{
    {"ink-input", 1, 22, 3072, 32, TaskLane::Input},
    {"ink-voice", 1, 20, 12288, 32, TaskLane::Voice},
    {"ink-control", 1, 18, 8192, 32, TaskLane::Control},
    {"ink-led", 1, 8, 3072, 16, TaskLane::Led},
    // Voice transport must preempt image conversion/storage on the slow core;
    // otherwise a long PNG conversion can starve WSS audio ingress.
    {"ink-storage", 0, 7, 8192, 8, TaskLane::Storage},
    {"ink-display", 0, 6, 12288, 4, TaskLane::Display},
    {"ink-network", 0, 9, 12288, 8, TaskLane::Network},
    {"ink-portal", 0, 3, 8192, 4, TaskLane::Portal},
}};

esp_err_t validate_task_topology();

}  // namespace inkloop
