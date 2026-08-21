#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_system.h>
#include <time.h>

#include "AppConfig.h"
#include "AlbumPrimitives.h"
#include "AlbumStore.h"
#include "AudioPrompt.h"
#include "BusyButtonCapture.h"
#include "ButtonRouter.h"
#include "CompatibilityPrimitives.h"
#include "Diagnostics.h"
#include "DisplayController.h"
#include "DisplayTransaction.h"
#include "InkloopClient.h"
#include "LedStatusController.h"
#include "PaperColorBoardSupport.h"
#include "PaperColorApplicationRuntime.h"
#include "PageSelectionPrimitives.h"
#include "SettingsStore.h"
#include "Storage.h"
#include "StorageRecoveryPrimitives.h"
#include "TaskStore.h"
#include "WifiProvisioningPrimitives.h"

extern "C" {
__attribute__((used)) char inkloop_api_url_slot[192] = "INKLOOP_API_URL_SLOT::";
}

namespace {

using namespace inkloop;

LittleFsStorage littleFs;
SdStorage sdStorage;
StorageManager storage(littleFs);
TaskStore tasks(storage.taskStorage());
SettingsStore settings;
LedStatusController leds(settings);
PaperColorBoardSupport board;
DisplayController display;
InkloopClient client(tasks);
ButtonRouter buttons;
AlbumStore album(storage);
AudioPrompt audioPrompt;
DisplayTransaction displayTransaction(storage, album, tasks);
BusyButtonCapture busyButtonCapture;
WiFiManager wifiManager;
WifiProvisioningState wifiProvisioning;

bool queueRuntimePage(void*, size_t page, const StorageBackendRef& backend);
void acceptPendingPage(
  size_t page,
  const StorageBackendRef& backend,
  uint32_t nowMilliseconds
);
void clearPendingPage(uint32_t nowMilliseconds);
PaperColorApplicationRuntime applicationRuntime(
  storage,
  sdStorage,
  album,
  tasks,
  client,
  display,
  displayTransaction,
  leds,
  settings,
  buttons,
  queueRuntimePage,
  nullptr
);

String wifiAccessPoint;
String serialCommand;
uint32_t lastSyncAt = 0;
uint32_t lastScheduleAt = 0;
uint32_t lastHeartbeatAt = 0;
bool littleFsReady = false;
bool albumReady = false;
bool applicationRuntimeReady = false;
bool onlineInitializationComplete = false;
bool onlineInitializationFailed = false;
volatile uint8_t lastWifiDisconnectReason = 0;
volatile bool wifiDisconnectObserved = false;
StorageRecoveryState storageRecovery = storageRecoveryState(false, false);
bool pendingPageReady = false;
size_t pendingPage = 0;
StorageBackendRef pendingPageBackend;
uint32_t pendingPageAt = 0;
uint32_t lastFullRefreshAt = 0;

constexpr uint8_t kNvsStartupAttempts = 3;
constexpr uint32_t kNvsRecoveryIntervalMs = 30000;
constexpr uint32_t kFullRefreshCooldownMs = 30000;
constexpr uint32_t kPageSelectionSettleMs = 1000;

void pollSerialConsole();
void printDiagnosticStatus();
void completeOnlineInitialization();

const char* savedWifiFailureName() {
  if (!wifiDisconnectObserved) return "CONNECT_TIMEOUT";
  switch (lastWifiDisconnectReason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "AUTHENTICATION_FAILED";
    case WIFI_REASON_NO_AP_FOUND:
      return "SSID_NOT_FOUND";
    default:
      return "ASSOCIATION_FAILED";
  }
}

bool beginSettingsWithRetry() {
  for (uint8_t attempt = 1; attempt <= kNvsStartupAttempts; ++attempt) {
    if (settings.begin()) return true;
    Diagnostics::event("WARN", String("SETTINGS_NVS_RETRY_") + String(attempt));
    delay(200U * attempt);
  }
  return false;
}

bool beginIdentityWithRetry() {
  for (uint8_t attempt = 1; attempt <= kNvsStartupAttempts; ++attempt) {
    if (client.beginIdentity()) return true;
    Diagnostics::event("WARN", String("IDENTITY_NVS_RETRY_") + String(attempt));
    delay(200U * attempt);
  }
  return false;
}

[[noreturn]] void haltCritical(const char* code, const String& detail, bool displayReady, bool ledReady) {
  Diagnostics::event("FATAL", code);
  if (ledReady) leds.setCombinedColor(255, 0, 0, 48);
  board.playTone(330, 320);
  if (displayReady) display.showStatus("Device storage error", detail, code, RED);
  Diagnostics::event("STATE", "SAFE_HALT");
  while (true) {
    M5.update();
    delay(250);
  }
}

[[noreturn]] void recoverPersistentState(
  PersistenceReadiness readiness,
  bool displayReady,
  bool ledReady
) {
  Diagnostics::event("FATAL", "PERSISTENCE_UNAVAILABLE");
  if (ledReady) leds.setCombinedColor(255, 0, 0, 48);
  board.playTone(330, 320);
  if (displayReady) {
    display.showStatus(
      "Device storage error",
      "Inkloop is offline; retrying every 30 seconds",
      "NVS",
      RED
    );
  }
  Diagnostics::event("STATE", "WAITING_NVS_RECOVERY");

  uint32_t lastRetryAt = millis() - kNvsRecoveryIntervalMs;
  while (true) {
    M5.update();
    const uint32_t now = millis();
    if (now - lastRetryAt >= kNvsRecoveryIntervalMs) {
      lastRetryAt = now;
      Diagnostics::event("NVS_RECOVERY_ATTEMPT");
      if (!readiness.settingsReady) readiness.settingsReady = settings.begin();
      if (readiness.settingsReady && !readiness.identityReady) {
        readiness.identityReady = client.beginIdentity();
      }
      if (readiness.safeToStartNetwork()) {
        Diagnostics::event("STATE", "NVS_RECOVERED_REBOOTING");
        Serial.flush();
        delay(200);
        ESP.restart();
      }
    }
    delay(100);
  }
}

void acceptPendingPage(
  size_t page,
  const StorageBackendRef& backend,
  uint32_t nowMilliseconds
) {
  pendingPage = page;
  pendingPageBackend = backend;
  pendingPageReady = true;
  pendingPageAt = nowMilliseconds;
  if (applicationRuntimeReady) {
    applicationRuntime.setExternalPagePending(true, nowMilliseconds);
  }
}

void clearPendingPage(uint32_t nowMilliseconds) {
  pendingPageReady = false;
  pendingPage = 0;
  pendingPageBackend = StorageBackendRef();
  pendingPageAt = 0;
  if (applicationRuntimeReady) {
    // Clearing is also meaningful activity. The full idle timeout starts here,
    // so a completed or failed page action cannot immediately enter sleep.
    applicationRuntime.setExternalPagePending(false, nowMilliseconds);
  }
}

void onButton(ButtonEvent event, void*) {
  Diagnostics::event("BUTTON", ButtonRouter::name(event));
  if (wifiProvisioning.provisioning() || !onlineInitializationComplete) {
    Diagnostics::event("BUTTON_REJECTED", wifiProvisioning.provisioning()
      ? "WIFI_PROVISIONING"
      : "BOOT_NOT_READY");
    return;
  }
  if (applicationRuntimeReady && !applicationRuntime.acceptsUserInput()) {
    Diagnostics::event("BUTTON_REJECTED", "WAKE_RECOVERY");
    return;
  }
  if (event == ButtonEvent::Voice) {
    if (applicationRuntimeReady) applicationRuntime.handleButton(event);
    return;
  }
  if (!storageRecovery.taskStoreReady) {
    Diagnostics::event("PAGE_REJECTED", "TASK_STORE_UNAVAILABLE");
    return;
  }
  if (!albumReady || useLegacyDirectDisplay(
          settings.current().features.myAiEnabled,
          settings.current().features.albumEnabled)) return;
  if (applicationRuntimeReady && applicationRuntime.albumUploadActive()) {
    Diagnostics::event("PAGE_REJECTED", "ALBUM_UPLOAD_ACTIVE");
    return;
  }
  const bool panelCooldown = lastFullRefreshAt &&
    millis() - lastFullRefreshAt < kFullRefreshCooldownMs;
  if (display.busy() || applicationRuntime.displayBusy() ||
      displayTransaction.active() || panelCooldown) {
    if (applicationRuntimeReady) applicationRuntime.notifyPageBusy();
    audioPrompt.requestDisplayBusy();
    Diagnostics::event("PAGE_REJECTED", panelCooldown
      ? "PANEL_COOLDOWN"
      : (display.busy() ? "DISPLAY_BUSY" : "TXN_PENDING"));
    return;
  }
  AlbumPageState state;
  const bool stateReady = pendingPageReady
    ? album.pageState(pendingPageBackend, state)
    : album.pageState(state);
  if (!stateReady) {
    clearPendingPage(millis());
    leds.setRoleState(LedRole::Image, LedState::Error, 40);
    return;
  }
  const size_t base = pendingPageReady ? pendingPage : state.current;
  const int8_t direction = event == ButtonEvent::PreviousPage ? -1 : 1;
  const PageSelection selection = selectAdjacentPage(base, state.count, direction, false);
  if (!selection.accepted) {
    audioPrompt.requestPageBoundary();
    Diagnostics::event("PAGE_REJECTED", "BOUNDARY");
    return;
  }
  acceptPendingPage(selection.page, state.backend, millis());
  if (applicationRuntimeReady) {
    applicationRuntime.notifyPageSelected(selection.page + 1);
  }
  Diagnostics::event("PAGE_QUEUED", String(pendingPage + 1));
}

bool queueRuntimePage(void*, size_t page, const StorageBackendRef& backend) {
  if (!storageRecovery.taskStoreReady || !albumReady || pendingPageReady ||
      (applicationRuntimeReady && applicationRuntime.albumUploadActive()) ||
      applicationRuntime.displayBusy() ||
      (lastFullRefreshAt && millis() - lastFullRefreshAt < kFullRefreshCooldownMs)) {
    return false;
  }
  acceptPendingPage(page, backend, millis());
  Diagnostics::event("VOICE_PAGE_QUEUED", String(page + 1));
  return true;
}

bool safeShowStatus(
  const String& title,
  const String& detail,
  const String& value = "",
  uint16_t accent = BLUE
) {
  if (applicationRuntime.ownsDisplay()) {
    Diagnostics::event("DISPLAY_STATUS_SERIAL_ONLY", title + ":" + detail + ":" + value);
    return false;
  }
  if (displayTransaction.active()) {
    Diagnostics::event("DISPLAY_STATUS_SKIPPED", "TXN_PENDING");
    return false;
  }
  return display.showStatus(title, detail, value, accent);
}

bool safeShowPortalAccess(
  const String& title,
  const String& detail,
  const String& accessCode,
  uint16_t accent = BLUE
) {
  if (applicationRuntime.ownsDisplay()) {
    Diagnostics::event("DISPLAY_PORTAL_ACCESS", "SERIAL_ONLY_NO_SECRET");
    return false;
  }
  if (displayTransaction.active()) {
    Diagnostics::event("DISPLAY_PORTAL_ACCESS", "TXN_PENDING");
    return false;
  }
  return display.showPortalAccess(title, detail, accessCode, accent);
}

bool safeShowSettingsPortal(
  const String& title,
  const String& detail,
  const String& accessPoint,
  const String& ipAddress,
  const String& accessCode,
  uint16_t accent = BLUE
) {
  if (applicationRuntime.ownsDisplay()) {
    Diagnostics::event("DISPLAY_SETTINGS_PORTAL", "SERIAL_ONLY_NO_SECRET");
    return false;
  }
  if (displayTransaction.active()) {
    Diagnostics::event("DISPLAY_SETTINGS_PORTAL", "TXN_PENDING");
    return false;
  }
  return display.showSettingsPortal(
    title, detail, accessPoint, ipAddress, accessCode, accent
  );
}

bool safeShowWifiSetup(
  const String& title,
  const String& detail,
  const String& accessPoint,
  uint16_t accent = YELLOW
) {
  if (applicationRuntime.ownsDisplay() || displayTransaction.active()) {
    Diagnostics::event("DISPLAY_WIFI_SETUP", "UNAVAILABLE");
    return false;
  }
  return display.showWifiSetup(title, detail, accessPoint, accent);
}

void reportWifiSetupTimeout() {
  Diagnostics::event("ERROR", "WIFI_SETUP_TIMEOUT");
  leds.setCombinedColor(255, 0, 0, 48);
  board.playTone(330, 260);
  safeShowStatus("Wi-Fi needed", "Restart to try setup again", "", RED);
}

void startClockSync() {
  Diagnostics::event("WIFI_CONNECTED", WiFi.localIP().toString());
  Diagnostics::event("STATE", "WAITING_CLOCK_SYNC");
  leds.setCombinedColor(0, 90, 255, 36);
  configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
}

void showWifiPortal(WifiPortalReason reason) {
  const char* typedReason = wifiPortalReasonName(reason);
  const bool savedAttemptFailed = reason == WifiPortalReason::SavedConnectTimeout;
  const char* savedFailure = savedAttemptFailed ? savedWifiFailureName() : "NONE";
  Diagnostics::event("WIFI_PROVISIONING_REASON", typedReason);
  if (savedAttemptFailed) {
    Diagnostics::event("WIFI_SAVED_NETWORK", savedFailure);
    Diagnostics::event(
      "WIFI_DISCONNECT_REASON",
      wifiDisconnectObserved ? String(lastWifiDisconnectReason) : String("NOT_OBSERVED")
    );
  }
  Diagnostics::event("WIFI_AP", wifiAccessPoint);
  Diagnostics::event("STATE", "WAITING_WIFI");
  leds.setCombinedColor(255, 150, 0, 42);
  safeShowWifiSetup(
    storageRecovery.internalRecoveryRequired ? "Storage recovery required" : "Connect Wi-Fi",
    storageRecovery.internalRecoveryRequired
      ? "Data not erased; SD is images only. Run serial diag for recovery."
      : (savedAttemptFailed
          ? (String(savedFailure) == "AUTHENTICATION_FAILED"
              ? "Saved password rejected - open http://192.168.4.1/"
              : "Saved network unavailable - open http://192.168.4.1/")
          : "No saved Wi-Fi - open http://192.168.4.1/"),
    wifiAccessPoint,
    storageRecovery.internalRecoveryRequired ? RED : YELLOW
  );
}

void applyWifiProvisioningActions(const WifiProvisioningActions& actions) {
  if (actions.stopPortal && wifiManager.getConfigPortalActive()) {
    wifiManager.stopConfigPortal();
  }
  if (actions.startPortal) {
    wifiManager.startConfigPortal(wifiAccessPoint.c_str());
    const bool portalActive = wifiManager.getConfigPortalActive();
    if (!portalActive && WiFi.status() != WL_CONNECTED) {
      applyWifiProvisioningActions(wifiProvisioning.failPortalStart());
      return;
    }
    if (portalActive) showWifiPortal(actions.portalReason);
  }
  if (actions.startClockSync) startClockSync();
  if (actions.reportTimeout) reportWifiSetupTimeout();
  if (actions.finalizeOnline) completeOnlineInitialization();
}

void beginWifiProvisioning() {
  const String hardwareId = client.hardwareId();
  const String suffix = hardwareId.substring(hardwareId.length() - 4);
  wifiAccessPoint = "Inkloop-" + suffix;
  wifiManager.setConfigPortalBlocking(false);
  // WiFiManager credential submission may synchronously try association.
  // Keep that single library call bounded; the full legacy 25-second saved
  // credential policy is enforced cooperatively by WifiProvisioningState.
  wifiManager.setConnectTimeout(kWifiManagerConnectCallBoundMs / 1000U);
  wifiManager.setSaveConnectTimeout(kWifiManagerConnectCallBoundMs / 1000U);
  wifiManager.setConfigPortalTimeout(0);
  wifiManager.setShowStaticFields(false);
  wifiManager.setShowDnsFields(false);
  wifiManager.setSaveConfigCallback([]() {
    // WiFiManager invokes this only after wifiConnectNew() has persisted and
    // successfully associated the submitted SSID/password.
    Diagnostics::event("WIFI_CREDENTIALS", "PERSISTED_AND_CONNECTED");
  });
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
      wifiDisconnectObserved = true;
    }
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  const bool connected = WiFi.status() == WL_CONNECTED;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  const bool hasSavedCredentials = wifiManager.getWiFiSSID(true).length() > 0;
  Diagnostics::event(
    "WIFI_CREDENTIALS", hasSavedCredentials ? "PRESENT" : "ABSENT");
  const WifiProvisioningActions initial = wifiProvisioning.start(
    millis(), connected, hasSavedCredentials
  );
  if (!connected && hasSavedCredentials) {
    Diagnostics::event("WIFI_SAVED_NETWORK", "CONNECTING");
    Diagnostics::event("STATE", "CONNECTING_SAVED_WIFI");
    leds.setCombinedColor(0, 90, 255, 36);
    // PaperColor is a slow full-refresh e-paper panel. Keep the existing
    // image while saved Wi-Fi is attempted; serial diagnostics and RGB carry
    // this transient state. Only stable states or required user actions draw.
    Diagnostics::event("DISPLAY_STABLE", "PRESERVED_DURING_WIFI_CONNECT");
    WiFi.begin();
  }
  applyWifiProvisioningActions(initial);
}

