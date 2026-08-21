#include "PaperColorPortalRuntime.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "Diagnostics.h"
#include "PortalEncoding.h"
#include "PortalSecurityPrimitives.h"

namespace inkloop {
namespace {

constexpr char kPortalNvsNamespace[] = "ink-portal";
constexpr char kPortalSlotA[] = "snap-a";
constexpr char kPortalSlotB[] = "snap-b";
constexpr char kPortalHead[] = "head";
constexpr char kPortalInitialized[] = "initialized";
constexpr uint8_t kPortalInitializedValue = 0xA5;
constexpr size_t kMaximumPortalRecordBytes = 12288;

std::string sha256Hex(const std::string& value) {
  uint8_t digest[32] = {};
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts_ret(&context, 0);
  mbedtls_sha256_update_ret(
      &context, reinterpret_cast<const uint8_t*>(value.data()), value.size());
  mbedtls_sha256_finish_ret(&context, digest);
  mbedtls_sha256_free(&context);
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (size_t index = 0; index < sizeof(digest); ++index) {
    result.push_back(hex[digest[index] >> 4]);
    result.push_back(hex[digest[index] & 15]);
  }
  return result;
}

bool safeEnum(uint32_t value, uint32_t maximum) { return value <= maximum; }

bool readString(JsonVariantConst value, std::string* output, size_t maximum) {
  if (!output || !value.is<const char*>()) return false;
  const char* text = value.as<const char*>();
  if (!text || std::strlen(text) > maximum) return false;
  *output = text;
  return true;
}

std::string pathWithQuery(WebServer& server) {
  std::string path = server.uri().c_str();
  if (path == "/api/album" && server.hasArg("cursor")) {
    path += "?cursor=";
    path += server.arg("cursor").c_str();
  }
  return path;
}

std::string formEncode(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.length() * 3U);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t ch = static_cast<uint8_t>(value[index]);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      output.push_back(static_cast<char>(ch));
    } else if (ch == ' ') {
      output.push_back('+');
    } else {
      output.push_back('%');
      output.push_back(hex[ch >> 4]);
      output.push_back(hex[ch & 15]);
    }
  }
  return output;
}

bool strictUploadUnsigned(const String& value, uint32_t* output) {
  if (!output || !value.length() || value.length() > 10) return false;
  uint64_t parsed = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char ch = value[index];
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10U + static_cast<uint64_t>(ch - '0');
    if (parsed > 0xffffffffULL) return false;
  }
  *output = static_cast<uint32_t>(parsed);
  return true;
}

}  // namespace

PaperColorPortalRuntime::PaperColorPortalRuntime(
    IPaperColorPortalServices& services)
    : services_(services), server_(80) {}

