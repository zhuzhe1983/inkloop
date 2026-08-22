#include "inkloop/button_debounce.hpp"

namespace inkloop {

bool ButtonDebounceCore::accept(size_t button_index, uint32_t now_ms) {
  if (button_index >= seen_.size()) return false;
  if (seen_[button_index] &&
      static_cast<uint32_t>(now_ms - last_accepted_[button_index]) <
          debounce_ms_) {
    return false;
  }
  seen_[button_index] = true;
  last_accepted_[button_index] = now_ms;
  return true;
}

}  // namespace inkloop
