#include "inkloop/runtime_supervisor.hpp"

#include <limits>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-supervisor";
constexpr uint32_t kSupervisorStopTimeoutMs = 65000U;
constexpr EventBits_t kAllTaskStoppedBits =
    (static_cast<EventBits_t>(1U) << kTaskLaneCount) - 1U;

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

uint32_t elapsedUs(int64_t started_us) {
  const int64_t elapsed = esp_timer_get_time() - started_us;
  if (elapsed <= 0) return 0;
  return elapsed > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(elapsed);
}

uint32_t boundedSize(size_t value) {
  return value > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(value);
}

uint8_t boundedPriority(UBaseType_t value) {
  return value > std::numeric_limits<uint8_t>::max()
             ? std::numeric_limits<uint8_t>::max()
             : static_cast<uint8_t>(value);
}

WorkDisposition rejectionDisposition(AdmissionResult result) {
  switch (result) {
    case AdmissionResult::StaleGeneration:
      return WorkDisposition::Cancelled;
    case AdmissionResult::Expired:
      return WorkDisposition::TimedOut;
    case AdmissionResult::QueueFull:
      return WorkDisposition::Busy;
    case AdmissionResult::Admitted:
      return WorkDisposition::Complete;
    case AdmissionResult::NotReady:
      return WorkDisposition::Busy;
    case AdmissionResult::InvalidEnvelope:
    case AdmissionResult::WrongLane:
    case AdmissionResult::Underflow:
      return WorkDisposition::Failed;
  }
  return WorkDisposition::Failed;
}

WorkEnvelope makeResult(const WorkEnvelope& command,
                        WorkDisposition disposition) {
  WorkEnvelope result = command;
  result.kind = EnvelopeKind::Result;
  result.disposition = disposition;
  result.deadline_ms = 0;
  result.payload_bytes = 0;
  return result;
}

}  // namespace

RuntimeSupervisor::RuntimeSupervisor() {
  for (size_t index = 0; index < slots_.size(); ++index) {
    slots_[index].task_context.supervisor = this;
    slots_[index].task_context.lane = static_cast<TaskLane>(index);
    const TaskSpec& spec = kTaskTopology[index];
    telemetry_.configureLane(spec.lane, spec.queue_depth, spec.core,
                             spec.priority);
  }
}

esp_err_t RuntimeSupervisor::initialize() {
  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Uninitialized) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  lifecycle_ = Lifecycle::Initializing;
  portEXIT_CRITICAL(&mux_);

  stop_events_ = xEventGroupCreateStatic(&stop_events_storage_);
  if (!stop_events_) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.startup_failed;
    lifecycle_ = Lifecycle::Uninitialized;
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_NO_MEM;
  }

  for (size_t index = 0; index < slots_.size(); ++index) {
    TaskSlot& slot = slots_[index];
    slot.queue = xQueueCreateStatic(
        static_cast<UBaseType_t>(kTaskQueueDepths[index]),
        static_cast<UBaseType_t>(sizeof(WorkEnvelope)),
        slot.queue_storage.data(), &slot.queue_control);
    if (!slot.queue) {
      for (size_t cleanup = 0; cleanup < index; ++cleanup) {
        vQueueDelete(slots_[cleanup].queue);
        slots_[cleanup].queue = nullptr;
      }
      vEventGroupDelete(stop_events_);
      stop_events_ = nullptr;
      portENTER_CRITICAL(&mux_);
      ++diagnostics_.startup_failed;
      lifecycle_ = Lifecycle::Uninitialized;
      portEXIT_CRITICAL(&mux_);
      return ESP_ERR_NO_MEM;
    }
  }

  portENTER_CRITICAL(&mux_);
  lifecycle_ = Lifecycle::Ready;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

esp_err_t RuntimeSupervisor::registerHandler(TaskLane lane,
                                             WorkHandler handler,
                                             void* context) {
  const size_t index = taskLaneIndex(lane);
  if (index >= slots_.size() || !handler) return ESP_ERR_INVALID_ARG;

  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Ready) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  slots_[index].handler = handler;
  slots_[index].handler_context = context;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

