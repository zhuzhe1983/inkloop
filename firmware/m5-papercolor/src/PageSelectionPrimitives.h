#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

enum class SettledPageDecision : uint8_t {
  Invalid = 0,
  AlreadyCurrent = 1,
  Refresh = 2,
};

inline SettledPageDecision settledPageDecision(
    size_t pageCount, size_t currentPage, size_t targetPage) {
  if (pageCount == 0 || currentPage >= pageCount || targetPage >= pageCount) {
    return SettledPageDecision::Invalid;
  }
  return currentPage == targetPage
      ? SettledPageDecision::AlreadyCurrent
      : SettledPageDecision::Refresh;
}

}  // namespace inkloop
