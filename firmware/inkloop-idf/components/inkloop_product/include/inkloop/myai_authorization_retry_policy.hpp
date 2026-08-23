#pragma once

#include <cstdint>

#include "inkloop/myai/MyAiTypes.h"

namespace inkloop {

// Pure scheduling policy shared by the physical runtime and host tests. A
// transient transport/5xx response changes MyAiClient to Offline but does not
// erase its durable device credential, so Offline must remain eligible for a
// bounded authorization retry. Payment/recovery/security states remain
// explicitly blocked and require their existing user-facing flows.
class MyAiAuthorizationRetryPolicy final {
 public:
  static constexpr uint32_t kVerifiedRefreshMs = 10U * 60U * 1000U;
  static constexpr uint32_t kMinimumRetryMs = 5000U;

  static constexpr bool mayCheck(myai::ActivationState state) {
    return state == myai::ActivationState::Bound ||
        state == myai::ActivationState::Offline;
  }

  static constexpr bool shouldCheck(myai::ActivationState state,
                                    bool refresh_due) {
    // The caller initializes the first deadline to zero and advances it after
    // every attempt.  Offline and an unverified Bound state must therefore
    // still honor that deadline; otherwise every Network tick would issue a
    // synchronous /devices/check request and starve voice/AIGC work.
    return mayCheck(state) && refresh_due;
  }

  static constexpr uint32_t nextDelay(bool verified,
                                      uint32_t requested_retry_ms) {
    if (verified) return kVerifiedRefreshMs;
    return requested_retry_ms < kMinimumRetryMs
        ? kMinimumRetryMs : requested_retry_ms;
  }

  static constexpr uint32_t nextDeadline(uint32_t completed_at_ms,
                                         bool verified,
                                         uint32_t requested_retry_ms) {
    // Anchor the delay to completion, never request start. A synchronous
    // authorization timeout can outlive the retry delay by many seconds.
    return completed_at_ms + nextDelay(verified, requested_retry_ms);
  }
};

}  // namespace inkloop