void pollWifiProvisioning() {
  if (!wifiProvisioning.provisioning()) return;
  pollSerialConsole();
  if (wifiProvisioning.portalActive()) wifiManager.process();
  pollSerialConsole();
  applyWifiProvisioningActions(wifiProvisioning.tick(
    millis(),
    WiFi.status() == WL_CONNECTED,
    time(nullptr) >= 1700000000
  ));
}

bool registerAndPresent() {
  if (applicationRuntimeReady) {
    Diagnostics::event("REGISTER_BLOCKED", "MYAI_CODE_REQUIRED");
    return false;
  }
  const RegistrationResult registration = client.registerDevice();
  if (!registration.ok) return false;
  if (!registration.paired) {
    Diagnostics::event("PAIR_CODE", registration.pairingCode);
    Diagnostics::event("STATE", "WAITING_BIND");
    leds.setCombinedColor(70, 30, 255, 42);
    board.playTone(1319, 100);
    safeShowStatus("Device code", "Bind in Inkloop > Add Device", registration.pairingCode, BLUE);
  } else {
    Diagnostics::event("STATE", "PAIRED");
  }
  return true;
}

bool syncAndPresent() {
  if (applicationRuntimeReady && applicationRuntime.mutationBusy()) {
    Diagnostics::event("TASK_SYNC_DEFERRED", "APPLICATION_MUTATION_BUSY");
    return false;
  }
  if (!storageRecovery.taskStoreReady) {
    Diagnostics::event("TASK_SYNC_BLOCKED", "TASK_STORE_UNAVAILABLE");
    return false;
  }
  const SyncResult sync = client.syncTasks();
  if (sync.requiresRegistration) {
    return applicationRuntimeReady ? false : registerAndPresent();
  }
  if (sync.becamePaired) {
    Diagnostics::event("STATE", "PAIRED");
    leds.setCombinedColor(0, 255, 60, 42);
    board.playTone(1568, 120);
    safeShowStatus("Inkloop connected", "Schedules now run on this device", "", GREEN);
  }
  return sync.ok;
}

