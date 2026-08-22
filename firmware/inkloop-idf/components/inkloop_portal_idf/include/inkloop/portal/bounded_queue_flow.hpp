#pragma once

#include <cstdint>

#include "inkloop/portal/portal_core.hpp"

namespace inkloop {
namespace portal {

// Retry only queue backpressure. The caller supplies an absolute deadline and
// a short cooperative yield; errors other than Busy fail immediately.
template <typename Attempt, typename Clock, typename Yield>
PortalResult retryBusyUntil(int64_t deadline_us, Attempt attempt,
                            Clock clock, Yield yield) {
  PortalResult result = attempt();
  while (result == PortalResult::Busy && clock() < deadline_us) {
    yield();
    result = attempt();
  }
  return result;
}

}  // namespace portal
}  // namespace inkloop
