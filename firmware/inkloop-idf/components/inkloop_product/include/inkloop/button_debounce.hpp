#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

// Portable, allocation-free debounce core. The hardware adapter maps each
// physical control to one stable slot, keeping ISR and GPIO details out of the
// decision logic and making wrap-around behavior independently testable.
class ButtonDebounceCore final {
 public:
  static constexpr size_t kButtonCount = 3;

  explicit ButtonDebounceCore(uint32_t debounce_ms = 35)
      : debounce_ms_(debounce_ms) {}

  bool accept(size_t button_index, uint32_t now_ms);

 private:
  uint32_t debounce_ms_;
  std::array<uint32_t, kButtonCount> last_accepted_{{0, 0, 0}};
  std::array<bool, kButtonCount> seen_{{false, false, false}};
};

}  // namespace inkloop