bool runDueTasks() {
  if (applicationRuntimeReady && applicationRuntime.mutationBusy()) {
    Diagnostics::event("TASK_RUN_DEFERRED", "APPLICATION_MUTATION_BUSY");
    return false;
  }
  const time_t now = time(nullptr);
  if (!storageRecovery.taskStoreReady || displayTransaction.active() ||
      applicationRuntime.displayBusy() ||
      !client.paired() || now < 1700000000 ||
      WiFi.status() != WL_CONNECTED) return false;
  tm local{};
  localtime_r(&now, &local);
  DueTask due;
  if (!tasks.firstDue(now, local, due)) return false;

  leds.setRoleState(LedRole::Image, LedState::Downloading, 40);
  DownloadedFrame frame;
  const bool downloaded = client.downloadFrame(due.frameUrl, frame);
  if (!downloaded) {
    leds.setRoleState(LedRole::Image, LedState::Error, 44);
    Diagnostics::event("ERROR", "FRAME_DOWNLOAD_FAILED");
    return false;
  }

  const uint32_t runDay = TaskStore::localDayStamp(local);
  if (useLegacyDirectDisplay(
          settings.current().features.myAiEnabled,
          settings.current().features.albumEnabled)) {
    leds.setRoleState(LedRole::Image, LedState::Writing, 44);
    buttons.suppressUntilRelease();
    const DirectDisplayResult direct = runAlbumDisabledDirectPath(
      [&]() {
        const String assetId = due.frameHash.length() ? due.frameHash : due.id;
        const bool shown = applicationRuntime.ownsDisplay()
          ? applicationRuntime.refreshFrame(
              assetId, frame.bytes, frame.length, true, due.renderStrategy)
          : display.showPng(frame.bytes, frame.length, frame.landscape);
        if (shown) lastFullRefreshAt = millis();
        return shown;
      },
      [&]() { return tasks.markRunWithDay(due, now, runDay); }
    );
    frame.release();
    leds.setRoleState(
      LedRole::Image,
      direct.displayed && direct.acknowledged ? LedState::Complete : LedState::Error,
      40
    );
    Diagnostics::event("DIRECT_0_2_DISPLAY", direct.displayed
      ? (direct.acknowledged ? "COMPLETE" : "TASK_ACK_RETRY")
      : "FAILED");
    return direct.displayed;
  }

  bool shown = false;
  AlbumAsset asset;
  size_t taskNextBytes = 0;
  size_t journalRecordBytes = 0;
  if (!tasks.acknowledgementPayloadSize(due, now, runDay, taskNextBytes) ||
      !DisplayTransaction::estimateJournalRecordBytes(
        &due, now, runDay, journalRecordBytes
      )) {
    frame.release();
    leds.setRoleState(LedRole::Image, LedState::Error, 44);
    Diagnostics::event("ERROR", "METADATA_MEASURE_FAILED");
    return false;
  }
  const MetadataBudget metadataBudget(taskNextBytes, journalRecordBytes);
  leds.setRoleState(LedRole::Image, LedState::Caching, 40);
  if (albumReady && applicationRuntimeReady &&
      applicationRuntime.reserveAlbumRevisionForScheduledCache() &&
      album.cacheFrame(
        frame, due.frameHash, due.id, due.renderStrategy, metadataBudget, asset
      ) && displayTransaction.begin(asset, "task", &due, now, runDay)) {
    leds.setRoleState(LedRole::Image, LedState::Writing, 44);
    buttons.suppressUntilRelease();
    shown = applicationRuntime.ownsDisplay()
      ? applicationRuntime.refreshFrame(
          asset.id, frame.bytes, frame.length, true, asset.renderStrategy)
      : display.showPng(frame.bytes, frame.length, frame.landscape);
    if (shown) {
      lastFullRefreshAt = millis();
      displayTransaction.confirmDisplayed();
      displayTransaction.retryFinalize();
    } else {
      displayTransaction.abortBeforeDisplay();
    }
  }
  frame.release();
  const bool completed = shown && !displayTransaction.active();
  leds.setRoleState(LedRole::Image, completed ? LedState::Complete : LedState::Error, 40);
  return shown;
}

