import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const portalSource = new URL(
  "../firmware/m5-papercolor/lib/InkloopPortal/",
  import.meta.url,
);

test("PaperColor portal state, routes, safety gates, and escaping run under C++11", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-papercolor-portal-"));
  const harnessPath = join(temporaryDirectory, "portal_test.cpp");
  const executablePath = join(temporaryDirectory, "portal_test");
  const harness = String.raw`
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "InkloopPortal.h"

using namespace inkloop::portal;

PortalPersistedSnapshot testFreshSnapshot() {
  PortalPersistedSnapshot snapshot = makeFreshPortalSnapshot();
  snapshot.settings.localManagementPassword = "INKLOOP7K9Q2";
  return snapshot;
}

class FakeAdapter final : public IPortalAdapter {
 public:
  int nonceCount = 0;
  int pairingStarts = 0;
  int pairingRestarts = 0;
  int reuseRequests = 0;
  int settingsSaves = 0;
  int audioPreviews = 0;
  uint8_t previewedVolume = 0;
  int ledTests = 0;
  uint8_t testedLedBrightness = 0;
  int destructiveExecutions = 0;
  int displayRequests = 0;
  int generateRequests = 0;
  bool busy = false;
  bool failPersistence = false;
  bool failReuse = false;
  std::string reportedMyAiState = "bound";
  std::string lastNonce;
  std::string lastAppId;
  std::string lastReusedCode;
  ConfirmedOperation lastOperation;
  PortalSettings savedSettings;
  PortalPersistedSnapshot durableSnapshot;
  uint32_t lastDirtyFields = 0;
  std::vector<AlbumItem> album;
  AlbumReadStatus forcedAlbumStatus = AlbumReadStatus::Ok;
  bool bypassAlbumBounds = false;
  mutable int albumPageReads = 0;
  mutable int albumLookups = 0;

  FakeAdapter() {
    durableSnapshot = testFreshSnapshot();
    AlbumItem safe;
    safe.id = "sha256:abc_123";
    safe.title = "<script>alert(1)</script>";
    safe.origin = "inkloop";
    safe.bytes = 42;
    safe.current = true;
    album.push_back(safe);

    AlbumItem factory;
    factory.id = "factory:tutorial";
    factory.title = "Tutorial";
    factory.origin = "factory";
    factory.factoryAsset = true;
    album.push_back(factory);
  }

  std::string createNonce(const char* purpose) override {
    ++nonceCount;
    lastNonce = std::string("nonce-") + purpose + "-1234567890-" + std::to_string(nonceCount);
    return lastNonce;
  }
  bool startMyAiPairing(const char* appId, std::string*) override {
    ++pairingStarts;
    lastAppId = appId;
    return true;
  }
  bool restartMyAiPairing(const char* appId, std::string*) override {
    ++pairingRestarts;
    lastAppId = appId;
    return true;
  }
  bool requestInkloopCodeReuse(
      const std::string& code, uint64_t, std::string* error) override {
    ++reuseRequests;
    lastReusedCode = code;
    if (failReuse) {
      if (error) *error = "reuse_failed_without_fallback";
      return false;
    }
    return true;
  }
  bool persistPortalSnapshot(
      const PortalSnapshotPatch& patch, std::string* error) override {
    if (failPersistence) {
      if (error) *error = "settings_backend_failed";
      return false;
    }
    assert(patch.schemaVersion == kPortalSnapshotSchemaVersion);
    assert(patch.expectedRevision == durableSnapshot.revision);
    assert(patch.nextRevision == patch.expectedRevision + 1);
    assert(patch.mergedSnapshot.revision == patch.nextRevision);
    lastDirtyFields = patch.dirtyFields;
    const uint32_t settingsMask = SnapshotStorageTarget | SnapshotVolume |
        SnapshotAssistantPrompt | SnapshotImagePromptTemplate |
        SnapshotLocalManagementPassword | SnapshotImageSettings |
        SnapshotLedRoles | SnapshotRefreshMode | SnapshotPowerMode |
        SnapshotIdleTimeout;
    if ((patch.dirtyFields & settingsMask) != 0) ++settingsSaves;
    savedSettings = patch.mergedSnapshot.settings;
    durableSnapshot = patch.mergedSnapshot;
    return true;
  }
  bool previewVolume(uint8_t volume, std::string* error) override {
    if (busy) {
      if (error) *error = "audio_busy";
      return false;
    }
    ++audioPreviews;
    previewedVolume = volume;
    return true;
  }
  bool testLedRoles(
      bool, uint8_t maximumBrightnessPercent,
      std::string* error) override {
    if (busy) {
      if (error) *error = "device_busy";
      return false;
    }
    ++ledTests;
    testedLedBrightness = maximumBrightnessPercent;
    return true;
  }
  bool executeConfirmedOperation(
      const ConfirmedOperation& operation, std::string*) override {
    ++destructiveExecutions;
    lastOperation = operation;
    return true;
  }
  bool displayAlbumItem(
      const std::string& assetId, std::string* error) override {
    if (busy) {
      if (error) *error = "device_busy";
      return false;
    }
    ++displayRequests;
    lastOperation.target = assetId;
    return true;
  }
  bool generateImage(
      const std::string& prompt, std::string* error) override {
    if (busy) {
      if (error) *error = "device_busy";
      return false;
    }
    ++generateRequests;
    lastAppId = prompt;
    return true;
  }
  bool mutationBusy() const override { return busy; }
  StorageStatus storageStatus() const override {
    StorageStatus status;
    status.sdPresent = true;
    status.sdWritable = true;
    status.internalFreeBytes = 100;
    status.internalTotalBytes = 200;
    status.sdFreeBytes = 300;
    status.sdTotalBytes = 400;
    return status;
  }
  AlbumReadStatus readAlbumPage(
      const AlbumPageRequest& request,
      AlbumPage* page) const override {
    ++albumPageReads;
    assert(page != nullptr);
    assert(request.maximumItems == kMaximumAlbumPageItems);
    assert(request.maximumIdBytes == kMaximumAlbumIdBytes);
    assert(request.maximumTitleBytes == kMaximumAlbumTitleBytes);
    assert(request.maximumOriginBytes == kMaximumAlbumOriginBytes);
    assert(request.maximumTotalFieldBytes == kMaximumAlbumPageFieldBytes);
    if (forcedAlbumStatus != AlbumReadStatus::Ok) return forcedAlbumStatus;
    size_t offset = 0;
    if (!request.cursor.empty()) {
      if (request.cursor != "page16") return AlbumReadStatus::NotFound;
      offset = 16;
    }
    page->totalItems = static_cast<uint32_t>(album.size());
    const size_t maximum = bypassAlbumBounds
        ? album.size() : request.maximumItems;
    for (size_t index = offset;
         index < album.size() && page->items.size() < maximum;
         ++index) {
      page->items.push_back(album[index]);
    }
    if (offset + page->items.size() < album.size()) page->nextCursor = "page16";
    return AlbumReadStatus::Ok;
  }
  AlbumReadStatus findAlbumItem(
      const std::string& assetId,
      AlbumItem* item) const override {
    ++albumLookups;
    if (!item) return AlbumReadStatus::InvalidData;
    for (size_t index = 0; index < album.size(); ++index) {
      if (album[index].id == assetId) {
        *item = album[index];
        return AlbumReadStatus::Ok;
      }
    }
    return AlbumReadStatus::NotFound;
  }
  DiagnosticsSnapshot diagnostics() const override {
    DiagnosticsSnapshot snapshot;
    snapshot.firmwareVersion = "0.3-dev";
    snapshot.hardwareSku = "m5-papercolor-c151";
    snapshot.wifiState = "connected";
    snapshot.storageState = "ready";
    snapshot.displayState = "ready";
    snapshot.myAiState = reportedMyAiState;
    snapshot.freeHeapBytes = 123;
    snapshot.freePsramBytes = 456;
    snapshot.serialLines.push_back("INKLOOP_STATE:ready");
    snapshot.serialLines.push_back("device_token=do-not-expose");
    snapshot.serialLines.push_back("Authorization: Bearer do-not-expose");
    snapshot.serialLines.push_back("deviceToken=camel-secret-value");
    snapshot.serialLines.push_back("gateway-token: kebab-secret-value");
    snapshot.serialLines.push_back("TOKEN = generic-secret-value");
    snapshot.serialLines.push_back("pairing value: pairing-secret-value");
    snapshot.serialLines.push_back("session.id=session-secret-value");
    return snapshot;
  }
};

PortalAccessConfig accessConfig() {
  PortalAccessConfig access;
  access.bootNonce = "boot-nonce-1234567890";
  access.sessionId = "session-id-1234567890";
  access.csrfToken = "csrf-token-1234567890";
  access.allowedOrigins.push_back("http://192.168.4.1");
  access.allowedOrigins.push_back("http://inkloop-c151.local");
  access.sessionLifetimeSeconds = 900;
  return access;
}

PortalRequest request(
    const std::string& method,
    const std::string& path,
    uint64_t now,
    const std::string& body = std::string()) {
  PortalRequest value;
  value.method = method;
  value.path = path;
  value.host = "192.168.4.1";
  value.origin = "http://192.168.4.1";
  value.contentType = body.empty() ? "" : "application/x-www-form-urlencoded";
  value.body = body;
  value.peerIp = "192.168.4.2";
  value.nowSeconds = now;
  return value;
}

void authorize(PortalRequest* value) {
  value->cookie = "other=x; inkloop_session=session-id-1234567890";
  value->csrfToken = "csrf-token-1234567890";
}

PortalResponse authenticated(
    InkloopPortal& portal,
    const std::string& method,
    const std::string& path,
    uint64_t now,
    const std::string& body = std::string()) {
  PortalRequest value = request(method, path, now, body);
  authorize(&value);
  return portal.handle(value);
}

int main() {
  FakeAdapter adapter;
  InkloopPortal portal(adapter, accessConfig());
  assert(!portal.ready());
  std::string error;
  assert(portal.hydrate(adapter.durableSnapshot, 0, &error));
  assert(portal.ready());
  assert(std::string(kMyAiAppId) == "inkloop");
  assert(portal.onboarding().stage() == OnboardingStage::WifiAccessPoint);

  PortalRequest badHost = request("GET", "/health", 1);
  badHost.host = "evil.example";
  assert(portal.handle(badHost).status == 400);
  const PortalResponse health = portal.handle(request("GET", "/health", 1));
  assert(health.status == 200);
  assert(health.body == "{\"ok\":true,\"service\":\"inkloop-portal\"}");

  const PortalResponse login = portal.handle(request("GET", "/", 2));
  assert(login.status == 401);
  assert(login.contentType == "text/html; charset=utf-8");
  assert(login.body.find("boot-nonce-1234567890") == std::string::npos);
  assert(login.body.find("fetch(\"/api/session\"") != std::string::npos);
  assert(login.body.find("location.replace(\"/\")") != std::string::npos);
  assert(login.body.find("Settings Wi") != std::string::npos);
  assert(login.body.find("本地管理密码") != std::string::npos);
  assert(login.body.find("六位绑定码") != std::string::npos);
  assert(login.body.find("家庭 Wi") != std::string::npos);

  PortalRequest wrongOrigin = request(
      "POST", "/api/session", 3, "nonce=boot-nonce-1234567890");
  wrongOrigin.origin = "http://evil.example";
  assert(portal.handle(wrongOrigin).status == 403);

  const PortalResponse session = portal.handle(request(
      "POST", "/api/session", 4, "nonce=boot-nonce-1234567890"));
  assert(session.status == 200);
  assert(session.body.find("csrf-token-1234567890") != std::string::npos);
  assert(session.setCookie.find("HttpOnly") != std::string::npos);
  assert(session.setCookie.find("SameSite=Strict") != std::string::npos);
  const PortalResponse renewedSession = portal.handle(request(
      "POST", "/api/session", 5, "nonce=boot-nonce-1234567890"));
  assert(renewedSession.status == 200);
  assert(renewedSession.body.find("\"expiresAt\":905") != std::string::npos);
  assert(renewedSession.setCookie.find("inkloop_session=session-id-1234567890") !=
      std::string::npos);
  assert(portal.handle(request(
      "POST", "/api/session", 6, "nonce=definitely-wrong-password")).status == 401);

  PortalRequest missingCsrf = request("POST", "/api/onboarding/myai/start", 6);
  missingCsrf.cookie = "inkloop_session=session-id-1234567890";
  assert(portal.handle(missingCsrf).status == 403);
  assert(authenticated(portal, "POST", "/api/onboarding/myai/start", 7).status == 409);

  assert(portal.onWifiConfigured(true, &error));
  assert(portal.onboarding().stage() == OnboardingStage::WifiConfigured);
  assert(authenticated(portal, "POST", "/api/onboarding/myai/start", 8).status == 202);
  assert(adapter.pairingStarts == 1);
  assert(adapter.lastAppId == "inkloop");

  // Firmware can use the same durable transition without fabricating a local
  // HTTP session, and can restore a MyAI-durable pending pairing after reset
  // without creating a second Center pairing request.
  FakeAdapter automaticAdapter;
  InkloopPortal automatic(automaticAdapter, accessConfig());
  assert(automatic.hydrate(automaticAdapter.durableSnapshot, 0, &error));
  assert(automatic.onWifiConfigured(true, &error));
  assert(automatic.requestMyAiPairing(&error));
  assert(automatic.onboarding().stage() == OnboardingStage::MyAiPairingRequested);
  assert(automaticAdapter.pairingStarts == 1);
  PortalSettings trustedSettings = automatic.settings();
  trustedSettings.volume = 42;
  trustedSettings.assistantPrompt = "Voice-persisted prompt";
  trustedSettings.image.steps = 24;
  assert(automatic.replaceSettings(trustedSettings, &error));
  assert(automatic.settings().volume == 42);
  assert(automaticAdapter.durableSnapshot.settings.assistantPrompt ==
         "Voice-persisted prompt");
  trustedSettings.volume = 101;
  assert(!automatic.replaceSettings(trustedSettings, &error));
  assert(automatic.settings().volume == 42);
  assert(automatic.onMyAiPairingCancelled(&error));
  assert(automatic.onboarding().stage() == OnboardingStage::WifiConfigured);
  assert(automatic.onboarding().onboardingCode().empty());
  assert(automatic.requestMyAiPairing(&error));
  assert(automaticAdapter.pairingStarts == 2);
  FakeAdapter resumedAdapter;
  InkloopPortal resumed(resumedAdapter, accessConfig());
  assert(resumed.hydrate(resumedAdapter.durableSnapshot, 0, &error));
  assert(resumed.onWifiConfigured(true, &error));
  assert(resumed.onMyAiPairingResumed(&error));
  assert(resumed.onboarding().stage() == OnboardingStage::MyAiPairingRequested);
  assert(resumedAdapter.pairingStarts == 0);

  assert(!portal.onAuthoritativeMyAiCode("12345", 100, 9, &error));
  assert(error == "invalid_onboarding_code");
  assert(!portal.onAuthoritativeMyAiCode("123456", 9, 9, &error));
  assert(error == "onboarding_code_expired");
  assert(portal.onAuthoritativeMyAiCode("123456", 100, 9, &error));
  assert(adapter.lastDirtyFields ==
      (SnapshotOnboardingStage | SnapshotMyAiActive |
       SnapshotCodeOwnership | SnapshotOnboardingCode |
       SnapshotInkloopCode | SnapshotCodeExpiry |
       SnapshotInkloopReuseAccepted));
  assert(adapter.reuseRequests == 1);
  assert(adapter.lastReusedCode == "123456");
  assert(portal.onboarding().inkloopCode() == "123456");
  assert(portal.onboarding().myAiRegistrationUrl() ==
      "https://myai.mess.host/?device_code=123456#devices");

  assert(portal.onInkloopBound(&error));
  assert(portal.onboarding().bindingCompletionState() ==
      BindingCompletionState::InkloopOnly);
  assert(portal.onboarding().codeRetentionState() ==
      PairingCodeRetentionState::NeededByMyAi);
  assert(portal.onboarding().onboardingCode() == "123456");
  assert(portal.onboarding().inkloopCode().empty());
  assert(portal.onboarding().myAiRegistrationUrl() ==
      "https://myai.mess.host/?device_code=123456#devices");
  assert(adapter.durableSnapshot.onboarding.onboardingCode == "123456");
  assert(adapter.durableSnapshot.onboarding.inkloopCode.empty());
  assert(adapter.durableSnapshot.onboarding.codeExpiresAtSeconds == 100);
  assert(portal.onAuthoritativeMyAiCode("123456", 200, 10, &error));
  assert(adapter.reuseRequests == 1);
  assert(!portal.onAuthoritativeMyAiCode("654321", 200, 10, &error));
  assert(error == "myai_code_replay_mismatch");
  assert(adapter.reuseRequests == 1);
  assert(portal.onMyAiActivation(true, &error));
  assert(portal.onboarding().terminalBindingComplete());
  assert(portal.onboarding().codeRetentionState() ==
      PairingCodeRetentionState::TerminalScrubbed);
  assert(portal.onboarding().onboardingCode().empty());
  assert(portal.onboarding().inkloopCode().empty());
  assert(portal.onboarding().myAiRegistrationUrl().empty());
  assert(adapter.durableSnapshot.onboarding.onboardingCode.empty());
  assert(adapter.durableSnapshot.onboarding.codeExpiresAtSeconds == 0);
  assert(portal.onboarding().stage() == OnboardingStage::VoiceTutorial);
  for (int step = 0; step < 5; ++step) {
    assert(authenticated(portal, "POST", "/api/tutorial/advance", 11 + step).status == 200);
  }
  assert(portal.onboarding().tutorialComplete());
  assert(portal.onboarding().stage() == OnboardingStage::SettingsReady);
  assert(authenticated(portal, "POST", "/api/tutorial/restart", 20).status == 200);
  assert(!portal.onboarding().tutorialComplete());
  assert(portal.onVoiceTutorialComplete(&error));
  assert(portal.onboarding().tutorialComplete());

  // An unbound device clears the mirrored Inkloop code when MyAI rotates/expires.
  FakeAdapter expiryAdapter;
  InkloopPortal expiryPortal(expiryAdapter, accessConfig());
  assert(expiryPortal.hydrate(expiryAdapter.durableSnapshot, 0, &error));
  assert(expiryPortal.onWifiConfigured(true, &error));
  PortalRequest expirySession = request(
      "POST", "/api/session", 20, "nonce=boot-nonce-1234567890");
  assert(expiryPortal.handle(expirySession).status == 200);
  assert(authenticated(expiryPortal, "POST", "/api/onboarding/myai/start", 21).status == 202);
  assert(expiryPortal.onAuthoritativeMyAiCode("111222", 50, 22, &error));
  assert(authenticated(expiryPortal, "GET", "/api/state", 50).status == 200);
  assert(expiryPortal.onboarding().onboardingCode().empty());
  assert(expiryPortal.onboarding().inkloopCode().empty());

  assert(authenticated(portal, "POST", "/api/settings", 30, "volume=101").status == 400);
  assert(authenticated(portal, "POST", "/api/settings", 30, "led_brightness=0").status == 400);
  assert(authenticated(portal, "POST", "/api/settings", 30, "led_brightness=101").status == 400);
  assert(authenticated(portal, "POST", "/api/settings", 30,
      "voice_assistance=1").status == 400);
  assert(authenticated(portal, "POST", "/api/settings", 31,
      std::string("assistant_prompt=") + std::string(513, 'a')).status == 400);
  const PortalResponse saved = authenticated(
      portal,
      "POST",
      "/api/settings",
      32,
      "storage=sd&volume=75&voice_assistance_present=1&led_brightness=80&assistant_prompt=Draw+calm+art&image_size=600x400&"
      "image_prompt_template=Vivid+%7Bprompt%7D&image_steps=30&negative_prompt=tiny+text&led_swap=1&"
      "refresh_mode=experimental-six-color&power_mode=battery&idle_timeout=300");
  assert(saved.status == 200);
  assert(adapter.settingsSaves == 1);
  assert(adapter.savedSettings.storageTarget == StorageTarget::SdCard);
  assert(adapter.savedSettings.volume == 75);
  assert(!adapter.savedSettings.voiceAssistanceEnabled);
  assert(adapter.savedSettings.ledMaximumBrightnessPercent == 80);
  assert(adapter.ledTests == 1 && adapter.testedLedBrightness == 80);
  assert(saved.body.find("\"ledDiagnosticRequested\":true") != std::string::npos);
  assert(saved.body.find("\"ledDiagnosticAccepted\":true") != std::string::npos);
  assert(saved.body.find("\"ledDiagnosticStarted\":true") != std::string::npos);
  assert(adapter.savedSettings.assistantPrompt == "Draw calm art");
  assert(adapter.savedSettings.imagePromptTemplate == "Vivid {prompt}");
  assert(adapter.savedSettings.image.width == 600);
  assert(adapter.savedSettings.image.height == 400);
  assert(adapter.savedSettings.image.steps == 30);
  assert(adapter.savedSettings.ledRolesSwapped);
  assert(adapter.savedSettings.refreshMode == RefreshMode::ExperimentalSixColor);
  assert(authenticated(portal, "POST", "/api/settings", 32,
      "local_password=short&local_password_confirm=short").status == 400);
  assert(authenticated(portal, "POST", "/api/settings", 32,
      "local_password=NEWPASS9Q2K&local_password_confirm=NEWPASS9Q3K").status == 400);
  const PortalResponse passwordSaved = authenticated(
      portal, "POST", "/api/settings", 32,
      "local_password=NEWPASS9Q2K&local_password_confirm=NEWPASS9Q2K");
  assert(passwordSaved.status == 200);
  assert(passwordSaved.body.find("NEWPASS9Q2K") == std::string::npos);
  assert(passwordSaved.body.find("\"restartRequired\":true") != std::string::npos);
  assert(adapter.durableSnapshot.settings.localManagementPassword == "NEWPASS9Q2K");
  assert(adapter.savedSettings.powerMode == PowerMode::Battery);
  assert(adapter.savedSettings.idleTimeoutSeconds == 300);
  PortalRequest htmlForm = request(
      "POST", "/api/settings", 32, "_csrf=csrf-token-1234567890&volume=76");
  htmlForm.cookie = "inkloop_session=session-id-1234567890";
  const PortalResponse htmlFormResponse = portal.handle(htmlForm);
  if (htmlFormResponse.status != 200) std::cerr << htmlFormResponse.body << "\n";
  assert(htmlFormResponse.status == 200);
  assert(adapter.savedSettings.volume == 76);
  assert(adapter.savedSettings.storageTarget == StorageTarget::SdCard);
  assert(adapter.savedSettings.assistantPrompt == "Draw calm art");
  assert(adapter.savedSettings.image.width == 600);
  assert(adapter.savedSettings.refreshMode == RefreshMode::ExperimentalSixColor);
  assert(adapter.lastDirtyFields == SnapshotVolume);

  // Persistence failures leave both the hydrated runtime and durable snapshot
  // unchanged; partial updates never reconstruct defaults.
  const uint64_t revisionBeforeFailure = portal.snapshotRevision();
  const uint8_t volumeBeforeFailure = portal.settings().volume;
  const PortalPersistedSnapshot durableBeforeFailure = adapter.durableSnapshot;
  adapter.failPersistence = true;
  const PortalResponse failedSave = authenticated(
      portal, "POST", "/api/settings", 32, "volume=12");
  assert(failedSave.status == 503);
  assert(portal.snapshotRevision() == revisionBeforeFailure);
  assert(portal.settings().volume == volumeBeforeFailure);
  assert(adapter.durableSnapshot.revision == durableBeforeFailure.revision);
  assert(adapter.durableSnapshot.settings.volume == durableBeforeFailure.settings.volume);
  adapter.failPersistence = false;
  const uint8_t savedVolumeBeforePreview = portal.settings().volume;
  assert(authenticated(
      portal, "POST", "/api/audio/preview", 33, "volume=35").status == 202);
  assert(adapter.audioPreviews == 1 && adapter.previewedVolume == 35);
  assert(portal.settings().volume == savedVolumeBeforePreview);
  assert(authenticated(
      portal, "POST", "/api/audio/preview", 33, "volume=101").status == 400);
  adapter.busy = true;
  const PortalResponse busyPreview = authenticated(
      portal, "POST", "/api/audio/preview", 33, "volume=40");
  assert(busyPreview.status == 409);
  assert(busyPreview.body.find("audio_busy") != std::string::npos);
  adapter.busy = false;
  assert(authenticated(portal, "POST", "/api/led/test", 33).status == 404);

  assert(authenticated(
      portal, "POST", "/api/album/display", 34,
      "asset_id=sha256%3Aabc_123").status == 202);
  assert(adapter.displayRequests == 1);
  assert(authenticated(
      portal, "POST", "/api/aigc/generate", 34,
      "prompt=bright+paper+cat").status == 202);
  assert(adapter.generateRequests == 1 && adapter.lastAppId == "bright paper cat");
  assert(authenticated(
      portal, "POST", "/api/aigc/generate", 34, "prompt=").status == 400);

  PortalRequest uploadAuth = request("POST", "/api/album/upload", 35);
  authorize(&uploadAuth);
  uploadAuth.contentType = "multipart/form-data; boundary=inkloop-boundary";
  assert(portal.authorizeStreamingAlbumUpload(uploadAuth).status == 202);
  PortalRequest uploadWrongCsrf = uploadAuth;
  uploadWrongCsrf.csrfToken = "wrong-csrf-token";
  assert(portal.authorizeStreamingAlbumUpload(uploadWrongCsrf).status == 403);
  adapter.busy = true;
  assert(portal.authorizeStreamingAlbumUpload(uploadAuth).status == 409);
  adapter.busy = false;

  PortalRequest previewAuth = request("GET", "/api/album/preview", 36);
  authorize(&previewAuth);
  assert(portal.authorizeStreamingAlbumPreview(previewAuth).status == 200);
  PortalRequest previewAnonymous = request("GET", "/api/album/preview", 36);
  assert(portal.authorizeStreamingAlbumPreview(previewAnonymous).status == 401);
  adapter.busy = true;
  assert(portal.authorizeStreamingAlbumPreview(previewAuth).status == 409);
  adapter.busy = false;

  std::string dashboard = portal.renderDashboardHtml();
  const size_t dashboardHtmlSize = dashboard.size();
  const PortalResponse portalScript = authenticated(
      portal, "GET", "/portal.js", 36);
  assert(portalScript.status == 200);
  assert(portalScript.contentType == "application/javascript; charset=utf-8");
  assert(portalScript.body.size() <= 24576);
  dashboard += portalScript.body;
  assert(dashboard.find("document.querySelectorAll(\"form[data-portal]\")") !=
         std::string::npos);
  assert(dashboard.find("fetch(path") != std::string::npos);
  assert(dashboard.find("BtnC / GPIO1") != std::string::npos);
  assert(dashboard.find("蓝/青闪2次") != std::string::npos);
  assert(dashboard.find("黄/橙闪2次") != std::string::npos);
  assert(dashboard.find("canvas.toBlob") != std::string::npos);
  assert(dashboard.find("X-Inkloop-Image-Bytes") != std::string::npos);
  assert(dashboard.find("image_prompt_template") != std::string::npos);
  assert(dashboard.find("type=\"range\" name=\"volume\"") != std::string::npos);
  assert(dashboard.find("name=\"voice_assistance\"") != std::string::npos);
  assert(dashboard.find("name=\"voice_assistance_present\"") != std::string::npos);
  assert(dashboard.find("type=\"range\" name=\"led_brightness\"") != std::string::npos);
  assert(dashboard.find("data-settings-group=\"sound-led\"") != std::string::npos);
  assert(dashboard.find("data-settings-group=\"ai-image\"") != std::string::npos);
  assert(dashboard.find("data-settings-group=\"display-power\"") != std::string::npos);
  assert(dashboard.find("data-settings-group=\"storage\"") != std::string::npos);
  assert(dashboard.find("data-settings-group=\"password\"") != std::string::npos);
  assert(dashboard.find("试听即时 · 保存后默认") != std::string::npos);
  assert(dashboard.find("下次对话 / 生成生效") != std::string::npos);
  assert(dashboard.find("保存后重启生效") != std::string::npos);
  assert(dashboard.find("RGB 检测已排队") != std::string::npos);
  assert(dashboard.find("action=\"/api/led/test\"") == std::string::npos);
  assert(dashboard.find("/api/audio/preview") != std::string::npos);
  assert(dashboard.find("data-tab=\"album\"") != std::string::npos);
  assert(dashboard.find("data-tab=\"ai\"") != std::string::npos);
  assert(dashboard.find("class=\"album-grid\"") != std::string::npos);
  assert(dashboard.find("class=\"album-thumb\"") != std::string::npos);
  assert(dashboard.find("/api/album/preview?asset_id=") != std::string::npos);
  assert(dashboard.find("loading=\"lazy\"") != std::string::npos);
  assert(dashboard.find("Inkloop 任务") != std::string::npos);
  assert(dashboard.find("/api/album/display") != std::string::npos);
  assert(dashboard.find("class=\"album-actions\"") != std::string::npos);
  assert(dashboard.find("删除（需顶部键确认）") == std::string::npos);
  assert(dashboard.find(">删除</button>") != std::string::npos);
  assert(dashboard.find("/api/aigc/generate") != std::string::npos);
  assert(dashboard.find("开始生成并上屏") != std::string::npos);
  assert(dashboard.find("松开滑杆") != std::string::npos);
  assert(dashboard.find("默认复用已保存的家庭 Wi") != std::string::npos);
  assert(dashboard.find("NEWPASS9Q2K") == std::string::npos);
  assert(dashboard.find("<script>alert(1)</script>") == std::string::npos);
  assert(dashboard.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
  assert(dashboard.find("device_code=") == std::string::npos);
  assert(dashboard.find(">123456<") == std::string::npos);
  assert(dashboard.find(">654321<") == std::string::npos);
  assert(dashboard.find("<div class=\"myai-qr\"") == std::string::npos);
  const std::string albumJson = portal.renderAlbumJson();
  assert(albumJson.find("sha256:abc_123") != std::string::npos);
  assert(albumJson.size() <= kMaximumAlbumJsonBytes);
  assert(dashboardHtmlSize <= kMaximumDashboardHtmlBytes);
  assert(adapter.albumPageReads == 2);
  const std::string stateJson = portal.renderStateJson();
  assert(stateJson.find("\"display\":{\"state\":\"ready\"}") != std::string::npos);
  assert(dashboard.find("屏幕刷新已完成，设备正在进行 30 秒保护冷却") !=
      std::string::npos);

  const std::string serialJson = portal.renderDiagnosticsJson(true);
  assert(serialJson.find("INKLOOP_STATE:ready") != std::string::npos);
  assert(serialJson.find("do-not-expose") == std::string::npos);
  assert(serialJson.find("camel-secret-value") == std::string::npos);
  assert(serialJson.find("kebab-secret-value") == std::string::npos);
  assert(serialJson.find("generic-secret-value") == std::string::npos);
  assert(serialJson.find("pairing-secret-value") == std::string::npos);
  assert(serialJson.find("session-secret-value") == std::string::npos);
  assert(serialJson.find("[REDACTED]") != std::string::npos);
  const std::string diagnosticsJson = portal.renderDiagnosticsJson(false);
  assert(diagnosticsJson.find("serialLines") == std::string::npos);

  assert(authenticated(portal, "POST", "/api/actions/prepare", 40,
      "action=delete_asset&target=missing").status == 400);
  assert(authenticated(portal, "POST", "/api/actions/prepare", 41,
      "action=delete_asset&target=factory%3Atutorial").status == 400);
  const PortalResponse prepared = authenticated(portal, "POST", "/api/actions/prepare", 42,
      "action=delete_asset&target=sha256%3Aabc_123");
  assert(prepared.status == 202);
  const std::string deleteNonce = adapter.lastNonce;
  assert(prepared.body.find("physicalConfirmationRequired\":true") != std::string::npos);
  assert(portal.renderStateJson().find(
      "\"state\":\"browser_confirmation_required\"") != std::string::npos);
  assert(authenticated(portal, "POST", "/api/actions/confirm", 43,
      "confirmation_id=" + deleteNonce + "&phrase=WRONG").status == 403);
  assert(authenticated(portal, "POST", "/api/actions/confirm", 44,
      "confirmation_id=" + deleteNonce + "&phrase=DELETE+sha256%3Aabc_123").status == 202);
  assert(portal.renderStateJson().find(
      "\"state\":\"awaiting_device_button\"") != std::string::npos);
  assert(!portal.confirmPhysical("wrong", 45, &error));
  assert(adapter.destructiveExecutions == 0);
  assert(portal.confirmPhysical(deleteNonce, 45, &error));
  assert(adapter.destructiveExecutions == 1);
  assert(adapter.lastOperation.action == DestructiveAction::DeleteAsset);
  assert(adapter.lastOperation.target == "sha256:abc_123");
  assert(portal.renderStateJson().find(
      "\"state\":\"complete\"") != std::string::npos);
  assert(!portal.confirmPhysical(deleteNonce, 45, &error));

  FakeAdapter missingAppAdapter;
  missingAppAdapter.reportedMyAiState = "app_not_registered";
  InkloopPortal missingAppPortal(missingAppAdapter, accessConfig());
  assert(missingAppPortal.hydrate(
      missingAppAdapter.durableSnapshot, 0, &error));
  assert(missingAppPortal.onWifiConfigured(true, &error));
  const std::string missingAppHtml = missingAppPortal.renderDashboardHtml();
  assert(missingAppHtml.find("inkloop 应用尚未在 MyAI 注册") !=
         std::string::npos);
  assert(missingAppHtml.find("重新请求六位绑定码") != std::string::npos);
  assert(missingAppHtml.size() <= kMaximumDashboardHtmlBytes);
  assert(missingAppPortal.renderStateJson().find(
      "\"state\":\"app_not_registered\"") != std::string::npos);

  assert(authenticated(portal, "POST", "/api/actions/prepare", 50,
      "action=clear_album").status == 202);
  const std::string clearNonce = adapter.lastNonce;
  assert(authenticated(portal, "POST", "/api/actions/confirm", 80,
      "confirmation_id=" + clearNonce + "&phrase=CLEAR+ALBUM").status == 410);
  assert(adapter.destructiveExecutions == 1);

  assert(authenticated(portal, "POST", "/api/actions/prepare", 90,
      "action=format_sd").status == 202);
  const std::string formatNonce = adapter.lastNonce;
  assert(authenticated(portal, "POST", "/api/actions/confirm", 91,
      "confirmation_id=" + formatNonce + "&phrase=FORMAT+SD").status == 202);
  adapter.busy = true;
  assert(!portal.confirmPhysical(formatNonce, 92, &error));
  assert(error == "device_busy");
  adapter.busy = false;
  assert(portal.confirmPhysical(formatNonce, 93, &error));
  assert(adapter.destructiveExecutions == 2);
  assert(adapter.lastOperation.action == DestructiveAction::FormatSdCard);

  // A reboot hydrates the complete onboarding/settings state and cannot replay
  // pairing for an already activated and Inkloop-bound device.
  FakeAdapter rebootAdapter;
  rebootAdapter.durableSnapshot = adapter.durableSnapshot;
  InkloopPortal rebooted(rebootAdapter, accessConfig());
  assert(rebooted.hydrate(rebootAdapter.durableSnapshot, 100, &error));
  assert(rebooted.onboarding().inkloopBound());
  assert(rebooted.onboarding().myAiActive());
  assert(rebooted.onboarding().tutorialComplete());
  assert(rebooted.onboarding().stage() == OnboardingStage::SettingsReady);
  assert(rebooted.settings().storageTarget == StorageTarget::SdCard);
  assert(rebooted.settings().volume == 76);
  assert(rebooted.settings().ledMaximumBrightnessPercent == 80);
  assert(rebooted.settings().assistantPrompt == "Draw calm art");
  assert(rebooted.settings().image.width == 600);
  assert(rebooted.settings().image.height == 400);
  assert(rebooted.settings().image.steps == 30);
  assert(rebooted.settings().image.negativePrompt == "tiny text");
  assert(rebooted.settings().ledRolesSwapped);
  assert(rebooted.settings().refreshMode == RefreshMode::ExperimentalSixColor);
  assert(rebooted.settings().powerMode == PowerMode::Battery);
  assert(rebooted.settings().idleTimeoutSeconds == 300);
  assert(rebooted.handle(request(
      "POST", "/api/session", 101, "nonce=boot-nonce-1234567890")).status == 200);
  assert(authenticated(
      rebooted, "POST", "/api/onboarding/myai/start", 102).status == 409);
  assert(rebootAdapter.pairingStarts == 0);
  const std::string rebootState = rebooted.renderStateJson();
  assert(rebootState.find("123456") == std::string::npos);
  assert(rebootState.find("654321") == std::string::npos);
  assert(rebootState.find("device_code=") == std::string::npos);
  assert(rebootState.find("\"onboardingCode\":\"\"") != std::string::npos);
  assert(rebootState.find("\"inkloopCode\":\"\"") != std::string::npos);
  assert(rebootState.find("inkloop_bound_historical") != std::string::npos);

  // A pre-fix bound snapshot is accepted only long enough to atomically scrub
  // every historical code surface during hydration.
  PortalPersistedSnapshot legacyBound = testFreshSnapshot();
  legacyBound.onboarding.stage = OnboardingStage::VoiceTutorial;
  legacyBound.onboarding.wifiConfigured = true;
  legacyBound.onboarding.myAiActive = true;
  legacyBound.onboarding.inkloopBound = true;
  legacyBound.onboarding.inkloopReuseAccepted = true;
  legacyBound.onboarding.codeOwnership = CodeOwnership::InkloopBoundHistorical;
  legacyBound.onboarding.onboardingCode = "654321";
  legacyBound.onboarding.inkloopCode = "123456";
  legacyBound.onboarding.codeExpiresAtSeconds = 999;
  FakeAdapter legacyAdapter;
  legacyAdapter.durableSnapshot = legacyBound;
  InkloopPortal legacyPortal(legacyAdapter, accessConfig());
  assert(legacyPortal.hydrate(legacyBound, 100, &error));
  assert(legacyPortal.onboarding().onboardingCode().empty());
  assert(legacyPortal.onboarding().inkloopCode().empty());
  assert(legacyPortal.onboarding().myAiRegistrationUrl().empty());
  assert(legacyAdapter.durableSnapshot.revision == 2);
  assert(legacyAdapter.durableSnapshot.onboarding.onboardingCode.empty());
  assert(legacyAdapter.durableSnapshot.onboarding.inkloopCode.empty());
  assert(legacyAdapter.durableSnapshot.onboarding.codeExpiresAtSeconds == 0);
  assert(legacyPortal.renderStateJson().find("654321") == std::string::npos);
  assert(legacyPortal.renderDashboardHtml().find(">123456<") == std::string::npos);

  // A durable Inkloop-first reboot keeps the sole MyAI authority, accepts the
  // exact replay without a second Inkloop registration, and rejects rotation.
  FakeAdapter inkloopFirstAdapter;
  InkloopPortal inkloopFirst(inkloopFirstAdapter, accessConfig());
  assert(inkloopFirst.hydrate(inkloopFirstAdapter.durableSnapshot, 0, &error));
  assert(inkloopFirst.onWifiConfigured(true, &error));
  assert(inkloopFirst.requestMyAiPairing(&error));
  assert(inkloopFirst.onAuthoritativeMyAiCode("515151", 900, 10, &error));
  assert(inkloopFirst.onInkloopBound(&error));
  FakeAdapter inkloopFirstRebootAdapter;
  inkloopFirstRebootAdapter.durableSnapshot =
      inkloopFirstAdapter.durableSnapshot;
  InkloopPortal inkloopFirstReboot(inkloopFirstRebootAdapter, accessConfig());
  assert(inkloopFirstReboot.hydrate(
      inkloopFirstRebootAdapter.durableSnapshot, 20, &error));
  assert(inkloopFirstReboot.onAuthoritativeMyAiCode(
      "515151", 1000, 21, &error));
  assert(inkloopFirstRebootAdapter.reuseRequests == 0);
  assert(!inkloopFirstReboot.onAuthoritativeMyAiCode(
      "515152", 1000, 22, &error));
  assert(error == "myai_code_replay_mismatch");

  // A snapshot written by the broken release may have already lost the local
  // copy. The still-authoritative MyAI pending credential can restore it.
  PortalPersistedSnapshot brokenInkloopFirst =
      inkloopFirstAdapter.durableSnapshot;
  brokenInkloopFirst.onboarding.onboardingCode.clear();
  brokenInkloopFirst.onboarding.codeExpiresAtSeconds = 0;
  FakeAdapter brokenRebootAdapter;
  brokenRebootAdapter.durableSnapshot = brokenInkloopFirst;
  InkloopPortal brokenReboot(brokenRebootAdapter, accessConfig());
  assert(brokenReboot.hydrate(brokenInkloopFirst, 20, &error));
  assert(brokenReboot.onAuthoritativeMyAiCode(
      "515151", 1000, 21, &error));
  assert(brokenReboot.onboarding().onboardingCode() == "515151");
  assert(brokenRebootAdapter.reuseRequests == 0);

  // The reverse MyAI-first partial order retains the shared code only until
  // Inkloop completes, then scrubs every portal representation.
  FakeAdapter myAiFirstAdapter;
  InkloopPortal myAiFirst(myAiFirstAdapter, accessConfig());
  assert(myAiFirst.hydrate(myAiFirstAdapter.durableSnapshot, 0, &error));
  assert(myAiFirst.onWifiConfigured(true, &error));
  assert(myAiFirst.requestMyAiPairing(&error));
  assert(myAiFirst.onAuthoritativeMyAiCode("616161", 900, 10, &error));
  assert(myAiFirst.onMyAiActivation(true, &error));
  assert(myAiFirst.onboarding().bindingCompletionState() ==
      BindingCompletionState::MyAiOnly);
  assert(myAiFirst.onboarding().onboardingCode() == "616161");
  assert(myAiFirst.onboarding().inkloopCode() == "616161");
  assert(myAiFirst.onInkloopBound(&error));
  assert(myAiFirst.onboarding().terminalBindingComplete());
  assert(myAiFirst.renderStateJson().find("616161") == std::string::npos);
  assert(myAiFirst.renderDashboardHtml().find(">616161<") == std::string::npos);

  // Payment-inactive is also a bound, code-free durable state.
  FakeAdapter inactiveAdapter;
  inactiveAdapter.reportedMyAiState = "inactive";
  InkloopPortal inactivePortal(inactiveAdapter, accessConfig());
  assert(inactivePortal.hydrate(inactiveAdapter.durableSnapshot, 0, &error));
  assert(inactivePortal.onWifiConfigured(true, &error));
  assert(inactivePortal.requestMyAiPairing(&error));
  assert(inactivePortal.onAuthoritativeMyAiCode("808080", 500, 10, &error));
  assert(inactivePortal.onInkloopBound(&error));
  assert(inactivePortal.onMyAiActivation(false, &error));
  assert(inactivePortal.onboarding().stage() == OnboardingStage::MyAiInactive);
  assert(inactivePortal.onboarding().onboardingCode().empty());
  assert(inactivePortal.renderStateJson().find("808080") == std::string::npos);
  const std::string inactiveHtml = inactivePortal.renderDashboardHtml();
  assert(inactiveHtml.find(">808080<") == std::string::npos);
  assert(inactiveHtml.find("https://myai.mess.host/#devices") != std::string::npos);
  assert(inactiveHtml.find("https://myai.mess.host/#billing") != std::string::npos);
  assert(inactiveHtml.find("检查 MyAI 订阅账单") != std::string::npos);
  assert(inactiveHtml.find("无需删除设备或重新绑定") != std::string::npos);
  const std::string inactiveJson = inactivePortal.renderStateJson();
  assert(inactiveJson.find("运行授权检查返回 402") != std::string::npos);
  assert(inactiveJson.find("设备列表显示已激活") != std::string::npos);
  assert(inactiveJson.find("无需重新绑定") != std::string::npos);
  // A runtime credential rejection cannot contradict the durable terminal
  // binding state or invite the user to create another pairing code.
  inactiveAdapter.reportedMyAiState = "unconfigured";
  const std::string rejectedHtml = inactivePortal.renderDashboardHtml();
  assert(rejectedHtml.find("已绑定，运行授权待恢复") != std::string::npos);
  assert(rejectedHtml.find("本机缺少可用的 MyAI 运行凭证") != std::string::npos);
  assert(rejectedHtml.find("MyAI 尚未配置") == std::string::npos);
  assert(rejectedHtml.find("重新请求六位绑定码") == std::string::npos);
  const std::string rejectedJson = inactivePortal.renderStateJson();
  assert(rejectedJson.find("\"state\":\"credential_recovery\"") != std::string::npos);
  assert(rejectedJson.find("无需重新绑定或重新请求六位码") != std::string::npos);

  // An authenticated owner can explicitly replace an unusable MyAI
  // credential without clearing the existing Inkloop binding or album.
  FakeAdapter rebindAdapter;
  rebindAdapter.reportedMyAiState = "unconfigured";
  InkloopPortal rebindPortal(rebindAdapter, accessConfig());
  assert(rebindPortal.hydrate(rebindAdapter.durableSnapshot, 0, &error));
  assert(rebindPortal.onWifiConfigured(true, &error));
  assert(rebindPortal.requestMyAiPairing(&error));
  assert(rebindPortal.onAuthoritativeMyAiCode("919191", 900, 10, &error));
  assert(rebindPortal.onInkloopBound(&error));
  assert(rebindPortal.onMyAiActivation(false, &error));
  assert(rebindPortal.handle(request(
      "POST", "/api/session", 20,
      "nonce=boot-nonce-1234567890")).status == 200);
  const std::string rebindHtml = rebindPortal.renderDashboardHtml();
  assert(rebindHtml.find("/api/onboarding/myai/rebind") != std::string::npos);
  const PortalResponse rebound = authenticated(
      rebindPortal, "POST", "/api/onboarding/myai/rebind", 21);
  assert(rebound.status == 202);
  assert(rebindAdapter.pairingRestarts == 1);
  assert(rebindPortal.onboarding().inkloopBound());
  assert(rebindPortal.onboarding().stage() ==
      OnboardingStage::MyAiPairingRequested);
  assert(rebindPortal.onAuthoritativeMyAiCode(
      "929292", 1000, 22, &error));
  assert(rebindPortal.onboarding().inkloopBound());
  assert(rebindPortal.onboarding().onboardingCode() == "929292");
  FakeAdapter inactiveRebootAdapter;
  inactiveRebootAdapter.durableSnapshot = inactiveAdapter.durableSnapshot;
  InkloopPortal inactiveReboot(inactiveRebootAdapter, accessConfig());
  assert(inactiveReboot.hydrate(inactiveRebootAdapter.durableSnapshot, 600, &error));
  assert(inactiveReboot.onboarding().stage() == OnboardingStage::MyAiInactive);
  assert(inactiveReboot.onboarding().onboardingCode().empty());

  // If remote Inkloop binding wins but the local partial-order commit fails,
  // keep the last durable MyAI code and accept its exact replay after reboot.
  FakeAdapter scrubFailureAdapter;
  InkloopPortal scrubFailure(scrubFailureAdapter, accessConfig());
  assert(scrubFailure.hydrate(scrubFailureAdapter.durableSnapshot, 0, &error));
  assert(scrubFailure.onWifiConfigured(true, &error));
  assert(scrubFailure.requestMyAiPairing(&error));
  assert(scrubFailure.onAuthoritativeMyAiCode("303030", 500, 10, &error));
  scrubFailureAdapter.failPersistence = true;
  assert(!scrubFailure.onInkloopBound(&error));
  assert(scrubFailure.ready());
  assert(!scrubFailure.onboarding().inkloopBound());
  assert(scrubFailure.onboarding().onboardingCode() == "303030");
  assert(scrubFailure.onboarding().inkloopCode() == "303030");
  assert(scrubFailureAdapter.durableSnapshot.onboarding.onboardingCode == "303030");
  scrubFailureAdapter.failPersistence = false;
  InkloopPortal crossStoreReboot(scrubFailureAdapter, accessConfig());
  assert(crossStoreReboot.hydrate(
      scrubFailureAdapter.durableSnapshot, 11, &error));
  assert(crossStoreReboot.onAuthoritativeMyAiCode(
      "303030", 600, 12, &error));
  assert(scrubFailureAdapter.lastReusedCode == "303030");

  // A terminal transition that cannot durably erase the code redacts RAM and
  // disables the Portal; no state or dashboard response can disclose it.
  FakeAdapter terminalFailureAdapter;
  InkloopPortal terminalFailure(terminalFailureAdapter, accessConfig());
  assert(terminalFailure.hydrate(
      terminalFailureAdapter.durableSnapshot, 0, &error));
  assert(terminalFailure.onWifiConfigured(true, &error));
  assert(terminalFailure.requestMyAiPairing(&error));
  assert(terminalFailure.onAuthoritativeMyAiCode("404040", 500, 10, &error));
  assert(terminalFailure.onInkloopBound(&error));
  terminalFailureAdapter.failPersistence = true;
  assert(!terminalFailure.onMyAiActivation(true, &error));
  assert(!terminalFailure.ready());
  assert(terminalFailure.onboarding().onboardingCode().empty());
  assert(terminalFailure.onboarding().inkloopCode().empty());
  assert(terminalFailure.renderStateJson().find("404040") == std::string::npos);
  assert(terminalFailure.renderDashboardHtml().find(">404040<") == std::string::npos);

  // Inkloop reuse is best-effort. A temporary mirror failure must not hide or
  // rotate the authoritative MyAI code; the exact code is retried later.
  FakeAdapter reuseFailureAdapter;
  InkloopPortal reuseFailure(reuseFailureAdapter, accessConfig());
  assert(reuseFailure.hydrate(reuseFailureAdapter.durableSnapshot, 0, &error));
  assert(reuseFailure.onWifiConfigured(true, &error));
  assert(reuseFailure.handle(request(
      "POST", "/api/session", 1, "nonce=boot-nonce-1234567890")).status == 200);
  assert(authenticated(
      reuseFailure, "POST", "/api/onboarding/myai/start", 2).status == 202);
  reuseFailureAdapter.failReuse = true;
  assert(reuseFailure.onAuthoritativeMyAiCode("707070", 100, 3, &error));
  assert(error.empty());
  assert(reuseFailure.onboarding().onboardingCode() == "707070");
  assert(reuseFailure.onboarding().inkloopCode().empty());
  assert(reuseFailure.onboarding().codeOwnership() == CodeOwnership::None);
  assert(!reuseFailure.onboarding().inkloopReuseAccepted());
  assert(reuseFailureAdapter.durableSnapshot.onboarding.onboardingCode == "707070");
  // A fast MyAI activation may win the race with the optional Inkloop mirror;
  // the retained code must still be retryable afterwards.
  assert(reuseFailure.onMyAiActivation(true, &error));
  reuseFailureAdapter.failReuse = false;
  assert(reuseFailure.retryInkloopCodeReuse(&error));
  assert(reuseFailure.onboarding().onboardingCode() == "707070");
  assert(reuseFailure.onboarding().inkloopCode() == "707070");
  assert(reuseFailure.onboarding().inkloopReuseAccepted());
  assert(reuseFailure.onboarding().codeOwnership() ==
      CodeOwnership::MyAiAuthoritativeShared);

  // Future, partial, enum-corrupt and internally inconsistent snapshots all
  // fail without partially hydrating the portal.
  FakeAdapter malformedAdapter;
  InkloopPortal malformed(malformedAdapter, accessConfig());
  PortalPersistedSnapshot bad = testFreshSnapshot();
  bad.schemaVersion = kPortalSnapshotSchemaVersion + 1;
  assert(!malformed.hydrate(bad, 0, &error));
  assert(error == "snapshot_future_schema");
  bad = testFreshSnapshot();
  bad.presentFields &= ~SnapshotVolume;
  assert(!malformed.hydrate(bad, 0, &error));
  assert(error == "snapshot_incomplete");
  bad = testFreshSnapshot();
  bad.presentFields &= ~SnapshotInkloopReuseAccepted;
  assert(!malformed.hydrate(bad, 0, &error));
  assert(error == "snapshot_incomplete");
  bad = testFreshSnapshot();
  bad.settings.storageTarget = static_cast<StorageTarget>(255);
  assert(!malformed.hydrate(bad, 0, &error));
  assert(error == "snapshot_invalid_setting_enum");
  bad = testFreshSnapshot();
  bad.onboarding.stage = OnboardingStage::SettingsReady;
  bad.onboarding.wifiConfigured = true;
  bad.onboarding.myAiActive = true;
  bad.onboarding.tutorialStep = TutorialStep::Complete;
  assert(!malformed.hydrate(bad, 0, &error));
  assert(error == "snapshot_invalid_active_stage");
  assert(!malformed.ready());
  assert(malformed.hydrate(malformedAdapter.durableSnapshot, 0, &error));

  PortalPersistedSnapshot expiredSnapshot = testFreshSnapshot();
  expiredSnapshot.onboarding.wifiConfigured = true;
  expiredSnapshot.onboarding.stage = OnboardingStage::AwaitingMyAiActivation;
  expiredSnapshot.onboarding.inkloopReuseAccepted = true;
  expiredSnapshot.onboarding.codeOwnership = CodeOwnership::MyAiAuthoritativeShared;
  expiredSnapshot.onboarding.onboardingCode = "909090";
  expiredSnapshot.onboarding.inkloopCode = "909090";
  expiredSnapshot.onboarding.codeExpiresAtSeconds = 10;
  FakeAdapter hydrateExpiryAdapter;
  hydrateExpiryAdapter.durableSnapshot = expiredSnapshot;
  InkloopPortal hydratedExpiry(hydrateExpiryAdapter, accessConfig());
  assert(hydratedExpiry.hydrate(expiredSnapshot, 10, &error));
  assert(hydratedExpiry.onboarding().onboardingCode().empty());
  assert(hydratedExpiry.onboarding().inkloopCode().empty());
  assert(hydratedExpiry.onboarding().codeOwnership() == CodeOwnership::None);
  assert(hydrateExpiryAdapter.durableSnapshot.revision == 2);
  assert(hydrateExpiryAdapter.lastDirtyFields ==
      (SnapshotOnboardingStage | SnapshotMyAiActive |
       SnapshotCodeOwnership | SnapshotOnboardingCode |
       SnapshotInkloopCode | SnapshotCodeExpiry |
       SnapshotInkloopReuseAccepted));

  FakeAdapter failedHydrateExpiryAdapter;
  failedHydrateExpiryAdapter.durableSnapshot = expiredSnapshot;
  failedHydrateExpiryAdapter.failPersistence = true;
  InkloopPortal failedHydrateExpiry(failedHydrateExpiryAdapter, accessConfig());
  assert(!failedHydrateExpiry.hydrate(expiredSnapshot, 10, &error));
  assert(!failedHydrateExpiry.ready());
  assert(failedHydrateExpiryAdapter.durableSnapshot.revision == 1);
  assert(failedHydrateExpiryAdapter.durableSnapshot.onboarding.onboardingCode == "909090");

  // Rate limits are per peer + session scope + normalized route, with separate
  // read/write/destructive budgets and an explicit Retry-After value.
  PortalAccessConfig limitedAccess = accessConfig();
  limitedAccess.rate.reads = RateBudget(2, 10);
  limitedAccess.rate.writes = RateBudget(2, 10);
  limitedAccess.rate.destructive = RateBudget(1, 10);
  FakeAdapter limitedAdapter;
  InkloopPortal limited(limitedAdapter, limitedAccess);
  assert(limited.hydrate(limitedAdapter.durableSnapshot, 0, &error));
  assert(limited.handle(request("GET", "/health", 1)).status == 200);
  assert(limited.handle(request("GET", "/health", 2)).status == 200);
  const PortalResponse readLimited = limited.handle(request("GET", "/health", 3));
  assert(readLimited.status == 429);
  assert(readLimited.retryAfterSeconds == 8);
  assert(readLimited.body.find("rate_limited") != std::string::npos);
  PortalRequest anotherPeer = request("GET", "/health", 3);
  anotherPeer.peerIp = "192.168.4.3";
  assert(limited.handle(anotherPeer).status == 200);
  assert(limited.handle(request("GET", "/", 3)).status == 401);
  assert(limited.handle(request("GET", "/health", 11)).status == 200);
  assert(limited.handle(request(
      "POST", "/api/session", 20, "nonce=boot-nonce-1234567890")).status == 200);
  assert(authenticated(limited, "POST", "/api/audio/preview", 21, "volume=20").status == 202);
  assert(authenticated(limited, "POST", "/api/audio/preview", 22, "volume=20").status == 202);
  assert(authenticated(limited, "POST", "/api/audio/preview", 23, "volume=20").status == 429);
  assert(authenticated(limited, "POST", "/api/actions/prepare", 24,
      "action=clear_album").status == 202);
  assert(authenticated(limited, "POST", "/api/actions/prepare", 25,
      "action=format_sd").status == 429);

  // Unsigned elapsed arithmetic resets a window correctly across uint32 wrap.
  PortalAccessConfig wrapAccess = accessConfig();
  wrapAccess.rate.reads = RateBudget(1, 2);
  FakeAdapter wrapAdapter;
  InkloopPortal wrapPortal(wrapAdapter, wrapAccess);
  assert(wrapPortal.hydrate(wrapAdapter.durableSnapshot, 0, &error));
  assert(wrapPortal.handle(request("GET", "/health", 4294967294ULL)).status == 200);
  assert(wrapPortal.handle(request("GET", "/health", 4294967295ULL)).status == 429);
  assert(wrapPortal.handle(request("GET", "/health", 4294967296ULL)).status == 200);

  // A bounded table recycles only expired keys, so an open AP cannot fill it
  // permanently with stale peers.
  PortalAccessConfig capacityAccess = accessConfig();
  capacityAccess.rate.reads = RateBudget(10, 10);
  capacityAccess.rate.maximumTrackedKeys = 8;
  FakeAdapter capacityAdapter;
  InkloopPortal capacityPortal(capacityAdapter, capacityAccess);
  assert(capacityPortal.hydrate(capacityAdapter.durableSnapshot, 0, &error));
  for (int index = 0; index < 8; ++index) {
    PortalRequest keyed = request("GET", "/health", 1);
    keyed.peerIp = "192.168.4." + std::to_string(10 + index);
    assert(capacityPortal.handle(keyed).status == 200);
  }
  PortalRequest ninth = request("GET", "/health", 2);
  ninth.peerIp = "192.168.4.99";
  assert(capacityPortal.handle(ninth).status == 429);
  ninth.nowSeconds = 11;
  assert(capacityPortal.handle(ninth).status == 200);

  // Album reads are paged and bounded before serialization. Invalid fields
  // are 422, aggregate/output excess is 413, and both JSON and HTML propagate
  // deterministic status without materializing the complete album.
  FakeAdapter pagedAdapter;
  pagedAdapter.album.clear();
  for (int index = 0; index < 18; ++index) {
    AlbumItem item;
    item.id = "asset:" + std::to_string(index);
    item.title = "Page item";
    item.origin = "test";
    pagedAdapter.album.push_back(item);
  }
  InkloopPortal pagedPortal(pagedAdapter, accessConfig());
  assert(pagedPortal.hydrate(pagedAdapter.durableSnapshot, 0, &error));
  assert(pagedPortal.handle(request(
      "POST", "/api/session", 1, "nonce=boot-nonce-1234567890")).status == 200);
  PortalResponse firstPage = authenticated(
      pagedPortal, "GET", "/api/album", 2);
  assert(firstPage.status == 200);
  assert(firstPage.body.find("\"nextCursor\":\"page16\"") != std::string::npos);
  PortalResponse secondPage = authenticated(
      pagedPortal, "GET", "/api/album?cursor=page16", 3);
  assert(secondPage.status == 200);
  assert(secondPage.body.find("asset:17") != std::string::npos);
  assert(authenticated(
      pagedPortal, "GET", "/api/album?cursor=bad%20cursor", 4).status == 422);

  pagedAdapter.album[0].title.assign(kMaximumAlbumTitleBytes + 1, 'x');
  assert(authenticated(pagedPortal, "GET", "/api/album", 5).status == 422);
  assert(authenticated(pagedPortal, "GET", "/", 6).status == 422);
  pagedAdapter.album[0].title = "Page item";
  pagedAdapter.forcedAlbumStatus = AlbumReadStatus::TooLarge;
  assert(authenticated(pagedPortal, "GET", "/api/album", 7).status == 413);
  assert(authenticated(pagedPortal, "GET", "/", 8).status == 413);
  pagedAdapter.forcedAlbumStatus = AlbumReadStatus::Ok;
  pagedAdapter.bypassAlbumBounds = true;
  assert(authenticated(pagedPortal, "GET", "/api/album", 9).status == 422);

  PortalAccessConfig rotated = accessConfig();
  rotated.bootNonce = "new-boot-nonce-123456";
  rotated.sessionId = "new-session-id-123456";
  rotated.csrfToken = "new-csrf-token-123456";
  assert(portal.rotateAccess(rotated, &error));
  assert(authenticated(portal, "GET", "/api/state", 100).status == 401);

  std::cout << "papercolor portal checks passed\n";
  return 0;
}
`;

  await writeFile(harnessPath, harness);
  const compiler = process.env.CXX || "c++";
  const compile = spawnSync(
    compiler,
    [
      "-std=c++11",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      portalSource.pathname,
      harnessPath,
      new URL("OnboardingState.cpp", portalSource).pathname,
      new URL("PortalEncoding.cpp", portalSource).pathname,
      new URL("InkloopPortal.cpp", portalSource).pathname,
      "-o",
      executablePath,
    ],
    { encoding: "utf8" },
  );
  assert.equal(compile.status, 0, `${compile.stdout}\n${compile.stderr}`);

  const run = spawnSync(executablePath, [], { encoding: "utf8" });
  assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
  assert.match(run.stdout, /papercolor portal checks passed/);

  const sanitizedExecutablePath = join(temporaryDirectory, "portal_test_sanitized");
  const sanitizedCompile = spawnSync(
    compiler,
    [
      "-std=c++11",
      "-O1",
      "-g",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-fsanitize=address,undefined",
      "-fno-omit-frame-pointer",
      "-I",
      portalSource.pathname,
      harnessPath,
      new URL("OnboardingState.cpp", portalSource).pathname,
      new URL("PortalEncoding.cpp", portalSource).pathname,
      new URL("InkloopPortal.cpp", portalSource).pathname,
      "-o",
      sanitizedExecutablePath,
    ],
    { encoding: "utf8" },
  );
  assert.equal(
    sanitizedCompile.status,
    0,
    `${sanitizedCompile.stdout}\n${sanitizedCompile.stderr}`,
  );
  const sanitizedRun = spawnSync(sanitizedExecutablePath, [], {
    encoding: "utf8",
    env: {
      ...process.env,
      ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    },
  });
  assert.equal(
    sanitizedRun.status,
    0,
    `${sanitizedRun.stdout}\n${sanitizedRun.stderr}`,
  );
  assert.match(sanitizedRun.stdout, /papercolor portal checks passed/);

  await rm(temporaryDirectory, { recursive: true, force: true });
});
