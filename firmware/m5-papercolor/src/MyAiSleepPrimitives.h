#pragma once

#include <string>

namespace inkloop {

inline bool myAiPairingTransactionActive(
    bool pairingPollActive,
    bool pairingCallbackPending,
    bool activationPairing) {
  return pairingPollActive || pairingCallbackPending || activationPairing;
}

inline bool myAiServiceQuiescentAndUnavailable(
    const std::string& typedState,
    bool pairingTransactionActive) {
  if (pairingTransactionActive) return false;
  return typedState == "app_not_registered" || typedState == "offline" ||
      typedState == "error" || typedState == "unconfigured";
}

inline bool onboardingBlocksSleep(
    bool myAiAuthorized,
    bool inkloopPaired,
    bool onboardingMyAiActive,
    bool onboardingInkloopBound,
    bool serviceQuiescentAndUnavailable) {
  if (serviceQuiescentAndUnavailable) return false;
  return !myAiAuthorized || !inkloopPaired || !onboardingMyAiActive ||
      !onboardingInkloopBound;
}

}  // namespace inkloop
