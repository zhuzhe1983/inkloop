#include "ResponsiveWorkExecutor.h"

namespace inkloop {

namespace {
constexpr uint32_t kResponsiveWorkerStackBytes = 16384;
constexpr UBaseType_t kResponsiveWorkerPriority = 1;
constexpr BaseType_t kResponsiveWorkerCore = 0;
}  // namespace

bool ResponsiveWorkExecutor::begin() {
  if (task_) return true;
  queue_ = xQueueCreate(1, sizeof(WorkItem));
  completion_ = xSemaphoreCreateBinary();
  dispatch_ = xSemaphoreCreateMutex();
  if (!queue_ || !completion_ || !dispatch_) {
    if (queue_) vQueueDelete(queue_);
    if (completion_) vSemaphoreDelete(completion_);
    if (dispatch_) vSemaphoreDelete(dispatch_);
    queue_ = nullptr;
    completion_ = nullptr;
    dispatch_ = nullptr;
    return false;
  }
  if (xTaskCreatePinnedToCore(
          taskEntry, "inkloop-io", kResponsiveWorkerStackBytes, this,
          kResponsiveWorkerPriority, &task_, kResponsiveWorkerCore) != pdPASS) {
    task_ = nullptr;
    vQueueDelete(queue_);
    vSemaphoreDelete(completion_);
    vSemaphoreDelete(dispatch_);
    queue_ = nullptr;
    completion_ = nullptr;
    dispatch_ = nullptr;
    return false;
  }
  return true;
}

void ResponsiveWorkExecutor::setPump(
    ResponsivePumpFunction pump, void* context) {
  pump_ = pump;
  pumpContext_ = context;
}

bool ResponsiveWorkExecutor::execute(
    ResponsiveWorkKind kind,
    ResponsiveWorkFunction work,
    void* context) {
  if (!work || !begin() || xTaskGetCurrentTaskHandle() == task_ ||
      xSemaphoreTake(dispatch_, 0) != pdTRUE) {
    return false;
  }
  while (xSemaphoreTake(completion_, 0) == pdTRUE) {}
  WorkItem item;
  item.function = work;
  item.context = context;
  activeKind_.store(kind, std::memory_order_release);
  active_.store(true, std::memory_order_release);
  const uint32_t startedAt = millis();
  if (xQueueSend(queue_, &item, 0) != pdTRUE) {
    active_.store(false, std::memory_order_release);
    xSemaphoreGive(dispatch_);
    return false;
  }
  while (xSemaphoreTake(completion_, 0) != pdTRUE) {
    if (pump_) pump_(pumpContext_, kind);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  lastElapsedMilliseconds_.store(
      millis() - startedAt, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  xSemaphoreGive(dispatch_);
  return true;
}

void ResponsiveWorkExecutor::taskEntry(void* context) {
  static_cast<ResponsiveWorkExecutor*>(context)->run();
}

void ResponsiveWorkExecutor::run() {
  WorkItem item;
  for (;;) {
    if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdTRUE) continue;
    if (item.function) item.function(item.context);
    xSemaphoreGive(completion_);
  }
}

ResponsiveWorkExecutor& responsiveWorkExecutor() {
  static ResponsiveWorkExecutor executor;
  return executor;
}

}  // namespace inkloop
