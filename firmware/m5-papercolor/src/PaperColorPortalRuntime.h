#pragma once

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>

#include <memory>
#include <string>

#include "InkloopPortal.h"
#include "PortalPersistencePrimitives.h"

namespace inkloop {

class IPaperColorPortalServices {
 public:
  virtual ~IPaperColorPortalServices() {}
  virtual bool startMyAiPairing(std::string* error) = 0;
  virtual bool requestInkloopCodeReuse(
      const std::string& onboardingCode,
      uint64_t expiresAtSeconds,
      std::string* error) = 0;
  virtual bool verifyInkloopBinding(std::string* error) = 0;
  virtual void applyPortalSettings(const portal::PortalSettings& settings) = 0;
  virtual bool testLedRoles(
      bool swapped, uint8_t maximumBrightnessPercent,
      std::string* error) = 0;
  virtual bool executeConfirmedOperation(
      const portal::ConfirmedOperation& operation,
      std::string* error) = 0;
  virtual bool mutationBusy() const = 0;
  virtual portal::StorageStatus storageStatus() const = 0;
  virtual portal::AlbumReadStatus readAlbumPage(
      const portal::AlbumPageRequest& request,
      portal::AlbumPage* page) const = 0;
  virtual portal::AlbumReadStatus findAlbumItem(
      const std::string& assetId,
      portal::AlbumItem* item) const = 0;
  virtual bool displayAlbumItem(
      const std::string& assetId, std::string* error) = 0;
  virtual bool generateImage(
      const std::string& prompt, std::string* error) = 0;
  virtual portal::DiagnosticsSnapshot portalDiagnostics() const = 0;
  virtual bool beginAlbumUpload(
      const std::string& untrustedName,
      size_t declaredImageBytes,
      std::string* error) = 0;
  virtual bool writeAlbumUpload(
      const uint8_t* bytes, size_t length, std::string* error) = 0;
  virtual bool finishAlbumUpload(
      portal::AlbumUploadResult* result, std::string* error) = 0;
  virtual void abortAlbumUpload() = 0;
  virtual bool previewVolume(uint8_t volume, std::string* error) = 0;
};

class PaperColorPortalRuntime final : public portal::IPortalAdapter {
 public:
  explicit PaperColorPortalRuntime(IPaperColorPortalServices& services);

  bool begin(
      PortalIdentityState identity,
      const std::string& defaultLocalManagementPassword = std::string());
  void loop();
  bool ready() const { return portal_ && portal_->ready() && serverStarted_; }
  bool requestActive() const { return requestActive_; }
  const std::string& accessCode() const { return access_.bootNonce; }
  const portal::PortalSettings& settings() const;
  const portal::OnboardingState* onboarding() const {
    return portal_ ? &portal_->onboarding() : nullptr;
  }

  bool onWifiConfigured(bool configured, std::string* error = nullptr);
  bool requestMyAiPairing(std::string* error = nullptr);
  bool onMyAiPairingResumed(std::string* error = nullptr);
  bool onMyAiPairingCancelled(std::string* error = nullptr);
  bool onAuthoritativeMyAiCode(
      const std::string& code,
      uint64_t expiresAtSeconds,
      std::string* error = nullptr);
  bool retryInkloopCodeReuse(std::string* error = nullptr);
  bool onInkloopBound(std::string* error = nullptr);
  bool onMyAiActivation(bool active, std::string* error = nullptr);
  bool onVoiceTutorialComplete(std::string* error = nullptr);
  bool replaceSettings(
      const portal::PortalSettings& settings, std::string* error = nullptr);
  bool confirmPhysical(std::string* error = nullptr);
  bool recoverBoundSnapshot(bool myAiActive, std::string* error = nullptr);

  std::string createNonce(const char* purpose) override;
  bool startMyAiPairing(const char* appId, std::string* error) override;
  bool requestInkloopCodeReuse(
      const std::string& onboardingCode,
      uint64_t expiresAtSeconds,
      std::string* error) override;
  bool persistPortalSnapshot(
      const portal::PortalSnapshotPatch& patch,
      std::string* error) override;
  bool previewVolume(uint8_t volume, std::string* error) override;
  bool testLedRoles(
      bool swapped, uint8_t maximumBrightnessPercent,
      std::string* error) override;
  bool executeConfirmedOperation(
      const portal::ConfirmedOperation& operation,
      std::string* error) override;
  bool mutationBusy() const override;
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
  portal::DiagnosticsSnapshot diagnostics() const override;

 private:
  PortalSnapshotLoadResult loadSnapshot(
      portal::PortalPersistedSnapshot& snapshot);
  bool markSnapshotInitialized();
  bool storeSnapshot(const portal::PortalPersistedSnapshot& snapshot);
  static bool encodeSnapshot(
      const portal::PortalPersistedSnapshot& snapshot,
      std::string& output);
  static bool decodeSnapshot(
      const std::string& input,
      portal::PortalPersistedSnapshot& snapshot);
  static std::string initialManagementPassword(
      const std::string& wifiPassword);
  static bool looksLikeLegacyGeneratedPassword(const std::string& value);
  void handleWebRequest();
  void handleAlbumUploadChunk();
  void finishAlbumUploadRequest();
  portal::PortalRequest requestFromServer(const char* path);
  static uint64_t nowSeconds();

  IPaperColorPortalServices& services_;
  WebServer server_;
  portal::PortalAccessConfig access_;
  std::unique_ptr<portal::InkloopPortal> portal_;
  std::string pendingPhysicalId_;
  bool serverStarted_ = false;
  bool requestActive_ = false;
  bool uploadAuthorized_ = false;
  bool uploadStarted_ = false;
  bool uploadEnded_ = false;
  portal::PortalResponse uploadResponse_;
};

}  // namespace inkloop
