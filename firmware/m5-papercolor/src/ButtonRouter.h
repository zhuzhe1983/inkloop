#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "FirmwarePrimitives.h"

namespace inkloop {

using ButtonEventHandler = void (*)(ButtonEvent event, void* context);

class ButtonRouter {
 public:
  ~ButtonRouter();
  void begin(ButtonEventHandler handler, void* context);
  void poll();
  void suppressUntilRelease() { suppressUntilRelease_ = true; }
  bool takeSuppressedPageAttempt();
  static const char* name(ButtonEvent event);

 private:
  static void taskEntry(void* context);
  void run();

  ButtonEventHandler handler_ = nullptr;
  void* context_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<uint8_t> pressedMask_{0};
  bool suppressUntilRelease_ = false;
  bool suppressedPageAttempt_ = false;
  bool suppressedPageReported_ = false;
};

}  // namespace inkloop
