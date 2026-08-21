#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "AlbumStore.h"
#include "AlbumMutationPrimitives.h"
#include "AudioPromptController.h"
#include "CanonicalJsonCodec.h"
#include "DisplayPowerAdapters.h"
#include "DisplayPowerRuntime.h"
#include "DisplayTransaction.h"
#include "InkloopClient.h"
#include "MyAiClient.h"
#include "PaperColorMyAiAdapters.h"
#include "PaperColorPortalRuntime.h"
#include "PaperColorStatusPng.h"
#include "PaperColorVoiceAdapters.h"
#include "SettingsStore.h"
#include "Storage.h"
#include "TaskStore.h"
#include "VoiceRuntime.h"

namespace inkloop {

using QueueAlbumPageHook = bool (*)(
    void* context, size_t page, const StorageBackendRef& backend);

enum class StableStartupDisplay : uint8_t {
  PreserveExisting,
  AwaitingMyAiPairing,
  SettingsReady,
  MyAiServiceUnavailable,
};

class PaperColorApplicationRuntime final
    : public myai::IMyAiEvents,
      public myai::ILocalTranscriptInterceptor,
      public IPaperColorPortalServices,
      public voice::ILocalDeviceActions,
      public voice::IVoiceRuntimeEvents {
 public:
  PaperColorApplicationRuntime(
      StorageManager& storage,
      SdStorage& sd,
      AlbumStore& album,
      TaskStore& tasks,
      InkloopClient& inkloopClient,
      DisplayController& display,
      DisplayTransaction& displayTransaction,
      LedStatusController& leds,
      SettingsStore& settings,
      ButtonRouter& buttons,
      QueueAlbumPageHook queuePage,
      void* queuePageContext);

  bool begin(bool wifiConfigured);
  bool recoverPortalBoundState(std::string* error = nullptr);
  bool activateDisplayOwner();
  void loop();
  bool handleButton(ButtonEvent event);
  bool refreshFrame(
      const String& assetId, const uint8_t* bytes, size_t length,
      bool transactionOwnsCommit = true);
  bool displayBusy() const;
  void onPageDisplayCommitted(
      const String& assetId, size_t oneBasedOrdinal, bool success);
  void notifyPageBusy();
  const std::string& portalAccessCode() const { return portal_.accessCode(); }
  const String& settingsAccessPoint() const { return settingsAccessPoint_; }
  String settingsIpAddress() const { return WiFi.softAPIP().toString(); }
  bool portalReady() const { return portal_.ready(); }
  bool myAiAuthorized() const { return myAiAuthorized_; }
  StableStartupDisplay stableStartupDisplay() const;
  bool ownsDisplay() const { return displayRuntime_ && displayRuntime_->enabled(); }
  bool generationActive() const { return aigcPhase_ != AigcPhase::Idle; }
  bool acceptsUserInput() const;
  void noteExternalActivity(uint32_t nowMilliseconds);
  void setExternalPagePending(bool pending, uint32_t nowMilliseconds);
  bool reserveAlbumRevisionForScheduledCache();

  // MyAI events + local transcript interception.
  void onActivationState(myai::ActivationState state, const myai::Status& status) override;
  void onPairingReady(const myai::PairingView& pairing) override;
  void onVoiceState(myai::VoiceState state) override;
  void onTranscript(const std::string& text, bool final) override;
  void onLocalCommand(const std::string& commandName,
                      const std::string& transcript) override;
  void onVoiceAction(const myai::VoiceEvent& action) override;
  void onAigcState(myai::AigcState state, const std::string& detail) override;
  void onError(const myai::Status& status) override;
  myai::LocalTranscriptDecision inspect(const std::string& transcript) override;

  // Portal services.
  bool startMyAiPairing(std::string* error) override;
  bool requestInkloopCodeReuse(
      const std::string& onboardingCode,
      uint64_t expiresAtSeconds,
      std::string* error) override;
  bool verifyInkloopBinding(std::string* error) override;
  void applyPortalSettings(const portal::PortalSettings& settings) override;
  bool previewVolume(uint8_t volume, std::string* error) override;
  bool testLedRoles(
      bool swapped, uint8_t maximumBrightnessPercent,
      std::string* error) override;
  bool executeConfirmedOperation(
      const portal::ConfirmedOperation& operation,
      std::string* error) override;
  bool mutationBusy() const override;
  bool albumUploadActive() const { return album_.userUploadActive(); }
  portal::StorageStatus storageStatus() const override;
  portal::AlbumReadStatus readAlbumPage(
      const portal::AlbumPageRequest& request,
      portal::AlbumPage* page) const override;
  portal::AlbumReadStatus findAlbumItem(
      const std::string& assetId,
      portal::AlbumItem* item) const override;
  bool displayAlbumItem(
      const std::string& assetId, std::string* error) override;
  bool generateImage(
      const std::string& prompt, std::string* error) override;
  portal::DiagnosticsSnapshot portalDiagnostics() const override;
  bool beginAlbumUpload(
      const std::string& untrustedName,
      size_t declaredImageBytes,
      std::string* error) override;
  bool writeAlbumUpload(
      const uint8_t* bytes, size_t length, std::string* error) override;
  bool finishAlbumUpload(
      portal::AlbumUploadResult* result, std::string* error) override;
  void abortAlbumUpload() override;

  // Local voice tools.
  voice::Status queryFreeSpace(voice::StorageSpace& output) override;
  voice::Status listImages(std::vector<voice::ImageEntry>& output) override;
  voice::Status currentAlbumRevision(voice::AlbumRevision& output) override;
  voice::Status selectImage(
      const std::string& target, voice::ImageSelection& output) override;
  voice::Status deleteImageById(const std::string& exactId) override;
  voice::Status clearAllUserImages() override;
  voice::Status setVolumePercent(uint8_t value) override;
  voice::Status formatStorage(const std::string& exactStorageId) override;
  voice::Status setAssistantPrompt(const std::string& prompt) override;
  voice::Status setImageSetting(
      const std::string& key, const std::string& value) override;
  voice::Status resetTarget(const std::string& exactTargetId) override;

  // Voice runtime events.
  void onRuntimeState(voice::RuntimeState state) override;
  void onCommandResult(
      const std::string& commandName, const std::string& detail) override;
  void onConfirmationRequired(
      const std::string& exactPhrase,
      bool physicalAlsoRequired,
      uint64_t expiresAtMs) override;
  void onError(const voice::Status& status) override;

 private:
  enum class AigcPhase : uint8_t { Idle, Start, Poll, Download, Display };

  static uint64_t parseIsoEpoch(const std::string& value);
  static std::string candidateCode();
  static bool parseCursor(const std::string& cursor, size_t* offset);
  static std::string albumIdFor(const StorageBackendRef& backend);
  static displaypower::PowerSnapshotResult capturePowerHook(
      void* context,
      displaypower::PowerInputs* inputs);
  static bool finalizePowerHook(void* context);
  static bool stopAudioPowerHook(void* context);
  static bool closeNetworkPowerHook(void* context);
  static bool reconnectWifiPowerHook(void* context);
  static bool syncInkloopPowerHook(void* context);
  bool connectVoiceIfAuthorized();
  voice::Status applyAssistantPromptRuntime(const std::string& prompt);
  void pollPairing();
  void pollAigc();
  AlbumMutationResult runAlbumMutation(
      const std::function<bool()>& mutation);
  void publishAlbumRevision(uint64_t revision, bool healthy);
  bool loadAlbumRevision();
  bool storeAlbumRevision(uint64_t next);
  void clearPendingPairingMaterial();
  void reconcileTerminalBindingState();
  void tryTerminalDisplayScrub();
  bool loadPairingScreenScrubbed(bool& scrubbed);
  bool storePairingScreenScrubbed(bool scrubbed);
  StorageBackendRef selectedBackend() const;
  bool readAllEntries(std::vector<AlbumCatalogEntry>& entries) const;
  void setImageLed(displaypower::ImageLedState state);
  bool ledDiagnosticWorkBusy() const;
  bool startPendingLedDiagnostic(std::string* error = nullptr);

  StorageManager& storage_;
  SdStorage& sd_;
  AlbumStore& album_;
  TaskStore& tasks_;
  InkloopClient& inkloopClient_;
  DisplayController& display_;
  DisplayTransaction& displayTransaction_;
  LedStatusController& leds_;
  SettingsStore& settingsStore_;
  ButtonRouter& buttons_;
  QueueAlbumPageHook queuePage_;
  void* queuePageContext_;

  PaperColorClock clock_;
  Esp32HttpsTransport http_;
  Esp32MyAiWebSocket websocket_;
  Esp32AigcOutputTransport aigcOutput_;
  Esp32PublicEndpointSecurity endpointSecurity_;
  NvsMyAiCredentialStore credentials_;
  myai::CanonicalJsonCodec codec_;
  PaperColorStreamingAudio streamingAudio_;
  AlbumImageSink imageSink_;
  myai::ClientConfig myAiConfig_;
  myai::MyAiClient myAi_;

  EmbeddedPromptPlayer promptPlayer_;
  PaperColorVoiceLed voiceLed_;
  PaperColorDisplayActivity displayActivity_;
  PaperColorConversationTransport conversation_;
  voice::AudioPromptController prompts_;
  voice::VoiceRuntime voice_;
  PaperColorPortalRuntime portal_;

  M5FullScreenDisplayAdapter displayAdapter_;
  M5ImageLedAdapter imageLed_;
  M5PngPixelDecoder decoder_;
  FreeRtosDisplayPowerLock displayLock_;
  ArduinoDisplayPowerClock displayClock_;
  std::unique_ptr<displaypower::DisplayRefreshRuntime> displayRuntime_;
  PaperColorPreSleepQuiescenceHooks sleepHooks_;
  Esp32DevicePowerAdapter powerAdapter_;
  displaypower::PowerPolicy powerPolicy_;
  displaypower::SleepAttemptRuntime sleepAttempt_;
  PaperColorWakeRecoveryHooks wakeHooks_;
  displaypower::WakeRecoveryRuntime wakeRecovery_;

  bool initialized_ = false;
  bool myAiAuthorized_ = false;
  bool pairingPollActive_ = false;
  bool pairingCallbackPending_ = false;
  myai::PairingView pendingPairing_;
  uint32_t pairingCallbackRetryAt_ = 0;
  uint32_t inkloopMirrorRetryAt_ = 0;
  bool terminalDisplayScrubPending_ = false;
  bool terminalDisplayScrubApplied_ = false;
  uint32_t terminalDisplayScrubRetryAt_ = 0;
  uint32_t lastPairingPollAt_ = 0;
  uint32_t lastHeartbeatAt_ = 0;
  uint32_t voiceReconnectAt_ = 0;
  uint32_t authorizationRetryAt_ = 0;
  displaypower::RuntimeActivityState activityState_;
  bool voiceWasReady_ = false;
  bool tutorialNarrationPending_ = false;
  bool tutorialNarrationInFlight_ = false;
  bool volumePreviewActive_ = false;
  bool ledDiagnosticPending_ = false;
  bool pendingLedRolesSwapped_ = false;
  uint8_t pendingLedMaximumBrightnessPercent_ = 60;
  myai::VoiceState lastMyAiVoiceState_ = myai::VoiceState::Idle;
  uint64_t albumRevision_ = 1;
  bool albumRevisionHealthy_ = false;
  std::string albumId_;
  std::string pendingDisplayFrame_;
  std::string pendingSystemPrompt_;
  std::string appliedSystemPrompt_;
  std::string portalMyAiState_;
  std::string portalUploadTitle_;
  String settingsAccessPoint_;
  uint64_t pendingDisplayRevision_ = 0;
  uint32_t pendingDisplayOrdinal_ = 0;
  uint32_t lastDisplayRefreshAt_ = 0;

  AigcPhase aigcPhase_ = AigcPhase::Idle;
  std::string aigcPrompt_;
  myai::ImageRequest aigcRequest_;
  myai::AigcGenerateResponse aigcGenerated_;
  myai::AigcStatusResponse aigcStatus_;
  uint32_t nextAigcPollAt_ = 0;
  AlbumAsset aigcAsset_;
};

}  // namespace inkloop
