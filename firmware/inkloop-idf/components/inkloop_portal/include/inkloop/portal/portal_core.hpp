#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inkloop {
namespace portal {

inline constexpr size_t kMaximumPortalRequestBodyBytes = 8192U;
inline constexpr size_t kMaximumPortalResponseBytes = 32768U;
inline constexpr size_t kMaximumAlbumUploadBytes = 1500000U;
inline constexpr size_t kMaximumAlbumPageItems = 16U;
inline constexpr size_t kMaximumAlbumTotalItems = 96U;
inline constexpr size_t kMaximumAlbumCursorBytes = 64U;
inline constexpr size_t kMaximumAlbumIdBytes = 96U;
inline constexpr size_t kMaximumAlbumTitleBytes = 64U;
inline constexpr size_t kMaximumAlbumPageFieldBytes = 4096U;
inline constexpr size_t kMaximumChatPageItems = 24U;
inline constexpr size_t kMaximumChatTotalItems = 4096U;
inline constexpr size_t kMaximumChatTextBytes = 2048U;
inline constexpr size_t kMaximumChatPageTextBytes = 16384U;
inline constexpr size_t kMaximumAssistantPromptBytes = 512U;
inline constexpr size_t kMaximumImagePromptBytes = 512U;
inline constexpr size_t kMaximumNegativePromptBytes = 384U;
inline constexpr size_t kMaximumGeneratePromptBytes = 1024U;
inline constexpr size_t kMaximumLocalManagementPasswordBytes = 63U;
inline constexpr size_t kMaximumPortalRenderStrategies = 4U;
inline constexpr uint8_t kMaximumPortalRgbPixels = 8U;
inline constexpr size_t kPortalRuntimeLaneCount = 8U;

enum class PortalResult : uint8_t {
  Ok,
  InvalidConfiguration,
  InvalidRequest,
  Unauthorized,
  Forbidden,
  TooLarge,
  Busy,
  Unavailable,
  InvalidData,
};

enum class ResponseDisposition : uint8_t {
  Immediate,
  StreamAlbumPreview,
  StreamAlbumUpload,
};

enum class MyAiPortalState : uint8_t {
  Unconfigured,
  Pairing,
  Bound,
  Active,
  PaymentRequired,
  RecoveryRequired,
  Unavailable,
};

enum class TutorialPortalStep : uint8_t {
  PressToTalk = 0,
  VoiceLedStates,
  GalleryPaging,
  DisplayBusyGuard,
  LocalPortal,
  Complete,
};

struct TutorialPortalSnapshot {
  TutorialPortalStep step = TutorialPortalStep::PressToTalk;
  bool in_flight = false;
  bool persistence_pending = false;
  bool persistence_error = false;
};

// Stable, credential-free categories for the most recent MyAI client error.
// They deliberately exclude remote response text, URLs and every token type.
enum class MyAiPortalErrorSource : uint8_t {
  None = 0,
  Command,
  Tick,
  Initialize,
  ApplyPrompt,
  Pairing,
  Authorization,
  Aigc,
  VoiceConnect,
  VoiceIngress,
  CaptureUpload,
  Heartbeat,
};

enum class MyAiPortalErrorCode : uint8_t {
  None = 0,
  InvalidArgument,
  InvalidState,
  Storage,
  Security,
  Transport,
  Protocol,
  Unauthorized,
  PaymentRequired,
  RecoveryRequired,
  PairingExpired,
  Conflict,
  AppNotRegistered,
  NoGateway,
  TooLarge,
  Cancelled,
};

struct MyAiPortalErrorSnapshot {
  MyAiPortalErrorSource source = MyAiPortalErrorSource::None;
  MyAiPortalErrorCode code = MyAiPortalErrorCode::None;
  uint16_t http_status = 0U;
  uint32_t retry_after_ms = 0U;
  uint32_t sequence = 0U;
  uint32_t observed_at_ms = 0U;
  bool available = false;
};

enum class ChatRole : uint8_t {
  User,
  Assistant,
  Tool,
};

// Public, credential-free firmware-update state. These enums deliberately do
// not carry endpoint, manifest, signature, remote response, or verifier data.
enum class PortalFirmwareUpdatePhase : uint8_t {
  Unavailable,
  Ready,
  AcceptedOffline,
};

enum class PortalFirmwareUpdateCode : uint8_t {
  None,
  UpToDate,
  ConfigurationInvalid,
  NetworkUnavailable,
  TimedOut,
  ManifestRejected,
  ImageRejected,
  VerificationFailed,
  StagingFailed,
  InternalError,
  UpdateConfirmed,
  UpdateRolledBack,
};

enum class PortalCommandType : uint8_t {
  UpdateSettings,
  PreviewVolume,
  StartMyAiPairing,
  RebindMyAi,
  RestartMyAiTutorial,
  DisplayAlbumItem,
  DeleteAlbumItem,
  SetAlbumRenderStrategy,
  GenerateImage,
  ClearLocalChat,
  RequestFirmwareUpdate,
};

struct PortalFirmwareUpdateSnapshot {
  bool configured = false;
  bool accepted_offline = false;
  PortalFirmwareUpdatePhase phase = PortalFirmwareUpdatePhase::Unavailable;
  PortalFirmwareUpdateCode code = PortalFirmwareUpdateCode::None;
};

struct PortalSettingsSnapshot {
  uint8_t volume = 60U;
  uint8_t led_maximum_brightness_percent = 60U;
  bool voice_assistance_enabled = true;
  std::string assistant_prompt;
  std::string image_prompt_template;
  std::string negative_prompt;
  std::string asset_storage_preference = "automatic";
  std::string default_render_strategy = "official-quality";
  // Never expose the credential itself through the read cache/API.
  bool local_management_password_overridden = false;
};

struct PortalRuntimeLaneTelemetry {
  uint32_t queue_capacity = 0;
  uint32_t queue_depth = 0;
  uint32_t queue_high_water = 0;
  uint32_t stack_low_water_bytes = 0;
  uint32_t handler_count = 0;
  uint32_t handler_max_us = 0;
  uint32_t tick_count = 0;
  uint32_t tick_max_us = 0;
  uint32_t tick_late_count = 0;
  uint32_t tick_missed = 0;
  uint32_t tick_late_max_us = 0;
  uint32_t last_progress_ms = 0;
  int8_t configured_core = -1;
  int8_t observed_core = -1;
  uint8_t configured_priority = 0;
  uint8_t observed_priority = 0;
  bool task_running = false;
  bool stack_sampled = false;
};

// Numeric-only copy of RuntimeSupervisor health. The Portal component remains
// independent of FreeRTOS and never receives task names, handles or work data.
struct PortalRuntimeTelemetry {
  std::array<PortalRuntimeLaneTelemetry, kPortalRuntimeLaneCount> lanes{};
  uint32_t sequence = 0;
  uint32_t last_managed_update_ms = 0;
  uint32_t internal_heap_min_free_bytes = 0;
  uint32_t psram_min_free_bytes = 0;
  uint32_t resource_sample_count = 0;
  uint8_t lane_count = 0;
  bool available = false;
  bool internal_heap_sampled = false;
  bool psram_available = false;
};

struct PortalRenderStrategyCapability {
  std::string id;
  std::string display_name;
};

// Capability-neutral by default. The product owner copies the selected board's
// bounded catalog and hardware facts before publishing an authenticated state.
struct PortalBoardCapabilities {
  std::array<PortalRenderStrategyCapability,
             kMaximumPortalRenderStrategies> render_strategies{};
  uint8_t render_strategy_count = 0U;
  uint8_t rgb_pixels = 0U;
  bool has_microphone = false;
  bool has_speaker = false;
  bool has_removable_storage = false;