bool PaperColorPortalRuntime::begin(
    PortalIdentityState identity,
    const std::string& defaultLocalManagementPassword) {
  if (portal_ || serverStarted_) return false;
  const std::string initialPassword =
      initialManagementPassword(defaultLocalManagementPassword);
  portal::PortalPersistedSnapshot snapshot;
  const PortalSnapshotLoadResult loaded = loadSnapshot(snapshot);
  bool snapshotMigrated = false;
  if (portalStorageMayInitializeFresh(loaded, identity)) {
    snapshot = portal::makeFreshPortalSnapshot();
    snapshot.settings.localManagementPassword = initialPassword;
    if (!portal::validLocalManagementPassword(
            snapshot.settings.localManagementPassword) ||
        !storeSnapshot(snapshot)) {
      return false;
    }
  } else if ((loaded == PortalSnapshotLoadResult::Loaded ||
              loaded == PortalSnapshotLoadResult::LoadedLegacy) &&
             snapshot.schemaVersion ==
                 portal::kLegacyPortalSnapshotSchemaVersion &&
             snapshot.presentFields == portal::kLegacyPortalSnapshotFields &&
             snapshot.revision != UINT64_MAX) {
    // v1 used an ephemeral 26-character boot token and did not persist a
    // management password. Preserve every existing onboarding/setting field,
    // add the two v2 settings, and atomically commit into the alternate slot.
    snapshot.schemaVersion = portal::kPortalSnapshotSchemaVersion;
    snapshot.presentFields = portal::kAllPortalSnapshotFields;
    ++snapshot.revision;
    snapshot.settings.localManagementPassword = initialPassword;
    if (!portal::validLocalManagementPassword(
            snapshot.settings.localManagementPassword) ||
        !storeSnapshot(snapshot)) {
      Diagnostics::event("PORTAL_RECOVERY", "V1_SETTINGS_MIGRATION_FAILED");
      return false;
    }
    snapshotMigrated = true;
    Diagnostics::event("PORTAL_MIGRATION", "V1_TO_V2_COMPLETE");
  } else if (loaded == PortalSnapshotLoadResult::Loaded &&
             snapshot.revision != UINT64_MAX &&
             snapshot.settings.localManagementPassword != initialPassword &&
             looksLikeLegacyGeneratedPassword(
                 snapshot.settings.localManagementPassword)) {
    // Development builds before the Wi-Fi-derived policy generated a random
    // 12-character local password. Replace only that recognizable default;
    // any owner-chosen password is preserved.
    ++snapshot.revision;
    snapshot.settings.localManagementPassword = initialPassword;
    if (!storeSnapshot(snapshot)) {
      Diagnostics::event("PORTAL_RECOVERY", "WIFI_PASSWORD_MIGRATION_FAILED");
      return false;
    }
    snapshotMigrated = true;
    Diagnostics::event("PORTAL_MIGRATION", "LOCAL_PASSWORD_FROM_WIFI");
  } else if (loaded == PortalSnapshotLoadResult::LoadedLegacy) {
    if (!markSnapshotInitialized()) {
      Diagnostics::event("PORTAL_RECOVERY", "MARKER_MIGRATION_FAILED");
      return false;
    }
  } else if (loaded != PortalSnapshotLoadResult::Loaded) {
    const char* reason = loaded == PortalSnapshotLoadResult::Absent
        ? "EXISTING_IDENTITY_WITHOUT_PORTAL"
        : (loaded == PortalSnapshotLoadResult::Corrupt
               ? "SNAPSHOT_CORRUPT" : "NVS_UNAVAILABLE");
    Diagnostics::event("PORTAL_RECOVERY", reason);
    Diagnostics::event(
        "PORTAL_RECOVERY_ACTION",
        "USB: portal-recover-bound (preserves MyAI credential)");
    return false;
  }
  if (!portal::validLocalManagementPassword(
          snapshot.settings.localManagementPassword)) {
    Diagnostics::event("PORTAL_RECOVERY", "LOCAL_PASSWORD_INVALID");
    return false;
  }

  access_.bootNonce = snapshot.settings.localManagementPassword;
  access_.sessionId = createNonce("session");
  access_.csrfToken = createNonce("csrf");
  access_.sessionLifetimeSeconds = 900;
  access_.allowedOrigins.push_back("http://inkloop.local");
  access_.allowedOrigins.push_back("http://192.168.4.1");
  if (WiFi.status() == WL_CONNECTED) {
    access_.allowedOrigins.push_back(
        std::string("http://") + WiFi.localIP().toString().c_str());
  }

  portal_.reset(new (std::nothrow) portal::InkloopPortal(*this, access_));
  if (!portal_) return false;
  (void)snapshotMigrated;
  const bool boundIdentity = identity == PortalIdentityState::BoundActive ||
      identity == PortalIdentityState::BoundInactive;
  if (boundIdentity && !snapshot.onboarding.inkloopBound) {
    std::string verificationError;
    if (!services_.verifyInkloopBinding(&verificationError) ||
        snapshot.revision == UINT64_MAX) {
      Diagnostics::event("PORTAL_RECOVERY", "BOUND_RECONCILIATION_REQUIRED");
      portal_.reset();
      return false;
    }
    ++snapshot.revision;
    snapshot.onboarding.wifiConfigured = true;
    snapshot.onboarding.myAiActive =
        identity == PortalIdentityState::BoundActive;
    snapshot.onboarding.inkloopBound = true;
    snapshot.onboarding.inkloopReuseAccepted = true;
    snapshot.onboarding.codeOwnership =
        portal::CodeOwnership::InkloopBoundHistorical;
    snapshot.onboarding.onboardingCode.clear();
    snapshot.onboarding.inkloopCode.clear();
    snapshot.onboarding.codeExpiresAtSeconds = 0;
    snapshot.onboarding.stage = snapshot.onboarding.myAiActive
        ? (snapshot.onboarding.tutorialStep == portal::TutorialStep::Complete
               ? portal::OnboardingStage::SettingsReady
               : portal::OnboardingStage::VoiceTutorial)
        : portal::OnboardingStage::MyAiInactive;
    if (!storeSnapshot(snapshot)) {
      Diagnostics::event("PORTAL_RECOVERY", "BOUND_RECONCILIATION_COMMIT_FAILED");
      portal_.reset();
      return false;
    }
  }
  const bool portalPairingPending =
      snapshot.onboarding.stage == portal::OnboardingStage::MyAiPairingRequested ||
      snapshot.onboarding.stage == portal::OnboardingStage::AwaitingMyAiActivation;
  if (!portalIdentityMatchesSnapshot(
          identity, snapshot.onboarding.inkloopBound,
          snapshot.onboarding.myAiActive,
          !snapshot.onboarding.onboardingCode.empty() ||
              !snapshot.onboarding.inkloopCode.empty(),
          portalPairingPending)) {
    Diagnostics::event("PORTAL_RECOVERY", "IDENTITY_SNAPSHOT_MISMATCH");
    Diagnostics::event(
        "PORTAL_RECOVERY_ACTION",
        "USB: portal-recover-bound (server verification required)");
    portal_.reset();
    return false;
  }
  std::string error;
  if (!portal_->hydrate(snapshot, nowSeconds(), &error)) {
    Diagnostics::event("ERROR", String("PORTAL_HYDRATE:") + error.c_str());
    portal_.reset();
    return false;
  }

  static const char* headers[] = {
      "Host", "Origin", "Cookie", "Content-Type", "X-Inkloop-CSRF",
      "Content-Length", "X-Inkloop-Image-Bytes"};
  server_.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  server_.on(
      "/api/album/upload", HTTP_POST,
      [this]() { finishAlbumUploadRequest(); },
      [this]() { handleAlbumUploadChunk(); });
  server_.onNotFound([this]() { handleWebRequest(); });
  server_.begin();
  serverStarted_ = true;
  if (!MDNS.begin("inkloop")) {
    Diagnostics::event("WARN", "MDNS_UNAVAILABLE_USE_IP");
  } else {
    MDNS.addService("http", "tcp", 80);
  }
  Diagnostics::event("PORTAL_URL", "http://inkloop.local/");
  Diagnostics::event("PORTAL_ACCESS", "READY");
  return true;
}

