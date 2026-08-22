#include "PaperColorApplicationRuntime.h"

#include "PagePromptPrimitives.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "AppConfig.h"
#include "CompatibilityPrimitives.h"
#include "Diagnostics.h"
#include "MyAiSleepPrimitives.h"

namespace inkloop {
namespace {

constexpr uint32_t kMinimumVoiceReconnectDelayMs = 30000U;
constexpr uint32_t kVoiceConnectHandshakeTimeoutMs = 30000U;
constexpr uint32_t kVoiceTtsCompletionGraceMs = 1500U;
constexpr char kMyAiChatLogDirectory[] = "/inkloop";
constexpr char kMyAiChatLogPath[] = "/inkloop/myai-chat.txt";
constexpr char kMyAiChatPreviousLogPath[] = "/inkloop/myai-chat.prev.txt";
constexpr size_t kMaximumMyAiChatLogBytes = 512U * 1024U;
constexpr size_t kMaximumMyAiChatLogLineBytes = 4096U;

size_t validUtf8SequenceLength(const std::string& value, size_t at) {
  if (at >= value.size()) return 0;
  const uint8_t first = static_cast<uint8_t>(value[at]);
  if (first < 0x80U) return 1;
  size_t length = 0;
  if (first >= 0xC2U && first <= 0xDFU) length = 2;
  else if (first >= 0xE0U && first <= 0xEFU) length = 3;
  else if (first >= 0xF0U && first <= 0xF4U) length = 4;
  else return 0;
  if (length > value.size() - at) return 0;
  for (size_t index = 1; index < length; ++index) {
    const uint8_t continuation = static_cast<uint8_t>(value[at + index]);
    if ((continuation & 0xC0U) != 0x80U) return 0;
  }
  const uint8_t second = static_cast<uint8_t>(value[at + 1]);
  if ((first == 0xE0U && second < 0xA0U) ||
      (first == 0xEDU && second >= 0xA0U) ||
      (first == 0xF0U && second < 0x90U) ||
      (first == 0xF4U && second >= 0x90U)) {
    return 0;
  }
  return length;
}

std::string boundedChatText(
    const std::string& input, size_t maximumBytes, bool trim = true) {
  std::string output;
  output.reserve(std::min(input.size(), maximumBytes));
  size_t at = 0;
  while (at < input.size()) {
    const size_t length = validUtf8SequenceLength(input, at);
    if (length == 0) {
      ++at;
      continue;
    }
    const uint8_t first = static_cast<uint8_t>(input[at]);
    if (first < 0x20U && first != '\n' && first != '\t') {
      if (output.size() < maximumBytes) output.push_back(' ');
      ++at;
      continue;
    }
    if (length > maximumBytes - output.size()) break;
    output.append(input, at, length);
    at += length;
  }
  if (!trim) return output;
  size_t first = 0;
  while (first < output.size() &&
         (output[first] == ' ' || output[first] == '\n' ||
          output[first] == '\t')) {
    ++first;
  }
  size_t last = output.size();
  while (last > first &&
         (output[last - 1] == ' ' || output[last - 1] == '\n' ||
          output[last - 1] == '\t')) {
    --last;
  }
  return output.substr(first, last - first);
}

bool isBlankAudioChatArtifact(const std::string& input) {
  std::string normalized = boundedChatText(input, 64);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   if (ch >= 'A' && ch <= 'Z')
                     return static_cast<char>(ch + ('a' - 'A'));
                   if (ch == ' ' || ch == '-') return '_';
                   return static_cast<char>(ch);
                 });
  if (normalized.size() >= 2 && normalized.front() == '[' &&
      normalized.back() == ']') {
    normalized = normalized.substr(1, normalized.size() - 2);
  }
  return normalized == "blank_audio";
}

uint32_t boundedVoiceReconnectDelay(uint32_t suggested) {
  return std::max<uint32_t>(suggested, kMinimumVoiceReconnectDelayMs);
}

void secureClear(std::string& value) {
  value.assign(value.size(), '\0');
  value.clear();
}

myai::ClientConfig makeMyAiConfig() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  char fingerprint[64] = {};
  snprintf(fingerprint, sizeof(fingerprint),
           "papercolor-c151-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  myai::ClientConfig config;
  config.installationFingerprint = fingerprint;
  char macAddress[18] = {};
  // Center stores the ESP32 eFuse identity in the public API's established
  // display order (for C151: 28:84:85:43:DA:0C). Keep the durable local
  // fingerprint in native eFuse order, but reverse the MyAI wire value so all
  // runtime calls address the already-bound device record.
  snprintf(macAddress, sizeof(macAddress),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  config.macAddress = macAddress;
  config.deviceLabel = "Inkloop M5 PaperColor";
  config.clientRegion = "cn";
  config.systemPrompt =
      "你是 Inkloop PaperColor 的语音助手。生成图片时使用当前 400x600 "
      "竖向六色电子纸画布（设备底边朝下），并优先输出高对比度、少渐变、"
      "清晰轮廓的素材。";
  return config;
}

voice::Status voiceFailure(const char* code, const std::string& detail) {
  return voice::Status::error(code, detail);
}

const char* portalRenderStrategyId(portal::RefreshMode mode) {
  switch (mode) {
    case portal::RefreshMode::OfficialQuality: return "official-quality";
    case portal::RefreshMode::ExperimentalSixColor: return "classic-six-color";
    case portal::RefreshMode::ReflectancePhoto: return "reflectance-photo";
    case portal::RefreshMode::SolidClean: return "solid-clean";
  }
  return "official-quality";
}

bool stableAssetId(const std::string& value) {
  if (value.size() != 64) return false;
  for (char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) return false;
  }
  return true;
}

bool composeImagePrompt(
    const std::string& promptTemplate,
    const std::string& subject,
    std::string* output) {
  if (!output || promptTemplate.empty() || promptTemplate.size() > 512 ||
      subject.empty() || subject.size() > 1024) return false;
  static const std::string marker = "{prompt}";
  const size_t at = promptTemplate.find(marker);
  output->clear();
  if (at == std::string::npos) {
    if (promptTemplate.size() + 1U + subject.size() > 1024U) return false;
    *output = promptTemplate + " " + subject;
  } else {
    if (promptTemplate.size() - marker.size() + subject.size() > 1024U)
      return false;
    output->assign(promptTemplate, 0, at);
    output->append(subject);
    output->append(promptTemplate, at + marker.size(), std::string::npos);
  }
  return !output->empty() && output->size() <= 1024U;
}

std::string sanitizedUploadTitle(const std::string& untrusted) {
  std::string title;
  title.reserve(portal::kMaximumAlbumUploadTitleBytes);
  for (size_t index = 0; index < untrusted.size() &&
       title.size() < portal::kMaximumAlbumUploadTitleBytes; ++index) {
    const unsigned char ch = static_cast<unsigned char>(untrusted[index]);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_' ||
        ch == '.') {
      title.push_back(static_cast<char>(ch));
    } else if (!title.empty() && title.back() != '_') {
      title.push_back('_');
    }
  }
  while (!title.empty() && (title.back() == ' ' || title.back() == '.'))
    title.pop_back();
  if (title.empty() || title == "." || title == "..") title = "Uploaded image";
  return title;
}

const char* activationName(myai::ActivationState state) {
  switch (state) {
    case myai::ActivationState::Unconfigured: return "unconfigured";
    case myai::ActivationState::Pairing: return "pairing";
    case myai::ActivationState::Bound: return "bound";
    case myai::ActivationState::PaymentRequired: return "inactive";
    case myai::ActivationState::RecoveryRequired: return "recovery";
    case myai::ActivationState::Offline: return "offline";
    case myai::ActivationState::Error: return "error";
  }
  return "unknown";
}

String sleepOutcomeDetail(
    const displaypower::PrepareSleepOutcome& outcome) {
  String detail(displaypower::prepareSleepResultName(outcome.result));
  if (outcome.result == displaypower::PrepareSleepResult::SnapshotFailed) {
    detail += ":";
    detail += displaypower::powerSnapshotPhaseName(outcome.snapshotPhase);
    detail += ":";
    detail += displaypower::powerSnapshotResultName(outcome.snapshotResult);
  } else if (outcome.result == displaypower::PrepareSleepResult::NotEligible ||
             outcome.result ==
                 displaypower::PrepareSleepResult::RecheckNotEligible) {
    detail += ":";
    detail += displaypower::sleepDecisionReasonName(outcome.decisionReason);
  } else if (outcome.result ==
             displaypower::PrepareSleepResult::DeepSleepRejected) {
    detail += ":";
    detail += displaypower::deepSleepExecutionResultName(outcome.deepSleepResult);
  }
  return detail;
}

constexpr char kPairingDisplayNamespace[] = "ink-pair-ui";
// v3 invalidates the earlier markers once. Older firmware could paint SERVICE
// UNAVAILABLE without first recording that the panel contained a transient
// onboarding frame, leaving that stale e-paper image after authorization.
// v4 invalidates an older optimistic marker which could say "scrubbed" even
// while a service-unavailable frame was still physically retained on e-paper.
constexpr char kPairingDisplayScrubbed[] = "scrubbed-v4";
}  // namespace

PaperColorApplicationRuntime::PaperColorApplicationRuntime(
    StorageManager& storage, SdStorage& sd, AlbumStore& album, TaskStore& tasks,
    InkloopClient& inkloopClient, DisplayController& display,
    DisplayTransaction& displayTransaction, LedStatusController& leds,
    SettingsStore& settings, ButtonRouter& buttons,
    QueueAlbumPageHook queuePage, void* queuePageContext)
    : storage_(storage),
      sd_(sd),
      album_(album),
      tasks_(tasks),
      inkloopClient_(inkloopClient),
      display_(display),
      displayTransaction_(displayTransaction),
      leds_(leds),
      settingsStore_(settings),
      buttons_(buttons),
      queuePage_(queuePage),
      queuePageContext_(queuePageContext),
      imageSink_(album),
      myAiConfig_(makeMyAiConfig()),
      myAi_(myAiConfig_, http_, websocket_, aigcOutput_, endpointSecurity_,
            credentials_, codec_, clock_, streamingAudio_, *this, *this),
      voiceLed_(leds),
      conversation_(myAi_, streamingAudio_),
      prompts_(clock_, displayActivity_, promptPlayer_),
      voice_(voice::VoiceRuntimeConfig(), clock_, conversation_, voiceLed_,
             displayActivity_, *this, prompts_, *this),
      portal_(*this),
      displayAdapter_(display),
      imageLed_(leds),
      sleepHooks_(imageLed_, capturePowerHook, finalizePowerHook,
                  stopAudioPowerHook, closeNetworkPowerHook, this),
      wakeHooks_(buttons_, reconnectWifiPowerHook, syncInkloopPowerHook, this),
      wakeRecovery_(wakeHooks_, displaypower::WakeRecoveryConfig()) {
  appliedSystemPrompt_ = myAiConfig_.systemPrompt;
  streamingAudio_.setEndedCallback([this]() {
    if (tutorialNarrationInFlight_) {
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Off);
    } else if (voiceResponseDonePending_) {
      voiceResponseDonePending_ = false;
      voiceTtsSegmentEndedAt_ = 0;
      voice_.onTtsStop(voice_.activeTurnGeneration());
    } else if (voice_.state() == voice::RuntimeState::Speaking) {
      // Preserve a short gap for the next tts.start segment. If the gateway
      // omits the final response.done, loop() will converge both client and
      // VoiceRuntime to Ready after this grace period.
      voiceTtsSegmentEndedAt_ = millis();
    }
  });
}

bool PaperColorApplicationRuntime::begin(bool wifiConfigured) {
  if (initialized_) return true;
  // Hydrate the protected MyAI identity first. Portal storage may only create
  // a fresh snapshot when that independent store proves this is unconfigured.
  const myai::Status initialized = myAi_.initialize();
  if (!initialized.ok()) {
    Diagnostics::event("MYAI_INIT", initialized.detail.c_str());
    if (initialized.code == myai::ErrorCode::Storage ||
        initialized.code == myai::ErrorCode::RecoveryRequired) return false;
  }
  const myai::ActivationState activation = myAi_.activationState();
  const bool hasDurableInkloopIdentity =
      inkloopClient_.deviceId().length() != 0;
  portalMyAiState_ = activation == myai::ActivationState::Unconfigured &&
          hasDurableInkloopIdentity
      ? "credential_recovery"
      : activationName(activation);
  const PortalIdentityState portalIdentity =
      activation == myai::ActivationState::Unconfigured
          ? (hasDurableInkloopIdentity
                 ? PortalIdentityState::BoundRecovery
                 : PortalIdentityState::Unconfigured)
          : (activation == myai::ActivationState::Pairing
                 ? PortalIdentityState::Pairing
                 : (activation == myai::ActivationState::PaymentRequired
                        ? PortalIdentityState::BoundInactive
                        : PortalIdentityState::BoundActive));
  const String stationPassword = WiFi.psk();
  if (!portal_.begin(
          portalIdentity,
          std::string(stationPassword.c_str(), stationPassword.length()))) {
    Diagnostics::event("ERROR", "PAPERCOLOR_PORTAL_START_FAILED");
    return false;
  }
  const String hardware = inkloopClient_.hardwareId();
  const String suffix = hardware.length() >= 4
      ? hardware.substring(hardware.length() - 4) : String("C151");
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    settingsAccessPoint_ = WiFi.SSID();
    settingsAccessPointActive_ = false;
    Diagnostics::event("SETTINGS_AP", "DISABLED_STATION_CONNECTED");
  } else {
    settingsAccessPoint_ = "Inkloop-" + suffix + "-Settings";
    WiFi.mode(WIFI_AP_STA);
    settingsAccessPointActive_ = WiFi.softAP(
        settingsAccessPoint_.c_str(), portal_.accessCode().c_str());
    if (!settingsAccessPointActive_) {
      Diagnostics::event("WARN", "SETTINGS_AP_UNAVAILABLE_USE_MDNS");
    } else {
      Diagnostics::event("SETTINGS_AP", settingsAccessPoint_);
      Diagnostics::event("SETTINGS_AP_URL", WiFi.softAPIP().toString());
    }
  }
  std::string portalError;
  if (!portal_.onWifiConfigured(wifiConfigured, &portalError)) {
    // A restored snapshot may already be in the same state; only fail if the
    // portal did not hydrate into a usable state.
    if (!portal_.ready()) return false;
  }
  applyPortalSettings(portal_.settings());
  if (!loadAlbumRevision()) return false;
  if (!loadMyAiChatHistoryFromStorage()) {
    Diagnostics::event("WARN", "MYAI_CHAT_HISTORY_LOAD_FAILED");
  }

  myAiAuthorized_ = activation == myai::ActivationState::Bound;
  if (myAiAuthorized_) {
    bool authorized = false;
    const myai::Status checked = myAi_.checkAuthorization(authorized);
    myAiAuthorized_ = checked.ok() && authorized;
    if (myAiAuthorized_) {
      const portal::OnboardingState* restored = portal_.onboarding();
      if (restored && !restored->myAiActive()) {
        std::string activationError;
        if (!portal_.onMyAiActivation(true, &activationError)) return false;
      }
      const portal::OnboardingState* onboarding = portal_.onboarding();
      tutorialNarrationPending_ = onboarding && !onboarding->tutorialComplete();
      // Do not nest the multi-request gateway login beneath boot
      // initialization; ESP32's loopTask stack is too small for that call
      // chain. The shallow loop-level scheduler connects after begin returns.
      voiceConnectPending_ = false;
      voiceReconnectAt_ = millis();
    }
  } else if (activation == myai::ActivationState::PaymentRequired) {
    const portal::OnboardingState* restored = portal_.onboarding();
    if (restored && restored->myAiActive()) {
      std::string activationError;
      if (!portal_.onMyAiActivation(false, &activationError)) return false;
    }
  } else if (activation == myai::ActivationState::Pairing) {
    // A power loss after MyAI durably accepted a candidate must not strand the
    // user with a hidden code. Recover only the public view and re-run the
    // idempotent Inkloop mirror before polling activation again.
    myai::PairingView resumed;
    const myai::Status pending = myAi_.pendingPairing(resumed);
    std::string error;
    const portal::OnboardingState* onboarding = portal_.onboarding();
    const bool portalAlreadyPending = onboarding &&
        (onboarding->stage() == portal::OnboardingStage::MyAiPairingRequested ||
         onboarding->stage() == portal::OnboardingStage::AwaitingMyAiActivation);
    if (!pending.ok() ||
        (!portalAlreadyPending && !portal_.onMyAiPairingResumed(&error))) {
      Diagnostics::event(
          "ERROR", String("MYAI_PAIRING_RESUME:") +
              (pending.ok() ? error.c_str() : pending.detail.c_str()));
      return false;
    }
    pendingPairing_ = resumed;
    pairingCallbackPending_ = true;
  } else if (activation == myai::ActivationState::Unconfigured &&
             !hasDurableInkloopIdentity &&
             wifiConfigured) {
    std::string error;
    if (!portal_.requestMyAiPairing(&error)) {
      // Keep the local portal and diagnostics available for an explicit retry
      // when Center is temporarily unreachable.
      Diagnostics::event("WARN", String("MYAI_AUTO_PAIRING:") + error.c_str());
    }
  }
  // A 401 clear may have completed immediately before a reset or power loss.
  // The durable Inkloop identity plus an empty MyAI credential is the stable
  // recovery marker; resume the fresh six-digit pairing from loop() instead
  // of stranding the device in a contradictory "bound but unavailable" UI.
  if (activation == myai::ActivationState::Unconfigured &&
      hasDurableInkloopIdentity && wifiConfigured) {
    myAiRebindPending_ = true;
    myAiRebindRetryAt_ = millis();
    Diagnostics::event("MYAI_REBIND", "RECOVERY_PENDING");
  }
  voice_.setEnabled(myAiAuthorized_);
  reconcileTerminalBindingState();
  activityState_.noteMeaningfulActivity(millis());
  wakeReason_ = Esp32DevicePowerAdapter::detectedWakeReason();
  wakeAnnouncementPending_ =
      wakeReason_ == displaypower::WakeReason::TopButton ||
      wakeReason_ == displaypower::WakeReason::PreviousButton ||
      wakeReason_ == displaypower::WakeReason::NextButton ||
      wakeReason_ == displaypower::WakeReason::MultipleButtons;
  if (!wakeRecovery_.beginAfterHardwareReady(wakeReason_)) {
    Diagnostics::event("ERROR", "WAKE_RECOVERY_START_FAILED");
    return false;
  }
  initialized_ = true;
  Diagnostics::event("PAPERCOLOR_RUNTIME", "READY");
  return true;
}

