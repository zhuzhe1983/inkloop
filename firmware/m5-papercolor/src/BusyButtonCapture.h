#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "DisplayController.h"

namespace inkloop {

class BusyButtonCapture {
 public:
  bool begin(DisplayController& display);
  uint8_t takeAttempts();

 private:
  static void taskEntry(void* context);
  void run();

  DisplayController* display_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
};

}  // namespace inkloop