bool processPendingPage() {
  if (!pendingPageReady ||
      millis() - pendingPageAt < kPageSelectionSettleMs ||
      displayTransaction.active()) return false;
  if (applicationRuntimeReady && applicationRuntime.albumUploadActive()) {
    Diagnostics::event("PAGE_DEFERRED", "ALBUM_UPLOAD_ACTIVE");
    return false;
  }
  if (!storageRecovery.taskStoreReady) {
    clearPendingPage(millis());
    Diagnostics::event("DISPLAY_JOURNAL_BLOCKED", "TASK_STORE_UNAVAILABLE");
    return false;
  }
  const size_t target = pendingPage;
  const StorageBackendRef backend = pendingPageBackend;
  AlbumPageState settledState;
  if (!album.pageState(backend, settledState)) {
    Diagnostics::event("PAGE_REJECTED", "STATE_UNAVAILABLE");
    clearPendingPage(millis());
    return false;
  }
  const SettledPageDecision decision = settledPageDecision(
      settledState.count, settledState.current, target);
  if (decision == SettledPageDecision::Invalid) {
    Diagnostics::event("PAGE_REJECTED", "INVALID_SETTLED_TARGET");
    clearPendingPage(millis());
    return false;
  }
  if (decision == SettledPageDecision::AlreadyCurrent) {
    Diagnostics::event("PAGE_SKIPPED", "ALREADY_CURRENT");
    clearPendingPage(millis());
    return true;
  }
  if (display.busy() || applicationRuntime.displayBusy() ||
      (lastFullRefreshAt && millis() - lastFullRefreshAt < kFullRefreshCooldownMs)) {
    if (applicationRuntimeReady) applicationRuntime.notifyPageBusy();
    audioPrompt.requestDisplayBusy();
    Diagnostics::event("PAGE_REJECTED", "DISPLAY_BUSY");
    clearPendingPage(millis());
    return false;
  }
  leds.setRoleState(LedRole::Image, LedState::Caching, 40);
  DownloadedFrame frame;
  AlbumAsset asset;
  if (!album.loadPage(backend, target, frame, asset)) {
    leds.setRoleState(LedRole::Image, LedState::Error, 44);
    Diagnostics::event("ERROR", "PAGE_LOAD_FAILED");
    clearPendingPage(millis());
    return false;
  }
  if (!displayTransaction.begin(asset, "page", nullptr, 0)) {
    frame.release();
    leds.setRoleState(LedRole::Image, LedState::Error, 44);
    clearPendingPage(millis());
    return false;
  }
  if (applicationRuntimeReady) {
    applicationRuntime.notifyPageRefreshStarting(target + 1);
  }
  leds.setRoleState(LedRole::Image, LedState::Writing, 44);
  buttons.suppressUntilRelease();
  const bool shown = applicationRuntime.ownsDisplay()
    ? applicationRuntime.refreshFrame(
        asset.id, frame.bytes, frame.length, true, asset.renderStrategy)
    : display.showPng(frame.bytes, frame.length, frame.landscape);
  frame.release();
  if (shown) {
    lastFullRefreshAt = millis();
    displayTransaction.confirmDisplayed();
    displayTransaction.retryFinalize();
  } else {
    displayTransaction.abortBeforeDisplay();
  }
  const bool committed = shown && !displayTransaction.active();
  leds.setRoleState(LedRole::Image, committed ? LedState::Complete : LedState::Error, 40);
  if (committed) {
    applicationRuntime.onPageDisplayCommitted(asset.id, target + 1, true);
    Diagnostics::event("PAGE_READY", String(target + 1));
  }
  if (!committed) applicationRuntime.onPageDisplayCommitted(asset.id, target + 1, false);
  clearPendingPage(millis());
  return shown;
}