bool PaperColorApplicationRuntime::recoverPortalBoundState(
    std::string* error) {
  const myai::ActivationState activation = myAi_.activationState();
  if ((activation != myai::ActivationState::Bound &&
       activation != myai::ActivationState::PaymentRequired) ||
      !inkloopClient_.deviceId().length()) {
    if (error) *error = "bound_myai_and_inkloop_identity_required";
    return false;
  }
  // A stored Inkloop device id exists before account binding, so verify the
  // server-side bound state before reconstructing a code-free Portal snapshot.
  if (!verifyInkloopBinding(error)) return false;
  return portal_.recoverBoundSnapshot(
      activation == myai::ActivationState::Bound, error);
}

bool PaperColorApplicationRuntime::activateDisplayOwner() {
  if (displayRuntime_) return displayRuntime_->enabled();
  if (!displayLock_.begin() || !imageLed_.beginAnimation()) return false;
  displaypower::DisplayRefreshRuntimeConfig config;
  config.enabled = true;
  // PaperColor owns the complete full-screen writer and all pre-quantized
  // strategies still go through the same ED2208 full-refresh capability.
  config.experimentalPrequantizationEnabled = true;
  config.cooldownMilliseconds = 30000;
  config.maximumEncodedPngBytes = kMaxFrameBytes;
  displayRuntime_.reset(new (std::nothrow) displaypower::DisplayRefreshRuntime(
      displayAdapter_, imageLed_, decoder_, displayLock_, displayClock_, config));
  return displayRuntime_ && displayRuntime_->enabled();
}

void PaperColorApplicationRuntime::loop() {
  if (!initialized_) return;
  // Runtime priority is deliberate: voice transport/capture/playback first,
  // then non-blocking LED feedback, and only then the adaptive local Portal.
  // Physical buttons are sampled by the higher-priority Core-1 input task and
  // dispatched by main before this method is entered.
  streamingAudio_.poll();
  if (!streamingAudio_.receiveBackpressured()) websocket_.loop();
  conversation_.pollCapture();
  voice_.tick();
  if (volumePreviewActive_ && !promptPlayer_.busy()) {
    volumePreviewActive_ = false;
    const uint8_t persistedVolume = portal_.settings().volume;
    promptPlayer_.setVolume(persistedVolume);
    M5.Speaker.setVolume(static_cast<uint8_t>(persistedVolume * 255U / 100U));
    Diagnostics::event("AUDIO_PREVIEW", "COMPLETE");
  }
  pollAssistancePromptQueue();
  leds_.pollPixelDiagnostic(millis());
  if (ledDiagnosticPending_ && !leds_.pixelDiagnosticActive() &&
      !ledDiagnosticWorkBusy()) {
    std::string diagnosticError;
    if (!startPendingLedDiagnostic(&diagnosticError)) {
      Diagnostics::event(
          "ERROR", String("LED_ROLE_DIAGNOSTIC_START:") +
              diagnosticError.c_str());
    }
  }
  // A diagnostic is only a status animation. It never owns the control loop
  // and therefore cannot pause a voice turn or physical input handling.
  if (leds_.pixelDiagnosticActive()) {
    activityState_.noteMeaningfulActivity(millis());
  }
  if (displayRuntime_) displayRuntime_->tickImageLed();
  portal_.loop();
  const uint32_t wakeNow = millis();
  wakeRecovery_.poll(wakeNow);
  if (wakeAnnouncementPending_ && wakeRecovery_.state().readyForUserInput()) {
    // The wake gesture is already consumed by WakeRecoveryRuntime's
    // release/debounce/re-arm barrier. A brief LED and optional local prompt
    // acknowledge recovery without touching the e-paper contents.
    leds_.setRoleState(LedRole::Voice, LedState::Complete, 52);
    wakeIndicatorOffAt_ = wakeNow + 1200U;
    if (!portal_.settings().voiceAssistanceEnabled ||
        enqueueAssistancePrompt("device.restored")) {
      wakeAnnouncementPending_ = false;
      Diagnostics::event("WAKE_ASSISTANCE", "RESTORED");
    }
  }
  if (wakeIndicatorOffAt_ &&
      static_cast<int32_t>(wakeNow - wakeIndicatorOffAt_) >= 0) {
    leds_.setRoleState(LedRole::Voice, LedState::Off, 40);
    wakeIndicatorOffAt_ = 0;
  }

  const uint32_t loopNow = millis();
  if (pairingCallbackPending_ &&
      static_cast<int32_t>(loopNow - pairingCallbackRetryAt_) >= 0) {
    pairingCallbackPending_ = false;
    const uint64_t expires = parseIsoEpoch(pendingPairing_.expiresAt);
    std::string error;
    if (!expires || !portal_.onAuthoritativeMyAiCode(
                        pendingPairing_.onboardingCode, expires, &error)) {
      Diagnostics::event("ERROR", String("PAIRING_CODE_REUSE:") + error.c_str());
      // MyAI owns this candidate. A temporary Inkloop or Portal failure must
      // not delete the only authoritative code; retry the same credential.
      pairingCallbackPending_ = true;
      pairingCallbackRetryAt_ = loopNow + 5000U;
      pairingPollActive_ = true;
    } else {
      pairingPollActive_ = true;
      lastPairingPollAt_ = millis();
      const portal::OnboardingState* accepted = portal_.onboarding();
      if (accepted && !accepted->inkloopBound() &&
          !accepted->inkloopReuseAccepted()) {
        inkloopMirrorRetryAt_ = loopNow + 15000U;
        Diagnostics::event("INKLOOP_CODE_REUSE", "DEFERRED");
      } else {
        inkloopMirrorRetryAt_ = 0;
      }
      Diagnostics::event("MYAI_CODE", pendingPairing_.onboardingCode.c_str());
      // Preserve the flasher's existing post-write binding prompt contract;
      // this is the same authoritative MyAI code, never a second Inkloop code.
      Diagnostics::event("PAIR_CODE", pendingPairing_.onboardingCode.c_str());
      Diagnostics::event("MYAI_BIND_URL", pendingPairing_.bindingUrl.c_str());
      GeneratedStatusPng pairingScreen;
      // A QR is allowed only for the exact authoritative MyAI binding URL and
      // six-digit code. Never replace an invalid/missing service response with
      // a local Wi-Fi QR. Persist "secret may be visible" before drawing it.
      const bool validPairing = validPairingStatusInputs(
          pendingPairing_.onboardingCode, pendingPairing_.bindingUrl);
      const bool pairingShown = validPairing &&
          storePairingScreenScrubbed(false) &&
          makePairingStatusPng(
              pendingPairing_.onboardingCode,
              pendingPairing_.bindingUrl,
              pairingScreen) &&
          refreshFrame(
              "myai-pairing", pairingScreen.bytes, pairingScreen.length,
              true);
      if (!pairingShown) {
        GeneratedStatusPng unavailable;
        // Never paint a transient failure page unless its durable scrub marker
        // was cleared first. E-paper retains the last frame across resets.
        if (!storePairingScreenScrubbed(false) ||
            !makePairingUnavailableStatusPng(unavailable) ||
            !refreshFrame(
                "myai-service-unavailable", unavailable.bytes,
                unavailable.length, true)) {
          Diagnostics::event("ERROR", "MYAI_UNAVAILABLE_SCREEN_FAILED");
        } else {
          Diagnostics::event("WARN", "MYAI_BINDING_SERVICE_UNAVAILABLE");
        }
      }
    }
  }

  const portal::OnboardingState* mirrorState = portal_.onboarding();
  const bool mirrorPending = mirrorState && !mirrorState->inkloopBound() &&
      !mirrorState->inkloopReuseAccepted() &&
      portal::OnboardingState::validSixDigitCode(
          mirrorState->onboardingCode());
  if (mirrorPending && inkloopMirrorRetryAt_ == 0) {
    inkloopMirrorRetryAt_ = loopNow;
  }
  if (mirrorPending &&
      static_cast<int32_t>(loopNow - inkloopMirrorRetryAt_) >= 0) {
    std::string mirrorError;
    if (portal_.retryInkloopCodeReuse(&mirrorError)) {
      inkloopMirrorRetryAt_ = 0;
      Diagnostics::event("INKLOOP_CODE_REUSE", "RETRY_ACCEPTED");
    } else {
      inkloopMirrorRetryAt_ = loopNow + 30000U;
      Diagnostics::event(
          "WARN", String("INKLOOP_CODE_REUSE_DEFERRED:") +
              mirrorError.c_str());
    }
  }
  pollPairing();
  pollAigc();
  tryTerminalDisplayScrub();

  const uint32_t now = millis();
  if (myAiRebindPending_ && !pairingPollActive_ &&
      !pairingCallbackPending_ && !mutationBusy() &&
      WiFi.status() == WL_CONNECTED &&
      static_cast<int32_t>(now - myAiRebindRetryAt_) >= 0) {
    std::string rebindError;
    const portal::OnboardingState* onboarding = portal_.onboarding();
    const bool terminalBinding =
        onboarding && onboarding->terminalBindingComplete();
    const bool started = terminalBinding
        ? portal_.requestMyAiRebind(&rebindError)
        : portal_.requestMyAiPairing(&rebindError);
    if (started) {
      myAiRebindPending_ = false;
      portalMyAiState_ = "pairing";
      Diagnostics::event("MYAI_REBIND", "AUTO_STARTED");
    } else {
      myAiRebindRetryAt_ = now + 30000U;
      Diagnostics::event(
          "WARN", String("MYAI_REBIND_DEFERRED:") + rebindError.c_str());
    }
  }
  if (!myAiAuthorized_ && !pairingPollActive_ &&
      (myAi_.activationState() == myai::ActivationState::Offline ||
       myAi_.activationState() == myai::ActivationState::PaymentRequired ||
       myAi_.activationState() == myai::ActivationState::RecoveryRequired) &&
      static_cast<int32_t>(now - authorizationRetryAt_) >= 0) {
    authorizationRetryAt_ = now + 30000U;
    bool authorized = false;
    const myai::Status checked = myAi_.checkAuthorization(authorized);
    if (checked.ok() && authorized) {
      myAiAuthorized_ = true;
      std::string error;
      if (!portal_.onMyAiActivation(true, &error)) {
        Diagnostics::event("ERROR", String("MYAI_PORTAL_ACTIVATION:") +
            error.c_str());
      } else {
        reconcileTerminalBindingState();
      }
      const portal::OnboardingState* onboarding = portal_.onboarding();
      tutorialNarrationPending_ = onboarding && !onboarding->tutorialComplete();
      voice_.setEnabled(true);
      voiceConnectPending_ = false;
      voiceReconnectAt_ = now;
    }
  }

  if (!pendingSystemPrompt_.empty() && !voice_.turnActive() &&
      !voice_.captureActive() && !streamingAudio_.active()) {
    const std::string next = pendingSystemPrompt_;
    const voice::Status applied = applyAssistantPromptRuntime(next);
    if (applied.success) pendingSystemPrompt_.clear();
    else Diagnostics::event("PROMPT_SETTING", applied.detail.c_str());
  }

  if (inkloopClient_.paired()) {
    const portal::OnboardingState* state = portal_.onboarding();
    if (state && !state->inkloopBound()) {
      std::string error;
      if (!portal_.onInkloopBound(&error))
        Diagnostics::event("ERROR", String("BOUND_CODE_SCRUB:") + error.c_str());
      else
        reconcileTerminalBindingState();
    }
  }

  // The public MyAI client contract and reference client use a 30-second
  // lease heartbeat. Do not lengthen that lease implicitly; defer it while a
  // user turn, capture or TTS is active, then send as soon as the session is
  // quiescent. ResponsiveWorkExecutor keeps buttons/Portal alive meanwhile.
  if (myAiAuthorized_ && voiceWasReady_ &&
      !voice_.turnActive() && !voice_.captureActive() &&
      !streamingAudio_.active() && now - lastHeartbeatAt_ >= 30000U) {
    lastHeartbeatAt_ = now;
    const myai::Status heartbeat = myAi_.heartbeatVoice();
    if (!heartbeat.ok()) {
      myAi_.disconnectVoice("heartbeat_failed");
      if (tutorialNarrationInFlight_) {
        tutorialNarrationInFlight_ = false;
        tutorialNarrationPending_ = true;
      }
      voiceWasReady_ = false;
      voiceConnectPending_ = false;
      voice_.onSessionLost(voiceFailure(
          "myai_heartbeat_failed", heartbeat.detail));
      voiceReconnectAt_ = now +
          boundedVoiceReconnectDelay(heartbeat.retryAfterMs);
    }
  }
  if (voiceTtsSegmentEndedAt_ != 0 &&
      static_cast<uint32_t>(now - voiceTtsSegmentEndedAt_) >=
          kVoiceTtsCompletionGraceMs &&
      !streamingAudio_.active() &&
      voice_.state() == voice::RuntimeState::Speaking) {
    voiceTtsSegmentEndedAt_ = 0;
    const myai::Status completed = myAi_.completeVoiceResponseAfterTtsStop();
    Diagnostics::event(
        "VOICE_TTS_COMPLETE",
        completed.ok() ? "GRACE_FALLBACK" : completed.detail.c_str());
  }
  if (myAiAuthorized_ && !voiceWasReady_ && voiceConnectPending_ &&
      static_cast<int32_t>(now - voiceConnectDeadline_) >= 0) {
    voiceConnectPending_ = false;
    myAi_.disconnectVoice("connect_timeout");
    voiceReconnectAt_ = now + kMinimumVoiceReconnectDelayMs;
    Diagnostics::event("VOICE_CONNECT", "HANDSHAKE_TIMEOUT");
  }
  if (acceptsUserInput()) {
    const displaypower::SleepAttemptObservation sleep = sleepAttempt_.poll(
        millis(), powerPolicy_, sleepHooks_, powerAdapter_);
    if (sleep.transition) {
      Diagnostics::event("SLEEP_STATE", sleepOutcomeDetail(sleep.outcome));
    } else if (sleep.summary) {
      String detail = sleepOutcomeDetail(sleep.outcome);
      detail += ":suppressed=";
      detail += String(static_cast<unsigned long>(sleep.suppressedAttempts));
      Diagnostics::event("SLEEP_SUMMARY", detail);
    }
  }
}