void PaperColorPortalRuntime::loop() {
  if (serverStarted_) server_.handleClient();
}

const portal::PortalSettings& PaperColorPortalRuntime::settings() const {
  static const portal::PortalSettings fallback;
  return portal_ ? portal_->settings() : fallback;
}

bool PaperColorPortalRuntime::onWifiConfigured(
    bool configured, std::string* error) {
  return portal_ && portal_->onWifiConfigured(configured, error);
}

bool PaperColorPortalRuntime::requestMyAiPairing(std::string* error) {
  return portal_ && portal_->requestMyAiPairing(error);
}

bool PaperColorPortalRuntime::onMyAiPairingResumed(std::string* error) {
  return portal_ && portal_->onMyAiPairingResumed(error);
}

bool PaperColorPortalRuntime::onMyAiPairingCancelled(std::string* error) {
  return portal_ && portal_->onMyAiPairingCancelled(error);
}

bool PaperColorPortalRuntime::onAuthoritativeMyAiCode(
    const std::string& code, uint64_t expiresAtSeconds, std::string* error) {
  return portal_ && portal_->onAuthoritativeMyAiCode(
      code, expiresAtSeconds, nowSeconds(), error);
}

bool PaperColorPortalRuntime::retryInkloopCodeReuse(std::string* error) {
  return portal_ && portal_->retryInkloopCodeReuse(error);
}

bool PaperColorPortalRuntime::onInkloopBound(std::string* error) {
  return portal_ && portal_->onInkloopBound(error);
}

bool PaperColorPortalRuntime::onMyAiActivation(
    bool active, std::string* error) {
  return portal_ && portal_->onMyAiActivation(active, error);
}

bool PaperColorPortalRuntime::onVoiceTutorialComplete(std::string* error) {
  return portal_ && portal_->onVoiceTutorialComplete(error);
}

bool PaperColorPortalRuntime::replaceSettings(
    const portal::PortalSettings& settings, std::string* error) {
  return portal_ && portal_->replaceSettings(settings, error);
}

bool PaperColorPortalRuntime::confirmPhysical(std::string* error) {
  if (!portal_ || pendingPhysicalId_.empty()) {
    if (error) *error = "no_pending_physical_confirmation";
    return false;
  }
  const std::string id = pendingPhysicalId_;
  if (!portal_->confirmPhysical(id, nowSeconds(), error)) return false;
  pendingPhysicalId_.clear();
  return true;
}

bool PaperColorPortalRuntime::recoverBoundSnapshot(
    bool myAiActive, std::string* error) {
  portal::PortalPersistedSnapshot snapshot = portal::makeFreshPortalSnapshot();
  const String wifiPassword = WiFi.psk();
  snapshot.settings.localManagementPassword = initialManagementPassword(
      std::string(wifiPassword.c_str(), wifiPassword.length()));
  if (!portal::validLocalManagementPassword(
          snapshot.settings.localManagementPassword)) {
    if (error) *error = "portal_management_password_generation_failed";
    return false;
  }
  snapshot.onboarding.wifiConfigured = true;
  snapshot.onboarding.myAiActive = myAiActive;
  snapshot.onboarding.inkloopBound = true;
  snapshot.onboarding.inkloopReuseAccepted = true;
  snapshot.onboarding.codeOwnership =
      portal::CodeOwnership::InkloopBoundHistorical;
  snapshot.onboarding.stage = myAiActive
      ? portal::OnboardingStage::VoiceTutorial
      : portal::OnboardingStage::MyAiInactive;
  snapshot.onboarding.onboardingCode.clear();
  snapshot.onboarding.inkloopCode.clear();
  snapshot.onboarding.codeExpiresAtSeconds = 0;
  std::string validationError;
  portal::OnboardingState verifier;
  if (!verifier.hydrate(snapshot.onboarding, &validationError) ||
      !storeSnapshot(snapshot)) {
    if (error) *error = validationError.empty()
        ? "portal_bound_recovery_commit_failed" : validationError;
    return false;
  }
  if (error) error->clear();
  return true;
}

