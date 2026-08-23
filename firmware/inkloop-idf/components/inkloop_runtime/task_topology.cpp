#include "inkloop/task_topology.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace inkloop {
namespace {
constexpr char kTag[] = "ink-runtime";
}

esp_err_t validate_task_topology() {
  static_assert(portNUM_PROCESSORS == 2,
                "Inkloop requires an ESP32 dual-core target");
  static_assert(kTaskTopology[0].priority > kTaskTopology[1].priority,
                "input must preempt voice");
  static_assert(kTaskTopology[0].priority > kTaskTopology[2].priority,
                "input must preempt button control");
  static_assert(kTaskTopology[2].priority > kTaskTopology[1].priority,
                "button control must preempt voice");
  static_assert(kTaskTopology[2].priority > kTaskTopology[3].priority,
                "LED status must never preempt control");
  static_assert(kTaskTopology[6].priority > kTaskTopology[4].priority &&
                    kTaskTopology[6].priority > kTaskTopology[5].priority,
                "voice transport must preempt storage/display on core 0");
  static_assert(kTaskTopology[6].priority > kTaskTopology[7].priority,
                "Portal must remain the lowest service priority");

  for (size_t index = 0; index < kTaskTopology.size(); ++index) {
    const TaskSpec& task = kTaskTopology[index];
    if (!task.name || task.core < 0 || task.core >= portNUM_PROCESSORS ||
        task.priority >= configMAX_PRIORITIES || task.stack_bytes < 2048 ||
        task.queue_depth == 0 || taskLaneIndex(task.lane) != index ||
        task.queue_depth != kTaskQueueDepths[index]) {
      return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(kTag, "%s core=%d priority=%u stack=%lu queue=%u", task.name,
             task.core, task.priority,
             static_cast<unsigned long>(task.stack_bytes),
             static_cast<unsigned>(task.queue_depth));
  }
  return ESP_OK;
}

}  // namespace inkloop