  bool hasDuplexAudio() const { return has_microphone && has_speaker; }

  bool supportsRenderStrategy(const std::string& strategy) const {
    if (render_strategy_count > render_strategies.size()) return false;
    for (size_t index = 0; index < render_strategy_count; ++index) {
      if (render_strategies[index].id == strategy) return true;
    }
    return false;
  }
};

// This is a credential-free, owner-maintained snapshot. The Portal never
// receives a device token, pairing token, gateway token, or provider URL.
struct PortalStateSnapshot {
  std::string firmware_version;
  PortalFirmwareUpdateSnapshot firmware_update;
  std::string device_name;
  uint16_t display_width = 0U;
  uint16_t display_height = 0U;
  bool wifi_online = false;
  bool storage_ready = false;
  uint64_t storage_free_bytes = 0;
  uint64_t storage_total_bytes = 0;
  bool display_busy = false;
  uint32_t display_completed_refreshes = 0;
  uint32_t display_load_decode_ms = 0;
  uint32_t display_conversion_ms = 0;
  uint32_t display_panel_refresh_ms = 0;
  uint32_t display_total_ms = 0;
  MyAiPortalState myai_state = MyAiPortalState::Unconfigured;
  MyAiPortalErrorSnapshot myai_error;
  TutorialPortalSnapshot tutorial;
  std::string pairing_code;
  std::string binding_url;
  PortalBoardCapabilities capabilities;
  PortalRuntimeTelemetry runtime;
  PortalSettingsSnapshot settings;
};

struct AlbumItem {
  std::string id;
  std::string title;
  std::string origin;
  uint64_t bytes = 0;
  bool current = false;
  bool factory_asset = false;
  std::string render_strategy = "official-quality";
};

struct AlbumPageQuery {
  std::string cursor;
  size_t limit = kMaximumAlbumPageItems;
};

struct AlbumPage {
  std::vector<AlbumItem> items;
  std::string next_cursor;
  size_t total_items = 0;
  uint64_t revision = 0;
};

struct ChatItem {
  uint64_t sequence = 0;
  ChatRole role = ChatRole::User;
  std::string text;
};

struct ChatPageQuery {
  uint64_t after_sequence = 0;
  size_t limit = kMaximumChatPageItems;
};

struct ChatPage {
  std::vector<ChatItem> items;
  uint64_t next_after_sequence = 0;
  bool has_more = false;
  size_t total_items = 0;
  bool corruption_observed = false;
};

struct PortalSettingsPatch {
  bool has_volume = false;
  uint8_t volume = 0;
  bool has_led_maximum_brightness = false;
  uint8_t led_maximum_brightness_percent = 0;
  bool has_voice_assistance_enabled = false;
  bool voice_assistance_enabled = false;
  bool has_assistant_prompt = false;
  std::string assistant_prompt;
  bool has_image_prompt_template = false;
  std::string image_prompt_template;
  bool has_negative_prompt = false;
  std::string negative_prompt;
  bool has_asset_storage_preference = false;
  std::string asset_storage_preference;
  bool has_default_render_strategy = false;
  std::string default_render_strategy;
  bool has_local_management_password_override = false;
  // Empty explicitly restores "use the saved home Wi-Fi password".
  std::string local_management_password_override;
};

struct PortalCommand {
  PortalCommandType type = PortalCommandType::UpdateSettings;
  uint64_t request_id = 0;
  PortalSettingsPatch settings;
  uint8_t volume = 0;
  std::string asset_id;
  std::string render_strategy;
  std::string prompt;
};

// Every method is called from the HTTP server task and therefore must only
// copy a precomputed, bounded local snapshot. It must not scan files, call a
// remote service, wait on hardware, or take an unbounded lock.
class IPortalReadCache {
 public:
  virtual ~IPortalReadCache() = default;
  virtual PortalResult readState(PortalStateSnapshot& output) const = 0;
  virtual PortalResult readAlbumPage(const AlbumPageQuery& query,
                                     AlbumPage& output) const = 0;
  virtual PortalResult readLocalChatPage(const ChatPageQuery& query,
                                         ChatPage& output) const = 0;
};

// Implementations may only copy into a bounded owner queue and return. The
// consumer task owns persistence, screen refresh, MyAI, audio, and LEDs.
class IPortalCommandQueue {
 public:
  virtual ~IPortalCommandQueue() = default;
  virtual PortalResult tryEnqueue(const PortalCommand& command) = 0;
};

struct PortalAccessConfig {
  std::string access_code;
  std::string session_id;
  std::string csrf_token;
  std::vector<std::string> allowed_hosts;
  std::vector<std::string> allowed_origins;
  uint32_t session_lifetime_seconds = 900U;
};

struct PortalRequest {
  std::string method;
  std::string path;
  std::string host;
  std::string origin;
  std::string cookie;
  std::string csrf_token;
  std::string content_type;
  std::string body;
  bool peer_is_local = false;
  uint64_t content_length = 0;
  uint64_t now_seconds = 0;
};

struct PortalStreamRequest {
  uint64_t request_id = 0;
  std::string asset_id;
  std::string upload_title;
  size_t content_length = 0;
};

struct PortalResponse {
  int status = 500;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::string set_cookie;
  uint32_t retry_after_seconds = 0;
  ResponseDisposition disposition = ResponseDisposition::Immediate;
  PortalStreamRequest stream;
};

class PortalCore {
 public:
  PortalCore(const PortalAccessConfig& access, const IPortalReadCache& cache,
             IPortalCommandQueue& commands);