void printDiagnosticStatus() {
  DiagnosticSnapshot snapshot;
  snapshot.buildVersion = kBuildVersion;
  snapshot.protocolVersion = kProtocolFirmwareVersion;
  snapshot.hardwareId = client.hardwareId();
  snapshot.board = board.boardId();
  snapshot.pm1Ready = board.pm1Ready();
  snapshot.wifiConnected = WiFi.status() == WL_CONNECTED;
  snapshot.ip = snapshot.wifiConnected ? WiFi.localIP().toString() : "";
  snapshot.deviceId = client.deviceId();
  snapshot.paired = client.paired();
  snapshot.pairingCode = client.paired() ? String() : client.pairingCode();
  snapshot.revision = client.appliedRevision();
  snapshot.displayBusy = display.busy() || applicationRuntime.displayBusy();
  snapshot.littleFsReady = littleFsReady;
  snapshot.bootPhase = onlineInitializationFailed
    ? "runtime_failed"
    : wifiProvisioning.phaseName();
  snapshot.wifiProvisioning = wifiProvisioning.provisioning();
  snapshot.wifiPortalActive = wifiProvisioning.portalActive() &&
    wifiManager.getConfigPortalActive();
  snapshot.internalMounted = storageRecovery.internalMounted;
  snapshot.internalRecoveryRequired = storageRecovery.internalRecoveryRequired;
  snapshot.taskStoreReady = storageRecovery.taskStoreReady;
  snapshot.assetBackend = recoveryAssetModeName(storageRecovery.assetMode);
  snapshot.dataPreserved = storageRecovery.dataPreserved;
  snapshot.ledCount = leds.count();
  snapshot.ledMappingCalibrated = leds.mappingCalibrated();
  snapshot.voiceLedIndex = leds.voiceLedIndex();
  Diagnostics::status(snapshot);
}

