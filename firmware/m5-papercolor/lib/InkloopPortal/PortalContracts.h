#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace inkloop {
namespace portal {

static const char kMyAiAppId[] = "inkloop";
static const char kMyAiRegistrationBaseUrl[] = "https://myai.mess.host/";
static const uint16_t kLegacyPortalSnapshotSchemaVersion = 1;
static const uint16_t kPortalSnapshotSchemaVersion = 2;

enum class OnboardingStage : uint8_t {
  WifiAccessPoint,
  WifiConfigured,
  MyAiPairingRequested,
  AwaitingMyAiActivation,
  MyAiInactive,
  VoiceTutorial,
  SettingsReady,
};

enum class TutorialStep : uint8_t {
  PressToTalk,
  VoiceLedStates,
  GalleryPaging,
  DisplayBusyGuard,
  LocalPortal,
  Complete,
};

enum class StorageTarget : uint8_t { Automatic, Internal, SdCard };
enum class RefreshMode : uint8_t { OfficialQuality, ExperimentalSixColor };
enum class PowerMode : uint8_t { Compatibility, Battery };
enum class CodeOwnership : uint8_t {
  None,
  MyAiAuthoritativeShared,
  InkloopBoundHistorical,
};

struct ImageGenerationSettings {
  uint16_t width;
  uint16_t height;
  uint8_t steps;
  std::string negativePrompt;

  ImageGenerationSettings()
      : width(400), height(600), steps(20),
        negativePrompt(
            "细小文字，水印，低对比，灰暗，复杂渐变，细碎纹理，超出边界") {}
};

struct PortalSettings {
  StorageTarget storageTarget;
  uint8_t volume;
  std::string assistantPrompt;
  std::string imagePromptTemplate;
  std::string localManagementPassword;
  ImageGenerationSettings image;
  bool ledRolesSwapped;
  uint8_t ledMaximumBrightnessPercent;
  RefreshMode refreshMode;
  PowerMode powerMode;
  uint16_t idleTimeoutSeconds;

  PortalSettings()
      : storageTarget(StorageTarget::Automatic),
        volume(60),
        assistantPrompt(
            "你是 Inkloop PaperColor 上友好、简洁、有个性的语音助手。设备是 "
            "400×600 Spectra 6 六色电子纸，设备底边朝下，整屏刷新约需15–30秒，"
            "应避免无意义频繁刷屏。"
            "你可以调用本地工具管理相册、查询剩余空间、调整音量与提示词、生成图片；"
            "删除图片、清空或格式化前必须明确确认。"),
        imagePromptTemplate(
            "为当前 400×600、设备底边朝下的屏幕方向创作适合 Spectra 6 六色电子纸的图片："
            "色彩鲜艳、高对比、清晰轮廓、大色块、简洁构图、少用细小文字。主题：{prompt}"),
        localManagementPassword(),
        image(),
        ledRolesSwapped(false),
        ledMaximumBrightnessPercent(60),
        refreshMode(RefreshMode::OfficialQuality),
        powerMode(PowerMode::Battery),
        idleTimeoutSeconds(120) {}
};

enum PortalSnapshotField : uint32_t {
  SnapshotOnboardingStage = 1UL << 0,
  SnapshotTutorialStep = 1UL << 1,
  SnapshotWifiConfigured = 1UL << 2,
  SnapshotMyAiActive = 1UL << 3,
  SnapshotInkloopBound = 1UL << 4,
  SnapshotCodeOwnership = 1UL << 5,
  SnapshotOnboardingCode = 1UL << 6,
  SnapshotInkloopCode = 1UL << 7,
  SnapshotCodeExpiry = 1UL << 8,
  SnapshotStorageTarget = 1UL << 9,
  SnapshotVolume = 1UL << 10,
  SnapshotAssistantPrompt = 1UL << 11,
  SnapshotImageSettings = 1UL << 12,
  SnapshotLedRoles = 1UL << 13,
  SnapshotRefreshMode = 1UL << 14,
  SnapshotPowerMode = 1UL << 15,
  SnapshotIdleTimeout = 1UL << 16,
  SnapshotInkloopReuseAccepted = 1UL << 17,
  SnapshotImagePromptTemplate = 1UL << 18,
  SnapshotLocalManagementPassword = 1UL << 19,
};

static const uint32_t kLegacyPortalSnapshotFields =
    SnapshotOnboardingStage | SnapshotTutorialStep | SnapshotWifiConfigured |
    SnapshotMyAiActive | SnapshotInkloopBound | SnapshotCodeOwnership |
    SnapshotOnboardingCode | SnapshotInkloopCode | SnapshotCodeExpiry |
    SnapshotStorageTarget | SnapshotVolume | SnapshotAssistantPrompt |
    SnapshotImageSettings | SnapshotLedRoles | SnapshotRefreshMode |
    SnapshotPowerMode | SnapshotIdleTimeout | SnapshotInkloopReuseAccepted;

static const uint32_t kAllPortalSnapshotFields =
    kLegacyPortalSnapshotFields | SnapshotImagePromptTemplate |
    SnapshotLocalManagementPassword;

struct OnboardingPersistedState {
  OnboardingStage stage;
  TutorialStep tutorialStep;
  bool wifiConfigured;
  bool myAiActive;
  bool inkloopBound;
  bool inkloopReuseAccepted;
  CodeOwnership codeOwnership;
  std::string onboardingCode;
  std::string inkloopCode;
  uint64_t codeExpiresAtSeconds;