std::string PaperColorPortalRuntime::createNonce(const char* purpose) {
  static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  std::string value;
  value.reserve(26);
  uint32_t entropy = esp_random();
  for (size_t index = 0; index < 24; ++index) {
    if ((index & 3U) == 0) entropy ^= esp_random();
    value.push_back(alphabet[(entropy >> ((index & 3U) * 8U)) %
                             (sizeof(alphabet) - 1)]);
  }
  if (purpose && *purpose) {
    value.push_back('-');
    value.push_back(purpose[0] >= 'a' && purpose[0] <= 'z'
                        ? static_cast<char>(purpose[0] - 32)
                        : purpose[0]);
  }
  return value;
}

std::string PaperColorPortalRuntime::initialManagementPassword(
    const std::string& wifiPassword) {
  if (portal::validLocalManagementPassword(wifiPassword)) {
    return wifiPassword;
  }
  // WPA2 itself requires 8..63 characters. An open or otherwise unusable
  // upstream network therefore gets a simple documented fallback which the
  // owner can replace from the local settings page.
  return "inkloop8";
}

bool PaperColorPortalRuntime::looksLikeLegacyGeneratedPassword(
    const std::string& value) {
  static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  if (value.size() != portal::kRecommendedLocalManagementPasswordBytes) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    if (!std::strchr(alphabet, value[index])) return false;
  }
  return true;
}

bool PaperColorPortalRuntime::startMyAiPairing(
    const char* appId, std::string* error) {
  if (!appId || std::strcmp(appId, portal::kMyAiAppId) != 0) {
    if (error) *error = "myai_app_id_rejected";
    return false;
  }
  return services_.startMyAiPairing(error);
}

bool PaperColorPortalRuntime::requestInkloopCodeReuse(
    const std::string& onboardingCode, uint64_t expiresAtSeconds,
    std::string* error) {
  return services_.requestInkloopCodeReuse(
      onboardingCode, expiresAtSeconds, error);
}

bool PaperColorPortalRuntime::persistPortalSnapshot(
    const portal::PortalSnapshotPatch& patch, std::string* error) {
  portal::PortalPersistedSnapshot current;
  const PortalSnapshotLoadResult loaded = loadSnapshot(current);
  if (patch.schemaVersion != portal::kPortalSnapshotSchemaVersion ||
      patch.dirtyFields == 0 ||
      (patch.dirtyFields & ~portal::kAllPortalSnapshotFields) != 0 ||
      patch.nextRevision != patch.expectedRevision + 1 ||
      (loaded != PortalSnapshotLoadResult::Loaded &&
       loaded != PortalSnapshotLoadResult::LoadedLegacy) ||
      current.revision != patch.expectedRevision ||
      patch.mergedSnapshot.revision != patch.nextRevision ||
      patch.mergedSnapshot.presentFields != portal::kAllPortalSnapshotFields) {
    if (error) *error = "portal_snapshot_compare_failed";
    return false;
  }
  if (!storeSnapshot(patch.mergedSnapshot)) {
    if (error) *error = "portal_snapshot_commit_failed";
    return false;
  }
  services_.applyPortalSettings(patch.mergedSnapshot.settings);
  if (error) error->clear();
  return true;
}

bool PaperColorPortalRuntime::testLedRoles(
    bool swapped, uint8_t maximumBrightnessPercent,
    std::string* error) {
  return services_.testLedRoles(
      swapped, maximumBrightnessPercent, error);
}

bool PaperColorPortalRuntime::previewVolume(
    uint8_t volume, std::string* error) {
  return services_.previewVolume(volume, error);
}

bool PaperColorPortalRuntime::executeConfirmedOperation(
    const portal::ConfirmedOperation& operation, std::string* error) {
  return services_.executeConfirmedOperation(operation, error);
}

bool PaperColorPortalRuntime::mutationBusy() const {
  return services_.mutationBusy();
}

portal::StorageStatus PaperColorPortalRuntime::storageStatus() const {
  return services_.storageStatus();
}

portal::AlbumReadStatus PaperColorPortalRuntime::readAlbumPage(
    const portal::AlbumPageRequest& request, portal::AlbumPage* page) const {
  return services_.readAlbumPage(request, page);
}

portal::AlbumReadStatus PaperColorPortalRuntime::findAlbumItem(
    const std::string& assetId, portal::AlbumItem* item) const {
  return services_.findAlbumItem(assetId, item);
}

bool PaperColorPortalRuntime::displayAlbumItem(
    const std::string& assetId, std::string* error) {
  return services_.displayAlbumItem(assetId, error);
}

bool PaperColorPortalRuntime::generateImage(
    const std::string& prompt, std::string* error) {
  return services_.generateImage(prompt, error);
}

portal::DiagnosticsSnapshot PaperColorPortalRuntime::diagnostics() const {
  return services_.portalDiagnostics();
}