void reportLedMapping() {
  const String value = leds.mappingCalibrated()
    ? String("VOICE_") + String(leds.voiceLedIndex())
    : "UNCALIBRATED";
  Diagnostics::event("LED_MAPPING", value);
}

bool isKnownSerialCommand(const String& command) {
  return command == "help" || command == "status" || command == "diag" ||
    command == "pair-code" || command == "portal-recover-bound" ||
    command == "album-status" || command == "display-txn" ||
    command == "display-recover target" || command == "display-recover previous" ||
    command == "led-test" || command == "led-map" || command == "led-map 0" ||
    command == "led-map 1" || command == "led-map swap" || command == "led-map auto" ||
    command == "sound-test" || command == "screen-test" ||
    command == "myai-enable" || command == "myai-disable" || command == "reboot";
}

void executeSerialCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (!command.length()) return;
  Diagnostics::event("COMMAND", command);
  const bool readOnlyDuringBoot = command == "help" || command == "status" || command == "diag";
  if (!readOnlyDuringBoot && isKnownSerialCommand(command) &&
      (wifiProvisioning.provisioning() || !onlineInitializationComplete)) {
    Diagnostics::event(
      "COMMAND_UNAVAILABLE",
      wifiProvisioning.provisioning() ? "WIFI_PROVISIONING" : "BOOT_NOT_READY"
    );
    return;
  }
  if (command == "help") {
    Diagnostics::event("HELP", "help,status,pair-code,portal-recover-bound,album-status,display-txn,display-recover [target|previous],led-test,led-map [0|1|swap|auto],sound-test,screen-test,myai-enable,myai-disable,reboot");
  } else if (command == "status" || command == "diag") {
    printDiagnosticStatus();
  } else if (command == "pair-code") {
    Diagnostics::event("PAIR_CODE", client.paired()
      ? "BOUND_NO_CODE"
      : (client.pairingCode().length() ? client.pairingCode() : "UNAVAILABLE"));
  } else if (command == "portal-recover-bound") {
    std::string error;
    const bool recovered = applicationRuntime.recoverPortalBoundState(&error);
    Diagnostics::event(
      "PORTAL_RECOVERY",
      recovered ? "BOUND_STATE_RESTORED_REBOOT_REQUIRED" : error.c_str()
    );
  } else if (command == "album-status") {
    AlbumPageState state;
    const bool ready = albumReady && album.pageState(state);
    Diagnostics::event("ALBUM", ready
      ? String(state.backend.identity) + ":" + String(state.count) + ":" + String(state.count ? state.current + 1 : 0)
      : "UNAVAILABLE");
  } else if (command == "display-txn") {
    Diagnostics::event("DISPLAY_TXN", displayTransaction.active()
      ? String(displayTransaction.backendIdentity()) + ":" + String(static_cast<int>(displayTransaction.stage())) + ":" + displayTransaction.assetId().substring(0, 12)
      : "NONE");
  } else if (command == "display-recover target") {
    const bool recovered = displayTransaction.resolveAmbiguousAsTarget();
    Diagnostics::event("DISPLAY_RECOVERY", recovered ? "TARGET_COMMITTED" : "FAILED");
  } else if (command == "display-recover previous") {
    const bool recovered = displayTransaction.resolveAmbiguousAsPrevious();
    Diagnostics::event("DISPLAY_RECOVERY", recovered ? "PREVIOUS_RETAINED" : "FAILED");
  } else if (command == "led-test") {
    Diagnostics::event(
      "TEST",
      leds.runPixelDiagnostic() ? "LED_ROLE_TEST_STARTED" : "LED_ROLE_TEST_REJECTED"
    );
  } else if (command == "led-map") {
    reportLedMapping();
  } else if (command == "led-map 0" || command == "led-map 1") {
    leds.setMapping(true, command.endsWith("1") ? 1 : 0);
  } else if (command == "led-map swap") {
    leds.setMapping(true, leds.voiceLedIndex() == 0 ? 1 : 0);
  } else if (command == "led-map auto") {
    leds.setMapping(false, 0);
  } else if (command == "sound-test") {
    board.playTone(880, 220);
    Diagnostics::event("TEST", "SOUND_OK");
  } else if (command == "screen-test") {
    if (displayTransaction.active()) {
      Diagnostics::event("ERROR", "DISPLAY_TXN_PENDING");
    } else if (applicationRuntime.ownsDisplay()) {
      Diagnostics::event("TEST", "SCREEN_TEST_REQUIRES_400X600_PNG");
    } else {
      buttons.suppressUntilRelease();
      safeShowStatus("Inkloop diagnostics", "Screen refresh completed", client.hardwareId(), GREEN);
      Diagnostics::event("TEST", "SCREEN_OK");
    }
  } else if (command == "myai-enable" || command == "myai-disable") {
    const bool enabled = command == "myai-enable";
    Diagnostics::event(
      "MYAI_FEATURE",
      settings.setMyAiEnabled(enabled) ? (enabled ? "ENABLED_REBOOT_REQUIRED" : "DISABLED_REBOOT_REQUIRED")
                                       : "PERSIST_FAILED"
    );
  } else if (command == "reboot") {
    Diagnostics::event("STATE", "REBOOTING");
    Serial.flush();
    delay(120);
    ESP.restart();
  } else {
    Diagnostics::event("ERROR", "UNKNOWN_COMMAND");
  }
}