  OnboardingPersistedState()
      : stage(OnboardingStage::WifiAccessPoint),
        tutorialStep(TutorialStep::PressToTalk),
        wifiConfigured(false),
        myAiActive(false),
        inkloopBound(false),
        inkloopReuseAccepted(false),
        codeOwnership(CodeOwnership::None),
        onboardingCode(),
        inkloopCode(),
        codeExpiresAtSeconds(0) {}
};

// This DTO is deliberately explicit and versioned. A default-constructed DTO
// has no presence bits and is invalid; integrations must load every field or
// deliberately use makeFreshPortalSnapshot() for a new device.
struct PortalPersistedSnapshot {
  uint16_t schemaVersion;
  uint32_t presentFields;
  uint64_t revision;
  OnboardingPersistedState onboarding;
  PortalSettings settings;

  PortalPersistedSnapshot()
      : schemaVersion(kPortalSnapshotSchemaVersion),
        presentFields(0),
        revision(0),
        onboarding(),
        settings() {}
};

struct PortalSnapshotPatch {
  uint16_t schemaVersion;
  uint64_t expectedRevision;
  uint64_t nextRevision;
  uint32_t dirtyFields;
  PortalPersistedSnapshot mergedSnapshot;

  PortalSnapshotPatch()
      : schemaVersion(kPortalSnapshotSchemaVersion),
        expectedRevision(0),
        nextRevision(0),
        dirtyFields(0),
        mergedSnapshot() {}
};

PortalPersistedSnapshot makeFreshPortalSnapshot();

struct RateBudget {
  uint16_t maximumRequests;
  uint16_t windowSeconds;

  RateBudget() : maximumRequests(1), windowSeconds(60) {}
  RateBudget(uint16_t maximum, uint16_t window)
      : maximumRequests(maximum), windowSeconds(window) {}
};

struct PortalRateConfig {
  RateBudget reads;
  RateBudget writes;
  RateBudget destructive;
  uint16_t maximumTrackedKeys;

  PortalRateConfig()
      : reads(120, 60),
        writes(30, 60),
        destructive(8, 60),
        maximumTrackedKeys(64) {}
};

enum class ActiveStorageBackend : uint8_t {
  Unavailable,
  Internal,
  SdCard,
};

struct StorageStatus {
  bool internalMounted;
  bool internalRecoveryRequired;
  bool taskStoreReady;
  bool sdPresent;
  bool sdWritable;
  uint64_t internalFreeBytes;
  uint64_t internalTotalBytes;
  uint64_t sdFreeBytes;
  uint64_t sdTotalBytes;
  ActiveStorageBackend activeBackend;
  bool activeMounted;
  bool activeWritable;
  uint64_t activeFreeBytes;
  uint64_t activeTotalBytes;

  StorageStatus()
      : internalMounted(false),
        internalRecoveryRequired(false),
        taskStoreReady(false),
        sdPresent(false),
        sdWritable(false),
        internalFreeBytes(0),
        internalTotalBytes(0),
        sdFreeBytes(0),
        sdTotalBytes(0),
        activeBackend(ActiveStorageBackend::Unavailable),
        activeMounted(false),
        activeWritable(false),
        activeFreeBytes(0),
        activeTotalBytes(0) {}
};

struct AlbumItem {
  std::string id;
  std::string title;
  std::string origin;
  uint64_t bytes;
  bool current;
  bool factoryAsset;

  AlbumItem()
      : id(), title(), origin(), bytes(0), current(false), factoryAsset(false) {}
};

// The storage adapter must honor these bounds while reading from flash/SD; it
// must not first materialize an unbounded album and truncate it afterwards.
static const size_t kMaximumAlbumPageItems = 16;
static const size_t kMaximumAlbumCursorBytes = 64;
static const size_t kMaximumAlbumIdBytes = 96;
static const size_t kMaximumAlbumTitleBytes = 192;
static const size_t kMaximumAlbumOriginBytes = 64;
static const size_t kMaximumAlbumPageFieldBytes = 4096;
static const size_t kMaximumAlbumJsonBytes = 12288;
static const size_t kMaximumDashboardHtmlBytes = 32768;
static const size_t kMaximumAlbumUploadBytes = 1500000;
static const size_t kMaximumAlbumUploadTitleBytes = 64;

struct AlbumUploadResult {
  std::string assetId;
  std::string title;
  std::string backend;
  uint64_t bytes;
  uint64_t revision;

