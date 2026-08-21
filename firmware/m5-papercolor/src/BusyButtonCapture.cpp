#include "BusyButtonCapture.h"

#include <driver/gpio.h>

#include "BusyButtonPrimitives.h"
#include "Diagnostics.h"

namespace inkloop {

namespace {
constexpr gpio_num_t kButtonPins[] = {GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_1};
constexpr uint8_t kButtonBits[] = {0x01, 0x02, 0x04};
}

bool BusyButtonCapture::begin(DisplayController& display) {
  display_ = &display;
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << GPIO_NUM_10) | (1ULL << GPIO_NUM_9) | (1ULL << GPIO_NUM_1);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) return false;
  queue_ = xQueueCreate(8, sizeof(uint8_t));
  if (!queue_) return false;
  if (xTaskCreate(taskEntry, "inkloop-buttons", 2048, this, 1, &task_) != pdPASS) {
    vQueueDelete(queue_);
    queue_ = nullptr;
    return false;
  }
  Diagnostics::event("BUSY_BUTTON_CAPTURE", "READY");
  return true;
}

void BusyButtonCapture::taskEntry(void* context) {
  static_cast<BusyButtonCapture*>(context)->run();
}

void BusyButtonCapture::run() {
  uint8_t stable[3] = {1, 1, 1};
  uint8_t candidate[3] = {1, 1, 1};
  uint8_t candidateCount[3] = {0, 0, 0};
  for (size_t i = 0; i < 3; ++i) {
    stable[i] = candidate[i] = static_cast<uint8_t>(gpio_get_level(kButtonPins[i]));
  }
  while (true) {
    for (size_t i = 0; i < 3; ++i) {
      const uint8_t sample = static_cast<uint8_t>(gpio_get_level(kButtonPins[i]));
      if (updateDebouncedActiveLowButton(
            stable[i], candidate[i], candidateCount[i], sample,
            display_ && display_->busy()
          )) {
        const uint8_t event = kButtonBits[i];
        xQueueSend(queue_, &event, 0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

uint8_t BusyButtonCapture::takeAttempts() {
  uint8_t combined = 0;
  uint8_t event = 0;
  while (queue_ && xQueueReceive(queue_, &event, 0) == pdTRUE) combined |= event;
  return combined;
}

}  // namespace inkloop
