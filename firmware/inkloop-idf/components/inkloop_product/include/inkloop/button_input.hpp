#pragma once

#include <array>
#include <cstdint>

#include "inkloop/board.hpp"
#include "inkloop/button_debounce.hpp"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/runtime_supervisor.hpp"

namespace inkloop {

class ButtonLatencyTelemetry;

// GPIO falling edges are copied into an internal-DRAM mailbox even while the
// flash cache is unavailable. The highest-priority Input lane drains it every
// millisecond, runs debounce, and RuntimeSupervisor forwards results to
// Control. The 1 kHz FreeRTOS tick preserves that bound without polling or
// doing queue work inside the ISR.
class EspButtonInputOwner final {
 public:
  static constexpr size_t kRawEdgeCapacity = 16U;

  EspButtonInputOwner(IBoardAdapter& board, RuntimeSupervisor& supervisor,
                      ButtonLatencyTelemetry* latency = nullptr);
  ~EspButtonInputOwner();

  EspButtonInputOwner(const EspButtonInputOwner&) = delete;
  EspButtonInputOwner& operator=(const EspButtonInputOwner&) = delete;

  esp_err_t configure();
  esp_err_t arm();
  void disarm();
  // Sleep admission calls this after every owner has quiesced. A GPIO edge
  // captured while supervisor admission is frozen remains visible here until
  // the Input task can drain it after the aborted sleep attempt.
  bool hasPendingRawEdge() const;
  uint32_t rawEdgeOverflowCount() const;

 private:
  struct IsrContext {
    EspButtonInputOwner* owner = nullptr;
    uint8_t button_index = 0U;
  };

  struct RawEdge {
    uint64_t request_id = 0U;
    uint32_t captured_us = 0U;
    uint32_t captured_ms = 0U;
    uint8_t button_index = 0U;
  };

  static void gpioIsr(void* opaque);
  static WorkDisposition handle(const WorkEnvelope& envelope, void* context);
  static void inputTick(void* context);
  void captureRawEdgeFromIsr(uint8_t button_index);
  void serviceRawEdges();
  bool popRawEdge(RawEdge& edge);
  void clearRawEdges();
  WorkDisposition handleInput(const WorkEnvelope& envelope);
  static ProductOpcode opcodeFor(BoardButton button);
  static size_t indexFor(BoardButton button);
  static bool buttonForOpcode(uint16_t opcode, BoardButton& button);

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  ButtonLatencyTelemetry* latency_ = nullptr;
  ButtonDebounceCore debounce_{};
  std::array<IsrContext, ButtonDebounceCore::kButtonCount> contexts_{};
  std::array<gpio_num_t, ButtonDebounceCore::kButtonCount> button_pins_{};
  std::array<bool, ButtonDebounceCore::kButtonCount> handler_installed_{};
  // EspProductRuntime is a static .bss owner and external-BSS placement is
  // disabled in sdkconfig. This fixed mailbox therefore stays in internal
  // DRAM and remains readable while the flash cache is unavailable.
  RawEdge raw_edges_[kRawEdgeCapacity]{};
  mutable portMUX_TYPE raw_edge_mux_ = portMUX_INITIALIZER_UNLOCKED;
  size_t raw_edge_head_ = 0U;
  size_t raw_edge_count_ = 0U;
  uint64_t sequence_ = 0;
  uint32_t raw_edge_overflow_ = 0U;
  bool mailbox_accepting_ = false;
  bool configured_ = false;
  bool armed_ = false;
  bool isr_service_owned_ = false;
};

}  // namespace inkloop