void PaperColorApplicationRuntime::pumpResponsiveUi(
    ResponsiveWorkKind kind) {
  if (!initialized_) return;
  const uint32_t now = millis();
  // Same priority contract as loop(): voice/audio before LEDs, Portal last.
  streamingAudio_.poll();
  pollAssistancePromptQueue();
  // Inkloop HTTP does not touch MyAI. Keep an established voice socket and
  // microphone flowing during a slow task sync. MyAI HTTP/handshake work keeps
  // its client non-reentrant while still servicing the rest of the device UI.
  if (kind == ResponsiveWorkKind::InkloopNetwork ||
      kind == ResponsiveWorkKind::DisplayHardware) {
    if (!streamingAudio_.receiveBackpressured()) websocket_.loop();
    conversation_.pollCapture();
    voice_.tick();
  }
  leds_.pollPixelDiagnostic(now);
  if (displayRuntime_) displayRuntime_->tickImageLed();
  // Storage transactions are isolated from all Portal album reads. A file
  // response already owns requestActive_, so PortalTransfer is also inert.
  if (kind != ResponsiveWorkKind::StorageHardware) portal_.loop();
}

bool PaperColorApplicationRuntime::handleButton(ButtonEvent event) {
  if (!initialized_ || !acceptsUserInput()) return false;
  if (album_.userUploadActive()) {
    Diagnostics::event("BUTTON", "IGNORED_DURING_ALBUM_UPLOAD");
    return false;
  }
  activityState_.noteMeaningfulActivity(millis());
  if (event != ButtonEvent::Voice) return false;
  if (tutorialNarrationInFlight_) {
    Diagnostics::event("VOICE_BUTTON", "TUTORIAL_PLAYING");
    return true;
  }
  std::string portalError;
  const bool confirmedPortal = portal_.confirmPhysical(&portalError);
  if (confirmedPortal) {
    Diagnostics::event("PORTAL_CONFIRM", "PHYSICAL_ACCEPTED");
    return true;
  }
  const voice::RuntimeState priorVoiceState = voice_.state();
  if (myAiAuthorized_ && !voiceWasReady_) {
    if (voiceConnectPending_) {
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Thinking);
      Diagnostics::event("VOICE_BUTTON", "SESSION_CONNECTING");
      return true;
    }
    if (static_cast<int32_t>(millis() - voiceReconnectAt_) < 0) {
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
      Diagnostics::event("VOICE_BUTTON", "RECONNECT_BACKOFF");
      return true;
    }
    // A physical top-button tap always means "start a conversation".  When
    // the voice socket is cold, remember the tap and enter Listening as soon
    // as session.ready arrives; do not replace the user's action with the
    // optional introduction/tutorial narration.
    listenAfterConnect_ = true;
    if (!connectVoiceIfAuthorized()) {
      listenAfterConnect_ = false;
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
      Diagnostics::event("VOICE_BUTTON", "SESSION_CONNECT_FAILED");
    } else {
      Diagnostics::event("VOICE_BUTTON", "SESSION_CONNECT_STARTED");
    }
    return true;
  }
  const bool cancellingTurn =
      priorVoiceState == voice::RuntimeState::Thinking ||
      priorVoiceState == voice::RuntimeState::Speaking;
  const voice::Status status = voice_.onTopButtonTap();
  if (shouldRecoverClosedVoiceAfterCancel(
          cancellingTurn, status.success,
          myAi_.voiceState() == myai::VoiceState::Idle)) {
    // The concrete MyAI adapter closes the socket/lease because the public
    // gateway contract has no per-response cancel message. VoiceRuntime has
    // already invalidated its readiness before publishing its post-cancel
    // state; this layer only updates the concrete reconnect scheduler.
    voiceWasReady_ = false;
    voiceConnectPending_ = false;
    voiceConnectDeadline_ = 0;
    voiceReconnectAt_ = millis();
    Diagnostics::event("VOICE_CANCEL", "SESSION_CLOSED_RECONNECTING");
  }
  if (!status.success) {
    voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
    Diagnostics::event("VOICE_BUTTON", status.code.c_str());
  }
  return true;
}

bool PaperColorApplicationRuntime::requestVoiceIntroduction() {
  if (!initialized_ || !acceptsUserInput() || !myAiAuthorized_) return false;
  if (tutorialNarrationInFlight_ || voice_.turnActive()) return false;
  listenAfterConnect_ = false;
  introductionPending_ = true;
  if (voiceWasReady_ && !voiceConnectPending_) {
    onVoiceState(myai::VoiceState::Ready);
    return !introductionPending_;
  }
  if (voiceConnectPending_) return true;
  if (static_cast<int32_t>(millis() - voiceReconnectAt_) < 0) {
    introductionPending_ = false;
    return false;
  }
  if (!connectVoiceIfAuthorized()) {
    introductionPending_ = false;
    return false;
  }
  return true;
}

bool PaperColorApplicationRuntime::refreshFrame(
    const String& assetId, const uint8_t* bytes, size_t length, bool,
    const String& requestedRenderStrategy) {
  if (!displayRuntime_ || !bytes || !length || album_.userUploadActive())
    return false;
  displaypower::EncodedFrameRequest request;
  request.assetId = assetId.c_str();
  request.bytes = bytes;
  request.length = length;
  displaypower::RenderStrategy selected = displaypower::RenderStrategy::OfficialQuality;
  const char* requested = requestedRenderStrategy.length()
      ? requestedRenderStrategy.c_str()
      : portalRenderStrategyId(portal_.settings().refreshMode);
  if (!displaypower::parseRenderStrategyId(requested, &selected)) {
    Diagnostics::event("DISPLAY_REFRESH_POLICY", "INVALID_STRATEGY_FALLBACK");
    selected = displaypower::RenderStrategy::OfficialQuality;
  }
  request.strategy = selected;
  Diagnostics::event(
      "DISPLAY_RENDER_STRATEGY", displaypower::renderStrategyId(selected));
  const uint32_t generation = displayActivity_.beginRefresh();
  activityState_.noteMeaningfulActivity(millis());
  struct DisplayWork {
    displaypower::DisplayRefreshRuntime* runtime;
    const displaypower::EncodedFrameRequest* request;
    displaypower::DisplayRefreshResult result;
  } displayWork{
      displayRuntime_.get(), &request,
      displaypower::DisplayRefreshResult::DisplayFailed};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::DisplayHardware,
      [](void* raw) {
        DisplayWork* work = static_cast<DisplayWork*>(raw);
        work->result = work->runtime->refresh(*work->request);
      },
      &displayWork);
  const displaypower::DisplayRefreshResult result = dispatched
      ? displayWork.result
      : displaypower::DisplayRefreshResult::Busy;
  displayActivity_.endRefresh(generation);
  prompts_.displayRefreshEnded(generation);
  if (result == displaypower::DisplayRefreshResult::Unchanged) {
    Diagnostics::event("DISPLAY_RUNTIME_RESULT", "UNCHANGED_STABLE_SKIP");
  } else if (result != displaypower::DisplayRefreshResult::Complete) {
    Diagnostics::event("DISPLAY_RUNTIME_RESULT", String(static_cast<int>(result)));
  } else {
    lastDisplayRefreshAt_ = millis();
  }
  return result == displaypower::DisplayRefreshResult::Complete ||
      result == displaypower::DisplayRefreshResult::Unchanged;
}

bool PaperColorApplicationRuntime::displayBusy() const {
  return displayActivity_.refreshBusy() ||
      (displayRuntime_ && displayRuntime_->busy()) || displayTransaction_.active() ||
      (lastDisplayRefreshAt_ &&
       millis() - lastDisplayRefreshAt_ < 30000U);
}

bool PaperColorApplicationRuntime::acceptsUserInput() const {
  return initialized_ && wakeRecovery_.state().readyForUserInput();
}

StableStartupDisplay PaperColorApplicationRuntime::stableStartupDisplay() const {
  // A local wake gesture is an intent to interact, not an intent to replace
  // the persistent e-paper contents. Preserve the frame even when MyAI is
  // inactive or storage needs later recovery; diagnostics and the Portal still
  // expose those conditions without spending another panel refresh.
  if (wakeAnnouncementPending_) return StableStartupDisplay::PreserveExisting;
  if (myAiAuthorized_) return StableStartupDisplay::PreserveExisting;
  if (pairingCallbackPending_ || pairingPollActive_ ||
      portalMyAiState_ == "pairing") {
    return StableStartupDisplay::AwaitingMyAiPairing;
  }
  if (portalMyAiState_ == "app_not_registered" ||
      portalMyAiState_ == "offline" || portalMyAiState_ == "error" ||
      portalMyAiState_ == "recovery" ||
      portalMyAiState_ == "auth_rejected" ||
      portalMyAiState_ == "credential_recovery" ||
      (portalMyAiState_ == "unconfigured" &&
       inkloopClient_.deviceId().length())) {
    return StableStartupDisplay::MyAiServiceUnavailable;
  }
  return StableStartupDisplay::SettingsReady;
}

void PaperColorApplicationRuntime::noteMyAiUnavailableDisplayApplied() {
  // The panel is persistent.  Record that the previously scrubbed/ready frame
  // has been replaced so a later successful authorization can restore a
  // non-error stable frame exactly once.
  if (!storePairingScreenScrubbed(false)) {
    Diagnostics::event("WARN", "MYAI_UNAVAILABLE_DISPLAY_MARKER_SAVE_FAILED");
  }
  terminalDisplayScrubPending_ = false;
  terminalDisplayScrubApplied_ = false;
}

void PaperColorApplicationRuntime::noteExternalActivity(
    uint32_t nowMilliseconds) {
  activityState_.noteMeaningfulActivity(nowMilliseconds);
}

void PaperColorApplicationRuntime::setExternalPagePending(
    bool pending,
    uint32_t nowMilliseconds) {
  activityState_.setExternalPagePending(pending, nowMilliseconds);
}

void PaperColorApplicationRuntime::onPageDisplayCommitted(
    const String& assetId, size_t oneBasedOrdinal, bool success) {
  if (!pendingDisplayFrame_.empty() && pendingDisplayFrame_ == assetId.c_str() &&
      pendingDisplayOrdinal_ == oneBasedOrdinal) {
    if (success) {
      voice_.onDisplayCommitSuccess(
          pendingDisplayFrame_, pendingDisplayRevision_, pendingDisplayOrdinal_);
    } else {
      voice_.onDisplayCommitFailure(pendingDisplayFrame_, pendingDisplayRevision_);
    }
    pendingDisplayFrame_.clear();
    pendingDisplayRevision_ = 0;
    pendingDisplayOrdinal_ = 0;
    return;
  }
  // Physical page keys announce selection before the one-second debounce and
  // announce refresh start immediately before the transaction. Do not repeat
  // the ordinal after the slow e-paper commit.
}

bool PaperColorApplicationRuntime::playAssistancePrompt(
    const char* promptId, uint32_t argument) {
  if (!promptId || !portal_.settings().voiceAssistanceEnabled ||
      promptPlayer_.busy() || streamingAudio_.active() ||
      voice_.turnActive() || voice_.captureActive() ||
      M5.Mic.isRecording()) return false;
  const voice::Status status = promptPlayer_.play(promptId, argument);
  return status.success;
}

void PaperColorApplicationRuntime::clearAssistancePromptQueue(
    bool stopCurrent) {
  assistancePromptQueueHead_ = 0;
  assistancePromptQueueCount_ = 0;
  for (uint8_t index = 0; index < kAssistancePromptQueueCapacity; ++index) {
    assistancePromptQueue_[index] = nullptr;
  }
  if (stopCurrent && promptPlayer_.busy() && !streamingAudio_.active() &&
      !voice_.turnActive() && !voice_.captureActive()) {
    promptPlayer_.stop();
  }
}

bool PaperColorApplicationRuntime::enqueueAssistancePrompt(
    const char* promptId) {
  if (!promptId || !portal_.settings().voiceAssistanceEnabled ||
      assistancePromptQueueCount_ >= kAssistancePromptQueueCapacity) {
    return false;
  }
  const uint8_t tail = static_cast<uint8_t>(
      (assistancePromptQueueHead_ + assistancePromptQueueCount_) %
      kAssistancePromptQueueCapacity);
  assistancePromptQueue_[tail] = promptId;
  ++assistancePromptQueueCount_;
  pollAssistancePromptQueue();
  return true;
}

bool PaperColorApplicationRuntime::enqueueOrdinalAssistance(
    size_t oneBasedOrdinal) {
  const PagePromptPlan plan = pageOrdinalPromptPlan(oneBasedOrdinal, false);
  if (!plan.count) return false;
  for (uint8_t index = 0; index < plan.count; ++index) {
    if (!enqueueAssistancePrompt(plan.prompts[index])) return false;
  }
  return true;
}

