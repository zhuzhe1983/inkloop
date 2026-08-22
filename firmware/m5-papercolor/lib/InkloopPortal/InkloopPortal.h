#pragma once

#include <map>

#include "OnboardingState.h"

namespace inkloop {
namespace portal {

struct PortalAccessConfig {
  std::string bootNonce;
  std::string sessionId;
  std::string csrfToken;
  std::vector<std::string> allowedOrigins;
  uint32_t sessionLifetimeSeconds;
  PortalRateConfig rate;

  PortalAccessConfig() : sessionLifetimeSeconds(900), rate() {}
};

class InkloopPortal {
 public:
  InkloopPortal(IPortalAdapter& adapter, const PortalAccessConfig& access);

  bool ready() const { return accessValid_ && hydrated_; }
  const PortalSettings& settings() const { return settings_; }
  const OnboardingState& onboarding() const { return onboarding_; }
  uint64_t snapshotRevision() const { return snapshotRevision_; }

  // Must be called exactly once from a trusted, durable-store load before any
  // stateful route or callback. Invalid/future/partial snapshots are rejected
  // without changing the current state.
  bool hydrate(
      const PortalPersistedSnapshot& snapshot,
      uint64_t nowSeconds,
      std::string* error);

  PortalResponse handle(const PortalRequest& request);
  PortalResponse authorizeStreamingAlbumUpload(const PortalRequest& request);
  PortalResponse authorizeStreamingAlbumPreview(const PortalRequest& request);

  // Trusted firmware callbacks. Web routes cannot assert activation, inject a
  // pairing code, or confirm a physical operation.
  bool onWifiConfigured(bool configured, std::string* error);
  // Trusted firmware boot-flow entry points. They use the same durable state
  // transition as the authenticated web route but require no synthetic HTTP
  // request or browser session.
  bool requestMyAiPairing(std::string* error);
  bool requestMyAiRebind(std::string* error);
  bool onMyAiPairingResumed(std::string* error);
  bool onMyAiPairingCancelled(std::string* error);
  bool onAuthoritativeMyAiCode(
      const std::string& onboardingCode,
      uint64_t expiresAtSeconds,
      uint64_t nowSeconds,
      std::string* error);
  bool retryInkloopCodeReuse(std::string* error);
  bool onInkloopBound(std::string* error);
  bool onMyAiActivation(bool active, std::string* error);
  bool onVoiceTutorialComplete(std::string* error);
  // Trusted local-device settings path used by voice tools. It applies the
  // same bounds and durable snapshot contract as the web form.
  bool replaceSettings(const PortalSettings& settings, std::string* error);
  bool confirmPhysical(
      const std::string& confirmationId,
      uint64_t nowSeconds,
      std::string* error);

  // Called only after the device presents a new access PIN (for example after
  // a physical button action). It invalidates every prior portal session.
  bool rotateAccess(const PortalAccessConfig& access, std::string* error);

  std::string renderStateJson() const;
  std::string renderSettingsJson() const;
  std::string renderAlbumJson() const;
  std::string renderDiagnosticsJson(bool includeSerial) const;
  std::string renderDashboardHtml() const;

 private:
  struct PendingAction {
    bool present;
    bool webConfirmed;
    DestructiveAction action;
    std::string target;
    std::string confirmationId;
    std::string phrase;
    uint64_t expiresAtSeconds;

    PendingAction()
        : present(false),
          webConfirmed(false),
          action(DestructiveAction::DeleteAsset),
          expiresAtSeconds(0) {}
  };

  enum class PhysicalConfirmationState : uint8_t {
    Idle,
    BrowserConfirmationRequired,
    AwaitingDeviceButton,
    Complete,
    Failed,
    Expired,
  };

  struct PhysicalConfirmationResult {
    PhysicalConfirmationState state;
    DestructiveAction action;
    uint64_t expiresAtSeconds;
    std::string error;

    PhysicalConfirmationResult()
        : state(PhysicalConfirmationState::Idle),
          action(DestructiveAction::DeleteAsset),
          expiresAtSeconds(0),
          error() {}
  };

  enum class RequestBudget : uint8_t { Read, Write, Destructive };

  struct RateEntry {
    std::string peerIp;
    std::string sessionScope;
    std::string route;
    RequestBudget budget;
    uint32_t windowStarted;
    uint16_t count;

    RateEntry()
        : budget(RequestBudget::Read), windowStarted(0), count(0) {}
  };

  PortalResponse handleSession(const PortalRequest& request);
  PortalResponse handleAuthenticated(const PortalRequest& request);
  PortalResponse handleSettings(
      const PortalRequest& request,
      const std::map<std::string, std::string>& fields);
  PortalResponse handlePrepareAction(
      const PortalRequest& request,
      const std::map<std::string, std::string>& fields);
  PortalResponse handleConfirmAction(
      const PortalRequest& request,
      const std::map<std::string, std::string>& fields);
  PortalResponse renderAlbumResponse(const std::string& cursor) const;
  PortalResponse renderMyAiChatResponse() const;
  PortalResponse renderDashboardResponse() const;

  bool validateAccess(const PortalAccessConfig& access) const;
  bool validateSettings(const PortalSettings& settings, std::string* error) const;
  bool validateSnapshot(
      const PortalPersistedSnapshot& snapshot,
      std::string* error) const;
  PortalPersistedSnapshot makeSnapshot(
      const OnboardingState& onboarding,
      const PortalSettings& settings,
      uint64_t revision) const;
  bool persistState(
      const OnboardingState& onboarding,
      const PortalSettings& settings,
      uint32_t dirtyFields,
      std::string* error);
  bool applyExpiredCode(uint64_t nowSeconds, std::string* error);
  bool hostAllowed(const std::string& host) const;
  bool originAllowed(const std::string& origin) const;
  bool hasSessionCookie(const std::string& cookie) const;
  bool sessionAuthorized(const PortalRequest& request) const;
  bool mutationAuthorized(const PortalRequest& request) const;
  bool parseFields(
      const PortalRequest& request,
      std::map<std::string, std::string>* fields,
      PortalResponse* failure) const;
  bool assetExistsAndMutable(const std::string& assetId) const;
  bool parseAlbumCursor(
      const std::string& path,
      std::string* cursor,
      PortalResponse* failure) const;
  AlbumReadStatus readValidatedAlbumPage(
      const std::string& cursor,
      AlbumPage* page) const;
  static PortalResponse albumReadError(AlbumReadStatus status, bool html);
  bool validPeerIp(const std::string& peerIp) const;
  std::string normalizedRateRoute(const std::string& path) const;
  RequestBudget requestBudget(const PortalRequest& request) const;
  bool consumeRate(
      const PortalRequest& request,
      PortalResponse* rejected);
  void clearRateEntries();
  void expirePending(uint64_t nowSeconds);
  void clearPending();
  std::string renderPhysicalConfirmationJson() const;

  static PortalResponse jsonResponse(int status, const std::string& body);
  static PortalResponse htmlResponse(int status, const std::string& body);
  static PortalResponse errorResponse(int status, const std::string& code);

  IPortalAdapter& adapter_;
  PortalAccessConfig access_;
  bool accessValid_;
  bool hydrated_;
  bool sessionIssued_;
  uint64_t sessionExpiresAtSeconds_;
  uint64_t snapshotRevision_;
  PortalSettings settings_;
  OnboardingState onboarding_;
  PendingAction pending_;
  PhysicalConfirmationResult physicalResult_;
  bool managementPasswordRestartRequired_;
  std::vector<RateEntry> rateEntries_;
};

}  // namespace portal
}  // namespace inkloop