PortalSnapshotLoadResult PaperColorPortalRuntime::loadSnapshot(
    portal::PortalPersistedSnapshot& snapshot) {
  Preferences preferences;
  PortalStorageProbe probe;
  // ESP32 Preferences cannot open a never-created namespace read-only. Open
  // it read/write so begin(false) distinguishes an available empty namespace
  // (true first boot) from an NVS subsystem failure; this probe writes no key.
  if (!preferences.begin(kPortalNvsNamespace, false))
    return PortalSnapshotLoadResult::Unavailable;
  probe.namespaceAvailable = true;
  probe.markerPresent = preferences.isKey(kPortalInitialized);
  probe.markerValid = probe.markerPresent &&
      preferences.getUChar(kPortalInitialized, 0) == kPortalInitializedValue;
  probe.headPresent = preferences.isKey(kPortalHead);
  const uint8_t head = preferences.getUChar(kPortalHead, 0);
  probe.headValid = head == 1 || head == 2;
  probe.slotAPresent = preferences.isKey(kPortalSlotA);
  probe.slotBPresent = preferences.isKey(kPortalSlotB);
  const char* keys[2] = {
      head == 2 ? kPortalSlotB : kPortalSlotA,
      head == 2 ? kPortalSlotA : kPortalSlotB};
  portal::PortalPersistedSnapshot decoded[2];
  for (size_t index = 0; index < 2; ++index) {
    const String raw = preferences.getString(keys[index], "");
    if (raw.length() && raw.length() <= kMaximumPortalRecordBytes &&
        decodeSnapshot(raw.c_str(), decoded[index])) {
      if (index == 0) probe.headSlotValid = true;
      else probe.fallbackSlotValid = true;
    }
  }
  preferences.end();
  const PortalSnapshotLoadResult result = classifyPortalStorage(probe);
  if (result == PortalSnapshotLoadResult::Loaded ||
      result == PortalSnapshotLoadResult::LoadedLegacy) {
    snapshot = probe.headSlotValid ? decoded[0] : decoded[1];
  }
  return result;
}

bool PaperColorPortalRuntime::markSnapshotInitialized() {
  Preferences preferences;
  if (!preferences.begin(kPortalNvsNamespace, false)) return false;
  const bool stored = preferences.putUChar(
      kPortalInitialized, kPortalInitializedValue) == 1;
  preferences.end();
  return stored;
}

bool PaperColorPortalRuntime::storeSnapshot(
    const portal::PortalPersistedSnapshot& snapshot) {
  std::string encoded;
  if (!encodeSnapshot(snapshot, encoded) ||
      encoded.size() > kMaximumPortalRecordBytes) return false;
  Preferences preferences;
  if (!preferences.begin(kPortalNvsNamespace, false)) return false;
  const uint8_t head = preferences.getUChar(kPortalHead, 0);
  const uint8_t next = head == 1 ? 2 : 1;
  const char* key = next == 1 ? kPortalSlotA : kPortalSlotB;
  const size_t stored = preferences.putString(key, encoded.c_str());
  if (stored != encoded.size()) {
    preferences.end();
    return false;
  }
  portal::PortalPersistedSnapshot verified;
  const String reread = preferences.getString(key, "");
  if (!decodeSnapshot(reread.c_str(), verified) ||
      verified.revision != snapshot.revision ||
      preferences.putUChar(kPortalHead, next) != 1 ||
      preferences.putUChar(kPortalInitialized, kPortalInitializedValue) != 1) {
    preferences.end();
    return false;
  }
  preferences.end();
  return true;
}

bool PaperColorPortalRuntime::encodeSnapshot(
    const portal::PortalPersistedSnapshot& snapshot, std::string& output) {
  JsonDocument payload;
  payload["schema"] = snapshot.schemaVersion;
  payload["fields"] = snapshot.presentFields;
  payload["revision"] = snapshot.revision;
  JsonObject onboarding = payload["onboarding"].to<JsonObject>();
  onboarding["stage"] = static_cast<uint8_t>(snapshot.onboarding.stage);
  onboarding["tutorial"] = static_cast<uint8_t>(snapshot.onboarding.tutorialStep);
  onboarding["wifi"] = snapshot.onboarding.wifiConfigured;
  onboarding["myai_active"] = snapshot.onboarding.myAiActive;
  onboarding["inkloop_bound"] = snapshot.onboarding.inkloopBound;
  onboarding["reuse"] = snapshot.onboarding.inkloopReuseAccepted;
  onboarding["owner"] = static_cast<uint8_t>(snapshot.onboarding.codeOwnership);
  onboarding["code"] = snapshot.onboarding.onboardingCode;
  onboarding["inkloop_code"] = snapshot.onboarding.inkloopCode;
  onboarding["expires"] = snapshot.onboarding.codeExpiresAtSeconds;
  JsonObject settings = payload["settings"].to<JsonObject>();
  settings["storage"] = static_cast<uint8_t>(snapshot.settings.storageTarget);
  settings["volume"] = snapshot.settings.volume;
  settings["prompt"] = snapshot.settings.assistantPrompt;
  settings["image_prompt"] = snapshot.settings.imagePromptTemplate;
  settings["local_password"] = snapshot.settings.localManagementPassword;
  settings["width"] = snapshot.settings.image.width;
  settings["height"] = snapshot.settings.image.height;
  settings["steps"] = snapshot.settings.image.steps;
  settings["negative"] = snapshot.settings.image.negativePrompt;
  settings["led_swap"] = snapshot.settings.ledRolesSwapped;
  settings["led_brightness"] =
      snapshot.settings.ledMaximumBrightnessPercent;
  settings["refresh"] = static_cast<uint8_t>(snapshot.settings.refreshMode);
  settings["power"] = static_cast<uint8_t>(snapshot.settings.powerMode);
  settings["idle"] = snapshot.settings.idleTimeoutSeconds;
  std::string canonical;
  serializeJson(payload, canonical);
  JsonDocument envelope;
  envelope["payload"] = canonical;
  envelope["sha256"] = sha256Hex(canonical);
  output.clear();
  return serializeJson(envelope, output) > 0;
}