void PaperColorApplicationRuntime::pollAssistancePromptQueue() {
  if (!portal_.settings().voiceAssistanceEnabled) {
    clearAssistancePromptQueue(false);
    return;
  }
  if (!assistancePromptQueueCount_ || promptPlayer_.busy() ||
      streamingAudio_.active() || voice_.turnActive() ||
      voice_.captureActive() || M5.Mic.isRecording()) return;
  const char* prompt = assistancePromptQueue_[assistancePromptQueueHead_];
  const voice::Status status = promptPlayer_.play(prompt ? prompt : "");
  if (!status.success) {
    Diagnostics::event(
        "AUDIO_ASSISTANCE_ERROR",
        status.code.empty() ? status.detail.c_str() : status.code.c_str());
    clearAssistancePromptQueue(false);
    return;
  }
  assistancePromptQueue_[assistancePromptQueueHead_] = nullptr;
  assistancePromptQueueHead_ = static_cast<uint8_t>(
      (assistancePromptQueueHead_ + 1U) % kAssistancePromptQueueCapacity);
  --assistancePromptQueueCount_;
}

void PaperColorApplicationRuntime::notifyPageSelected(
    size_t oneBasedOrdinal) {
  clearAssistancePromptQueue(true);
  enqueueOrdinalAssistance(oneBasedOrdinal);
}

void PaperColorApplicationRuntime::notifyPageRefreshStarting(
    size_t oneBasedOrdinal) {
  // A long selection announcement should never suppress the more important
  // transaction-start cue after the debounce expires.
  clearAssistancePromptQueue(true);
  const PagePromptPlan plan = pageOrdinalPromptPlan(oneBasedOrdinal, true);
  for (uint8_t index = 0; index < plan.count; ++index) {
    if (!enqueueAssistancePrompt(plan.prompts[index])) break;
  }
}

void PaperColorApplicationRuntime::notifyPageBusy() {
  playAssistancePrompt("display.please_wait");
}

bool PaperColorApplicationRuntime::voiceAssistanceEnabled() const {
  return initialized_ && portal_.settings().voiceAssistanceEnabled;
}

void PaperColorApplicationRuntime::onActivationState(
    myai::ActivationState state, const myai::Status& status) {
  Diagnostics::event("MYAI_ACTIVATION", activationName(state));
  portalMyAiState_ = status.code == myai::ErrorCode::AppNotRegistered
      ? "app_not_registered" : activationName(state);
  if (state == myai::ActivationState::Bound) {
    myAiAuthorized_ = true;
    // A transient pairing/authorization failure may have left the persistent
    // e-paper panel on the service-unavailable frame.  Reconcile the durable
    // display marker every time Center confirms Bound, not only during boot or
    // the original pairing callback, so the next stable loop can replace that
    // stale error page exactly once.
    reconcileTerminalBindingState();
  }
  if (state == myai::ActivationState::Unconfigured ||
      state == myai::ActivationState::PaymentRequired ||
      state == myai::ActivationState::RecoveryRequired) {
    myAiAuthorized_ = false;
    voiceConnectPending_ = false;
    voiceConnectDeadline_ = 0;
    voice_.setEnabled(false);
    voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
  }
}

void PaperColorApplicationRuntime::onPairingReady(
    const myai::PairingView& pairing) {
  myAiRebindPending_ = false;
  pendingPairing_ = pairing;
  pairingCallbackPending_ = true;
  portalMyAiState_ = "pairing";
}

void PaperColorApplicationRuntime::onVoiceState(myai::VoiceState state) {
  const myai::VoiceState previous = lastMyAiVoiceState_;
  lastMyAiVoiceState_ = state;
  if (state == myai::VoiceState::Ready) {
    voiceTtsSegmentEndedAt_ = 0;
    voiceConnectPending_ = false;
    voiceConnectDeadline_ = 0;
    if (tutorialNarrationInFlight_ &&
        (previous == myai::VoiceState::Thinking ||
         previous == myai::VoiceState::Speaking)) {
      tutorialNarrationInFlight_ = false;
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Off);
      std::string error;
      if (!portal_.onVoiceTutorialComplete(&error))
        Diagnostics::event("WARN", String("VOICE_TUTORIAL_SAVE:") + error.c_str());
      else
        Diagnostics::event("VOICE_TUTORIAL", "COMPLETE");
    }
    if (!voiceWasReady_) {
      voiceWasReady_ = true;
      voice_.onSessionReady();
    } else if (voice_.state() == voice::RuntimeState::Speaking) {
      // The gateway may split one response across several tts.start/tts.stop
      // segments. A segment stop only quiesces the current speaker buffer; the
      // voice turn becomes Ready exclusively when response.done arrives.
      if (streamingAudio_.active()) {
        voiceResponseDonePending_ = true;
      } else {
        voiceResponseDonePending_ = false;
        voice_.onTtsStop(voice_.activeTurnGeneration());
      }
    } else if (previous == myai::VoiceState::Thinking) {
      voice_.onResponseDone(voice_.activeTurnGeneration());
    }
    if (listenAfterConnect_) {
      listenAfterConnect_ = false;
      const voice::Status listening = voice_.onTopButtonTap();
      Diagnostics::event(
          "VOICE_BUTTON",
          listening.success ? "LISTENING_AFTER_CONNECT" : listening.code.c_str());
    } else if (introductionPending_) {
      introductionPending_ = false;
      tutorialNarrationPending_ = false;
      tutorialNarrationInFlight_ = true;
      resetAssistantChatTurn();
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Thinking);
      const myai::Status requested = myAi_.requestResponse(
          "请用简短自然的中文做一次自我介绍：你是 Inkloop PaperColor 的 MyAI "
          "语音助手，可以聊天、管理本机相册与设置，也能为 400x600 六色墨水屏"
          "生成高对比度图片。不要调用工具，不要超过三句话。");
      if (!requested.ok()) {
        tutorialNarrationInFlight_ = false;
        voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
        Diagnostics::event("WARN", String("VOICE_INTRO_START:") +
            requested.detail.c_str());
      } else {
        Diagnostics::event("VOICE_INTRO", "TEXT_REQUEST_SENT");
      }
    } else if (tutorialNarrationPending_ && !tutorialNarrationInFlight_) {
      tutorialNarrationPending_ = false;
      tutorialNarrationInFlight_ = true;
      resetAssistantChatTurn();
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Thinking);
      const myai::Status requested = myAi_.requestResponse(
          "这是首次开机语音教程。请用简短中文告诉用户：按一下顶部按钮开始或结束说话；"
          "左灯表示聆听、思考和说话；左右翻页键切换缓存图片；屏幕或右灯闪烁时请稍等；"
          "两分钟无操作会休眠；可访问 inkloop.local 修改存储、音量、提示词和图片设置。"
          "只做教程，不调用工具。");
      if (!requested.ok()) {
        tutorialNarrationInFlight_ = false;
        voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
        Diagnostics::event("WARN", String("VOICE_TUTORIAL_START:") +
            requested.detail.c_str());
      }
    }
  } else if (state == myai::VoiceState::Speaking) {
    voiceTtsSegmentEndedAt_ = 0;
    if (tutorialNarrationInFlight_) {
      voiceLed_.setLeftVoiceState(voice::VoiceLedState::Speaking);
      if (!streamingAudio_.authorize().ok()) {
        streamingAudio_.abort();
        tutorialNarrationInFlight_ = false;
        voiceLed_.setLeftVoiceState(voice::VoiceLedState::Error);
      }
    } else {
      const voice::Status accepted =
          voice_.state() == voice::RuntimeState::Speaking
              ? voice::Status::ok()
              : voice_.onTtsStart(voice_.activeTurnGeneration());
      if (!accepted.success || !streamingAudio_.authorize().ok()) {
        streamingAudio_.abort();
        voice_.onTransportError(voiceFailure(
            "tts_authorization_failed", "speaker/microphone handoff failed"));
      }
    }
  } else if (state == myai::VoiceState::Error) {
    voiceTtsSegmentEndedAt_ = 0;
    listenAfterConnect_ = false;
    voiceResponseDonePending_ = false;
    voiceConnectPending_ = false;
    if (tutorialNarrationInFlight_) {
      tutorialNarrationInFlight_ = false;
      tutorialNarrationPending_ = true;
    }
    myAi_.disconnectVoice("voice_error_cleanup");
    voiceWasReady_ = false;
    voiceReconnectAt_ = millis() + boundedVoiceReconnectDelay(
        myAi_.suggestedVoiceReconnectDelayMs());
    voice_.onSessionLost(voiceFailure("myai_socket_lost", "voice gateway disconnected"));
  }
}

void PaperColorApplicationRuntime::onTranscript(
    const std::string& text, bool final) {
  if (!final) {
    voice_.onAsrPartial(text, voice_.activeTurnGeneration());
    return;
  }
  appendMyAiChatMessage("user", text);
  resetAssistantChatTurn();
}

void PaperColorApplicationRuntime::onAssistantText(
    const std::string& text, bool final) {
  if (!final) {
    if (assistantChatFinalized_) resetAssistantChatTurn();
    const size_t remaining = pendingAssistantText_.size() <
            portal::kMaximumMyAiChatTextBytes
        ? portal::kMaximumMyAiChatTextBytes - pendingAssistantText_.size()
        : 0;
    if (!remaining) {
      myAiChatTruncated_ = true;
      return;
    }
    const std::string delta = boundedChatText(text, remaining, false);
    pendingAssistantText_.append(delta);
    if (delta.size() < text.size()) myAiChatTruncated_ = true;
    return;
  }
  if (assistantChatFinalized_) return;
  const std::string finalText = pendingAssistantText_.empty()
      ? boundedChatText(text, portal::kMaximumMyAiChatTextBytes)
      : pendingAssistantText_;
  appendMyAiChatMessage("assistant", finalText);
  pendingAssistantText_.clear();
  assistantChatFinalized_ = true;
}

void PaperColorApplicationRuntime::onLocalCommand(
    const std::string&, const std::string&) {}

void PaperColorApplicationRuntime::onVoiceAction(
    const myai::VoiceEvent& action) {
  if (action.kind != "aigc.generate" || action.prompt.empty() ||
      action.prompt.size() > 1024 || aigcPhase_ != AigcPhase::Idle ||
      album_.userUploadActive() || tutorialNarrationInFlight_) {
    // A tutorial/self-introduction is narration-only even if a remote model
    // ignores the explicit no-tools instruction. Never let it start a costly
    // display mutation behind the user's back.
    Diagnostics::event("AIGC_ACTION", "REJECTED");
    return;
  }
  aigcPrompt_ = action.prompt;
  aigcPhase_ = AigcPhase::Start;
  activityState_.noteMeaningfulActivity(millis());
  Diagnostics::event("AIGC_ACTION", "ACCEPTED");
}

void PaperColorApplicationRuntime::onAigcState(
    myai::AigcState state, const std::string&) {
  switch (state) {
    case myai::AigcState::Generating:
    case myai::AigcState::Polling:
      setImageLed(displaypower::ImageLedState::Generating);
      break;
    case myai::AigcState::Downloading:
      setImageLed(displaypower::ImageLedState::Downloading);
      break;
    case myai::AigcState::Error:
      setImageLed(displaypower::ImageLedState::Error);
      break;
    default: break;
  }
}

void PaperColorApplicationRuntime::onError(const myai::Status& status) {
  Diagnostics::event("MYAI_ERROR", status.detail.c_str());
  if (!myAiAuthorized_) {
    if (status.code == myai::ErrorCode::AppNotRegistered)
      portalMyAiState_ = "app_not_registered";
    else if (status.code == myai::ErrorCode::PaymentRequired)
      portalMyAiState_ = "inactive";
    else if (status.code == myai::ErrorCode::RecoveryRequired)
      portalMyAiState_ = "recovery";
    else if (status.code == myai::ErrorCode::Unauthorized)
      portalMyAiState_ = "auth_rejected";
    else if (status.code == myai::ErrorCode::Transport)
      portalMyAiState_ = "offline";
    else if (!status.ok())
      portalMyAiState_ = "error";
  }
  if (status.code == myai::ErrorCode::Unauthorized &&
      myAi_.activationState() == myai::ActivationState::Unconfigured) {
    myAiRebindPending_ = true;
    myAiRebindRetryAt_ = millis();
  }
}

myai::LocalTranscriptDecision PaperColorApplicationRuntime::inspect(
    const std::string& transcript) {
  const voice::TranscriptDecision decision = voice_.onAsrFinal(
      transcript, voice_.activeTurnGeneration());
  String route = decision.handledLocally ? "LOCAL:" : "REMOTE:";
  route += String(static_cast<unsigned long>(transcript.size()));
  Diagnostics::event("VOICE_ASR_FINAL", route);
  // VoiceRuntime itself dispatches remote response for unmatched transcripts.
  // Always consume here to prevent MyAiClient from sending a duplicate
  // response.create after the local interceptor returns.
  const bool delegatedResponse = !decision.handledLocally &&
      voice_.turnActive() && voice_.state() == voice::RuntimeState::Thinking;
  return myai::LocalTranscriptDecision(
      true,
      decision.commandName.empty() ? "voice.runtime" : decision.commandName,
      delegatedResponse);
}

