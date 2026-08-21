#pragma once

#include "PortalContracts.h"

namespace inkloop {
namespace portal {

enum class BindingCompletionState : uint8_t {
  Neither,
  MyAiOnly,
  InkloopOnly,
  Both,
};

enum class PairingCodeRetentionState : uint8_t {
  NotAvailable,
  NeededByMyAi,
  NeededByInkloop,
  TerminalScrubbed,
};

class OnboardingState {
 public:
  OnboardingState();

  OnboardingStage stage() const { return stage_; }
  TutorialStep tutorialStep() const { return tutorialStep_; }
  bool wifiConfigured() const { return wifiConfigured_; }
  bool myAiActive() const { return myAiActive_; }
  bool inkloopBound() const { return inkloopBound_; }
  bool tutorialComplete() const { return tutorialStep_ == TutorialStep::Complete; }
  bool myAiBindingComplete() const;
  BindingCompletionState bindingCompletionState() const;
  PairingCodeRetentionState codeRetentionState() const;
  bool terminalBindingComplete() const {
    return bindingCompletionState() == BindingCompletionState::Both;
  }
  bool inkloopReuseAccepted() const { return inkloopReuseAccepted_; }
  CodeOwnership codeOwnership() const { return codeOwnership_; }
  const std::string& onboardingCode() const { return onboardingCode_; }
  const std::string& inkloopCode() const { return inkloopCode_; }
  uint64_t codeExpiresAtSeconds() const { return codeExpiresAtSeconds_; }

  bool setWifiConfigured(bool configured, std::string* error);
  bool requestMyAiPairing(IPortalAdapter& adapter, std::string* error);
  // Authenticated owner recovery. Inkloop ownership and the album remain
  // intact; only the MyAI side returns to a fresh pairing transaction.
  bool requestMyAiRebind(IPortalAdapter& adapter, std::string* error);
  // Trusted boot recovery: the MyAI credential store already proves a pending
  // pairing, so only restore the local portal stage without starting a second
  // server-side pairing attempt.
  bool resumeMyAiPairing(std::string* error);
  bool cancelMyAiPairing(std::string* error);
  bool receiveAuthoritativeMyAiCode(
      const std::string& code,
      uint64_t expiresAtSeconds,
      uint64_t nowSeconds,
      IPortalAdapter& adapter,
      std::string* error);
  // The MyAI code remains authoritative even when the optional Inkloop mirror
  // is temporarily unavailable. This trusted retry never rotates that code.
  bool retryInkloopCodeReuse(IPortalAdapter& adapter, std::string* error);
  bool markInkloopBound(std::string* error);
  bool setMyAiActivation(bool active, std::string* error);
  bool advanceTutorial(std::string* error);
  bool completeTutorial(std::string* error);
  bool restartTutorial(std::string* error);
  bool expireCodeIfNeeded(uint64_t nowSeconds);
  bool clearBoundCodeIfNeeded();

  OnboardingPersistedState persistedState() const;
  bool hydrate(const OnboardingPersistedState& state, std::string* error);
  static bool validatePersistedState(
      const OnboardingPersistedState& state,
      std::string* error);

  std::string myAiRegistrationUrl() const;
  std::string toJson() const;

  static bool validSixDigitCode(const std::string& code);

 private:
  void setError(const char* message, std::string* error) const;

  OnboardingStage stage_;
  TutorialStep tutorialStep_;
  bool wifiConfigured_;
  bool myAiActive_;
  bool inkloopBound_;
  bool inkloopReuseAccepted_;
  CodeOwnership codeOwnership_;
  std::string onboardingCode_;
  std::string inkloopCode_;
  uint64_t codeExpiresAtSeconds_;
};

const char* onboardingStageName(OnboardingStage stage);
const char* tutorialStepName(TutorialStep step);

}  // namespace portal
}  // namespace inkloop
