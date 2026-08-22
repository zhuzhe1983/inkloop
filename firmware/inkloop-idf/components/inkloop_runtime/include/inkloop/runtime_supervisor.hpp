#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "inkloop/runtime_admission.hpp"
#include "inkloop/runtime_telemetry.hpp"
#include "inkloop/task_topology.hpp"

namespace inkloop {

using WorkHandler = WorkDisposition (*)(const WorkEnvelope& envelope,
                                        void* context);
using TickHandler = void (*)(void* context);

struct SupervisorDiagnostics {
  std::array<uint32_t, kTaskLaneCount> queue_full{};
  std::array<uint32_t, kTaskLaneCount> not_ready{};
  std::array<uint32_t, kTaskLaneCount> invalid{};
  std::array<uint32_t, kTaskLaneCount> stale{};
  std::array<uint32_t, kTaskLaneCount> expired{};
  std::array<uint32_t, kTaskLaneCount> handler_failed{};
  uint32_t input_isr_overflow = 0;
  uint32_t result_delivery_failed = 0;
  uint32_t startup_failed = 0;
};

// Native queue/task owner. Queue storage is static and producers never wait.
// Task stacks are bounded by kTaskTopology and allocated by FreeRTOS only when
// every handler is registered; partial startup is deleted before readiness.
class RuntimeSupervisor final {
 public:
  RuntimeSupervisor();
  RuntimeSupervisor(const RuntimeSupervisor&) = delete;
  RuntimeSupervisor& operator=(const RuntimeSupervisor&) = delete;

  esp_err_t initialize();
  esp_err_t registerHandler(TaskLane lane, WorkHandler handler,
                            void* context);
  // Optional cooperative progress callback for owners such as I2S, WSS and
  // LED animation. It runs on that lane's pinned task at a bounded interval,
  // even while command traffic is present. Zero disables ticking.
  esp_err_t registerTickHandler(TaskLane lane, TickHandler handler,
                                void* context, uint32_t interval_ms);
  esp_err_t start();
  esp_err_t stop();
  // Idempotent final teardown for composition rollback. Running tasks are
  // stopped first, every static queue is deleted, and handler/tick pointers
  // are cleared so initialize() can safely be attempted again.
  esp_err_t shutdown();

  AdmissionResult post(const WorkEnvelope& envelope);
  AdmissionResult postButtonFromIsr(
      const WorkEnvelope& envelope,
      BaseType_t* higher_priority_task_woken);
  AdmissionResult cancelBefore(WorkClass work_class,
                               uint64_t generation_floor);

  bool initialized() const;
  bool started() const;
  SupervisorDiagnostics diagnostics() const;
  RuntimeTelemetrySnapshot telemetry() const;

 private:
  enum class Lifecycle : uint8_t {
    Uninitialized,
    Initializing,
    Ready,
    Starting,
    Running,
    Stopping,
  };

  static constexpr size_t kMaxQueueDepth = kInputQueueDepth;
  static constexpr size_t kQueueStorageBytes =
      sizeof(WorkEnvelope) * kMaxQueueDepth;

  struct TaskContext {
    RuntimeSupervisor* supervisor = nullptr;
    TaskLane lane = TaskLane::Count;
  };

  struct TaskSlot {
    StaticQueue_t queue_control{};
    alignas(WorkEnvelope)
        std::array<uint8_t, kQueueStorageBytes> queue_storage{};
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    WorkHandler handler = nullptr;
    void* handler_context = nullptr;
    TickHandler tick_handler = nullptr;
    void* tick_context = nullptr;
    TickType_t tick_interval = 0;
    TaskContext task_context{};
  };

  static void taskEntry(void* opaque);
  void run(TaskLane lane);
  void handleDequeued(TaskLane lane, const WorkEnvelope& envelope);
  void recordAdmissionLocked(TaskLane lane, AdmissionResult result);
  void recordHandlerFailure(TaskLane lane);
  void recordResultFailure();
  void recordTaskStarted(TaskLane lane);
  void recordHandlerTiming(TaskLane lane, uint32_t duration_us);
  void recordTickTiming(TaskLane lane, uint32_t duration_us,
                        TickType_t elapsed_ticks, TickType_t interval_ticks);
  void markTaskQuiescent(TaskLane lane);
  void sampleManagedResources(TaskLane lane);
  bool allHandlersRegisteredLocked() const;
  bool calledFromManagedTaskLocked() const;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  RuntimeAdmission admission_{};
  RuntimeTelemetryAccumulator telemetry_{};
  std::array<TaskSlot, kTaskLaneCount> slots_{};
  StaticEventGroup_t stop_events_storage_{};
  EventGroupHandle_t stop_events_ = nullptr;
  SupervisorDiagnostics diagnostics_{};
  Lifecycle lifecycle_ = Lifecycle::Uninitialized;
};

}  // namespace inkloop