void pollSerialConsole() {
  while (Serial.available()) {
    const char next = static_cast<char>(Serial.read());
    if (next == '\r') continue;
    if (next == '\n') {
      executeSerialCommand(serialCommand);
      serialCommand = "";
    } else if (serialCommand.length() < 96 && next >= 32 && next <= 126) {
      serialCommand += next;
    }
  }
}

void completeOnlineInitialization() {
  if (onlineInitializationComplete || onlineInitializationFailed) return;
  if (WiFi.status() != WL_CONNECTED) {
    Diagnostics::event("ERROR", "ONLINE_INIT_WITHOUT_WIFI");
    onlineInitializationFailed = true;
    return;
  }
  if (settings.current().features.myAiEnabled) {
    applicationRuntimeReady = applicationRuntime.begin(true);
    if (!applicationRuntimeReady) {
      onlineInitializationFailed = true;
      leds.setCombinedColor(255, 0, 0, 48);
      safeShowStatus(
        "Runtime unavailable",
        "MyAI identity or local portal could not start",
        "Check serial diagnostics",
        RED
      );
      Diagnostics::event("STATE", "PAPERCOLOR_RUNTIME_FAIL_CLOSED");
      printDiagnosticStatus();
      return;
    }
    const StableStartupDisplay startupDisplay =
      applicationRuntime.stableStartupDisplay();
    if (startupDisplay == StableStartupDisplay::PreserveExisting) {
      Diagnostics::event("DISPLAY_STABLE", "PRESERVED_EXISTING_CONTENT");
    } else if (storageRecovery.internalRecoveryRequired) {
      safeShowSettingsPortal(
        "Storage recovery required",
        "Data not erased; SD is images only. Run serial diag for recovery.",
        applicationRuntime.settingsAccessPoint(),
        applicationRuntime.settingsIpAddress(),
        applicationRuntime.portalAccessCode().c_str(),
        RED
      );
    } else if (startupDisplay == StableStartupDisplay::MyAiServiceUnavailable) {
      safeShowSettingsPortal(
        "MyAI service unavailable",
        "No binding QR - contact Inkloop developer",
        applicationRuntime.settingsAccessPoint(),
        applicationRuntime.settingsIpAddress(),
        applicationRuntime.portalAccessCode().c_str(),
        RED
      );
    } else if (startupDisplay == StableStartupDisplay::SettingsReady) {
      safeShowSettingsPortal(
        "Inkloop settings",
        "Connect to Settings Wi-Fi, then open inkloop.local",
        applicationRuntime.settingsAccessPoint(),
        applicationRuntime.settingsIpAddress(),
        applicationRuntime.portalAccessCode().c_str(),
        BLUE
      );
    } else {
      Diagnostics::event(
        "DISPLAY_STABLE",
        startupDisplay == StableStartupDisplay::AwaitingMyAiPairing
          ? "WAITING_FOR_AUTHORITATIVE_MYAI_QR"
          : "NO_TRANSIENT_DISPLAY"
      );
    }
    if (!applicationRuntime.activateDisplayOwner()) {
      onlineInitializationFailed = true;
      leds.setCombinedColor(255, 0, 0, 48);
      Diagnostics::event("FATAL", "SOLE_DISPLAY_WRITER_UNAVAILABLE");
      printDiagnosticStatus();
      return;
    }
  } else {
    Diagnostics::event("PAPERCOLOR_RUNTIME", "LEGACY_0_2_FEATURES_DISABLED");
  }
  if (storageRecovery.taskStoreReady && displayTransaction.active()) {
    // Do not replace /tasks.json while a displayed task is waiting for its
    // durable acknowledgement. The normal loop starts sync only after the
    // journal has finalized, preserving retry ordering without another draw.
    Diagnostics::event("STATE", "DISPLAY_TRANSACTION_BLOCKS_INITIAL_SYNC");
  } else if (!storageRecovery.taskStoreReady) {
    Diagnostics::event("STATE", "TASK_CONTROL_UNAVAILABLE_RECOVERY_REQUIRED");
  } else if (client.deviceId().length()) {
    syncAndPresent();
  } else if (applicationRuntimeReady) {
    Diagnostics::event("STATE", "WAITING_MYAI_PAIRING_FROM_PORTAL");
  } else {
    registerAndPresent();
  }
  lastSyncAt = millis();
  lastScheduleAt = millis();
  lastHeartbeatAt = millis();
  onlineInitializationComplete = true;
  printDiagnosticStatus();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStartedAt = millis();
  while (!Serial && millis() - serialWaitStartedAt < 2500) delay(20);
  delay(100);
  Diagnostics::event("RESET_REASON", String(static_cast<int>(esp_reset_reason())));
  Diagnostics::event("BOOT", kBuildVersion);

  const bool boardReady = board.begin();
  const bool displayReady = display.begin();
  if (!boardReady) haltCritical(
      "BOARD_SUPPORT_UNAVAILABLE",
      "PM1 or RGB power failed; inspect USB diagnostics",
      displayReady,
      false);
  const bool settingsReady = beginSettingsWithRetry();
  const bool ledReady = leds.begin();
  if (!displayReady) haltCritical("DISPLAY_CONTROLLER_UNAVAILABLE", "Restart after checking memory", false, ledReady);
  if (!ledReady) haltCritical("LED_CONTROLLER_UNAVAILABLE", "Restart after checking memory", true, false);
  if (!settingsReady) {
    recoverPersistentState(PersistenceReadiness(false, false), displayReady, ledReady);
  }
  leds.setCombinedColor(0, 70, 255, 36);
  board.playTone();
  littleFsReady = storage.beginDataSafeMode();
  Diagnostics::event("LITTLEFS", littleFsReady ? "READY" : "ERROR");
  storage.attachOptionalSdBackend(&sdStorage);
  bool sdReady = false;
  if (board.prepareSdCard() && board.sdCardInserted()) {
    sdReady = sdStorage.begin(false);
    Diagnostics::event("SD", sdReady ? "READY" : "ERROR");
  } else {
    Diagnostics::event("SD", "NOT_PRESENT");
  }
  storageRecovery = storageRecoveryState(littleFsReady, sdReady);
  if (storageRecovery.internalRecoveryRequired) {
    Diagnostics::event("STORAGE_RECOVERY_REQUIRED", "LITTLEFS_MOUNT_FAILED");
    Diagnostics::event(
      "STORAGE_FALLBACK",
      storageRecovery.assetMode == RecoveryAssetMode::SdAssetsOnly
        ? "SD_ASSETS_ONLY"
        : "NONE"
    );
    Diagnostics::event("TASK_STORE", "UNAVAILABLE_DATA_PRESERVED");
  } else {
    Diagnostics::event("TASK_STORE", "READY");
  }
  albumReady = !useLegacyDirectDisplay(
      settings.current().features.myAiEnabled,
      settings.current().features.albumEnabled) && album.begin();
  Diagnostics::event("ALBUM", albumReady ? album.backendName() : "DISABLED_OR_ERROR");
  if (storageRecovery.taskStoreReady) {
    const bool displayRecoveryReady = displayTransaction.recoverAtBoot();
    if (!displayRecoveryReady && displayTransaction.active()) {
      leds.setRoleState(LedRole::Image, LedState::Error, 44);
      Diagnostics::event("STATE", displayTransaction.ambiguous()
        ? "DISPLAY_RECOVERY_DECISION_REQUIRED"
        : "DISPLAY_METADATA_RETRY");
    }
  } else {
    Diagnostics::event("DISPLAY_JOURNAL_BLOCKED", "TASK_STORE_UNAVAILABLE");
  }
  const bool identityReady = beginIdentityWithRetry();
  const PersistenceReadiness readiness(settingsReady, identityReady);
  if (!readiness.safeToStartNetwork()) recoverPersistentState(readiness, displayReady, ledReady);
  buttons.begin(onButton, nullptr);
  if (!busyButtonCapture.begin(display)) {
    Diagnostics::event("ERROR", "BUSY_BUTTON_CAPTURE_FAILED");
  }

  beginWifiProvisioning();
  printDiagnosticStatus();
}

