#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"

namespace inkloop {

enum class ProductTextKind : uint8_t {
  AsrFinal,
  AssistantFinal,
  ToolState,
  AigcState,
};

// Fixed-capacity cross-task text pool. Envelopes carry only the returned
// ticket; no std::string, pointer or audio payload crosses a task queue.
class BoundedTextPool final {
 public:
  static constexpr size_t kSlotCount = 8;
  static constexpr size_t kMaximumTextBytes = 3072;

  uint64_t put(ProductTextKind kind, const std::string& text);
  bool take(uint64_t ticket, ProductTextKind& kind, std::string& text);
  bool release(uint64_t ticket);
  void clear();
  size_t used() const;

 private:
  struct Slot {
    std::array<char, kMaximumTextBytes + 1> bytes{};
    uint64_t ticket = 0;
    size_t length = 0;
    ProductTextKind kind = ProductTextKind::ToolState;
    bool occupied = false;
  };

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::array<Slot, kSlotCount> slots_{};
  uint64_t next_ticket_ = 0;
};

}  // namespace inkloop