  AlbumUploadResult()
      : assetId(), title(), backend(), bytes(0), revision(0) {}
};

struct AlbumPageRequest {
  std::string cursor;
  size_t maximumItems;
  size_t maximumIdBytes;
  size_t maximumTitleBytes;
  size_t maximumOriginBytes;
  size_t maximumTotalFieldBytes;

  AlbumPageRequest()
      : cursor(),
        maximumItems(kMaximumAlbumPageItems),
        maximumIdBytes(kMaximumAlbumIdBytes),
        maximumTitleBytes(kMaximumAlbumTitleBytes),
        maximumOriginBytes(kMaximumAlbumOriginBytes),
        maximumTotalFieldBytes(kMaximumAlbumPageFieldBytes) {}
};

struct AlbumPage {
  std::vector<AlbumItem> items;
  std::string nextCursor;
  uint32_t totalItems;

  AlbumPage() : items(), nextCursor(), totalItems(0) {}
};

enum class AlbumReadStatus : uint8_t {
  Ok,
  NotFound,
  InvalidData,
  TooLarge,
  Unavailable,
};

struct DiagnosticsSnapshot {
  std::string firmwareVersion;
  std::string hardwareSku;
  std::string wifiState;
  std::string storageState;
  std::string displayState;
  std::string myAiState;
  uint32_t freeHeapBytes;
  uint32_t freePsramBytes;
  std::vector<std::string> serialLines;

  DiagnosticsSnapshot() : freeHeapBytes(0), freePsramBytes(0) {}
};

enum class DestructiveAction : uint8_t { DeleteAsset, ClearAlbum, FormatSdCard };

struct ConfirmedOperation {
  DestructiveAction action;
  std::string target;
  std::string confirmationId;

  ConfirmedOperation()
      : action(DestructiveAction::DeleteAsset), target(), confirmationId() {}
};

struct PortalRequest {
  std::string method;
  std::string path;
  std::string host;
  std::string origin;
  std::string cookie;
  std::string csrfToken;
  std::string contentType;
  std::string body;
  // Supplied by the local network adapter after parsing the socket peer. It is
  // required even for bootstrap and health requests; forwarded headers do not
  // satisfy this field.
  std::string peerIp;
  uint64_t nowSeconds;

  PortalRequest() : nowSeconds(0) {}
};

struct PortalResponse {
  int status;
  std::string contentType;
  std::string body;
  std::string setCookie;
  // A nonzero value must be emitted as Retry-After by the WebServer adapter.
  uint32_t retryAfterSeconds;

  PortalResponse()
      : status(500),
        contentType("application/json; charset=utf-8"),
        body(),
        setCookie(),
        retryAfterSeconds(0) {}
};

// The firmware integration owns network, persistence, storage, LEDs, and all
// destructive work. This isolated module only validates and dispatches typed
// requests. Implementations must never return credentials in the typed reads.
class IPortalAdapter {
 public:
  virtual ~IPortalAdapter() {}

  virtual std::string createNonce(const char* purpose) = 0;
  virtual bool startMyAiPairing(const char* appId, std::string* error) = 0;
  virtual bool requestInkloopCodeReuse(
      const std::string& onboardingCode,
      uint64_t expiresAtSeconds,
      std::string* error) = 0;
  // Atomically compare expectedRevision and replace the durable snapshot. A
  // false return must leave the durable snapshot byte-for-byte unchanged.
  virtual bool persistPortalSnapshot(
      const PortalSnapshotPatch& patch,
      std::string* error) = 0;
  // Plays one short local prompt at the requested level without persisting
  // the setting. Implementations must reject microphone/voice/audio overlap.
  virtual bool previewVolume(uint8_t volume, std::string* error) = 0;
  virtual bool testLedRoles(
      bool swapped, uint8_t maximumBrightnessPercent,
      std::string* error) = 0;
  virtual bool executeConfirmedOperation(
      const ConfirmedOperation& operation,
      std::string* error) = 0;

  virtual bool mutationBusy() const = 0;
  virtual StorageStatus storageStatus() const = 0;
  // Implementations read at most request.maximumItems and enforce every field
  // and aggregate bound before constructing the returned strings.
  virtual AlbumReadStatus readAlbumPage(
      const AlbumPageRequest& request,
      AlbumPage* page) const = 0;
  // Exact lookup avoids a complete-vector scan for destructive preparation.
  virtual AlbumReadStatus findAlbumItem(
      const std::string& assetId,
      AlbumItem* item) const = 0;
  virtual bool displayAlbumItem(
      const std::string& assetId, std::string* error) {
    (void)assetId;
    if (error) *error = "album_display_unavailable";
    return false;
  }
  virtual bool generateImage(
      const std::string& prompt, std::string* error) {
    (void)prompt;
    if (error) *error = "aigc_unavailable";
    return false;
  }
  virtual DiagnosticsSnapshot diagnostics() const = 0;
};

}  // namespace portal
}  // namespace inkloop