void loop() {
  M5.update();
  buttons.poll();
  const uint8_t busyAttempts = busyButtonCapture.takeAttempts();
  if (busyAttempts) {
    if (applicationRuntimeReady) applicationRuntime.notifyPageBusy();
    audioPrompt.requestDisplayBusy();
    Diagnostics::event("BUTTON_REJECTED_BUSY", String(busyAttempts));
  }
  pollSerialConsole();
  if (wifiProvisioning.provisioning()) {
    pollWifiProvisioning();
    delay(kProvisioningLoopIntervalMs);
    return;
  }
  if (!onlineInitializationComplete) {
    delay(kProvisioningLoopIntervalMs);
    return;
  }
  audioPrompt.setEnabled(
      !applicationRuntimeReady || applicationRuntime.voiceAssistanceEnabled());
  audioPrompt.poll();
  if (applicationRuntimeReady) applicationRuntime.loop();
  if (displayTransaction.active()) {
    if (!displayTransaction.ambiguous() && displayTransaction.retryFinalize()) {
      leds.setRoleState(LedRole::Image, LedState::Complete, 40);
    } else {
      leds.setRoleState(LedRole::Image, LedState::Error, 44);
    }
    delay(100);
    return;
  }
  if (processPendingPage()) {
    delay(50);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(500);
    return;
  }
  const uint32_t now = millis();
  if (now - lastSyncAt >= kSyncIntervalMs) {
    lastSyncAt = now;
    if (storageRecovery.taskStoreReady && client.deviceId().length()) syncAndPresent();
  }
  if (storageRecovery.taskStoreReady && now - lastScheduleAt >= kScheduleTickMs &&
      (!lastFullRefreshAt || now - lastFullRefreshAt >= kFullRefreshCooldownMs)) {
    lastScheduleAt = now;
    if (runDueTasks()) {
      delay(50);
      return;
    }
  }
  if (now - lastHeartbeatAt >= 5000) {
    lastHeartbeatAt = now;
    Diagnostics::event("HEARTBEAT", String(now));
  }
  delay(50);
}
