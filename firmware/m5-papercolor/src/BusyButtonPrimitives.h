#pragma once

#include <stdint.h>

namespace inkloop {

inline bool updateDebouncedActiveLowButton(
  uint8_t& stable,
  uint8_t& candidate,
  uint8_t& candidateCount,
  uint8_t sample,
  bool displayBusy
) {
  if (sample != candidate) {
    candidate = sample;
    candidateCount = 1;
  } else if (candidateCount < 3) {
    ++candidateCount;
  }
  if (candidateCount < 2 || stable == candidate) return false;
  stable = candidate;
  return stable == 0 && displayBusy;
}

}  // namespace inkloop