bool PaperColorPortalRuntime::decodeSnapshot(
    const std::string& input, portal::PortalPersistedSnapshot& snapshot) {
  if (input.empty() || input.size() > kMaximumPortalRecordBytes) return false;
  JsonDocument envelope;
  if (deserializeJson(envelope, input) ||
      !envelope["payload"].is<const char*>() ||
      !envelope["sha256"].is<const char*>()) return false;
  const std::string payload = envelope["payload"].as<const char*>();
  const std::string expected = envelope["sha256"].as<const char*>();
  if (expected.size() != 64 || sha256Hex(payload) != expected) return false;
  JsonDocument document;
  if (deserializeJson(document, payload)) return false;
  JsonObjectConst onboarding = document["onboarding"].as<JsonObjectConst>();
  JsonObjectConst settings = document["settings"].as<JsonObjectConst>();
  if (onboarding.isNull() || settings.isNull() ||
      !document["schema"].is<uint16_t>() ||
      !document["fields"].is<uint32_t>() ||
      !document["revision"].is<uint64_t>()) return false;
  portal::PortalPersistedSnapshot decoded;
  decoded.schemaVersion = document["schema"].as<uint16_t>();
  decoded.presentFields = document["fields"].as<uint32_t>();
  decoded.revision = document["revision"].as<uint64_t>();
  const bool legacy =
      decoded.schemaVersion == portal::kLegacyPortalSnapshotSchemaVersion &&
      decoded.presentFields == portal::kLegacyPortalSnapshotFields;
  const bool current =
      decoded.schemaVersion == portal::kPortalSnapshotSchemaVersion &&
      decoded.presentFields == portal::kAllPortalSnapshotFields;
  if (!legacy && !current) return false;
  const uint32_t stage = onboarding["stage"] | 255U;
  const uint32_t tutorial = onboarding["tutorial"] | 255U;
  const uint32_t owner = onboarding["owner"] | 255U;
  const uint32_t storage = settings["storage"] | 255U;
  const uint32_t refresh = settings["refresh"] | 255U;
  const uint32_t power = settings["power"] | 255U;
  if (!safeEnum(stage, static_cast<uint8_t>(portal::OnboardingStage::SettingsReady)) ||
      !safeEnum(tutorial, static_cast<uint8_t>(portal::TutorialStep::Complete)) ||
      !safeEnum(owner, static_cast<uint8_t>(portal::CodeOwnership::InkloopBoundHistorical)) ||
      !safeEnum(storage, static_cast<uint8_t>(portal::StorageTarget::SdCard)) ||
      !safeEnum(refresh, static_cast<uint8_t>(portal::RefreshMode::ExperimentalSixColor)) ||
      !safeEnum(power, static_cast<uint8_t>(portal::PowerMode::Battery))) return false;
  decoded.onboarding.stage = static_cast<portal::OnboardingStage>(stage);
  decoded.onboarding.tutorialStep = static_cast<portal::TutorialStep>(tutorial);
  decoded.onboarding.wifiConfigured = onboarding["wifi"] | false;
  decoded.onboarding.myAiActive = onboarding["myai_active"] | false;
  decoded.onboarding.inkloopBound = onboarding["inkloop_bound"] | false;
  decoded.onboarding.inkloopReuseAccepted = onboarding["reuse"] | false;
  decoded.onboarding.codeOwnership = static_cast<portal::CodeOwnership>(owner);
  if (!readString(onboarding["code"], &decoded.onboarding.onboardingCode, 6) ||
      !readString(onboarding["inkloop_code"], &decoded.onboarding.inkloopCode, 6) ||
      !onboarding["expires"].is<uint64_t>()) return false;
  decoded.onboarding.codeExpiresAtSeconds = onboarding["expires"].as<uint64_t>();
  decoded.settings.storageTarget = static_cast<portal::StorageTarget>(storage);
  decoded.settings.volume = settings["volume"] | 255U;
  if (!readString(settings["prompt"], &decoded.settings.assistantPrompt, 2048) ||
      !readString(settings["negative"], &decoded.settings.image.negativePrompt, 1024)) return false;
  if (current &&
      (!readString(settings["image_prompt"],
                   &decoded.settings.imagePromptTemplate, 512) ||
       !readString(settings["local_password"],
                   &decoded.settings.localManagementPassword,
                   portal::kMaximumLocalManagementPasswordBytes))) {
    return false;
  }
  decoded.settings.image.width = settings["width"] | 0;
  decoded.settings.image.height = settings["height"] | 0;
  decoded.settings.image.steps = settings["steps"] | 0;
  decoded.settings.ledRolesSwapped = settings["led_swap"] | false;
  // SnapshotLedRoles owns both the role mapping and the global brightness
  // ceiling. Early v2 records predate led_brightness and safely adopt the
  // current 60% default on first load.
  decoded.settings.ledMaximumBrightnessPercent =
      settings["led_brightness"] | 60U;
  decoded.settings.refreshMode = static_cast<portal::RefreshMode>(refresh);
  decoded.settings.powerMode = static_cast<portal::PowerMode>(power);
  decoded.settings.idleTimeoutSeconds = settings["idle"] | 0;
  snapshot = decoded;
  return true;
}