bool PaperColorApplicationRuntime::startMyAiPairing(std::string* error) {
  if (pairingPollActive_ || myAiAuthorized_) {
    if (error) *error = "myai_pairing_not_available";
    return false;
  }
  myai::PairingView ignored;
  myai::Status status;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    status = myAi_.startPairing(candidateCode(), ignored);
    if (status.ok() || status.code != myai::ErrorCode::Conflict) break;
  }
  if (!status.ok()) {
    portalMyAiState_ = status.code == myai::ErrorCode::AppNotRegistered
        ? "app_not_registered"
        : (status.code == myai::ErrorCode::Transport ? "offline" : "error");
    if (error) *error = status.detail;
    return false;
  }
  portalMyAiState_ = "pairing";
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::restartMyAiPairing(std::string* error) {
  if (pairingPollActive_ || pairingCallbackPending_ || mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  myAi_.disconnectVoice("owner_rebind");
  myAiAuthorized_ = false;
  voiceConnectPending_ = false;
  voiceConnectDeadline_ = 0;
  voice_.setEnabled(false);
  const myai::Status reset = myAi_.resetCredentialForRebind();
  if (!reset.ok()) {
    if (error) *error = reset.detail.empty()
        ? "myai_rebind_reset_failed" : reset.detail;
    return false;
  }
  portalMyAiState_ = "unconfigured";
  if (!startMyAiPairing(error)) {
    if (myAi_.activationState() == myai::ActivationState::RecoveryRequired) {
      portalMyAiState_ = "recovery";
      if (error) *error = "device_credential_recovery_required";
    }
    return false;
  }
  myAiRebindPending_ = false;
  activityState_.noteMeaningfulActivity(millis());
  return true;
}

bool PaperColorApplicationRuntime::requestInkloopCodeReuse(
    const std::string& onboardingCode, uint64_t, std::string* error) {
  if (!myai::isSixDigitCode(onboardingCode)) {
    if (error) *error = "invalid_authoritative_myai_code";
    return false;
  }
  const RegistrationResult result =
      inkloopClient_.registerDevice(onboardingCode.c_str());
  bool accepted = result.ok && result.requestedPairingCodeAccepted;
  // The Inkloop API intentionally rejects a supplied code once the device is
  // already owned. A power loss may leave Portal one commit behind that
  // server transition, so authenticate a normal sync with the stored device
  // identity instead of asking the API to disclose or re-accept the code.
  if (!accepted && inkloopClient_.deviceId().length()) {
    const SyncResult verified = inkloopClient_.syncTasks();
    accepted = verified.ok && inkloopClient_.paired();
  }
  if (!accepted) {
    if (error) *error = std::string("inkloop_pairing_code_rejected_http_") +
        std::to_string(result.httpStatus);
    return false;
  }
  Diagnostics::event(
      "INKLOOP_CODE_REUSE",
      inkloopClient_.paired() ? "already_bound" : "accepted");
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::verifyInkloopBinding(std::string* error) {
  if (!inkloopClient_.deviceId().length()) {
    if (error) *error = "inkloop_identity_missing";
    return false;
  }
  const SyncResult sync = inkloopClient_.syncTasks();
  if (!sync.ok || !inkloopClient_.paired()) {
    if (error) *error = "inkloop_binding_verification_failed";
    return false;
  }
  if (error) error->clear();
  return true;
}

void PaperColorApplicationRuntime::applyPortalSettings(
    const portal::PortalSettings& settings) {
  StorageManager::AssetPreference storage = StorageManager::AssetPreference::Automatic;
  if (settings.storageTarget == portal::StorageTarget::Internal)
    storage = StorageManager::AssetPreference::Internal;
  else if (settings.storageTarget == portal::StorageTarget::SdCard)
    storage = StorageManager::AssetPreference::SdCard;
  storage_.setAssetPreference(storage);
  promptPlayer_.setVolume(settings.volume);
  streamingAudio_.setVolume(settings.volume);
  M5.Speaker.setVolume(static_cast<uint8_t>(settings.volume * 255U / 100U));
  if (!settings.voiceAssistanceEnabled && promptPlayer_.busy() &&
      !volumePreviewActive_ && !streamingAudio_.active() &&
      !voice_.turnActive() && !voice_.captureActive()) {
    promptPlayer_.stop();
  }
  if (!settings.voiceAssistanceEnabled) clearAssistancePromptQueue(false);
  if (!leds_.setMaximumBrightnessPercent(
          settings.ledMaximumBrightnessPercent)) {
    Diagnostics::event("ERROR", "LED_MAX_BRIGHTNESS_APPLY_FAILED");
  }
  if (!settings.assistantPrompt.empty()) {
    if (voice_.turnActive() || voice_.captureActive() || streamingAudio_.active()) {
      pendingSystemPrompt_ = settings.assistantPrompt;
      Diagnostics::event("PROMPT_SETTING", "DEFERRED_UNTIL_TURN_IDLE");
    } else {
      const voice::Status applied = applyAssistantPromptRuntime(settings.assistantPrompt);
      if (!applied.success) Diagnostics::event("PROMPT_SETTING", applied.detail.c_str());
    }
  }
  const uint8_t desiredVoiceLedIndex = settings.ledRolesSwapped ? 1 : 0;
  if (!leds_.mappingCalibrated() ||
      desiredVoiceLedIndex != leds_.voiceLedIndex()) {
    // The factory/default mapping still needs to be marked calibrated. The
    // previous equality-only check left a fresh device "UNCALIBRATED", so the
    // explicit RGB test rejected without ever lighting either pixel.
    leds_.setMapping(true, desiredVoiceLedIndex);
  }
  displaypower::PowerPolicyConfig power;
  power.mode = (settingsStore_.current().features.deepSleepEnabled ||
                settings.powerMode == portal::PowerMode::Battery)
      ? displaypower::PowerMode::BatteryOptIn
      : displaypower::PowerMode::AlwaysAwake;
  power.eligibleIdleMilliseconds =
      static_cast<uint32_t>(settings.idleTimeoutSeconds) * 1000U;
  powerPolicy_.setConfig(power);
  if (displayRuntime_) {
    Diagnostics::event("DISPLAY_SETTING", "REFRESH_MODE_APPLIES_AFTER_REBOOT");
  }
}

bool PaperColorApplicationRuntime::previewVolume(
    uint8_t volume, std::string* error) {
  if (volume > 100) {
    if (error) *error = "invalid_volume";
    return false;
  }
  if (volumePreviewActive_ || promptPlayer_.busy() || streamingAudio_.active() ||
      voice_.turnActive() || voice_.captureActive() || M5.Mic.isRecording() ||
      tutorialNarrationInFlight_) {
    if (error) *error = "audio_busy";
    return false;
  }
  promptPlayer_.setVolume(volume);
  const voice::Status started = promptPlayer_.play("settings.saved");
  // The active M5 speaker keeps the selected preview level. Restore the
  // player's durable default immediately so the next prompt cannot inherit a
  // non-saved slider value; loop() restores the global speaker after playback.
  promptPlayer_.setVolume(portal_.settings().volume);
  if (!started.success) {
    M5.Speaker.setVolume(static_cast<uint8_t>(
        portal_.settings().volume * 255U / 100U));
    if (error) *error = started.code.empty()
        ? "audio_preview_failed" : started.code;
    return false;
  }
  volumePreviewActive_ = true;
  activityState_.noteMeaningfulActivity(millis());
  Diagnostics::event("AUDIO_PREVIEW", "STARTED");
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::testLedRoles(
    bool swapped, uint8_t maximumBrightnessPercent,
    std::string* error) {
  if (maximumBrightnessPercent < 1 || maximumBrightnessPercent > 100) {
    if (error) *error = "invalid_led_brightness";
    return false;
  }
  if (leds_.count() != 2) {
    if (error) *error = "led_count_must_be_two";
    return false;
  }
  // A settings save can arrive while the volume preview, display refresh, or
  // another cooperative operation is still active. Treat the diagnostic as
  // accepted work instead of dropping it with device_busy; loop() starts it
  // at the first safe point and keeps the full 4.6-second sequence observable.
  pendingLedRolesSwapped_ = swapped;
  pendingLedMaximumBrightnessPercent_ = maximumBrightnessPercent;
  ledDiagnosticPending_ = true;
  activityState_.noteMeaningfulActivity(millis());
  Diagnostics::event("LED_ROLE_DIAGNOSTIC", "QUEUED");
  if (!leds_.pixelDiagnosticActive() && !ledDiagnosticWorkBusy() &&
      !startPendingLedDiagnostic(error)) return false;
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::ledDiagnosticWorkBusy() const {
  return displayBusy() || generationActive() || streamingAudio_.active() ||
      voice_.captureActive() || promptPlayer_.busy() ||
      album_.userUploadActive() || activityState_.externalPagePending();
}

bool PaperColorApplicationRuntime::startPendingLedDiagnostic(
    std::string* error) {
  if (!ledDiagnosticPending_) {
    if (error) error->clear();
    return true;
  }
  if (leds_.pixelDiagnosticActive() || ledDiagnosticWorkBusy()) {
    if (error) error->clear();
    return true;
  }
  if (!leds_.setMaximumBrightnessPercent(
          pendingLedMaximumBrightnessPercent_) ||
      !leds_.setMapping(true, pendingLedRolesSwapped_ ? 1 : 0) ||
      !leds_.runPixelDiagnostic()) {
    ledDiagnosticPending_ = false;
    if (error) *error = "led_role_test_failed";
    return false;
  }
  ledDiagnosticPending_ = false;
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::executeConfirmedOperation(
    const portal::ConfirmedOperation& operation, std::string* error) {
  if (mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  const AlbumMutationResult result = runAlbumMutation([&]() {
    if (operation.action == portal::DestructiveAction::DeleteAsset)
      return album_.deleteUserAsset(selectedBackend(), operation.target.c_str());
    if (operation.action == portal::DestructiveAction::ClearAlbum)
      return album_.clearUserAssets(selectedBackend());
    if (operation.action == portal::DestructiveAction::FormatSdCard)
      return sd_.formatFat();
    return false;
  });
  if (result != AlbumMutationResult::Complete) {
    if (error) {
      *error = result == AlbumMutationResult::RevisionPersistenceFailed ||
                       result == AlbumMutationResult::RevisionUnavailable
          ? "album_revision_persist_failed"
          : "confirmed_operation_failed";
    }
    return false;
  }
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::mutationBusy() const {
  return displayBusy() || generationActive() || streamingAudio_.active() ||
      responsiveWorkExecutor().active() ||
      voiceConnectPending_ || voice_.turnActive() || voice_.captureActive() ||
      promptPlayer_.busy() ||
      leds_.pixelDiagnosticActive() || ledDiagnosticPending_ ||
      album_.userUploadActive() || activityState_.externalPagePending();
}

bool PaperColorApplicationRuntime::beginAlbumUpload(
    const std::string& untrustedName,
    size_t declaredImageBytes,
    std::string* error) {
  if (mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  const std::string title = sanitizedUploadTitle(untrustedName);
  String storageError;
  if (!album_.beginUserUpload(
          title.c_str(), declaredImageBytes, storageError)) {
    setImageLed(displaypower::ImageLedState::Error);
    if (error) *error = storageError.c_str();
    return false;
  }
  setImageLed(displaypower::ImageLedState::Downloading);
  portalUploadTitle_ = title;
  activityState_.noteMeaningfulActivity(millis());
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::writeAlbumUpload(
    const uint8_t* bytes, size_t length, std::string* error) {
  String storageError;
  if (!album_.appendUserUpload(bytes, length, storageError)) {
    setImageLed(displaypower::ImageLedState::Error);
    if (error) *error = storageError.c_str();
    return false;
  }
  activityState_.noteMeaningfulActivity(millis());
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::finishAlbumUpload(
    portal::AlbumUploadResult* result, std::string* error) {
  if (!result || !album_.userUploadActive()) {
    if (error) *error = "upload_not_active";
    return false;
  }
  setImageLed(displaypower::ImageLedState::Writing);
  AlbumAsset committed;
  String storageError;
  const AlbumMutationResult mutation = runAlbumMutation([&]() {
    return album_.finishUserUpload(committed, storageError);
  });
  if (mutation != AlbumMutationResult::Complete) {
    album_.abortUserUpload();
    setImageLed(displaypower::ImageLedState::Error);
    if (error) {
      *error = storageError.length() ? storageError.c_str()
          : "album_revision_persist_failed";
    }
    return false;
  }
  result->assetId = committed.id.c_str();
  result->title = portalUploadTitle_.empty()
      ? "Uploaded image" : portalUploadTitle_;
  result->backend = committed.backend.valid()
      ? committed.backend.identity : "unavailable";
  result->bytes = committed.bytes;
  result->revision = albumRevision_;
  portalUploadTitle_.clear();
  setImageLed(displaypower::ImageLedState::Complete);
  activityState_.noteMeaningfulActivity(millis());
  if (error) error->clear();
  return true;
}

void PaperColorApplicationRuntime::abortAlbumUpload() {
  if (!album_.userUploadActive()) return;
  album_.abortUserUpload();
  portalUploadTitle_.clear();
  setImageLed(displaypower::ImageLedState::Error);
}

bool PaperColorApplicationRuntime::openAlbumPreview(
    const std::string& assetId, File* file, size_t* bytes,
    std::string* error) {
  if (!file || !bytes) {
    if (error) *error = "album_preview_invalid_output";
    return false;
  }
  *file = File();
  *bytes = 0;
  if (mutationBusy() || album_.userUploadActive()) {
    if (error) *error = "device_busy";
    return false;
  }
  if (!stableAssetId(assetId)) {
    if (error) *error = "invalid_asset_target";
    return false;
  }
  const StorageBackendRef backend = selectedBackend();
  AlbumCatalogEntry source;
  if (!album_.findCatalogEntry(backend, assetId.c_str(), source) ||
      !backend.available() || source.bytes < 45 ||
      source.bytes > kMaxFrameBytes) {
    if (error) *error = "album_item_not_found";
    return false;
  }
  File opened = backend.backend->open(source.path.c_str(), FILE_READ);
  if (!opened || opened.size() != source.bytes) {
    if (opened) opened.close();
    if (error) *error = "album_item_not_found";
    return false;
  }
  *bytes = source.bytes;
  *file = opened;
  if (error) error->clear();
  return true;
}

portal::StorageStatus PaperColorApplicationRuntime::storageStatus() const {
  portal::StorageStatus status;
  const StorageBackendRef internal = storage_.taskBackend();
  status.internalMounted = internal.available();
  status.internalRecoveryRequired = !status.internalMounted;
  status.taskStoreReady = tasks_.ready();
  status.internalTotalBytes = internal.available() ? internal.backend->totalBytes() : 0;
  const uint64_t internalUsed = internal.available()
      ? internal.backend->usedBytes() : 0;
  status.internalFreeBytes = internalUsed <= status.internalTotalBytes
      ? status.internalTotalBytes - internalUsed : 0;
  const StorageCapabilities sdCapabilities = sd_.capabilities();
  status.sdPresent = sdCapabilities.mounted;
  status.sdWritable = sdCapabilities.mounted && sdCapabilities.writable;
  status.sdTotalBytes = status.sdPresent ? sd_.totalBytes() : 0;
  const uint64_t sdUsed = status.sdPresent ? sd_.usedBytes() : 0;
  status.sdFreeBytes = sdUsed <= status.sdTotalBytes
      ? status.sdTotalBytes - sdUsed : 0;
  const StorageBackendRef active = selectedBackend();
  if (active.available()) {
    status.activeBackend = active.backend == &sd_
        ? portal::ActiveStorageBackend::SdCard
        : portal::ActiveStorageBackend::Internal;
    const StorageCapabilities capabilities = active.backend->capabilities();
    status.activeMounted = capabilities.mounted;
    status.activeWritable = capabilities.mounted && capabilities.writable;
    status.activeTotalBytes = active.backend->totalBytes();
    const uint64_t used = active.backend->usedBytes();
    status.activeFreeBytes = used <= status.activeTotalBytes
        ? status.activeTotalBytes - used : 0;
  }
  return status;
}

portal::AlbumReadStatus PaperColorApplicationRuntime::readAlbumPage(
    const portal::AlbumPageRequest& request, portal::AlbumPage* page) const {
  if (album_.userUploadActive()) return portal::AlbumReadStatus::Unavailable;
  if (!page || request.maximumItems == 0 ||
      request.maximumItems > portal::kMaximumAlbumPageItems ||
      request.maximumTotalFieldBytes > portal::kMaximumAlbumPageFieldBytes) {
    return portal::AlbumReadStatus::TooLarge;
  }
  size_t offset = 0;
  if (!parseCursor(request.cursor, &offset)) return portal::AlbumReadStatus::InvalidData;
  std::vector<AlbumCatalogEntry> entries;
  size_t total = 0;
  size_t next = 0;
  if (!const_cast<AlbumStore&>(album_).readCatalogPage(
          selectedBackend(), offset, request.maximumItems,
          request.maximumTotalFieldBytes, entries, total, next)) {
    return portal::AlbumReadStatus::Unavailable;
  }
  portal::AlbumPage result;
  result.totalItems = total;
  for (const AlbumCatalogEntry& source : entries) {
    if (source.id.length() > request.maximumIdBytes ||
        source.taskId.length() > request.maximumTitleBytes) {
      return portal::AlbumReadStatus::TooLarge;
    }
    portal::AlbumItem item;
    item.id = source.id.c_str();
    if (source.taskId.startsWith("myai:")) {
      item.origin = "myai";
      item.title = source.taskId.substring(5).c_str();
    } else if (source.taskId.startsWith("upload:")) {
      item.origin = "user";
      item.title = source.taskId.substring(7).c_str();
    } else {
      item.origin = "inkloop";
      String taskTitle;
      item.title = const_cast<TaskStore&>(tasks_).findTitle(source.taskId, taskTitle)
          ? taskTitle.c_str() : source.taskId.c_str();
    }
    if (item.title.empty()) item.title = "Inkloop image";
    if (item.title.size() > request.maximumTitleBytes)
      return portal::AlbumReadStatus::TooLarge;
    if (item.origin.size() > request.maximumOriginBytes)
      return portal::AlbumReadStatus::TooLarge;
    item.bytes = source.bytes;
    item.current = source.current;
    item.factoryAsset = source.factoryAsset;
    item.renderStrategy = source.renderStrategy.c_str();
    result.items.push_back(item);
  }
  if (next) result.nextCursor = std::to_string(next);
  *page = result;
  return portal::AlbumReadStatus::Ok;
}

portal::AlbumReadStatus PaperColorApplicationRuntime::findAlbumItem(
    const std::string& assetId, portal::AlbumItem* item) const {
  if (album_.userUploadActive()) return portal::AlbumReadStatus::Unavailable;
  if (!item || !stableAssetId(assetId)) return portal::AlbumReadStatus::InvalidData;
  AlbumCatalogEntry source;
  if (!const_cast<AlbumStore&>(album_).findCatalogEntry(
          selectedBackend(), assetId.c_str(), source)) {
    return portal::AlbumReadStatus::NotFound;
  }
  item->id = source.id.c_str();
  if (source.taskId.startsWith("myai:")) {
    item->origin = "myai";
    item->title = source.taskId.substring(5).c_str();
  } else if (source.taskId.startsWith("upload:")) {
    item->origin = "user";
    item->title = source.taskId.substring(7).c_str();
  } else {
    item->origin = "inkloop";
    String taskTitle;
    item->title = const_cast<TaskStore&>(tasks_).findTitle(source.taskId, taskTitle)
        ? taskTitle.c_str() : source.taskId.c_str();
  }
  if (item->title.empty()) item->title = "Inkloop image";
  item->bytes = source.bytes;
  item->current = source.current;
  item->factoryAsset = source.factoryAsset;
  item->renderStrategy = source.renderStrategy.c_str();
  return portal::AlbumReadStatus::Ok;
}

bool PaperColorApplicationRuntime::displayAlbumItem(
    const std::string& assetId, std::string* error) {
  if (mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  if (!stableAssetId(assetId)) {
    if (error) *error = "invalid_asset_target";
    return false;
  }
  AlbumCatalogEntry source;
  const StorageBackendRef backend = selectedBackend();
  if (!album_.findCatalogEntry(backend, assetId.c_str(), source) ||
      source.ordinal == 0) {
    if (error) *error = "album_item_not_found";
    return false;
  }
  if (!source.current &&
      (!queuePage_ || !queuePage_(queuePageContext_, source.ordinal - 1, backend))) {
    if (error) *error = "page_queue_failed";
    return false;
  }
  activityState_.noteMeaningfulActivity(millis());
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::setAlbumRenderStrategy(
    const std::string& assetId,
    const std::string& requestedRenderStrategy,
    std::string* error) {
  displaypower::RenderStrategy parsed;
  if (!stableAssetId(assetId) ||
      !displaypower::parseRenderStrategyId(
          requestedRenderStrategy.c_str(), &parsed)) {
    if (error) *error = "invalid_render_strategy";
    return false;
  }
  if (mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  const AlbumMutationResult result = runAlbumMutation([&]() {
    return album_.setRenderStrategy(
        selectedBackend(), assetId.c_str(),
        displaypower::renderStrategyId(parsed));
  });
  if (result != AlbumMutationResult::Complete) {
    if (error) {
      *error = result == AlbumMutationResult::RevisionPersistenceFailed ||
                       result == AlbumMutationResult::RevisionUnavailable
          ? "album_revision_persist_failed"
          : "album_render_strategy_failed";
    }
    return false;
  }
  activityState_.noteMeaningfulActivity(millis());
  if (error) error->clear();
  return true;
}

bool PaperColorApplicationRuntime::generateImage(
    const std::string& prompt, std::string* error) {
  if (prompt.empty() || prompt.size() > 1024) {
    if (error) *error = "invalid_aigc_prompt";
    return false;
  }
  if (!myAiAuthorized_) {
    if (error) *error = "myai_not_activated";
    return false;
  }
  if (mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  aigcPrompt_ = prompt;
  aigcPhase_ = AigcPhase::Start;
  appendMyAiChatMessage(
      "tool", std::string("AIGC 已排队；用户主题：") + prompt);
  activityState_.noteMeaningfulActivity(millis());
  Diagnostics::event("AIGC_ACTION", "ACCEPTED_PORTAL");
  if (error) error->clear();
  return true;
}

void PaperColorApplicationRuntime::appendMyAiChatMessage(
    const char* role, const std::string& text) {
  appendMyAiChatMessageInternal(role, text, true);
}

void PaperColorApplicationRuntime::appendMyAiChatMessageInternal(
    const char* role, const std::string& text, bool persist) {
  if (!role || (std::strcmp(role, "user") != 0 &&
                std::strcmp(role, "assistant") != 0 &&
                std::strcmp(role, "tool") != 0)) {
    return;
  }
  const std::string bounded = boundedChatText(
      text, portal::kMaximumMyAiChatTextBytes);
  if (bounded.empty() ||
      (std::strcmp(role, "user") == 0 &&
       isBlankAudioChatArtifact(bounded))) return;
  portal::MyAiChatMessage message;
  message.sequence = nextMyAiChatSequence_++;
  if (nextMyAiChatSequence_ == 0) nextMyAiChatSequence_ = 1;
  message.role = role;
  message.text = bounded;
  myAiChatHistory_.push_back(message);
  if (persist && !appendMyAiChatLogRecord(message, text)) {
    Diagnostics::event("WARN", "MYAI_CHAT_LOG_APPEND_FAILED");
  }
  size_t aggregate = 0;
  for (size_t index = 0; index < myAiChatHistory_.size(); ++index)
    aggregate += myAiChatHistory_[index].text.size();
  while (myAiChatHistory_.size() > portal::kMaximumMyAiChatItems ||
         aggregate > portal::kMaximumMyAiChatAggregateBytes) {
    aggregate -= myAiChatHistory_.front().text.size();
    myAiChatHistory_.erase(myAiChatHistory_.begin());
    myAiChatTruncated_ = true;
  }
}

bool PaperColorApplicationRuntime::appendMyAiChatLogRecord(
    const portal::MyAiChatMessage& message, const std::string& fullText) {
  if (!sd_.capabilities().mounted || !sd_.mkdir(kMyAiChatLogDirectory))
    return false;
  if (sd_.exists(kMyAiChatLogPath)) {
    File existing = sd_.open(kMyAiChatLogPath, FILE_READ);
    const size_t bytes = existing ? existing.size() : 0;
    if (existing) existing.close();
    if (bytes >= kMaximumMyAiChatLogBytes) {
      if (sd_.exists(kMyAiChatPreviousLogPath) &&
          !sd_.remove(kMyAiChatPreviousLogPath)) return false;
      if (!sd_.rename(kMyAiChatLogPath, kMyAiChatPreviousLogPath))
        return false;
    }
  }
  File file = sd_.open(kMyAiChatLogPath, FILE_APPEND);
  if (!file) return false;
  JsonDocument record;
  record["sequence"] = message.sequence;
  record["time"] = clock_.utcIso8601();
  record["role"] = message.role;
  record["text"] = boundedChatText(
      fullText, kMaximumMyAiChatLogLineBytes - 256U);
  const size_t written = serializeJson(record, file);
  const size_t newline = file.write(static_cast<uint8_t>('\n'));
  file.flush();
  const bool ok = written != 0 && newline == 1 && file.getWriteError() == 0;
  file.close();
  return ok;
}

bool PaperColorApplicationRuntime::loadMyAiChatLogFile(const char* path) {
  if (!path || !sd_.exists(path)) return true;
  File file = sd_.open(path, FILE_READ);
  if (!file) return false;
  bool ok = true;
  while (file.available()) {
    String raw = file.readStringUntil('\n');
    if (raw.length() == 0) continue;
    if (raw.length() > kMaximumMyAiChatLogLineBytes) {
      ok = false;
      continue;
    }
    JsonDocument record;
    if (deserializeJson(record, raw) ||
        !record["sequence"].is<uint64_t>() ||
        !record["role"].is<const char*>() ||
        !record["text"].is<const char*>()) {
      ok = false;
      continue;
    }
    const uint64_t sequence = record["sequence"].as<uint64_t>();
    const std::string role = record["role"].as<const char*>();
    const std::string text = record["text"].as<const char*>();
    if (sequence == 0 || sequence >= UINT64_MAX ||
        (role != "user" && role != "assistant" && role != "tool")) {
      ok = false;
      continue;
    }
    if (sequence < nextMyAiChatSequence_) {
      ok = false;
      continue;
    }
    // Preserve the durable sequence exactly; append increments it only after
    // assigning the current record.
    nextMyAiChatSequence_ = sequence;
    appendMyAiChatMessageInternal(role.c_str(), text, false);
  }
  file.close();
  return ok;
}

bool PaperColorApplicationRuntime::loadMyAiChatHistoryFromStorage() {
  if (!sd_.capabilities().mounted) return true;
  myAiChatHistory_.clear();
  myAiChatTruncated_ = false;
  nextMyAiChatSequence_ = 1;
  const bool previous = loadMyAiChatLogFile(kMyAiChatPreviousLogPath);
  const bool current = loadMyAiChatLogFile(kMyAiChatLogPath);
  return previous && current;
}

void PaperColorApplicationRuntime::clearMyAiChatLogFiles() {
  if (!sd_.capabilities().mounted) return;
  if (sd_.exists(kMyAiChatLogPath)) sd_.remove(kMyAiChatLogPath);
  if (sd_.exists(kMyAiChatPreviousLogPath))
    sd_.remove(kMyAiChatPreviousLogPath);
}

void PaperColorApplicationRuntime::resetAssistantChatTurn() {
  pendingAssistantText_.clear();
  assistantChatFinalized_ = false;
}

bool PaperColorApplicationRuntime::readMyAiChatHistory(
    portal::MyAiChatHistory* history) const {
  if (!history) return false;
  history->messages = myAiChatHistory_;
  history->truncated = myAiChatTruncated_;
  return true;
}

bool PaperColorApplicationRuntime::clearMyAiChatHistory(std::string* error) {
  myAiChatHistory_.clear();
  resetAssistantChatTurn();
  myAiChatTruncated_ = false;
  nextMyAiChatSequence_ = 1;
  clearMyAiChatLogFiles();
  if (error) error->clear();
  return true;
}

portal::DiagnosticsSnapshot PaperColorApplicationRuntime::portalDiagnostics() const {
  portal::DiagnosticsSnapshot snapshot;
  snapshot.firmwareVersion = kBuildVersion;
  snapshot.hardwareSku = kSkuId;
  snapshot.wifiState = WiFi.status() == WL_CONNECTED ? "connected" : "offline";
  snapshot.storageState = selectedBackend().available()
      ? selectedBackend().identity : "unavailable";
  const bool displayTransactionActive = displayActivity_.refreshBusy() ||
      (displayRuntime_ && displayRuntime_->busy()) ||
      displayTransaction_.active();
  const bool displayCooldown = !displayTransactionActive &&
      lastDisplayRefreshAt_ && millis() - lastDisplayRefreshAt_ < 30000U;
  snapshot.displayState = displayTransactionActive
      ? "refreshing" : (displayCooldown ? "cooldown" : "ready");
  snapshot.myAiState = portalMyAiState_.empty()
      ? activationName(myAi_.activationState()) : portalMyAiState_;
  snapshot.freeHeapBytes = ESP.getFreeHeap();
  snapshot.freePsramBytes = ESP.getFreePsram();
  snapshot.serialLines.push_back("Sensitive credentials are never exposed here.");
  return snapshot;
}

voice::Status PaperColorApplicationRuntime::queryFreeSpace(
    voice::StorageSpace& output) {
  const StorageBackendRef backend = selectedBackend();
  if (!backend.available()) return voiceFailure("storage_unavailable", "selected storage unavailable");
  output.storageId = backend.identity;
  output.totalBytes = backend.backend->totalBytes();
  output.freeBytes = output.totalBytes - backend.backend->usedBytes();
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::listImages(
    std::vector<voice::ImageEntry>& output) {
  output.clear();
  std::vector<AlbumCatalogEntry> entries;
  if (!readAllEntries(entries)) return voiceFailure("album_unavailable", "album index unavailable");
  for (const AlbumCatalogEntry& entry : entries) {
    voice::ImageEntry image;
    image.id = entry.id.c_str();
    image.label = entry.taskId.c_str();
    image.ordinal = entry.ordinal;
    output.push_back(image);
  }
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::currentAlbumRevision(
    voice::AlbumRevision& output) {
  const StorageBackendRef backend = selectedBackend();
  if (!backend.available() || !albumRevisionHealthy_ || albumRevision_ == 0)
    return voiceFailure("album_unavailable", "album revision unavailable");
  albumId_ = albumIdFor(backend);
  output.albumId = albumId_;
  output.revision = albumRevision_;
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::selectImage(
    const std::string& target, voice::ImageSelection& output) {
  std::vector<AlbumCatalogEntry> entries;
  if (!readAllEntries(entries) || entries.empty())
    return voiceFailure("image_not_found", "album is empty");
  size_t selected = entries.size();
  if (target.size() > 1 && target[0] == '@') {
    char* end = nullptr;
    const unsigned long ordinal = std::strtoul(target.c_str() + 1, &end, 10);
    if (end && *end == '\0' && ordinal > 0 && ordinal <= entries.size())
      selected = ordinal - 1;
  } else {
    for (size_t index = 0; index < entries.size(); ++index) {
      if (target == entries[index].id.c_str()) { selected = index; break; }
    }
  }
  if (selected >= entries.size()) return voiceFailure("image_not_found", "exact image target not found");
  const StorageBackendRef backend = selectedBackend();
  if (!queuePage_ || !queuePage_(queuePageContext_, selected, backend))
    return voiceFailure("page_queue_failed", "image refresh could not be queued");
  albumId_ = albumIdFor(backend);
  output.id = entries[selected].id.c_str();
  output.albumId = albumId_;
  output.frameId = entries[selected].id.c_str();
  output.revision = albumRevision_;
  output.ordinal = selected + 1;
  output.total = entries.size();
  pendingDisplayFrame_ = output.frameId;
  pendingDisplayRevision_ = output.revision;
  pendingDisplayOrdinal_ = output.ordinal;
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::deleteImageById(
    const std::string& exactId) {
  if (!stableAssetId(exactId)) return voiceFailure("invalid_image_id", "exact stable image ID required");
  const AlbumMutationResult mutation = runAlbumMutation([&]() {
    return album_.deleteUserAsset(selectedBackend(), exactId.c_str());
  });
  if (mutation == AlbumMutationResult::RevisionPersistenceFailed ||
      mutation == AlbumMutationResult::RevisionUnavailable)
    return voiceFailure("album_revision_persist_failed", "album revision is unavailable");
  if (mutation != AlbumMutationResult::Complete)
    return voiceFailure("image_delete_failed", "factory, missing, or storage failure");
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::clearAllUserImages() {
  const AlbumMutationResult mutation = runAlbumMutation([&]() {
    return album_.clearUserAssets(selectedBackend());
  });
  if (mutation == AlbumMutationResult::RevisionPersistenceFailed ||
      mutation == AlbumMutationResult::RevisionUnavailable)
    return voiceFailure("album_revision_persist_failed", "album revision is unavailable");
  if (mutation != AlbumMutationResult::Complete)
    return voiceFailure("album_clear_failed", "user image cleanup failed");
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::setVolumePercent(uint8_t value) {
  if (value > 100) return voiceFailure("invalid_volume", "volume must be 0..100");
  portal::PortalSettings next = portal_.settings();
  next.volume = value;
  std::string error;
  if (!portal_.replaceSettings(next, &error))
    return voiceFailure("volume_persist_failed", error);
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::formatStorage(
    const std::string& exactStorageId) {
  if (exactStorageId != "sd")
    return voiceFailure("format_target_rejected", "only exact storage ID sd is format-capable");
  const AlbumMutationResult mutation = runAlbumMutation([&]() {
    return sd_.formatFat();
  });
  if (mutation == AlbumMutationResult::RevisionPersistenceFailed ||
      mutation == AlbumMutationResult::RevisionUnavailable)
    return voiceFailure("album_revision_persist_failed", "album revision is unavailable");
  if (mutation != AlbumMutationResult::Complete)
    return voiceFailure("sd_format_failed", "FAT format or remount failed");
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::setAssistantPrompt(
    const std::string& prompt) {
  if (prompt.empty() || prompt.size() > 512)
    return voiceFailure("invalid_prompt", "assistant prompt must be 1..512 bytes");
  portal::PortalSettings next = portal_.settings();
  next.assistantPrompt = prompt;
  std::string error;
  if (!portal_.replaceSettings(next, &error))
    return voiceFailure("prompt_persist_failed", error);
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::applyAssistantPromptRuntime(
    const std::string& prompt) {
  if (prompt.empty() || prompt.size() > 2048)
    return voiceFailure("invalid_prompt", "assistant prompt must be 1..2048 bytes");
  if (prompt == appliedSystemPrompt_) return voice::Status::ok();
  if (voiceWasReady_) {
    const myai::Status disconnected = myAi_.disconnectVoice("prompt_update");
    if (!disconnected.ok()) return voiceFailure("prompt_disconnect_failed", disconnected.detail);
    voiceWasReady_ = false;
    voiceConnectPending_ = false;
    voiceConnectDeadline_ = 0;
    voice_.onSessionLost(voiceFailure(
        "prompt_session_restart", "voice session is restarting with the new prompt"));
  }
  const myai::Status applied = myAi_.setSystemPrompt(prompt);
  if (!applied.ok()) return voiceFailure("prompt_update_failed", applied.detail);
  appliedSystemPrompt_ = prompt;
  voiceReconnectAt_ = millis();
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::setImageSetting(
    const std::string& key, const std::string& value) {
  portal::PortalSettings next = portal_.settings();
  if (key == "size") {
    if (value == "400x600") {
      next.image.width = 400;
      next.image.height = 600;
    } else if (value == "600x400") {
      next.image.width = 600;
      next.image.height = 400;
    } else {
      return voiceFailure(
          "invalid_image_size", "size must be 400x600 or 600x400");
    }
  }
  else if (key == "steps") {
    const int steps = std::atoi(value.c_str());
    if (steps < 1 || steps > 50)
      return voiceFailure("invalid_image_steps", "steps must be 1..50");
    next.image.steps = static_cast<uint8_t>(steps);
  } else if (key == "negative_prompt") {
    if (value.size() > 384)
      return voiceFailure(
          "invalid_negative_prompt", "negative prompt must be at most 384 bytes");
    next.image.negativePrompt = value;
  } else if (key == "prompt_template") {
    if (value.empty() || value.size() > 512)
      return voiceFailure(
          "invalid_image_prompt", "image prompt template must be 1..512 bytes");
    next.imagePromptTemplate = value;
  } else if (key == "model") {
    // The device has no source-image capture contract, so only text-to-image
    // is a truthful local setting. Do not silently pretend i2i was applied.
    if (value != "t2i")
      return voiceFailure(
          "image_model_not_supported", "PaperColor currently supports t2i only");
  } else {
    return voiceFailure("unknown_image_setting", key);
  }
  std::string error;
  if (!portal_.replaceSettings(next, &error))
    return voiceFailure("image_setting_persist_failed", error);
  return voice::Status::ok();
}

voice::Status PaperColorApplicationRuntime::resetTarget(
    const std::string&) {
  return voiceFailure("reset_requires_reflash", "credential reset is available through USB recovery only");
}

void PaperColorApplicationRuntime::onRuntimeState(voice::RuntimeState state) {
  Diagnostics::event("VOICE_STATE", String(static_cast<int>(state)));
}

void PaperColorApplicationRuntime::onCommandResult(
    const std::string& commandName, const std::string& detail) {
  Diagnostics::event("VOICE_TOOL", String(commandName.c_str()) + ":" + detail.c_str());
  appendMyAiChatMessage(
      "tool", commandName + (detail.empty() ? std::string() : ": " + detail));
}

void PaperColorApplicationRuntime::onConfirmationRequired(
    const std::string& exactPhrase, bool physicalAlsoRequired, uint64_t) {
  Diagnostics::event("VOICE_CONFIRM", exactPhrase.c_str());
  if (physicalAlsoRequired)
    Diagnostics::event("VOICE_CONFIRM_PHYSICAL", "PRESS_TOP_BUTTON");
}

void PaperColorApplicationRuntime::onError(const voice::Status& status) {
  Diagnostics::event("VOICE_ERROR", String(status.code.c_str()) + ":" + status.detail.c_str());
}

uint64_t PaperColorApplicationRuntime::parseIsoEpoch(const std::string& value) {
  if (value.size() < 20 || value.size() > 64) return 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
             &year, &month, &day, &hour, &minute, &second) != 6 ||
      year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60) return 0;
  // Proleptic Gregorian days since 1970-01-01, independent of the device's
  // configured local timezone. MyAI expirations are ISO-8601 UTC values.
  int adjustedYear = year - (month <= 2 ? 1 : 0);
  const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
  const unsigned adjustedMonth = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
  const unsigned dayOfYear = (153U * adjustedMonth + 2U) / 5U +
      static_cast<unsigned>(day - 1);
  const unsigned dayOfEra = yearOfEra * 365U + yearOfEra / 4U -
      yearOfEra / 100U + dayOfYear;
  const int64_t days = static_cast<int64_t>(era) * 146097LL +
      static_cast<int64_t>(dayOfEra) - 719468LL;
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400ULL +
      static_cast<uint64_t>(hour) * 3600ULL +
      static_cast<uint64_t>(minute) * 60ULL + static_cast<uint64_t>(second);
}

std::string PaperColorApplicationRuntime::candidateCode() {
  char result[7] = {};
  const uint32_t random = esp_random() % 1000000U;
  snprintf(result, sizeof(result), "%06lu", static_cast<unsigned long>(random));
  return result;
}

bool PaperColorApplicationRuntime::parseCursor(
    const std::string& cursor, size_t* offset) {
  if (!offset) return false;
  *offset = 0;
  if (cursor.empty()) return true;
  if (cursor.size() > 8) return false;
  size_t value = 0;
  for (char character : cursor) {
    if (character < '0' || character > '9') return false;
    value = value * 10U + static_cast<size_t>(character - '0');
    if (value > 96) return false;
  }
  *offset = value;
  return true;
}

std::string PaperColorApplicationRuntime::albumIdFor(
    const StorageBackendRef& backend) {
  return std::string("album:") + (backend.valid() ? backend.identity : "missing");
}

displaypower::PowerSnapshotResult PaperColorApplicationRuntime::capturePowerHook(
    void* context, displaypower::PowerInputs* inputs) {
  PaperColorApplicationRuntime* runtime =
      static_cast<PaperColorApplicationRuntime*>(context);
  if (!runtime || !inputs) {
    return displaypower::PowerSnapshotResult::InvalidTarget;
  }
  inputs->nowMilliseconds = millis();
  inputs->lastMeaningfulActivityMilliseconds =
      runtime->activityState_.lastMeaningfulActivityMilliseconds();
  const time_t now = time(nullptr);
  inputs->rtcSynchronized = now >= 1700000000;
  inputs->rtcNowEpochSeconds = inputs->rtcSynchronized
      ? static_cast<uint64_t>(now) : 0;
  inputs->nextHeartbeatEpochSeconds = inputs->rtcSynchronized
      ? inputs->rtcNowEpochSeconds + 300U : 0;
  inputs->wakeButtonsReleased = !M5.BtnA.isPressed() && !M5.BtnB.isPressed() &&
      !M5.BtnC.isPressed();
  inputs->blockers.audioActive = runtime->streamingAudio_.active() ||
      runtime->promptPlayer_.busy();
  inputs->blockers.generationActive = runtime->generationActive();
  inputs->blockers.conversionActive = runtime->displayActivity_.refreshBusy();
  const bool uploadActive = runtime->album_.userUploadActive();
  inputs->blockers.writeActive = runtime->displayBusy() || uploadActive;
  inputs->blockers.taskFinalizationActive = runtime->displayTransaction_.active();
  inputs->blockers.voiceActive = runtime->voice_.captureActive() ||
      runtime->voice_.turnActive();
  inputs->blockers.displayActive = runtime->displayBusy();
  inputs->blockers.downloadActive =
      runtime->aigcPhase_ == AigcPhase::Download || uploadActive;
  inputs->blockers.pendingJournal = runtime->displayTransaction_.active();
  const portal::OnboardingState* onboarding = runtime->portal_.onboarding();
  const myai::ActivationState activation = runtime->myAi_.activationState();
  inputs->blockers.pairingActive = myAiPairingTransactionActive(
      runtime->pairingPollActive_, runtime->pairingCallbackPending_,
      activation == myai::ActivationState::Pairing);
  const bool unavailableAndQuiescent = myAiServiceQuiescentAndUnavailable(
      runtime->portalMyAiState_, inputs->blockers.pairingActive);
  inputs->blockers.portalRequestActive = runtime->portal_.requestActive();
  inputs->blockers.tutorialActive = !unavailableAndQuiescent &&
      (runtime->tutorialNarrationPending_ ||
       runtime->tutorialNarrationInFlight_ ||
       (onboarding && !onboarding->tutorialComplete()));
  inputs->blockers.onboardingActive = onboardingBlocksSleep(
      runtime->myAiAuthorized_, runtime->inkloopClient_.paired(),
      onboarding && onboarding->myAiActive(),
      onboarding && onboarding->inkloopBound(), unavailableAndQuiescent);
  inputs->blockers.portalActive = inputs->blockers.pairingActive ||
      inputs->blockers.onboardingActive ||
      inputs->blockers.portalRequestActive ||
      inputs->blockers.tutorialActive;
  inputs->blockers.externalPagePending =
      runtime->activityState_.externalPagePending();
  inputs->blockers.unacknowledgedTask = runtime->displayTransaction_.active();
  // Active interaction is already a fail-closed decision; a damaged task
  // store must not hide the more actionable blocker or allow sleep.
  if (inputs->blockers.any()) {
    return displaypower::PowerSnapshotResult::Captured;
  }
  if (!inputs->rtcSynchronized) {
    return displaypower::PowerSnapshotResult::ClockUnsynchronized;
  }
  if (!runtime->tasks_.ready()) {
    return displaypower::PowerSnapshotResult::TaskStoreUnavailable;
  }
  uint64_t nextTask = 0;
  if (!runtime->tasks_.nextDueEpoch(now, nextTask)) {
    return displaypower::PowerSnapshotResult::TaskScheduleInvalid;
  }
  inputs->nextLocalTaskEpochSeconds = nextTask;
  if (nextTask && nextTask <= inputs->rtcNowEpochSeconds)
    inputs->blockers.unacknowledgedTask = true;
  return displaypower::PowerSnapshotResult::Captured;
}

bool PaperColorApplicationRuntime::finalizePowerHook(void* context) {
  PaperColorApplicationRuntime* runtime =
      static_cast<PaperColorApplicationRuntime*>(context);
  if (!runtime) return false;
  if (runtime->displayTransaction_.active() &&
      !runtime->displayTransaction_.ambiguous()) {
    runtime->displayTransaction_.retryFinalize();
  }
  return !runtime->displayTransaction_.active() && !runtime->displayBusy();
}

bool PaperColorApplicationRuntime::stopAudioPowerHook(void* context) {
  PaperColorApplicationRuntime* runtime =
      static_cast<PaperColorApplicationRuntime*>(context);
  if (!runtime) return false;
  runtime->voice_.setEnabled(false);
  runtime->streamingAudio_.abort();
  M5.Mic.end();
  M5.Speaker.stop();
  return !M5.Mic.isRecording() && !M5.Speaker.isPlaying();
}

bool PaperColorApplicationRuntime::closeNetworkPowerHook(void* context) {
  PaperColorApplicationRuntime* runtime =
      static_cast<PaperColorApplicationRuntime*>(context);
  if (!runtime) return false;
  const myai::Status voice = runtime->myAi_.disconnectVoice("deep_sleep");
  const myai::Status image = runtime->myAi_.disconnectImage("deep_sleep");
  runtime->websocket_.close(1000, "deep_sleep");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  return (voice.ok() || voice.code == myai::ErrorCode::InvalidState) &&
      (image.ok() || image.code == myai::ErrorCode::InvalidState);
}

bool PaperColorApplicationRuntime::reconnectWifiPowerHook(void*) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.reconnect();
  return false;
}

bool PaperColorApplicationRuntime::syncInkloopPowerHook(void* context) {
  PaperColorApplicationRuntime* runtime =
      static_cast<PaperColorApplicationRuntime*>(context);
  if (!runtime) return false;
  if (!runtime->inkloopClient_.deviceId().length()) return true;
  const SyncResult result = runtime->inkloopClient_.syncTasks();
  return result.ok;
}

bool PaperColorApplicationRuntime::connectVoiceIfAuthorized() {
  if (!myAiAuthorized_ || WiFi.status() != WL_CONNECTED ||
      voiceConnectPending_) return false;
  voiceConnectPending_ = true;
  voiceConnectDeadline_ = 0;
  const myai::Status connected = myAi_.connectVoice();
  if (!connected.ok()) {
    voiceConnectPending_ = false;
    voiceConnectDeadline_ = 0;
    voiceReconnectAt_ = millis() +
        boundedVoiceReconnectDelay(connected.retryAfterMs);
    return false;
  }
  // Gateway authorization, model preference lookup, candidate probing and
  // session selection are synchronous on ESP32.  Start the WebSocket handshake
  // budget only after those HTTP steps finish, otherwise a healthy socket can
  // be torn down immediately after its 101 response.
  voiceConnectDeadline_ = millis() + kVoiceConnectHandshakeTimeoutMs;
  lastHeartbeatAt_ = millis();
  return true;
}

void PaperColorApplicationRuntime::pollPairing() {
  if (!pairingPollActive_ || millis() - lastPairingPollAt_ < 3000U) return;
  lastPairingPollAt_ = millis();
  bool bound = false;
  const myai::Status status = myAi_.pollPairing(bound);
  if (!status.ok()) {
    if (status.code == myai::ErrorCode::PairingExpired ||
        status.code == myai::ErrorCode::Unauthorized ||
        status.code == myai::ErrorCode::PaymentRequired ||
        status.code == myai::ErrorCode::RecoveryRequired ||
        status.code == myai::ErrorCode::InvalidState) {
      pairingPollActive_ = false;
      if (status.code == myai::ErrorCode::PairingExpired ||
          status.code == myai::ErrorCode::Unauthorized) {
        const myai::Status cancelled = myAi_.cancelPairing();
        if (cancelled.ok()) {
          clearPendingPairingMaterial();
          std::string error;
          if (!portal_.onMyAiPairingCancelled(&error))
            Diagnostics::event("ERROR", String("PAIRING_PORTAL_RESET:") +
                error.c_str());
        }
      }
    }
    return;
  }
  if (!bound) return;
  pairingPollActive_ = false;
  bool active = false;
  const myai::Status checked = myAi_.checkAuthorization(active);
  myAiAuthorized_ = checked.ok() && active;
  std::string error;
  if (!portal_.onMyAiActivation(myAiAuthorized_, &error)) {
    Diagnostics::event("ERROR", String("MYAI_PORTAL_ACTIVATION:") +
        error.c_str());
  } else {
    reconcileTerminalBindingState();
  }
  voice_.setEnabled(myAiAuthorized_);
  if (myAiAuthorized_) {
    const portal::OnboardingState* onboarding = portal_.onboarding();
    tutorialNarrationPending_ = onboarding && !onboarding->tutorialComplete();
    voiceConnectPending_ = false;
    voiceReconnectAt_ = millis();
    Diagnostics::event("VOICE_TUTORIAL", "PRESS_TOP_BUTTON_TO_TALK");
  }
}

void PaperColorApplicationRuntime::pollAigc() {
  if (aigcPhase_ == AigcPhase::Idle) return;
  myai::Status status;
  if (aigcPhase_ == AigcPhase::Start) {
    Diagnostics::event("AIGC_PHASE", "STARTING");
    aigcRequest_ = myai::ImageRequest();
    const portal::ImageGenerationSettings& image = portal_.settings().image;
    const std::string orientedSubject =
        std::string("当前所选屏幕尺寸与方向：") +
        std::to_string(image.width) + "x" + std::to_string(image.height) +
        "。" + aigcPrompt_;
    if (!composeImagePrompt(
            portal_.settings().imagePromptTemplate, orientedSubject,
            &aigcRequest_.prompt)) {
      status = myai::Status(
          myai::ErrorCode::InvalidArgument, 0,
          "local image prompt template is invalid or too large");
    }
    aigcRequest_.negativePrompt = portal_.settings().image.negativePrompt;
    aigcRequest_.size = image.width == 600 ? "600x400" : "400x600";
    aigcRequest_.steps = portal_.settings().image.steps;
    aigcRequest_.maxEncodedBytes = 2U * 1024U * 1024U;
    aigcRequest_.maxDecodedBytes = kMaxFrameBytes;
    if (status.ok()) {
      appendMyAiChatMessage(
          "tool",
          std::string("AIGC 实际请求\n尺寸：") + aigcRequest_.size +
              "\n步数：" + std::to_string(aigcRequest_.steps) +
              "\n正向提示词：" + aigcRequest_.prompt +
              "\n负向提示词：" + aigcRequest_.negativePrompt);
    }
    if (status.ok()) status = myAi_.startImage(aigcRequest_, aigcGenerated_);
    if (status.ok()) {
      aigcPhase_ = AigcPhase::Poll;
      nextAigcPollAt_ = millis() + 5000U;
      Diagnostics::event("AIGC_PHASE", "SUBMITTED");
      appendMyAiChatMessage("tool", "AIGC 已提交，开始每 5 秒查询生成状态。");
    }
  } else if (aigcPhase_ == AigcPhase::Poll) {
    if (static_cast<int32_t>(millis() - nextAigcPollAt_) < 0) return;
    nextAigcPollAt_ = millis() + 5000U;
    status = myAi_.pollImage(aigcGenerated_.promptId, aigcStatus_);
    if (status.ok() && (aigcStatus_.status == "completed" ||
                        aigcStatus_.status == "complete" ||
                        aigcStatus_.status == "succeeded") &&
        !aigcStatus_.outputs.empty()) {
      aigcPhase_ = AigcPhase::Download;
      Diagnostics::event("AIGC_PHASE", "GENERATION_COMPLETE");
      appendMyAiChatMessage("tool", "AIGC 生成完成，正在下载并校验 PNG。");
    } else if (status.ok()) {
      return;
    }
  } else if (aigcPhase_ == AigcPhase::Download) {
    myai::AigcOutputMetadata metadata;
    imageSink_.setMaximumBytes(kMaxFrameBytes);
    const AlbumMutationResult mutation = runAlbumMutation([&]() {
      status = myAi_.downloadImage(
          aigcGenerated_.promptId, aigcStatus_.outputs.front(), aigcRequest_,
          imageSink_, metadata);
      return status.ok() && imageSink_.takeCommittedAsset(aigcAsset_);
    }, false);
    if (mutation == AlbumMutationResult::Complete) {
      setImageLed(displaypower::ImageLedState::Caching);
      aigcPhase_ = AigcPhase::Display;
      Diagnostics::event("AIGC_PHASE", "CACHED");
      appendMyAiChatMessage(
          "tool",
          std::string("AIGC 已写入相册；asset=") +
              aigcAsset_.id.c_str() + "，bytes=" +
              std::to_string(aigcAsset_.bytes));
    } else if (mutation == AlbumMutationResult::RevisionPersistenceFailed ||
               mutation == AlbumMutationResult::RevisionUnavailable) {
      status = myai::Status(
          myai::ErrorCode::Storage, 0,
          "album revision persistence blocked AIGC cache promotion");
    } else if (status.ok()) {
      status = myai::Status(
          myai::ErrorCode::Storage, 0,
          "AIGC output did not commit to the album");
    }
  } else if (aigcPhase_ == AigcPhase::Display) {
    if (displayBusy()) return;
    Diagnostics::event("AIGC_PHASE", "DISPLAY_START");
    DownloadedFrame frame;
    AlbumAsset asset;
    const bool loaded = album_.loadPage(
        aigcAsset_.backend, aigcAsset_.page, frame, asset);
    const bool shown = loaded && refreshFrame(
        asset.id, frame.bytes, frame.length, false, asset.renderStrategy);
    frame.release();
    if (shown) {
      album_.markCurrent(asset.backend, asset.id);
      setImageLed(displaypower::ImageLedState::Complete);
      Diagnostics::event("AIGC_PHASE", "DISPLAY_COMPLETE");
      appendMyAiChatMessage("tool", "AIGC 图片已完成上屏。");
    } else {
      setImageLed(displaypower::ImageLedState::Error);
      Diagnostics::event("AIGC_PHASE", "DISPLAY_FAILED");
      appendMyAiChatMessage("tool", "AIGC 图片已缓存，但上屏失败。");
    }
    myAi_.disconnectImage(shown ? "complete" : "display_failed");
    aigcPhase_ = AigcPhase::Idle;
    aigcPrompt_.clear();
    return;
  }
  if (!status.ok()) {
    Diagnostics::event("AIGC_ERROR", status.detail.c_str());
    appendMyAiChatMessage(
        "tool", std::string("AIGC 失败：") +
            (status.detail.empty()
                 ? std::to_string(static_cast<int>(status.code))
                 : status.detail));
    setImageLed(displaypower::ImageLedState::Error);
    myAi_.disconnectImage("failed");
    imageSink_.abort();
    aigcPhase_ = AigcPhase::Idle;
    aigcPrompt_.clear();
  }
}

AlbumMutationResult PaperColorApplicationRuntime::runAlbumMutation(
    const std::function<bool()>& mutation,
    bool dispatchMutation) {
  return runRevisionGatedAlbumMutation(
      albumRevision_,
      [this](uint64_t next) {
        struct RevisionWork {
          PaperColorApplicationRuntime* runtime;
          uint64_t next;
          bool stored;
        } work{this, next, false};
        const bool dispatched = responsiveWorkExecutor().execute(
            ResponsiveWorkKind::StorageHardware,
            [](void* raw) {
              RevisionWork* item = static_cast<RevisionWork*>(raw);
              item->stored = item->runtime->storeAlbumRevision(item->next);
            },
            &work);
        return dispatched && work.stored;
      },
      [this](uint64_t revision, bool healthy) {
        publishAlbumRevision(revision, healthy);
      },
      [&mutation, dispatchMutation]() {
        if (!dispatchMutation) return mutation();
        struct MutationWork {
          const std::function<bool()>* operation;
          bool completed;
        } work{&mutation, false};
        const bool dispatched = responsiveWorkExecutor().execute(
            ResponsiveWorkKind::StorageHardware,
            [](void* raw) {
              MutationWork* item = static_cast<MutationWork*>(raw);
              item->completed = (*item->operation)();
            },
            &work);
        return dispatched && work.completed;
      },
      nullptr);
}

void PaperColorApplicationRuntime::publishAlbumRevision(
    uint64_t revision, bool healthy) {
  albumRevisionHealthy_ = healthy;
  if (!healthy) {
    Diagnostics::event("ERROR", "ALBUM_REVISION_PERSIST_FAILED");
    // Zero is never a valid confirmation binding, so this terminally clears
    // any pending authority without pretending the failed value was durable.
    voice_.onAlbumRevisionChanged(albumId_, 0);
    return;
  }
  albumRevision_ = revision;
  albumId_ = albumIdFor(selectedBackend());
  voice_.onAlbumRevisionChanged(albumId_, albumRevision_);
}

bool PaperColorApplicationRuntime::reserveAlbumRevisionForScheduledCache() {
  if (album_.userUploadActive()) return false;
  const AlbumMutationResult result = runAlbumMutation([]() { return true; });
  return result == AlbumMutationResult::Complete;
}

bool PaperColorApplicationRuntime::loadAlbumRevision() {
  Preferences preferences;
  if (!preferences.begin("ink-album-meta", false)) return false;
  const uint8_t head = preferences.getUChar("head", 0);
  uint64_t revision = preferences.getULong64(head == 2 ? "rev-b" : "rev-a", 0);
  if (!revision) revision = preferences.getULong64(head == 2 ? "rev-a" : "rev-b", 0);
  if (!revision) {
    revision = 1;
    if (preferences.putULong64("rev-a", revision) != sizeof(uint64_t) ||
        preferences.putUChar("head", 1) != 1) {
      preferences.end();
      return false;
    }
  }
  preferences.end();
  albumRevision_ = revision;
  albumRevisionHealthy_ = true;
  albumId_ = albumIdFor(selectedBackend());
  return true;
}

bool PaperColorApplicationRuntime::storeAlbumRevision(uint64_t next) {
  Preferences preferences;
  if (!preferences.begin("ink-album-meta", false)) return false;
  const uint8_t head = preferences.getUChar("head", 1);
  const uint8_t target = head == 1 ? 2 : 1;
  const char* key = target == 1 ? "rev-a" : "rev-b";
  const bool success = preferences.putULong64(key, next) == sizeof(uint64_t) &&
      preferences.getULong64(key, 0) == next &&
      preferences.putUChar("head", target) == 1;
  preferences.end();
  return success;
}

void PaperColorApplicationRuntime::clearPendingPairingMaterial() {
  secureClear(pendingPairing_.onboardingCode);
  secureClear(pendingPairing_.bindingUrl);
  secureClear(pendingPairing_.expiresAt);
  pendingPairing_ = myai::PairingView();
  pairingCallbackPending_ = false;
  pairingCallbackRetryAt_ = 0;
  inkloopMirrorRetryAt_ = 0;
  pairingPollActive_ = false;
}

bool PaperColorApplicationRuntime::loadPairingScreenScrubbed(bool& scrubbed) {
  scrubbed = false;
  Preferences preferences;
  if (!preferences.begin(kPairingDisplayNamespace, false)) return false;
  if (!preferences.isKey(kPairingDisplayScrubbed)) {
    preferences.end();
    return true;
  }
  const uint8_t value = preferences.getUChar(kPairingDisplayScrubbed, 0xff);
  preferences.end();
  if (value > 1) return false;
  scrubbed = value == 1;
  return true;
}

bool PaperColorApplicationRuntime::storePairingScreenScrubbed(bool scrubbed) {
  Preferences preferences;
  if (!preferences.begin(kPairingDisplayNamespace, false)) return false;
  const uint8_t value = scrubbed ? 1 : 0;
  const bool stored = preferences.putUChar(kPairingDisplayScrubbed, value) == 1 &&
      preferences.getUChar(kPairingDisplayScrubbed, 0xff) == value;
  preferences.end();
  return stored;
}

void PaperColorApplicationRuntime::reconcileTerminalBindingState() {
  const portal::OnboardingState* state = portal_.onboarding();
  if (!state || !state->terminalBindingComplete()) return;
  clearPendingPairingMaterial();
  bool scrubbed = false;
  if (loadPairingScreenScrubbed(scrubbed) && scrubbed) {
    terminalDisplayScrubPending_ = false;
    terminalDisplayScrubApplied_ = true;
    return;
  }
  terminalDisplayScrubPending_ = true;
  terminalDisplayScrubApplied_ = false;
  terminalDisplayScrubRetryAt_ = millis();
}

void PaperColorApplicationRuntime::tryTerminalDisplayScrub() {
  if (!terminalDisplayScrubPending_) return;
  const portal::OnboardingState* state = portal_.onboarding();
  if (!state || !state->terminalBindingComplete()) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - terminalDisplayScrubRetryAt_) < 0) return;
  if (terminalDisplayScrubApplied_) {
    if (storePairingScreenScrubbed(true)) {
      terminalDisplayScrubPending_ = false;
    } else {
      terminalDisplayScrubRetryAt_ = now + 5000U;
    }
    return;
  }
  if (!displayRuntime_ || displayBusy()) {
    terminalDisplayScrubRetryAt_ = now + 1000U;
    return;
  }
  GeneratedStatusPng readyScreen;
  if (!makeBoundStatusPng(readyScreen) ||
      !refreshFrame(
          "binding-complete", readyScreen.bytes, readyScreen.length, true)) {
    terminalDisplayScrubRetryAt_ = now + 5000U;
    Diagnostics::event("WARN", "PAIRING_DISPLAY_SCRUB_RETRY");
    return;
  }
  terminalDisplayScrubApplied_ = true;
  if (storePairingScreenScrubbed(true)) {
    terminalDisplayScrubPending_ = false;
    Diagnostics::event("PAIRING_DISPLAY", "BOUND_READY");
  } else {
    terminalDisplayScrubRetryAt_ = millis() + 5000U;
    Diagnostics::event("WARN", "PAIRING_DISPLAY_MARKER_RETRY");
  }
}

StorageBackendRef PaperColorApplicationRuntime::selectedBackend() const {
  return const_cast<StorageManager&>(storage_).assetBackend();
}

bool PaperColorApplicationRuntime::readAllEntries(
    std::vector<AlbumCatalogEntry>& entries) const {
  entries.clear();
  if (album_.userUploadActive()) return false;
  size_t offset = 0;
  do {
    std::vector<AlbumCatalogEntry> page;
    size_t total = 0;
    size_t next = 0;
    if (!const_cast<AlbumStore&>(album_).readCatalogPage(
            selectedBackend(), offset, 16, portal::kMaximumAlbumPageFieldBytes,
            page, total, next)) return false;
    entries.insert(entries.end(), page.begin(), page.end());
    offset = next;
  } while (offset != 0 && entries.size() <= 96);
  return entries.size() <= 96;
}

void PaperColorApplicationRuntime::setImageLed(
    displaypower::ImageLedState state) {
  imageLed_.setImageState(state, millis());
}

}  // namespace inkloop
