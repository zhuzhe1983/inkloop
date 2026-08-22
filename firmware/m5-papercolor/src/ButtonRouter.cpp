#include "ButtonRouter.h"

#include <M5Unified.h>
#include <driver/gpio.h>

#include "BusyButtonPrimitives.h"
#include "Diagnostics.h"

namespace inkloop {

namespace {
constexpr gpio_num_t kButtonPins[] = {
    GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_1};
constexpr ButtonEvent kButtonEvents[] = {
    ButtonEvent::PreviousPage, ButtonEvent::NextPage, ButtonEvent::Voice};
constexpr uint8_t kButtonBits[] = {0x01, 0x02, 0x04};
}  // namespace

ButtonRouter::~ButtonRouter() {
  running_.store(false, std::memory_order_release);
  if (task_) vTaskDelete(task_);
  if (queue_) vQueueDelete(queue_);
}

void ButtonRouter::begin(ButtonEventHandler handler, void* context) {
  handler_ = handler;
  context_ = context;
  if (task_) return;
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << GPIO_NUM_10) |
      (1ULL << GPIO_NUM_9) | (1ULL << GPIO_NUM_1);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) {
    Diagnostics::event("ERROR", "BUTTON_CAPTURE_GPIO_FAILED");
    return;
  }
  queue_ = xQueueCreate(16, sizeof(uint8_t));
  if (!queue_) {
    Diagnostics::event("ERROR", "BUTTON_CAPTURE_QUEUE_FAILED");
    return;
  }
  running_.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(
          taskEntry, "inkloop-input", 2048, this, 4, &task_, 1) != pdPASS) {
    running_.store(false, std::memory_order_release);
    vQueueDelete(queue_);
    queue_ = nullptr;
    Diagnostics::event("ERROR", "BUTTON_CAPTURE_TASK_FAILED");
    return;
  }
  Diagnostics::event("BUTTON_CAPTURE", "CORE_1_PRIORITY_4_READY");
}

void ButtonRouter::poll() {
  if (!handler_) return;
  uint8_t encoded = 0;
  if (suppressUntilRelease_) {
    while (queue_ && xQueueReceive(queue_, &encoded, 0) == pdTRUE) {
      if (!suppressedPageReported_ && encoded < 2U) {
        suppressedPageAttempt_ = true;
        suppressedPageReported_ = true;
      }
    }
    if (pressedMask_.load(std::memory_order_acquire) == 0) {
      suppressUntilRelease_ = false;
      suppressedPageReported_ = false;
    }
    return;
  }
  while (queue_ && xQueueReceive(queue_, &encoded, 0) == pdTRUE) {
    if (encoded < sizeof(kButtonEvents) / sizeof(kButtonEvents[0]))
      handler_(kButtonEvents[encoded], context_);
  }
}

void ButtonRouter::taskEntry(void* context) {
  static_cast<ButtonRouter*>(context)->run();
}

void ButtonRouter::run() {
  uint8_t stable[3] = {1, 1, 1};
  uint8_t candidate[3] = {1, 1, 1};
  uint8_t candidateCount[3] = {0, 0, 0};
  for (size_t index = 0; index < 3; ++index) {
    stable[index] = candidate[index] =
        static_cast<uint8_t>(gpio_get_level(kButtonPins[index]));
  }
  while (running_.load(std::memory_order_acquire)) {
    uint8_t pressed = 0;
    for (size_t index = 0; index < 3; ++index) {
      const uint8_t sample =
          static_cast<uint8_t>(gpio_get_level(kButtonPins[index]));
      if (updateDebouncedActiveLowButton(
              stable[index], candidate[index], candidateCount[index],
              sample, true)) {
        const uint8_t event = static_cast<uint8_t>(index);
        xQueueSend(queue_, &event, 0);
      }
      if (stable[index] == 0) pressed |= kButtonBits[index];
    }
    pressedMask_.store(pressed, std::memory_order_release);
    vTaskDelay(pdMS_TO_TICKS(8));
  }
  vTaskDelete(nullptr);
}

bool ButtonRouter::takeSuppressedPageAttempt() {
  const bool result = suppressedPageAttempt_;
  suppressedPageAttempt_ = false;
  return result;
}

const char* ButtonRouter::name(ButtonEvent event) {
  switch (event) {
    case ButtonEvent::Voice: return "VOICE";
    case ButtonEvent::PreviousPage: return "PAGE_PREVIOUS";
    case ButtonEvent::NextPage: return "PAGE_NEXT";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