void PaperColorPortalRuntime::handleWebRequest() {
  if (!portal_) {
    server_.send(503, "application/json", "{\"error\":\"portal_unavailable\"}");
    return;
  }
  requestActive_ = true;
  portal::PortalRequest request;
  request.method = server_.method() == HTTP_GET ? "GET" :
      (server_.method() == HTTP_POST ? "POST" : "UNSUPPORTED");
  request.path = pathWithQuery(server_);
  request.host = server_.header("Host").c_str();
  request.origin = server_.header("Origin").c_str();
  request.cookie = server_.header("Cookie").c_str();
  request.csrfToken = server_.header("X-Inkloop-CSRF").c_str();
  request.contentType = server_.header("Content-Type").c_str();
  request.body = server_.arg("plain").c_str();
  if (request.body.empty() &&
      request.contentType.find("application/x-www-form-urlencoded") == 0) {
    for (uint8_t index = 0; index < server_.args(); ++index) {
      if (!request.body.empty()) request.body.push_back('&');
      request.body += formEncode(server_.argName(index));
      request.body.push_back('=');
      request.body += formEncode(server_.arg(index));
      if (request.body.size() > 8192U) {
        server_.send(413, "application/json", "{\"error\":\"request_too_large\"}");
        requestActive_ = false;
        return;
      }
    }
  }
  request.peerIp = server_.client().remoteIP().toString().c_str();
  request.nowSeconds = nowSeconds();
  const portal::PortalResponse response = portal_->handle(request);
  if (!response.setCookie.empty()) {
    server_.sendHeader("Set-Cookie", response.setCookie.c_str());
  }
  if (response.retryAfterSeconds) {
    server_.sendHeader("Retry-After", String(response.retryAfterSeconds));
  }
  if (request.path == "/api/actions/confirm" && response.status == 202) {
    JsonDocument parsed;
    if (!deserializeJson(parsed, response.body) &&
        parsed["confirmationId"].is<const char*>()) {
      pendingPhysicalId_ = parsed["confirmationId"].as<const char*>();
    }
  }
  server_.send(response.status, response.contentType.c_str(), response.body.c_str());
  requestActive_ = false;
}

portal::PortalRequest PaperColorPortalRuntime::requestFromServer(
    const char* path) {
  portal::PortalRequest request;
  request.method = server_.method() == HTTP_POST ? "POST" : "GET";
  request.path = path ? path : "";
  request.host = server_.header("Host").c_str();
  request.origin = server_.header("Origin").c_str();
  request.cookie = server_.header("Cookie").c_str();
  request.csrfToken = server_.header("X-Inkloop-CSRF").c_str();
  request.contentType = server_.header("Content-Type").c_str();
  request.peerIp = server_.client().remoteIP().toString().c_str();
  request.nowSeconds = nowSeconds();
  return request;
}

