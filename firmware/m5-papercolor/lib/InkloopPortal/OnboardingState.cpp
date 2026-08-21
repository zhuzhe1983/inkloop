#include "OnboardingState.h"

#include <sstream>

#include "PortalEncoding.h"

namespace inkloop {
namespace portal {

PortalPersistedSnapshot makeFreshPortalSnapshot() {
  PortalPersistedSnapshot snapshot;
  snapshot.schemaVersion = kPortalSnapshotSchemaVersion;
  snapshot.presentFields = kAllPortalSnapshotFields;
  snapshot.revision = 1;
  return snapshot;
}

OnboardingState::OnboardingState()
    : stage_(OnboardingStage::WifiAccessPoint),
      tutorialStep_(TutorialStep::PressToTalk),
      wifiConfigured_(false),
      myAiActive_(false),
      inkloopBound_(false),
      inkloopReuseAccepted_(false),
      codeOwnership_(CodeOwnership::None),
      onboardingCode_(),
      inkloopCode_(),
      codeExpiresAtSeconds_(0) {}

void OnboardingState::setError(const char* message, std::string* error) const {
  if (error) *error = message;
}

bool OnboardingState::validSixDigitCode(const std::string& code) {
  if (code.size() != 6) return false;
  for (size_t index = 0; index < code.size(); ++index) {
    if (code[index] < '0' || code[index] > '9') return false;
  }
  return true;
}

bool OnboardingState::myAiBindingComplete() const {
  return myAiActive_ || stage_ == OnboardingStage::MyAiInactive ||
      stage_ == OnboardingStage::VoiceTutorial ||
      stage_ == OnboardingStage::SettingsReady;
}

BindingCompletionState OnboardingState::bindingCompletionState() const {
  const bool myAiBound = myAiBindingComplete();
  if (myAiBound && inkloopBound_) return BindingCompletionState::Both;
  if (myAiBound) return BindingCompletionState::MyAiOnly;
  if (inkloopBound_) return BindingCompletionState::InkloopOnly;
  return BindingCompletionState::Neither;
}

PairingCodeRetentionState OnboardingState::codeRetentionState() const {
  switch (bindingCompletionState()) {
    case BindingCompletionState::Both:
      return PairingCodeRetentionState::TerminalScrubbed;
    case BindingCompletionState::InkloopOnly:
      return PairingCodeRetentionState::NeededByMyAi;
    case BindingCompletionState::MyAiOnly:
      return PairingCodeRetentionState::NeededByInkloop;
    case BindingCompletionState::Neither:
      return onboardingCode_.empty()
          ? PairingCodeRetentionState::NotAvailable
          : PairingCodeRetentionState::NeededByMyAi;
  }
  return PairingCodeRetentionState::NotAvailable;
}

bool OnboardingState::setWifiConfigured(bool configured, std::string* error) {
  wifiConfigured_ = configured;
  if (!configured) {
    stage_ = OnboardingStage::WifiAccessPoint;
    if (error) error->clear();
    return true;
  }
  if (myAiActive_) {
    stage_ = tutorialComplete()
        ? OnboardingStage::SettingsReady
        : OnboardingStage::VoiceTutorial;
  } else if (stage_ == OnboardingStage::WifiAccessPoint) {
    stage_ = OnboardingStage::WifiConfigured;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::requestMyAiPairing(IPortalAdapter& adapter, std::string* error) {
  if (!wifiConfigured_) {
    setError("wifi_not_configured", error);
    return false;
  }
  if (stage_ == OnboardingStage::MyAiPairingRequested ||
      stage_ == OnboardingStage::AwaitingMyAiActivation) {
    setError("myai_pairing_already_pending", error);
    return false;
  }
  if (myAiActive_ || stage_ == OnboardingStage::MyAiInactive ||
      stage_ == OnboardingStage::VoiceTutorial ||
      stage_ == OnboardingStage::SettingsReady) {
    setError("myai_pairing_not_required", error);
    return false;
  }
  std::string adapterError;
  if (!adapter.startMyAiPairing(kMyAiAppId, &adapterError)) {
    if (error) *error = adapterError.empty() ? "myai_pairing_not_started" : adapterError;
    return false;
  }
  stage_ = OnboardingStage::MyAiPairingRequested;
  if (error) error->clear();
  return true;
}

bool OnboardingState::resumeMyAiPairing(std::string* error) {
  if (!wifiConfigured_) {
    setError("wifi_not_configured", error);
    return false;
  }
  if (myAiActive_ || stage_ == OnboardingStage::MyAiInactive ||
      stage_ == OnboardingStage::VoiceTutorial ||
      stage_ == OnboardingStage::SettingsReady) {
    setError("myai_pairing_not_required", error);
    return false;
  }
  if (stage_ != OnboardingStage::WifiConfigured &&
      stage_ != OnboardingStage::MyAiPairingRequested &&
      stage_ != OnboardingStage::AwaitingMyAiActivation) {
    setError("myai_pairing_resume_rejected", error);
    return false;
  }
  stage_ = OnboardingStage::MyAiPairingRequested;
  if (error) error->clear();
  return true;
}

bool OnboardingState::cancelMyAiPairing(std::string* error) {
  if (myAiActive_ ||
      (stage_ != OnboardingStage::MyAiPairingRequested &&
       stage_ != OnboardingStage::AwaitingMyAiActivation)) {
    setError("myai_pairing_cancel_rejected", error);
    return false;
  }
  onboardingCode_.clear();
  codeExpiresAtSeconds_ = 0;
  stage_ = wifiConfigured_
      ? OnboardingStage::WifiConfigured
      : OnboardingStage::WifiAccessPoint;
  if (!inkloopBound_) {
    inkloopCode_.clear();
    inkloopReuseAccepted_ = false;
    codeOwnership_ = CodeOwnership::None;
  } else {
    codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::receiveAuthoritativeMyAiCode(
    const std::string& code,
    uint64_t expiresAtSeconds,
    uint64_t nowSeconds,
    IPortalAdapter& adapter,
    std::string* error) {
  if (!wifiConfigured_) {
    setError("wifi_not_configured", error);
    return false;
  }
  if (stage_ != OnboardingStage::MyAiPairingRequested &&
      stage_ != OnboardingStage::AwaitingMyAiActivation) {
    setError("myai_pairing_not_requested", error);
    return false;
  }
  if (!validSixDigitCode(code)) {
    setError("invalid_onboarding_code", error);
    return false;
  }
  if (expiresAtSeconds <= nowSeconds) {
    setError("onboarding_code_expired", error);
    return false;
  }

  // Inkloop-first is a valid partial order. A reboot may replay the same MyAI
  // code while Inkloop is already bound; retain/restore that sole authority
  // until MyAI completes, but never accept a different candidate.
  if (!onboardingCode_.empty() && onboardingCode_ != code) {
    setError("myai_code_replay_mismatch", error);
    return false;
  }
  if (inkloopBound_) {
    if (myAiBindingComplete()) {
      setError("terminal_binding_already_complete", error);
      return false;
    }
  }

  bool mirrored = inkloopReuseAccepted_;
  if (!inkloopBound_ && !mirrored) {
    std::string adapterError;
    mirrored = adapter.requestInkloopCodeReuse(
        code, expiresAtSeconds, &adapterError);
  }

  onboardingCode_ = code;
  codeExpiresAtSeconds_ = expiresAtSeconds;
  myAiActive_ = false;
  stage_ = OnboardingStage::AwaitingMyAiActivation;
  if (!inkloopBound_ && mirrored) {
    inkloopCode_ = code;
    inkloopReuseAccepted_ = true;
    codeOwnership_ = CodeOwnership::MyAiAuthoritativeShared;
  } else if (!inkloopBound_) {
    // Inkloop mirroring is best-effort. The MyAI pairing code and URL are
    // still valid and must be shown immediately; a trusted background retry
    // can promote this state to Shared without rotating the code.
    inkloopCode_.clear();
    inkloopReuseAccepted_ = false;
    codeOwnership_ = CodeOwnership::None;
  } else {
    inkloopCode_.clear();
    codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::retryInkloopCodeReuse(
    IPortalAdapter& adapter, std::string* error) {
  if (inkloopBound_ || inkloopReuseAccepted_ ||
      codeOwnership_ != CodeOwnership::None ||
      !validSixDigitCode(onboardingCode_) || codeExpiresAtSeconds_ == 0) {
    setError("inkloop_code_reuse_not_pending", error);
    return false;
  }
  std::string adapterError;
  if (!adapter.requestInkloopCodeReuse(
          onboardingCode_, codeExpiresAtSeconds_, &adapterError)) {
    if (error) {
      *error = adapterError.empty()
          ? "inkloop_code_reuse_not_queued" : adapterError;
    }
    return false;
  }
  inkloopCode_ = onboardingCode_;
  inkloopReuseAccepted_ = true;
  codeOwnership_ = CodeOwnership::MyAiAuthoritativeShared;
  if (error) error->clear();
  return true;
}

bool OnboardingState::markInkloopBound(std::string* error) {
  if (!validSixDigitCode(inkloopCode_) ||
      codeOwnership_ != CodeOwnership::MyAiAuthoritativeShared ||
      !inkloopReuseAccepted_) {
    setError("shared_onboarding_code_not_ready", error);
    return false;
  }
  inkloopBound_ = true;
  codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
  inkloopCode_.clear();
  if (myAiBindingComplete()) {
    onboardingCode_.clear();
    codeExpiresAtSeconds_ = 0;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::setMyAiActivation(bool active, std::string* error) {
  // A previously Inkloop-bound device may legitimately outlive the transient
  // onboarding-code TTL. A trusted successful MyAI authorization can restore
  // its tutorial/settings stage without resurrecting or exposing that code.
  if (onboardingCode_.empty() && !inkloopBound_) {
    setError("myai_pairing_not_started", error);
    return false;
  }
  myAiActive_ = active;
  if (!active) {
    stage_ = OnboardingStage::MyAiInactive;
    if (inkloopBound_) {
      onboardingCode_.clear();
      inkloopCode_.clear();
      codeExpiresAtSeconds_ = 0;
      codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
    }
    return true;
  }
  stage_ = tutorialComplete()
      ? OnboardingStage::SettingsReady
      : OnboardingStage::VoiceTutorial;
  if (inkloopBound_) {
    onboardingCode_.clear();
    inkloopCode_.clear();
    codeExpiresAtSeconds_ = 0;
    codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::advanceTutorial(std::string* error) {
  if (!myAiActive_) {
    setError("myai_not_active", error);
    return false;
  }
  if (tutorialStep_ != TutorialStep::Complete) {
    tutorialStep_ = static_cast<TutorialStep>(
        static_cast<uint8_t>(tutorialStep_) + 1U);
  }
  if (tutorialStep_ == TutorialStep::Complete) {
    stage_ = OnboardingStage::SettingsReady;
  } else {
    stage_ = OnboardingStage::VoiceTutorial;
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::completeTutorial(std::string* error) {
  if (!myAiActive_) {
    setError("myai_not_active", error);
    return false;
  }
  tutorialStep_ = TutorialStep::Complete;
  stage_ = OnboardingStage::SettingsReady;
  if (error) error->clear();
  return true;
}

bool OnboardingState::restartTutorial(std::string* error) {
  if (!myAiActive_) {
    setError("myai_not_active", error);
    return false;
  }
  tutorialStep_ = TutorialStep::PressToTalk;
  stage_ = OnboardingStage::VoiceTutorial;
  if (error) error->clear();
  return true;
}

bool OnboardingState::expireCodeIfNeeded(uint64_t nowSeconds) {
  if (codeExpiresAtSeconds_ == 0 || nowSeconds < codeExpiresAtSeconds_ || myAiActive_) {
    return false;
  }
  onboardingCode_.clear();
  codeExpiresAtSeconds_ = 0;
  myAiActive_ = false;
  stage_ = wifiConfigured_
      ? OnboardingStage::WifiConfigured
      : OnboardingStage::WifiAccessPoint;
  if (!inkloopBound_) {
    inkloopCode_.clear();
    inkloopReuseAccepted_ = false;
    codeOwnership_ = CodeOwnership::None;
  }
  return true;
}

bool OnboardingState::clearBoundCodeIfNeeded() {
  if (!terminalBindingComplete() ||
      (onboardingCode_.empty() && inkloopCode_.empty() &&
       codeExpiresAtSeconds_ == 0)) {
    return false;
  }
  onboardingCode_.clear();
  inkloopCode_.clear();
  codeExpiresAtSeconds_ = 0;
  codeOwnership_ = CodeOwnership::InkloopBoundHistorical;
  return true;
}

namespace {

bool validStage(OnboardingStage stage) {
  switch (stage) {
    case OnboardingStage::WifiAccessPoint:
    case OnboardingStage::WifiConfigured:
    case OnboardingStage::MyAiPairingRequested:
    case OnboardingStage::AwaitingMyAiActivation:
    case OnboardingStage::MyAiInactive:
    case OnboardingStage::VoiceTutorial:
    case OnboardingStage::SettingsReady:
      return true;
  }
  return false;
}

bool validTutorialStep(TutorialStep step) {
  switch (step) {
    case TutorialStep::PressToTalk:
    case TutorialStep::VoiceLedStates:
    case TutorialStep::GalleryPaging:
    case TutorialStep::DisplayBusyGuard:
    case TutorialStep::LocalPortal:
    case TutorialStep::Complete:
      return true;
  }
  return false;
}

bool validCodeOwnership(CodeOwnership ownership) {
  switch (ownership) {
    case CodeOwnership::None:
    case CodeOwnership::MyAiAuthoritativeShared:
    case CodeOwnership::InkloopBoundHistorical:
      return true;
  }
  return false;
}

void persistedError(const char* message, std::string* error) {
  if (error) *error = message;
}

}  // namespace

OnboardingPersistedState OnboardingState::persistedState() const {
  OnboardingPersistedState state;
  state.stage = stage_;
  state.tutorialStep = tutorialStep_;
  state.wifiConfigured = wifiConfigured_;
  state.myAiActive = myAiActive_;
  state.inkloopBound = inkloopBound_;
  state.inkloopReuseAccepted = inkloopReuseAccepted_;
  state.codeOwnership = codeOwnership_;
  state.onboardingCode = onboardingCode_;
  state.inkloopCode = inkloopCode_;
  state.codeExpiresAtSeconds = codeExpiresAtSeconds_;
  return state;
}

bool OnboardingState::validatePersistedState(
    const OnboardingPersistedState& state,
    std::string* error) {
  if (!validStage(state.stage) || !validTutorialStep(state.tutorialStep) ||
      !validCodeOwnership(state.codeOwnership)) {
    persistedError("snapshot_invalid_enum", error);
    return false;
  }
  if ((!state.onboardingCode.empty() &&
       !validSixDigitCode(state.onboardingCode)) ||
      (!state.inkloopCode.empty() && !validSixDigitCode(state.inkloopCode))) {
    persistedError("snapshot_invalid_code", error);
    return false;
  }
  if (state.onboardingCode.empty() != (state.codeExpiresAtSeconds == 0)) {
    persistedError("snapshot_invalid_code_expiry", error);
    return false;
  }
  if (!state.wifiConfigured && state.stage != OnboardingStage::WifiAccessPoint) {
    persistedError("snapshot_invalid_wifi_stage", error);
    return false;
  }
  if (state.wifiConfigured && state.myAiActive) {
    const OnboardingStage expected = state.tutorialStep == TutorialStep::Complete
        ? OnboardingStage::SettingsReady : OnboardingStage::VoiceTutorial;
    if (state.stage != expected ||
        (state.onboardingCode.empty() && !state.inkloopBound)) {
      persistedError("snapshot_invalid_active_stage", error);
      return false;
    }
  }
  if (state.wifiConfigured && !state.myAiActive &&
      (state.stage == OnboardingStage::VoiceTutorial ||
       state.stage == OnboardingStage::SettingsReady ||
       state.stage == OnboardingStage::WifiAccessPoint)) {
    persistedError("snapshot_invalid_inactive_stage", error);
    return false;
  }
  if (state.stage == OnboardingStage::MyAiPairingRequested &&
      !state.onboardingCode.empty()) {
    persistedError("snapshot_pairing_has_code", error);
    return false;
  }
  if ((state.stage == OnboardingStage::AwaitingMyAiActivation ||
       state.stage == OnboardingStage::MyAiInactive) &&
      state.onboardingCode.empty() && !state.inkloopBound) {
    persistedError("snapshot_stage_missing_code", error);
    return false;
  }
  if (state.codeOwnership == CodeOwnership::None) {
    if (!state.inkloopCode.empty() || state.inkloopReuseAccepted ||
        state.inkloopBound) {
      persistedError("snapshot_invalid_code_owner", error);
      return false;
    }
  } else if (state.codeOwnership == CodeOwnership::MyAiAuthoritativeShared) {
    if (state.inkloopBound || !state.inkloopReuseAccepted ||
        state.onboardingCode.empty() ||
        state.inkloopCode != state.onboardingCode) {
      persistedError("snapshot_invalid_shared_code", error);
      return false;
    }
  } else {
    if (!state.inkloopBound || !state.inkloopReuseAccepted) {
      persistedError("snapshot_invalid_bound_state", error);
      return false;
    }
    const bool myAiComplete = state.myAiActive ||
        state.stage == OnboardingStage::MyAiInactive ||
        state.stage == OnboardingStage::VoiceTutorial ||
        state.stage == OnboardingStage::SettingsReady;
    const bool codeFree = state.onboardingCode.empty() &&
        state.inkloopCode.empty() && state.codeExpiresAtSeconds == 0;
    const bool retainedForMyAi = !myAiComplete &&
        validSixDigitCode(state.onboardingCode) &&
        state.inkloopCode.empty() && state.codeExpiresAtSeconds != 0;
    const bool legacyCodeBearing = myAiComplete &&
        validSixDigitCode(state.onboardingCode) &&
        validSixDigitCode(state.inkloopCode) &&
        state.codeExpiresAtSeconds != 0;
    if (!codeFree && !retainedForMyAi && !legacyCodeBearing) {
      persistedError("snapshot_invalid_bound_code", error);
      return false;
    }
  }
  if (error) error->clear();
  return true;
}

bool OnboardingState::hydrate(
    const OnboardingPersistedState& state,
    std::string* error) {
  if (!validatePersistedState(state, error)) return false;
  stage_ = state.stage;
  tutorialStep_ = state.tutorialStep;
  wifiConfigured_ = state.wifiConfigured;
  myAiActive_ = state.myAiActive;
  inkloopBound_ = state.inkloopBound;
  inkloopReuseAccepted_ = state.inkloopReuseAccepted;
  codeOwnership_ = state.codeOwnership;
  onboardingCode_ = state.onboardingCode;
  inkloopCode_ = state.inkloopCode;
  codeExpiresAtSeconds_ = state.codeExpiresAtSeconds;
  if (error) error->clear();
  return true;
}

std::string OnboardingState::myAiRegistrationUrl() const {
  if (terminalBindingComplete()) return std::string();
  if (!validSixDigitCode(onboardingCode_)) return kMyAiRegistrationBaseUrl;
  return std::string(kMyAiRegistrationBaseUrl) + "?device_code=" +
      urlEncode(onboardingCode_) + "#devices";
}

std::string OnboardingState::toJson() const {
  std::ostringstream output;
  output << "{\"appId\":\"" << kMyAiAppId
         << "\",\"stage\":\"" << onboardingStageName(stage_)
         << "\",\"wifiConfigured\":" << (wifiConfigured_ ? "true" : "false")
         << ",\"myAiActive\":" << (myAiActive_ ? "true" : "false")
         << ",\"inkloopBound\":" << (inkloopBound_ ? "true" : "false")
         << ",\"tutorialStep\":\"" << tutorialStepName(tutorialStep_)
         << "\",\"tutorialComplete\":" << (tutorialComplete() ? "true" : "false")
         << ",\"onboardingCode\":\"" << jsonEscape(onboardingCode_)
         << "\",\"inkloopCode\":\""
         << jsonEscape(inkloopBound_ ? std::string() : inkloopCode_)
         << "\",\"inkloopReuseAccepted\":"
         << (inkloopReuseAccepted_ ? "true" : "false")
         << ",\"codeOwnership\":\""
         << (codeOwnership_ == CodeOwnership::MyAiAuthoritativeShared
                 ? "myai_authoritative_shared"
                 : (codeOwnership_ == CodeOwnership::InkloopBoundHistorical
                        ? "inkloop_bound_historical" : "none"))
         << "\""
         << ",\"codeExpiresAt\":" << codeExpiresAtSeconds_
         << ",\"myAiRegistrationUrl\":\""
         << jsonEscape(myAiRegistrationUrl()) << "\"}";
  return output.str();
}

const char* onboardingStageName(OnboardingStage stage) {
  switch (stage) {
    case OnboardingStage::WifiAccessPoint: return "wifi_ap";
    case OnboardingStage::WifiConfigured: return "wifi_configured";
    case OnboardingStage::MyAiPairingRequested: return "myai_pairing_requested";
    case OnboardingStage::AwaitingMyAiActivation: return "awaiting_myai_activation";
    case OnboardingStage::MyAiInactive: return "myai_inactive";
    case OnboardingStage::VoiceTutorial: return "voice_tutorial";
    case OnboardingStage::SettingsReady: return "settings_ready";
  }
  return "unknown";
}

const char* tutorialStepName(TutorialStep step) {
  switch (step) {
    case TutorialStep::PressToTalk: return "press_to_talk";
    case TutorialStep::VoiceLedStates: return "voice_led_states";
    case TutorialStep::GalleryPaging: return "gallery_paging";
    case TutorialStep::DisplayBusyGuard: return "display_busy_guard";
    case TutorialStep::LocalPortal: return "local_portal";
    case TutorialStep::Complete: return "complete";
  }
  return "unknown";
}

}  // namespace portal
}  // namespace inkloop
