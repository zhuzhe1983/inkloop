#pragma once

#include "inkloop/board.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/status_led_core.hpp"

namespace inkloop {

class EspStatusLedOwner final {
 public:
  EspStatusLedOwner(IBoardAdapter& board, RuntimeSupervisor& supervisor)
      : board_(board), supervisor_(supervisor) {}

  esp_err_t configure();
  // Called only after the supervisor task has stopped. Leaves the physical
  // pixels dark and makes a later configure() attempt well-defined.
  void shutdown();
  StatusLedCore snapshot() const;
  // Settings owner calls this from the low-priority Portal lane. The update
  // is bounded and the LED owner still performs all hardware writes.
  void setMaximumBrightnessPercent(uint8_t percent,
                                   bool run_hardware_test = true);
  // Atomically updates both SKU-neutral presentation settings. Role swap is
  // ignored on boards with fewer than two physical pixels.
  void setPresentation(uint8_t maximum_brightness_percent,
                       bool roles_swapped,
                       bool run_hardware_test = true);

 private:
  static WorkDisposition handle(const WorkEnvelope& envelope, void* context);
  static void tick(void* context);
  WorkDisposition handleCommand(const WorkEnvelope& envelope);
  void service();

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  StatusLedCore core_{};
  uint8_t physical_pixels_ = 0;
  bool roles_swapped_ = false;
  bool configured_ = false;
};

}  // namespace inkloop