void PaperColorPortalRuntime::handleAlbumUploadChunk() {
  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    requestActive_ = true;
    services_.abortAlbumUpload();
    uploadAuthorized_ = false;
    uploadStarted_ = false;
    uploadEnded_ = false;
    uploadResponse_ = portal::PortalResponse();
    const portal::PortalResponse authorization =
        portal_->authorizeStreamingAlbumUpload(
            requestFromServer("/api/album/upload"));
    if (authorization.status != 202) {
      uploadResponse_ = authorization;
      requestActive_ = false;
      return;
    }
    uint32_t imageBytes = 0;
    uint32_t contentBytes = 0;
    if (!strictUploadUnsigned(server_.header("X-Inkloop-Image-Bytes"),
                        &imageBytes) ||
        !strictUploadUnsigned(server_.header("Content-Length"),
                        &contentBytes) ||
        imageBytes < 45 ||
        imageBytes > portal::kMaximumAlbumUploadBytes ||
        contentBytes < imageBytes ||
        contentBytes > portal::kMaximumAlbumUploadBytes + 8192U) {
      uploadResponse_.status = 413;
      uploadResponse_.contentType = "application/json; charset=utf-8";
      uploadResponse_.body =
          "{\"ok\":false,\"error\":\"upload_size_invalid\"}";
      requestActive_ = false;
      return;
    }
    std::string error;
    if (!services_.beginAlbumUpload(
            upload.filename.c_str(), imageBytes, &error)) {
      uploadResponse_.status = error == "device_busy" ? 409 : 422;
      uploadResponse_.contentType = "application/json; charset=utf-8";
      uploadResponse_.body = "{\"ok\":false,\"error\":\"" +
          portal::jsonEscape(error.empty() ? "upload_start_failed" : error) +
          "\"}";
      requestActive_ = false;
      return;
    }
    uploadAuthorized_ = true;
    uploadStarted_ = true;
    uploadResponse_.status = 202;
    uploadResponse_.contentType = "application/json; charset=utf-8";
    uploadResponse_.body =
        "{\"ok\":true,\"state\":\"upload_receiving\"}";
    return;
  }
  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadAuthorized_ || !uploadStarted_) return;
    std::string error;
    if (!services_.writeAlbumUpload(upload.buf, upload.currentSize, &error)) {
      services_.abortAlbumUpload();
      uploadAuthorized_ = false;
      uploadResponse_.status = error == "upload_too_large" ? 413 : 507;
      uploadResponse_.contentType = "application/json; charset=utf-8";
      uploadResponse_.body = "{\"ok\":false,\"error\":\"" +
          portal::jsonEscape(error.empty() ? "upload_write_failed" : error) +
          "\"}";
      requestActive_ = false;
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_END) {
    uploadEnded_ = true;
    if (!uploadAuthorized_ || !uploadStarted_) return;
    portal::AlbumUploadResult result;
    std::string error;
    if (!services_.finishAlbumUpload(&result, &error)) {
      services_.abortAlbumUpload();
      uploadAuthorized_ = false;
      uploadResponse_.status = error == "insufficient_storage" ? 507 : 422;
      uploadResponse_.contentType = "application/json; charset=utf-8";
      uploadResponse_.body = "{\"ok\":false,\"error\":\"" +
          portal::jsonEscape(error.empty() ? "upload_commit_failed" : error) +
          "\"}";
      requestActive_ = false;
      return;
    }
    uploadAuthorized_ = false;
    uploadResponse_.status = 201;
    uploadResponse_.contentType = "application/json; charset=utf-8";
    std::ostringstream body;
    body << "{\"ok\":true,\"state\":\"upload_complete\",\"assetId\":\""
         << portal::jsonEscape(result.assetId) << "\",\"title\":\""
         << portal::jsonEscape(result.title) << "\",\"backend\":\""
         << portal::jsonEscape(result.backend) << "\",\"bytes\":"
         << result.bytes << ",\"revision\":" << result.revision << "}";
    uploadResponse_.body = body.str();
    requestActive_ = false;
    return;
  }
  if (upload.status == UPLOAD_FILE_ABORTED) {
    services_.abortAlbumUpload();
    uploadAuthorized_ = false;
    uploadEnded_ = true;
    uploadResponse_.status = 400;
    uploadResponse_.contentType = "application/json; charset=utf-8";
    uploadResponse_.body =
        "{\"ok\":false,\"error\":\"upload_aborted\"}";
    requestActive_ = false;
  }
}

void PaperColorPortalRuntime::finishAlbumUploadRequest() {
  requestActive_ = true;
  if (!uploadEnded_ && uploadStarted_) {
    services_.abortAlbumUpload();
    uploadResponse_.status = 422;
    uploadResponse_.contentType = "application/json; charset=utf-8";
    uploadResponse_.body =
        "{\"ok\":false,\"error\":\"upload_incomplete\"}";
  }
  if (uploadResponse_.body.empty()) {
    uploadResponse_.status = 400;
    uploadResponse_.contentType = "application/json; charset=utf-8";
    uploadResponse_.body =
        "{\"ok\":false,\"error\":\"upload_not_started\"}";
  }
  if (uploadResponse_.retryAfterSeconds) {
    server_.sendHeader(
        "Retry-After", String(uploadResponse_.retryAfterSeconds));
  }
  server_.send(
      uploadResponse_.status, uploadResponse_.contentType.c_str(),
      uploadResponse_.body.c_str());
  uploadAuthorized_ = false;
  uploadStarted_ = false;
  uploadEnded_ = false;
  uploadResponse_ = portal::PortalResponse();
  requestActive_ = false;
}

uint64_t PaperColorPortalRuntime::nowSeconds() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<uint64_t>(now) : 0;
}

}  // namespace inkloop
