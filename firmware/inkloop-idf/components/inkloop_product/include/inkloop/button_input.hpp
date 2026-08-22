#pragma once

#include <array>
#include <cstdint>

#include "inkloop/board.hpp"
#include "inkloop/button_debounce.hpp"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/runtime_supervisor.hpp"

namespace inkloop {

// GPIO falling edges are copied into the zero-wait Input lane. Debounce runs
// on the highest-priority task and successful events are automatically
// forwarded by RuntimeSupervisor as results to the Control lane.
class EspButtonInputOwner final {
 public:
  EspButtonInputOwner(IBoardAdapter& board, RuntimeSupervisor& supervisor);
  ~EspButtonInputOwner();

  EspButtonInputOwner(const EspButtonInputOwner&) = delete;
  EspButtonInputOwner& operator=(const EspButtonInputOwner&) = delete;

  esp_err_t configure();
  esp_err_t arm();
  void disarm();

 private:
  struct IsrContext {
    EspButtonInputOwner* owner = nullptr;
    BoardButton button = BoardButton::Previous;
  };

  static void gpioIsr(void* opaque);
  static WorkDisposition handle(const WorkEnvelope& envelope, void* context);
  void postFromIsr(BoardButton button);
  WorkDisposition handleInput(const WorkEnvelope& envelope);
  static ProductOpcode opcodeFor(BoardButton button);
  static size_t indexFor(BoardButton button);
  static bool buttonForOpcode(uint16_t opcode, BoardButton& button);

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  ButtonDebounceCore debounce_{};
  std::array<IsrContext, ButtonDebounceCore::kButtonCount> contexts_{};
  std::array<gpio_num_t, ButtonDebounceCore::kButtonCount> button_pins_{};
  std::array<bool, ButtonDebounceCore::kButtonCount> handler_installed_{};
  portMUX_TYPE sequence_mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint64_t sequence_ = 0;
  bool configured_ = false;
  bool armed_ = false;
  bool isr_service_owned_ = false;
};

}  // namespace inkloop
