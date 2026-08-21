#pragma once

#include <stddef.h>

namespace inkloop {

constexpr bool exactSixDigitPairingCode(const char* value, size_t length) {
  return value && length == 6 &&
      value[0] >= '0' && value[0] <= '9' &&
      value[1] >= '0' && value[1] <= '9' &&
      value[2] >= '0' && value[2] <= '9' &&
      value[3] >= '0' && value[3] <= '9' &&
      value[4] >= '0' && value[4] <= '9' &&
      value[5] >= '0' && value[5] <= '9';
}

}  // namespace inkloop
