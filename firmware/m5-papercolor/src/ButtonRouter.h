#pragma once

#include <Arduino.h>

#include "FirmwarePrimitives.h"

namespace inkloop {

using ButtonEventHandler = void (*)(ButtonEvent event, void* context);

class ButtonRouter {
 public:
  void begin(ButtonEventHandler handler, void* context);
  void poll();
  void suppressUntilRelease() { suppressUntilRelease_ = true; }
  bool takeSuppressedPageAttempt();
  static const char* name(ButtonEvent event);

 private:
  ButtonEventHandler handler_ = nullptr;
  void* context_ = nullptr;
  bool suppressUntilRelease_ = false;
  bool suppressedPageAttempt_ = false;
  bool suppressedPageReported_ = false;
};

}  // namespace inkloop