  bool ready() const { return ready_; }
  PortalResponse handle(const PortalRequest& request);
  static const char* dashboardHtml();

 private:
  bool validateConfiguration() const;
  bool hostAllowed(const std::string& host) const;
  bool originAllowed(const std::string& origin) const;
  bool sessionAuthorized(const PortalRequest& request) const;
  bool mutationAuthorized(const PortalRequest& request) const;
  PortalResponse handleSession(const PortalRequest& request);
  PortalResponse handleAuthenticated(const PortalRequest& request);
  PortalResponse renderState();
  PortalResponse renderAlbum(const PortalRequest& request);
  PortalResponse renderChat(const PortalRequest& request);
  PortalResponse updateSettings(const PortalRequest& request);
  PortalResponse requestFirmwareUpdate(const PortalRequest& request);
  PortalResponse enqueueSimple(const PortalRequest& request,
                               PortalCommandType type);
  PortalResponse enqueueAsset(const PortalRequest& request,
                              PortalCommandType type);
  PortalResponse enqueueCommand(PortalCommand command);
  uint64_t nextRequestId();

  PortalAccessConfig access_;
  const IPortalReadCache& cache_;
  IPortalCommandQueue& commands_;
  bool ready_ = false;
  bool session_issued_ = false;
  uint64_t session_expires_at_seconds_ = 0;
  uint64_t next_request_id_ = 1;
};

const char* portalResultName(PortalResult value);
const char* portalCommandTypeName(PortalCommandType value);

}  // namespace portal
}  // namespace inkloop