esp_err_t RuntimeSupervisor::registerTickHandler(
    TaskLane lane, TickHandler handler, void* context, uint32_t interval_ms) {
  const size_t index = taskLaneIndex(lane);
  if (index >= slots_.size() || !handler || interval_ms == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  const TickType_t interval = pdMS_TO_TICKS(interval_ms);
  if (interval == 0) return ESP_ERR_INVALID_ARG;

  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Ready) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  slots_[index].tick_handler = handler;
  slots_[index].tick_context = context;
  slots_[index].tick_interval = interval;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

bool RuntimeSupervisor::allHandlersRegisteredLocked() const {
  for (const TaskSlot& slot : slots_) {
    if (!slot.queue || !slot.handler) return false;
  }
  return true;
}

esp_err_t RuntimeSupervisor::start() {
  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Ready || !allHandlersRegisteredLocked()) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  lifecycle_ = Lifecycle::Starting;
  portEXIT_CRITICAL(&mux_);
  xEventGroupClearBits(stop_events_, kAllTaskStoppedBits);

  size_t created = 0;
  for (; created < slots_.size(); ++created) {
    const TaskSpec& spec = kTaskTopology[created];
    TaskSlot& slot = slots_[created];
    const BaseType_t result = xTaskCreatePinnedToCore(
        &RuntimeSupervisor::taskEntry, spec.name, spec.stack_bytes,
        &slot.task_context, spec.priority, &slot.task, spec.core);
    if (result != pdPASS || !slot.task) {
      if (slot.task) {
        vTaskDelete(slot.task);
        slot.task = nullptr;
      }
      break;
    }
  }

  if (created != slots_.size()) {
    for (size_t index = 0; index < created; ++index) {
      vTaskDelete(slots_[index].task);
      slots_[index].task = nullptr;
    }
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.startup_failed;
    lifecycle_ = Lifecycle::Ready;
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_NO_MEM;
  }

  portENTER_CRITICAL(&mux_);
  lifecycle_ = Lifecycle::Running;
  for (TaskSlot& slot : slots_) xTaskNotifyGive(slot.task);
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

bool RuntimeSupervisor::calledFromManagedTaskLocked() const {
  const TaskHandle_t current = xTaskGetCurrentTaskHandle();
  for (const TaskSlot& slot : slots_) {
    if (slot.task == current) return true;
  }
  return false;
}

esp_err_t RuntimeSupervisor::stop() {
  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Running || calledFromManagedTaskLocked()) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  lifecycle_ = Lifecycle::Stopping;
  portEXIT_CRITICAL(&mux_);

  for (TaskSlot& slot : slots_) {
    if (slot.task) (void)xTaskAbortDelay(slot.task);
  }
  const EventBits_t stopped = xEventGroupWaitBits(
      stop_events_, kAllTaskStoppedBits, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(kSupervisorStopTimeoutMs));
  const bool all_stopped =
      (stopped & kAllTaskStoppedBits) == kAllTaskStoppedBits;
  for (TaskSlot& slot : slots_) {
    if (slot.task) {
      vTaskDelete(slot.task);
    }
    xQueueReset(slot.queue);
  }

  portENTER_CRITICAL(&mux_);
  for (size_t index = 0; index < slots_.size(); ++index) {
    slots_[index].task = nullptr;
    if (!all_stopped)
      telemetry_.recordTaskStopped(static_cast<TaskLane>(index));
  }
  const AdmissionSnapshot previous = admission_.snapshot();
  admission_ = RuntimeAdmission();
  for (size_t index = 0; index < previous.generation_floor.size(); ++index) {
    const uint64_t floor = previous.generation_floor[index];
    if (floor != 0) {
      admission_.cancelBefore(static_cast<WorkClass>(index), floor);
    }
  }
  lifecycle_ = Lifecycle::Ready;
  portEXIT_CRITICAL(&mux_);
  return all_stopped ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t RuntimeSupervisor::shutdown() {
  if (started()) {
    const esp_err_t stopped = stop();
    if (stopped != ESP_OK) return stopped;
  }

  portENTER_CRITICAL(&mux_);
  if (lifecycle_ == Lifecycle::Uninitialized) {
    portEXIT_CRITICAL(&mux_);
    return ESP_OK;
  }
  if (lifecycle_ != Lifecycle::Ready) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  lifecycle_ = Lifecycle::Stopping;
  portEXIT_CRITICAL(&mux_);

  for (TaskSlot& slot : slots_) {
    if (slot.queue) {
      vQueueDelete(slot.queue);
      slot.queue = nullptr;
    }
    slot.task = nullptr;
    slot.handler = nullptr;
    slot.handler_context = nullptr;
    slot.tick_handler = nullptr;
    slot.tick_context = nullptr;
    slot.tick_interval = 0;
  }
  if (stop_events_) {
    vEventGroupDelete(stop_events_);
    stop_events_ = nullptr;
  }

  portENTER_CRITICAL(&mux_);
  admission_ = RuntimeAdmission();
  lifecycle_ = Lifecycle::Uninitialized;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

void RuntimeSupervisor::recordAdmissionLocked(TaskLane lane,
                                              AdmissionResult result) {
  const size_t index = taskLaneIndex(lane);
  if (index >= kTaskLaneCount) return;
  switch (result) {
    case AdmissionResult::QueueFull:
      ++diagnostics_.queue_full[index];
      break;
    case AdmissionResult::NotReady:
      ++diagnostics_.not_ready[index];
      break;
    case AdmissionResult::InvalidEnvelope:
    case AdmissionResult::WrongLane:
    case AdmissionResult::Underflow:
      ++diagnostics_.invalid[index];
      break;
    case AdmissionResult::StaleGeneration:
      ++diagnostics_.stale[index];
      break;
    case AdmissionResult::Expired:
      ++diagnostics_.expired[index];
      break;
    case AdmissionResult::Admitted:
      break;
  }
}

AdmissionResult RuntimeSupervisor::post(const WorkEnvelope& envelope) {
  const TaskLane lane = RuntimeAdmission::routeFor(envelope);
  const size_t index = taskLaneIndex(lane);
  if (index >= slots_.size()) return AdmissionResult::WrongLane;

  portENTER_CRITICAL(&mux_);
  if (lifecycle_ != Lifecycle::Running) {
    recordAdmissionLocked(lane, AdmissionResult::NotReady);
    portEXIT_CRITICAL(&mux_);
    return AdmissionResult::NotReady;
  }
  const AdmissionResult admission = admission_.admit(lane, envelope, nowMs());
  recordAdmissionLocked(lane, admission);
  if (admission != AdmissionResult::Admitted) {
    portEXIT_CRITICAL(&mux_);
    return admission;
  }

  if (xQueueSend(slots_[index].queue, &envelope, 0) != pdPASS) {
    admission_.release(lane);
    telemetry_.recordQueueDepth(lane, admission_.used(lane));
    recordAdmissionLocked(lane, AdmissionResult::QueueFull);
    portEXIT_CRITICAL(&mux_);
    return AdmissionResult::QueueFull;
  }
  telemetry_.recordQueueDepth(lane, admission_.used(lane));
  portEXIT_CRITICAL(&mux_);
  return AdmissionResult::Admitted;
}

AdmissionResult RuntimeSupervisor::postButtonFromIsr(
    const WorkEnvelope& envelope,
    BaseType_t* higher_priority_task_woken) {
  if (!higher_priority_task_woken || envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Button) {
    return AdmissionResult::InvalidEnvelope;
  }
  constexpr TaskLane lane = TaskLane::Input;
  constexpr size_t index = taskLaneIndex(lane);

  portENTER_CRITICAL_ISR(&mux_);
  if (lifecycle_ != Lifecycle::Running) {
    ++diagnostics_.not_ready[index];
    portEXIT_CRITICAL_ISR(&mux_);
    return AdmissionResult::NotReady;
  }
  const AdmissionResult admission = admission_.admit(lane, envelope, nowMs());
  recordAdmissionLocked(lane, admission);
  if (admission != AdmissionResult::Admitted) {
    if (admission == AdmissionResult::QueueFull) {
      ++diagnostics_.input_isr_overflow;
    }
    portEXIT_CRITICAL_ISR(&mux_);
    return admission;
  }

  if (xQueueSendFromISR(slots_[index].queue, &envelope,
                        higher_priority_task_woken) != pdPASS) {
    admission_.release(lane);
    telemetry_.recordQueueDepth(lane, admission_.used(lane));
    ++diagnostics_.queue_full[index];
    ++diagnostics_.input_isr_overflow;
    portEXIT_CRITICAL_ISR(&mux_);
    return AdmissionResult::QueueFull;
  }
  // ISR telemetry remains constant-time counter maintenance only. Stack and
  // heap APIs are sampled later by the managed task itself.
  telemetry_.recordQueueDepth(lane, admission_.used(lane));
  portEXIT_CRITICAL_ISR(&mux_);
  return AdmissionResult::Admitted;
}

AdmissionResult RuntimeSupervisor::cancelBefore(
    WorkClass work_class, uint64_t generation_floor) {
  portENTER_CRITICAL(&mux_);
  const AdmissionResult result =
      admission_.cancelBefore(work_class, generation_floor);
  portEXIT_CRITICAL(&mux_);
  return result;
}

void RuntimeSupervisor::taskEntry(void* opaque) {
  auto* context = static_cast<TaskContext*>(opaque);
  if (!context || !context->supervisor ||
      taskLaneIndex(context->lane) >= kTaskLaneCount) {
    vTaskDelete(nullptr);
    return;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  context->supervisor->run(context->lane);
  context->supervisor->markTaskQuiescent(context->lane);
  vTaskSuspend(nullptr);
}

void RuntimeSupervisor::run(TaskLane lane) {
  const size_t index = taskLaneIndex(lane);
  TaskSlot& slot = slots_[index];
  WorkEnvelope envelope{};
  TickType_t last_tick = xTaskGetTickCount();
  TickType_t last_resource_sample = last_tick;
  // Stack scanning is intentionally infrequent on responsive lanes. Global
  // heap locks are taken only by the lowest-priority Portal task.
  const TickType_t resource_interval = pdMS_TO_TICKS(10000);
  recordTaskStarted(lane);
  sampleManagedResources(lane);
  for (;;) {
    portENTER_CRITICAL(&mux_);
    const bool stopping = lifecycle_ == Lifecycle::Stopping;
    portEXIT_CRITICAL(&mux_);
    if (stopping) break;
    // Lanes without an owner tick must still wake occasionally so their task,
    // stack and core-placement health cannot go permanently stale while the
    // device is idle. This timeout performs no product work and is deliberately
    // much slower than every interactive lane.
    const TickType_t wait = slot.tick_handler ? slot.tick_interval
                                              : resource_interval;
    if (xQueueReceive(slot.queue, &envelope, wait) == pdPASS) {
      portENTER_CRITICAL(&mux_);
      const bool stop_before_handler = lifecycle_ == Lifecycle::Stopping;
      portEXIT_CRITICAL(&mux_);
      if (stop_before_handler) break;
      handleDequeued(lane, envelope);
    }
    portENTER_CRITICAL(&mux_);
    const bool stop_before_tick = lifecycle_ == Lifecycle::Stopping;
    portEXIT_CRITICAL(&mux_);
    if (stop_before_tick) break;
    if (slot.tick_handler) {
      const TickType_t now = xTaskGetTickCount();
      const TickType_t elapsed = static_cast<TickType_t>(now - last_tick);
      if (elapsed >= slot.tick_interval) {
        last_tick = now;
        const int64_t started_us = esp_timer_get_time();
        slot.tick_handler(slot.tick_context);
        recordTickTiming(lane, elapsedUs(started_us), elapsed,
                         slot.tick_interval);
      }
    }
    const TickType_t resource_now = xTaskGetTickCount();
    if (resource_interval == 0 ||
        static_cast<TickType_t>(resource_now - last_resource_sample) >=
            resource_interval) {
      last_resource_sample = resource_now;
      sampleManagedResources(lane);
    }
  }
}

void RuntimeSupervisor::markTaskQuiescent(TaskLane lane) {
  const size_t index = taskLaneIndex(lane);
  if (index >= slots_.size()) return;
  portENTER_CRITICAL(&mux_);
  telemetry_.recordTaskStopped(lane);
  portEXIT_CRITICAL(&mux_);
  if (stop_events_)
    xEventGroupSetBits(stop_events_, static_cast<EventBits_t>(1U) << index);
}

void RuntimeSupervisor::handleDequeued(TaskLane lane,
                                       const WorkEnvelope& envelope) {
  portENTER_CRITICAL(&mux_);
  const AdmissionResult released = admission_.release(lane);
  telemetry_.recordQueueDepth(lane, admission_.used(lane));
  AdmissionResult execution = released;
  if (released == AdmissionResult::Admitted) {
    execution = admission_.shouldExecute(lane, envelope, nowMs());
  }
  recordAdmissionLocked(lane, execution);
  WorkHandler handler = slots_[taskLaneIndex(lane)].handler;
  void* context = slots_[taskLaneIndex(lane)].handler_context;
  portEXIT_CRITICAL(&mux_);

  if (execution != AdmissionResult::Admitted) {
    if (lane != TaskLane::Control && envelope.kind == EnvelopeKind::Command &&
        post(makeResult(envelope, rejectionDisposition(execution))) !=
            AdmissionResult::Admitted) {
      recordResultFailure();
    }
    return;
  }

  const int64_t started_us = esp_timer_get_time();
  WorkDisposition disposition = handler(envelope, context);
  recordHandlerTiming(lane, elapsedUs(started_us));
  if (disposition == WorkDisposition::Accepted) {
    disposition = WorkDisposition::Failed;
    recordHandlerFailure(lane);
  } else if (disposition == WorkDisposition::Failed) {
    recordHandlerFailure(lane);
  }

  if (lane != TaskLane::Control && envelope.kind == EnvelopeKind::Command &&
      post(makeResult(envelope, disposition)) != AdmissionResult::Admitted) {
    recordResultFailure();
  }
}

void RuntimeSupervisor::recordHandlerFailure(TaskLane lane) {
  const size_t index = taskLaneIndex(lane);
  portENTER_CRITICAL(&mux_);
  if (index < diagnostics_.handler_failed.size()) {
    ++diagnostics_.handler_failed[index];
  }
  portEXIT_CRITICAL(&mux_);
}

void RuntimeSupervisor::recordResultFailure() {
  portENTER_CRITICAL(&mux_);
  ++diagnostics_.result_delivery_failed;
  portEXIT_CRITICAL(&mux_);
}

void RuntimeSupervisor::recordTaskStarted(TaskLane lane) {
  const int core = xPortGetCoreID();
  const UBaseType_t priority = uxTaskPriorityGet(nullptr);
  portENTER_CRITICAL(&mux_);
  telemetry_.recordTaskStarted(
      lane, core < std::numeric_limits<int8_t>::min()
                ? std::numeric_limits<int8_t>::min()
                : core > std::numeric_limits<int8_t>::max()
                      ? std::numeric_limits<int8_t>::max()
                      : static_cast<int8_t>(core),
      boundedPriority(priority), nowMs());
  portEXIT_CRITICAL(&mux_);
}

void RuntimeSupervisor::recordHandlerTiming(TaskLane lane,
                                            uint32_t duration_us) {
  portENTER_CRITICAL(&mux_);
  telemetry_.recordHandler(lane, duration_us, nowMs());
  portEXIT_CRITICAL(&mux_);
}

void RuntimeSupervisor::recordTickTiming(TaskLane lane, uint32_t duration_us,
                                         TickType_t elapsed_ticks,
                                         TickType_t interval_ticks) {
  portENTER_CRITICAL(&mux_);
  telemetry_.recordTick(lane, duration_us,
                        static_cast<uint64_t>(elapsed_ticks),
                        static_cast<uint64_t>(interval_ticks),
                        configTICK_RATE_HZ, nowMs());
  portEXIT_CRITICAL(&mux_);
}

void RuntimeSupervisor::sampleManagedResources(TaskLane lane) {
  // In ESP-IDF's FreeRTOS port this high-water mark is reported in bytes.
  const uint32_t stack_low_water =
      boundedSize(static_cast<size_t>(uxTaskGetStackHighWaterMark(nullptr)));
  const bool sample_heap = lane == TaskLane::Portal;
  const uint32_t internal_min =
      sample_heap ? boundedSize(heap_caps_get_minimum_free_size(
                        MALLOC_CAP_INTERNAL))
                  : 0;
  const size_t psram_total =
      sample_heap ? heap_caps_get_total_size(MALLOC_CAP_SPIRAM) : 0;
  const uint32_t psram_min =
      psram_total == 0
          ? 0
          : boundedSize(
                heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  const int core = xPortGetCoreID();
  const int8_t observed_core =
      core < std::numeric_limits<int8_t>::min()
          ? std::numeric_limits<int8_t>::min()
          : core > std::numeric_limits<int8_t>::max()
                ? std::numeric_limits<int8_t>::max()
                : static_cast<int8_t>(core);
  const uint8_t priority = boundedPriority(uxTaskPriorityGet(nullptr));
  portENTER_CRITICAL(&mux_);
  telemetry_.recordResourceSample(
      lane, stack_low_water, sample_heap, internal_min, psram_total != 0,
      psram_min, observed_core, priority, nowMs());
  portEXIT_CRITICAL(&mux_);
}

bool RuntimeSupervisor::initialized() const {
  portENTER_CRITICAL(&mux_);
  const bool value = lifecycle_ != Lifecycle::Uninitialized &&
                     lifecycle_ != Lifecycle::Initializing;
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool RuntimeSupervisor::started() const {
  portENTER_CRITICAL(&mux_);
  const bool value = lifecycle_ == Lifecycle::Running;
  portEXIT_CRITICAL(&mux_);
  return value;
}

SupervisorDiagnostics RuntimeSupervisor::diagnostics() const {
  portENTER_CRITICAL(&mux_);
  const SupervisorDiagnostics value = diagnostics_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

RuntimeTelemetrySnapshot RuntimeSupervisor::telemetry() const {
  portENTER_CRITICAL(&mux_);
  const RuntimeTelemetrySnapshot value = telemetry_.snapshot();
  portEXIT_CRITICAL(&mux_);
  return value;
}

}  // namespace inkloop
