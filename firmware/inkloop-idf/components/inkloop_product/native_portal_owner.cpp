#include "inkloop/native_portal_owner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mdns.h"
#include "inkloop/board_prompt_policy.hpp"
#include "inkloop/product_opcodes.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-portal-own";
constexpr char kFirmwareVersion[] = "idf-native";
constexpr uint32_t kRecentPortalWindowMs = 30000U;
constexpr uint32_t kStateRefreshMs = 1000U;
constexpr uint32_t kAlbumRefreshMs = 1000U;
constexpr uint32_t kChatRefreshMs = 2000U;
constexpr uint32_t kRuntimeSummaryMs = 60000U;
constexpr size_t kMaximumCachedChatItems = 96U;
static_assert(kMaximumBoardRenderStrategies ==
                  portal::kMaximumPortalRenderStrategies,
              "board and Portal strategy bounds must match");

bool due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool takeMutex(SemaphoreHandle_t mutex) {
  return mutex && xSemaphoreTake(mutex, 0) == pdTRUE;
}

void giveMutex(SemaphoreHandle_t mutex) {
  if (mutex) xSemaphoreGive(mutex);
}

std::string boundedLabel(const storage::AlbumIndexAsset& asset,
                         size_t ordinal) {
  if (!asset.task_id.empty()) {
    if (asset.task_id.rfind("dtask-", 0) == 0) return "Inkloop 任务";
    if (asset.task_id.rfind("aigc:", 0) == 0) return "AI 生成图片";
    if (asset.task_id.rfind("upload:", 0) == 0) return "上传图片";
  }
  return std::string("图片 ") + std::to_string(ordinal + 1U);
}

std::string albumOrigin(const storage::AlbumIndexAsset& asset) {
  if (asset.task_id.rfind("dtask-", 0) == 0) return "inkloop";
  if (asset.task_id.rfind("aigc:", 0) == 0) return "aigc";
  if (asset.task_id.rfind("upload:", 0) == 0) return "upload";
  return "local";
}

uint32_t bigEndian32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24U) |
         (static_cast<uint32_t>(bytes[1]) << 16U) |
         (static_cast<uint32_t>(bytes[2]) << 8U) |
         static_cast<uint32_t>(bytes[3]);
}

bool copyBoardCapabilities(IBoardAdapter& board,
                           portal::PortalBoardCapabilities& output) {
  const BoardDescriptor& descriptor = board.descriptor();
  if (descriptor.rgb_pixels > portal::kMaximumPortalRgbPixels) return false;
  IBoardRenderer* renderer = board.renderer();
  if (!renderer) return false;
  const BoardRenderStrategyCatalog catalog = renderer->renderStrategyCatalog();
  if (!catalog.valid()) return false;
  output = portal::PortalBoardCapabilities{};
  output.has_microphone = descriptor.has_microphone;
  output.has_speaker = descriptor.has_speaker;
  output.rgb_pixels = descriptor.rgb_pixels;
  output.has_removable_storage = descriptor.has_sd;
  output.render_strategy_count = catalog.count;
  for (size_t index = 0; index < catalog.count; ++index) {
    if (!renderer->supportsRenderStrategy(catalog.entries[index].id))
      return false;
    output.render_strategies[index].id = catalog.entries[index].id;
    output.render_strategies[index].display_name =
        catalog.entries[index].display_name;
  }
  return true;
}

bool boardSupportsRenderStrategy(IBoardAdapter& board,
                                 const std::string& strategy) {
  IBoardRenderer* renderer = board.renderer();
  if (!renderer) return false;
  const BoardRenderStrategyCatalog catalog = renderer->renderStrategyCatalog();
  return catalog.valid() && catalog.contains(strategy) &&
         renderer->supportsRenderStrategy(strategy);
}

}  // namespace

NativePortalOwner::NativePortalOwner(
    IBoardAdapter& board, RuntimeSupervisor& supervisor,
    EspWifiStationOwner& wifi, EspStatusLedOwner& leds,
    NativeDisplayService& display, NativeVoiceService& voice,
    const char* storage_root, storage::PosixAtomicAlbumStore* album_store)
    : board_(board), supervisor_(supervisor), wifi_(wifi), leds_(leds),
      display_(display), voice_(voice), storage_root_(storage_root),
      album_store_(album_store) {}

NativePortalOwner::~NativePortalOwner() {
  (void)stopServer();
  if (upload_pool_) {
    heap_caps_free(upload_pool_);
    upload_pool_ = nullptr;
  }
}

uint32_t NativePortalOwner::nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

std::string NativePortalOwner::cursorFor(size_t ordinal) {
  return ordinal == 0U ? std::string() : std::to_string(ordinal);
}

bool NativePortalOwner::parseCursor(const std::string& cursor,
                                    size_t& ordinal) {
  ordinal = 0;
  if (cursor.empty()) return true;
  if (cursor.size() > 3U) return false;
  size_t value = 0;
  for (char ch : cursor) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10U + static_cast<size_t>(ch - '0');
  }
  if (value > portal::kMaximumAlbumTotalItems) return false;
  ordinal = value;
  return true;
}

portal::MyAiPortalState NativePortalOwner::portalMyAiState(
    myai::ActivationState state) {
  switch (state) {
    case myai::ActivationState::Unconfigured:
      return portal::MyAiPortalState::Unconfigured;
    case myai::ActivationState::Pairing:
      return portal::MyAiPortalState::Pairing;
    case myai::ActivationState::Bound:
      // Bound only proves that a device token exists. Authorization is a
      // separate server check, so never claim the stronger Active state here.
      return portal::MyAiPortalState::Bound;
    case myai::ActivationState::PaymentRequired:
    case myai::ActivationState::RecoveryRequired:
      return portal::MyAiPortalState::RecoveryRequired;
    case myai::ActivationState::Offline:
    case myai::ActivationState::Error:
      return portal::MyAiPortalState::Unavailable;
  }
  return portal::MyAiPortalState::Unavailable;
}

std::string NativePortalOwner::bootToken(size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes * 2U);
  for (size_t at = 0; at < bytes; ++at) {
    const uint8_t value = static_cast<uint8_t>(esp_random());
    token.push_back(kHex[value >> 4U]);
    token.push_back(kHex[value & 0x0FU]);
  }
  return token;
}

bool NativePortalOwner::copyTitle(
    const std::string& title,
    std::array<char, portal::kMaximumAlbumTitleBytes + 1U>& out) {
  if (title.empty() || title.size() >= out.size()) return false;
  out.fill('\0');
  std::copy(title.begin(), title.end(), out.begin());
  return true;
}

void NativePortalOwner::noteAccess() const {
  portENTER_CRITICAL(&activity_mux_);
  last_access_ms_ = nowMs();
  portEXIT_CRITICAL(&activity_mux_);
}

NativePortalOwner::StorageHttpGuard::StorageHttpGuard(
    const NativePortalOwner& owner, bool allow_during_maintenance)
    : owner_(owner),
      active_(owner_.beginStorageHttpOperation(allow_during_maintenance)) {}

NativePortalOwner::StorageHttpGuard::~StorageHttpGuard() {
  if (active_) owner_.endStorageHttpOperation();
}

bool NativePortalOwner::beginStorageHttpOperation(
    bool allow_during_maintenance) const {
  portENTER_CRITICAL(&activity_mux_);
  if ((!storage_available_ || storage_maintenance_active_) &&
      !allow_during_maintenance) {
    portEXIT_CRITICAL(&activity_mux_);
    return false;
  }
  ++storage_http_operations_;
  portEXIT_CRITICAL(&activity_mux_);
  return true;
}

void NativePortalOwner::endStorageHttpOperation() const {
  portENTER_CRITICAL(&activity_mux_);
  if (storage_http_operations_ != 0U) --storage_http_operations_;
  portEXIT_CRITICAL(&activity_mux_);
}

uint32_t NativePortalOwner::lastAccessMs() const {
  portENTER_CRITICAL(&activity_mux_);
  const uint32_t value = last_access_ms_;
  portEXIT_CRITICAL(&activity_mux_);
  return value;
}

bool NativePortalOwner::beginStorageMaintenance() {
  portENTER_CRITICAL(&activity_mux_);
  if (storage_maintenance_active_) {
    portEXIT_CRITICAL(&activity_mux_);
    return true;
  }
  storage_maintenance_active_ = true;
  const bool http_busy = storage_http_operations_ != 0U;
  portEXIT_CRITICAL(&activity_mux_);
  if (http_busy) {
    endStorageMaintenance();
    return false;
  }

  bool preview_busy = false;
  for (const PreviewHandle& handle : preview_handles_) {
    if (handle.file) {
      preview_busy = true;
      break;
    }
  }
  if (preview_busy || mutationBusy() ||
      (command_queue_ && uxQueueMessagesWaiting(command_queue_) != 0U)) {
    endStorageMaintenance();
    return false;
  }

  // No new storage-facing request can enter after the gate was raised. Stop
  // and join the HTTP server only after every previously admitted stream and
  // preview handle is gone.
  if (stopServer() != ESP_OK) {
    endStorageMaintenance();
    return false;
  }
  portENTER_CRITICAL(&activity_mux_);
  const bool raced = storage_http_operations_ != 0U;
  portEXIT_CRITICAL(&activity_mux_);
  if (raced || mutationBusy()) {
    endStorageMaintenance();
    return false;
  }
  return true;
}

bool NativePortalOwner::finishStorageMaintenance(bool storage_changed,
                                                  bool storage_available) {
  if (!storageMaintenanceActive()) return false;
  if (storage_changed) {
    if (!takeMutex(cache_mutex_)) return false;
    album_cache_ = storage::AlbumIndex{};
    chat_cache_ = portal::ChatPage{};
    album_cache_ready_ = false;
    chat_cache_ready_ = false;
    state_cache_.storage_ready = false;
    state_cache_.storage_free_bytes = 0U;
    state_cache_.storage_total_bytes = 0U;
    ++album_revision_;
    giveMutex(cache_mutex_);
  }
  portENTER_CRITICAL(&activity_mux_);
  storage_available_ = storage_available;
  portEXIT_CRITICAL(&activity_mux_);
  restart_refresh_required_ = true;
  return true;
}

void NativePortalOwner::endStorageMaintenance() {
  portENTER_CRITICAL(&activity_mux_);
  storage_maintenance_active_ = false;
  portEXIT_CRITICAL(&activity_mux_);
}

bool NativePortalOwner::storageMaintenanceActive() const {
  portENTER_CRITICAL(&activity_mux_);
  const bool active = storage_maintenance_active_;
  portEXIT_CRITICAL(&activity_mux_);
  return active;
}

esp_err_t NativePortalOwner::initialize() {
  if (initialized_ || !storage_root_ || storage_root_[0] != '/' ||
      !album_store_) {
    return ESP_ERR_INVALID_STATE;
  }
  cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
  command_mutex_ = xSemaphoreCreateMutexStatic(&command_mutex_storage_);
  upload_mutex_ = xSemaphoreCreateMutexStatic(&upload_mutex_storage_);
  command_queue_ = xQueueCreateStatic(
      kCommandSlots, sizeof(uint8_t), command_queue_bytes_.data(),
      &command_queue_storage_);
  upload_queue_ = xQueueCreateStatic(
      kUploadEventDepth, sizeof(UploadEvent), upload_queue_bytes_.data(),
      &upload_queue_storage_);
  upload_pool_ = static_cast<uint8_t*>(heap_caps_malloc(
      kUploadSlots * kUploadChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!upload_pool_) {
    upload_pool_ = static_cast<uint8_t*>(heap_caps_malloc(
        kUploadSlots * kUploadChunkBytes, MALLOC_CAP_8BIT));
  }
  if (!cache_mutex_ || !command_mutex_ || !upload_mutex_ || !command_queue_ ||
      !upload_queue_ || !upload_pool_) {
    return ESP_ERR_NO_MEM;
  }
  state_cache_.firmware_version = kFirmwareVersion;
  const BoardDescriptor& board_descriptor = board_.descriptor();
  const char* board_id = board_descriptor.id && board_descriptor.id[0] != '\0'
                             ? board_descriptor.id
                             : "device";
  state_cache_.device_name = board_id;
  state_cache_.display_width = board_descriptor.width;
  state_cache_.display_height = board_descriptor.height;
  if (!copyBoardCapabilities(board_, state_cache_.capabilities))
    return ESP_ERR_INVALID_STATE;
  std::snprintf(mdns_instance_name_.data(), mdns_instance_name_.size(),
                "Inkloop %.55s", board_id);
  state_cache_.settings.volume = 60U;
  state_cache_.settings.led_maximum_brightness_percent = 60U;
  state_cache_.settings.voice_assistance_enabled = true;
  state_cache_.settings.assistant_prompt =
      defaultAssistantPrompt(board_.descriptor());
  state_cache_.settings.image_prompt_template =
      defaultImagePromptTemplate(board_.descriptor());
  state_cache_.settings.negative_prompt =
      defaultNegativePrompt(board_.descriptor());
  settings_ready_ = loadSettings();
  if (!settings_ready_) return ESP_FAIL;
  if (voice_.seedPersistedVoiceSettings(
          state_cache_.settings.volume,
          state_cache_.settings.voice_assistance_enabled,
          state_cache_.settings.assistant_prompt) != ESP_OK) {
    return ESP_FAIL;
  }
  leds_.setMaximumBrightnessPercent(
      state_cache_.settings.led_maximum_brightness_percent, false);
  refreshState();
  refreshAlbum();
  refreshChat();
  initialized_ = true;
  return ESP_OK;
}

esp_err_t NativePortalOwner::shutdown() {
  const esp_err_t stopped = stopServer();
  if (stopped != ESP_OK) return stopped;

  if (album_store_) album_store_->abort();
  active_upload_ = ActiveUpload{};
  for (PreviewHandle& handle : preview_handles_) {
    if (handle.file) std::fclose(handle.file);
    handle = PreviewHandle{};
  }
  if (command_queue_) {
    vQueueDelete(command_queue_);
    command_queue_ = nullptr;
  }
  if (upload_queue_) {
    vQueueDelete(upload_queue_);
    upload_queue_ = nullptr;
  }
  if (cache_mutex_) {
    vSemaphoreDelete(cache_mutex_);
    cache_mutex_ = nullptr;
  }
  if (command_mutex_) {
    vSemaphoreDelete(command_mutex_);
    command_mutex_ = nullptr;
  }
  if (upload_mutex_) {
    vSemaphoreDelete(upload_mutex_);
    upload_mutex_ = nullptr;
  }
  if (upload_pool_) {
    std::memset(upload_pool_, 0, kUploadSlots * kUploadChunkBytes);
    heap_caps_free(upload_pool_);
    upload_pool_ = nullptr;
  }

  commands_ = {};
  command_used_.fill(false);
  upload_slot_used_.fill(false);
  accepted_upload_request_ = 0U;
  abort_requested_id_ = 0U;
  state_cache_ = portal::PortalStateSnapshot{};
  album_cache_ = storage::AlbumIndex{};
  chat_cache_ = portal::ChatPage{};
  state_cache_ready_ = false;
  album_cache_ready_ = false;
  chat_cache_ready_ = false;
  album_revision_ = 0U;
  preview_sequence_ = 0U;
  request_sequence_ = 0U;
  portENTER_CRITICAL(&activity_mux_);
  last_access_ms_ = 0U;
  storage_http_operations_ = 0U;
  storage_maintenance_active_ = false;
  storage_available_ = true;
  portEXIT_CRITICAL(&activity_mux_);
  restart_refresh_required_ = false;
  next_state_refresh_ms_ = 0U;
  next_album_refresh_ms_ = 0U;
  next_chat_refresh_ms_ = 0U;
  next_runtime_summary_ms_ = 0U;
  settings_ready_ = false;
  initialized_ = false;
  mdns_instance_name_.fill('\0');
  return ESP_OK;
}

esp_err_t NativePortalOwner::attachSettingsOwner(
    IPortalSettingsOwner& owner) {
  if (initialized_) return ESP_ERR_INVALID_STATE;
  settings_owner_ = &owner;
  return ESP_OK;
}

esp_err_t NativePortalOwner::attachAlbumMutationOwner(
    IPortalAlbumMutationOwner& owner) {
  if (initialized_) return ESP_ERR_INVALID_STATE;
  album_mutation_owner_ = &owner;
  return ESP_OK;
}

esp_err_t NativePortalOwner::attachFirmwareUpdateOwner(
    IPortalFirmwareUpdateOwner& owner) {
  if (initialized_) return ESP_ERR_INVALID_STATE;
  firmware_update_owner_ = &owner;
  return ESP_OK;
}

portal::PortalResult NativePortalOwner::publishLocalChatSnapshot(
    const portal::ChatPage& snapshot) {
  if (snapshot.items.size() > kMaximumCachedChatItems ||
      snapshot.total_items > portal::kMaximumChatTotalItems ||
      snapshot.items.size() > snapshot.total_items ||
      !takeMutex(cache_mutex_)) {
    return portal::PortalResult::InvalidData;
  }
  uint64_t previous = 0;
  size_t aggregate = 0;
  for (const portal::ChatItem& item : snapshot.items) {
    if (item.sequence <= previous || item.text.empty() ||
        item.text.size() > portal::kMaximumChatTextBytes ||
        aggregate > portal::kMaximumChatPageTextBytes ||
        item.text.size() > portal::kMaximumChatPageTextBytes - aggregate) {
      giveMutex(cache_mutex_);
      return portal::PortalResult::InvalidData;
    }
    previous = item.sequence;
    aggregate += item.text.size();
  }
  chat_cache_ = snapshot;
  chat_cache_ready_ = true;
  giveMutex(cache_mutex_);
  return portal::PortalResult::Ok;
}

bool NativePortalOwner::accept(const NativeLocalChatSnapshot& snapshot) {
  if (snapshot.item_count > snapshot.items.size() ||
      snapshot.text_bytes > snapshot.text.size()) return false;
  portal::ChatPage page;
  page.next_after_sequence = snapshot.next_after_sequence;
  page.has_more = snapshot.has_more;
  page.corruption_observed = snapshot.corruption_observed;
  page.total_items = snapshot.item_count + (snapshot.has_more ? 1U : 0U);
  page.items.reserve(snapshot.item_count);
  for (size_t at = 0; at < snapshot.item_count; ++at) {
    const NativeLocalChatItem& native = snapshot.items[at];
    if (native.text_bytes == 0U || native.text_offset > snapshot.text_bytes ||
        native.text_bytes > snapshot.text_bytes - native.text_offset) {
      return false;
    }
    portal::ChatItem item;
    item.sequence = native.sequence;
    switch (native.role) {
      case NativeLocalChatRole::User:
        item.role = portal::ChatRole::User;
        break;
      case NativeLocalChatRole::Assistant:
        item.role = portal::ChatRole::Assistant;
        break;
      case NativeLocalChatRole::Tool:
        item.role = portal::ChatRole::Tool;
        break;
    }
    item.text.assign(snapshot.text.data() + native.text_offset,
                     native.text_bytes);
    page.items.push_back(std::move(item));
  }
  return publishLocalChatSnapshot(page) == portal::PortalResult::Ok;
}

portal::PortalResult NativePortalOwner::readState(
    portal::PortalStateSnapshot& output) const {
  noteAccess();
  if (!takeMutex(cache_mutex_)) return portal::PortalResult::Busy;
  const bool ready = state_cache_ready_;
  if (ready) output = state_cache_;
  giveMutex(cache_mutex_);
  return ready ? portal::PortalResult::Ok : portal::PortalResult::Unavailable;
}

portal::PortalResult NativePortalOwner::readAlbumPage(
    const portal::AlbumPageQuery& query, portal::AlbumPage& output) const {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  noteAccess();
  size_t start = 0;
  if (!parseCursor(query.cursor, start)) return portal::PortalResult::InvalidData;
  if (!takeMutex(cache_mutex_)) return portal::PortalResult::Busy;
  if (!album_cache_ready_ || start > album_cache_.assets.size()) {
    giveMutex(cache_mutex_);
    return portal::PortalResult::Unavailable;
  }
  output = portal::AlbumPage();
  output.total_items = album_cache_.assets.size();
  output.revision = album_revision_;
  const size_t end = std::min(album_cache_.assets.size(), start + query.limit);
  output.items.reserve(end - start);
  for (size_t at = start; at < end; ++at) {
    const storage::AlbumIndexAsset& asset = album_cache_.assets[at];
    portal::AlbumItem item;
    item.id = asset.id;
    item.title = boundedLabel(asset, at);
    item.origin = albumOrigin(asset);
    item.bytes = asset.bytes;
    item.current = album_cache_.current == asset.id;
    item.render_strategy = asset.render_strategy;
    output.items.push_back(std::move(item));
  }
  if (end < album_cache_.assets.size()) output.next_cursor = cursorFor(end);
  giveMutex(cache_mutex_);
  return portal::PortalResult::Ok;
}

portal::PortalResult NativePortalOwner::readLocalChatPage(
    const portal::ChatPageQuery& query, portal::ChatPage& output) const {
  noteAccess();
  if (!takeMutex(cache_mutex_)) return portal::PortalResult::Busy;
  if (!chat_cache_ready_) {
    giveMutex(cache_mutex_);
    return portal::PortalResult::Unavailable;
  }
  output = portal::ChatPage();
  output.total_items = chat_cache_.total_items;
  output.corruption_observed = chat_cache_.corruption_observed;
  for (const portal::ChatItem& item : chat_cache_.items) {
    if (item.sequence <= query.after_sequence) continue;
    if (output.items.size() >= query.limit) {
      output.has_more = true;
      break;
    }
    output.items.push_back(item);
  }
  output.next_after_sequence = output.items.empty()
      ? query.after_sequence : output.items.back().sequence;
  if (!output.has_more) {
    output.has_more = std::any_of(
        chat_cache_.items.begin(), chat_cache_.items.end(),
        [&](const portal::ChatItem& item) {
          return item.sequence > output.next_after_sequence;
        });
  }
  giveMutex(cache_mutex_);
  return portal::PortalResult::Ok;
}

portal::PortalResult NativePortalOwner::tryEnqueue(
    const portal::PortalCommand& command) {
  noteAccess();
  const bool album_command =
      command.type == portal::PortalCommandType::DisplayAlbumItem ||
       command.type == portal::PortalCommandType::DeleteAlbumItem ||
       command.type == portal::PortalCommandType::SetAlbumRenderStrategy;
  portENTER_CRITICAL(&activity_mux_);
  const bool storage_blocked =
      storage_maintenance_active_ || !storage_available_;
  portEXIT_CRITICAL(&activity_mux_);
  if (album_command && storage_blocked) {
    return portal::PortalResult::Busy;
  }
  switch (command.type) {
    case portal::PortalCommandType::UpdateSettings:
      if (!settings_owner_) return portal::PortalResult::Unavailable;
      if ((command.settings.has_volume ||
           command.settings.has_voice_assistance_enabled) &&
          !(board_.descriptor().has_microphone &&
            board_.descriptor().has_speaker)) {
        return portal::PortalResult::InvalidData;
      }
      if (command.settings.has_led_maximum_brightness &&
          board_.descriptor().rgb_pixels == 0U) {
        return portal::PortalResult::InvalidData;
      }
      if (command.settings.has_asset_storage_preference &&
          command.settings.asset_storage_preference == "removable" &&
          !board_.descriptor().has_sd) {
        return portal::PortalResult::InvalidData;
      }
      if (command.settings.has_default_render_strategy &&
          !boardSupportsRenderStrategy(
              board_, command.settings.default_render_strategy)) {
        return portal::PortalResult::InvalidData;
      }
      break;
    case portal::PortalCommandType::DisplayAlbumItem:
      break;
    case portal::PortalCommandType::SetAlbumRenderStrategy:
      if (!boardSupportsRenderStrategy(board_, command.render_strategy))
        return portal::PortalResult::InvalidData;
      break;
    case portal::PortalCommandType::DeleteAlbumItem:
      if (!album_mutation_owner_) return portal::PortalResult::Unavailable;
      break;
    case portal::PortalCommandType::PreviewVolume:
      if (command.volume > 100U || !board_.descriptor().has_microphone ||
          !board_.descriptor().has_speaker) {
        return portal::PortalResult::InvalidData;
      }
      break;
    case portal::PortalCommandType::StartMyAiPairing: {
      const myai::ActivationState state =
          voice_.onboardingSnapshot().activation_state;
      if (state != myai::ActivationState::Unconfigured &&
          state != myai::ActivationState::Pairing) {
        return portal::PortalResult::InvalidData;
      }
      break;
    }
    case portal::PortalCommandType::RebindMyAi:
      break;
    case portal::PortalCommandType::GenerateImage:
      if (command.prompt.empty() ||
          command.prompt.size() > portal::kMaximumGeneratePromptBytes) {
        return portal::PortalResult::InvalidData;
      }
      if (voice_.portalBusy()) return portal::PortalResult::Busy;
      break;
    case portal::PortalCommandType::ClearLocalChat:
      break;
    case portal::PortalCommandType::RequestFirmwareUpdate:
      if (!firmware_update_owner_) return portal::PortalResult::Unavailable;
      {
        portal::PortalFirmwareUpdateSnapshot update;
        const portal::PortalResult read =
            firmware_update_owner_->readPortalFirmwareUpdate(update);
        if (read != portal::PortalResult::Ok) return read;
        if (!update.configured) return portal::PortalResult::Unavailable;
        if (update.accepted_offline)
          return portal::PortalResult::Busy;
      }
      break;
  }
  if (!takeMutex(command_mutex_)) return portal::PortalResult::Busy;
  size_t slot = kCommandSlots;
  for (size_t at = 0; at < command_used_.size(); ++at) {
    if (!command_used_[at]) {
      slot = at;
      break;
    }
  }
  if (slot == kCommandSlots) {
    giveMutex(command_mutex_);
    return portal::PortalResult::Busy;
  }
  commands_[slot] = command;
  command_used_[slot] = true;
  const uint8_t queued = static_cast<uint8_t>(slot);
  if (xQueueSend(command_queue_, &queued, 0) != pdTRUE) {
    commands_[slot] = portal::PortalCommand();
    command_used_[slot] = false;
    giveMutex(command_mutex_);
    return portal::PortalResult::Busy;
  }
  giveMutex(command_mutex_);
  return portal::PortalResult::Ok;
}

bool NativePortalOwner::takeCommand(portal::PortalCommand& command) {
  uint8_t slot = 0;
  if (xQueueReceive(command_queue_, &slot, 0) != pdTRUE ||
      slot >= kCommandSlots || !takeMutex(command_mutex_)) {
    return false;
  }
  if (!command_used_[slot]) {
    giveMutex(command_mutex_);
    return false;
  }
  command = std::move(commands_[slot]);
  commands_[slot] = portal::PortalCommand();
  command_used_[slot] = false;
  giveMutex(command_mutex_);
  return true;
}

portal::PortalResult NativePortalOwner::enqueueUploadEvent(
    const UploadEvent& event) {
  return xQueueSend(upload_queue_, &event, 0) == pdTRUE
             ? portal::PortalResult::Ok : portal::PortalResult::Busy;
}

portal::PortalResult NativePortalOwner::tryBegin(
    const portal::PortalStreamRequest& request) {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  noteAccess();
  if (request.request_id == 0U || request.content_length == 0U ||
      request.content_length > portal::kMaximumAlbumUploadBytes) {
    return portal::PortalResult::InvalidRequest;
  }
  if (!takeMutex(upload_mutex_)) return portal::PortalResult::Busy;
  if (accepted_upload_request_ != 0U) {
    giveMutex(upload_mutex_);
    return portal::PortalResult::Busy;
  }
  UploadEvent event;
  event.kind = UploadEventKind::Begin;
  event.request_id = request.request_id;
  event.content_length = request.content_length;
  if (!copyTitle(request.upload_title, event.title)) {
    giveMutex(upload_mutex_);
    return portal::PortalResult::InvalidData;
  }
  accepted_upload_request_ = request.request_id;
  giveMutex(upload_mutex_);
  const portal::PortalResult queued = enqueueUploadEvent(event);
  if (queued != portal::PortalResult::Ok && takeMutex(upload_mutex_)) {
    accepted_upload_request_ = 0;
    giveMutex(upload_mutex_);
  }
  return queued;
}

portal::PortalResult NativePortalOwner::tryWrite(
    uint64_t request_id, const uint8_t* bytes, size_t length) {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  noteAccess();
  if (!bytes || length == 0U || length > kUploadChunkBytes) {
    return portal::PortalResult::InvalidRequest;
  }
  if (!takeMutex(upload_mutex_)) return portal::PortalResult::Busy;
  if (accepted_upload_request_ != request_id) {
    giveMutex(upload_mutex_);
    return portal::PortalResult::InvalidRequest;
  }
  size_t slot = kUploadSlots;
  for (size_t at = 0; at < upload_slot_used_.size(); ++at) {
    if (!upload_slot_used_[at]) {
      upload_slot_used_[at] = true;
      slot = at;
      break;
    }
  }
  giveMutex(upload_mutex_);
  if (slot == kUploadSlots) return portal::PortalResult::Busy;
  std::memcpy(upload_pool_ + slot * kUploadChunkBytes, bytes, length);
  UploadEvent event;
  event.kind = UploadEventKind::Chunk;
  event.slot = static_cast<uint8_t>(slot);
  event.length = static_cast<uint16_t>(length);
  event.request_id = request_id;
  const portal::PortalResult queued = enqueueUploadEvent(event);
  if (queued != portal::PortalResult::Ok) releaseUploadSlot(event.slot);
  return queued;
}

portal::PortalResult NativePortalOwner::tryFinish(uint64_t request_id) {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  noteAccess();
  if (!takeMutex(upload_mutex_)) return portal::PortalResult::Busy;
  const bool accepted = accepted_upload_request_ == request_id;
  giveMutex(upload_mutex_);
  if (!accepted) return portal::PortalResult::InvalidRequest;
  UploadEvent event;
  event.kind = UploadEventKind::Finish;
  event.request_id = request_id;
  return enqueueUploadEvent(event);
}

void NativePortalOwner::tryAbort(uint64_t request_id) {
  if (request_id == 0U) return;
  UploadEvent event;
  event.kind = UploadEventKind::Abort;
  event.request_id = request_id;
  if (enqueueUploadEvent(event) == portal::PortalResult::Ok) return;
  if (takeMutex(upload_mutex_)) {
    abort_requested_id_ = request_id;
    giveMutex(upload_mutex_);
  }
}

bool NativePortalOwner::takeUploadEvent(UploadEvent& event) {
  return xQueueReceive(upload_queue_, &event, 0) == pdTRUE;
}

void NativePortalOwner::releaseUploadSlot(uint8_t slot) {
  if (slot >= kUploadSlots || !takeMutex(upload_mutex_)) return;
  upload_slot_used_[slot] = false;
  giveMutex(upload_mutex_);
}

void NativePortalOwner::failActiveUpload() {
  if (album_store_) album_store_->abort();
  const uint64_t request = active_upload_.request_id;
  active_upload_ = ActiveUpload();
  if (takeMutex(upload_mutex_)) {
    if (accepted_upload_request_ == request) accepted_upload_request_ = 0;
    if (abort_requested_id_ == request) abort_requested_id_ = 0;
    giveMutex(upload_mutex_);
  }
}

void NativePortalOwner::finishActiveUpload() {
  if (!album_store_ || active_upload_.received != active_upload_.expected ||
      active_upload_.header_bytes < active_upload_.png_header.size()) {
    failActiveUpload();
    return;
  }
  static constexpr std::array<uint8_t, 8> kPngSignature{{
      0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU}};
  if (!std::equal(kPngSignature.begin(), kPngSignature.end(),
                  active_upload_.png_header.begin())) {
    failActiveUpload();
    return;
  }
  const uint32_t width = bigEndian32(active_upload_.png_header.data() + 16U);
  const uint32_t height = bigEndian32(active_upload_.png_header.data() + 20U);
  bool landscape = false;
  if (!classifyBoardPngGeometry(
          board_.descriptor(), width, height, landscape)) {
    failActiveUpload();
    return;
  }
  storage::AlbumCommitRequest request;
  request.prompt_id = std::string("upload-") +
                      std::to_string(active_upload_.request_id);
  request.task_id = std::string("upload:") +
                    std::to_string(active_upload_.request_id);
  request.source_filename = active_upload_.title + ".png";
  request.render_strategy = std::string(kOfficialQualityRenderStrategy);
  if (takeMutex(cache_mutex_)) {
    if (state_cache_.capabilities.supportsRenderStrategy(
            state_cache_.settings.default_render_strategy) &&
        boardSupportsRenderStrategy(
            board_, state_cache_.settings.default_render_strategy)) {
      request.render_strategy =
          state_cache_.settings.default_render_strategy;
    }
    giveMutex(cache_mutex_);
  }
  request.bytes = active_upload_.received;
  request.landscape = landscape;
  storage::AlbumCommitResult committed;
  if (!album_store_->commitValidated(request, committed).ok()) {
    failActiveUpload();
    return;
  }
  const uint64_t completed = active_upload_.request_id;
  active_upload_ = ActiveUpload();
  if (takeMutex(upload_mutex_)) {
    if (accepted_upload_request_ == completed) accepted_upload_request_ = 0;
    giveMutex(upload_mutex_);
  }
  display_.reloadCatalog();
  refreshAlbum();
}

void NativePortalOwner::serviceUpload(const UploadEvent& event) {
  uint64_t abort_request = 0;
  if (takeMutex(upload_mutex_)) {
    abort_request = abort_requested_id_;
    giveMutex(upload_mutex_);
  }
  if (abort_request != 0U && abort_request == active_upload_.request_id) {
    failActiveUpload();
  }
  switch (event.kind) {
    case UploadEventKind::Begin:
      if (active_upload_.request_id != 0U || !album_store_ ||
          !album_store_->begin(event.content_length).ok()) {
        if (takeMutex(upload_mutex_)) {
          if (accepted_upload_request_ == event.request_id)
            accepted_upload_request_ = 0;
          giveMutex(upload_mutex_);
        }
        return;
      }
      active_upload_.request_id = event.request_id;
      active_upload_.expected = event.content_length;
      active_upload_.title = event.title.data();
      break;
    case UploadEventKind::Chunk: {
      const bool valid = active_upload_.request_id == event.request_id &&
                         event.slot < kUploadSlots && event.length > 0U &&
                         event.length <= kUploadChunkBytes &&
                         active_upload_.received <= active_upload_.expected &&
                         event.length <=
                             active_upload_.expected - active_upload_.received;
      if (valid) {
        const uint8_t* bytes = upload_pool_ + event.slot * kUploadChunkBytes;
        const size_t header_remaining = active_upload_.png_header.size() -
                                        active_upload_.header_bytes;
        const size_t header_copy = std::min<size_t>(header_remaining,
                                                    event.length);
        if (header_copy != 0U) {
          std::memcpy(active_upload_.png_header.data() +
                          active_upload_.header_bytes,
                      bytes, header_copy);
          active_upload_.header_bytes += header_copy;
        }
        if (!album_store_->append(bytes, event.length).ok()) {
          active_upload_.failed = true;
        } else {
          active_upload_.received += event.length;
        }
      }
      releaseUploadSlot(event.slot);
      if (!valid || active_upload_.failed) failActiveUpload();
      break;
    }
    case UploadEventKind::Finish:
      if (active_upload_.request_id == event.request_id) finishActiveUpload();
      break;
    case UploadEventKind::Abort:
      if (active_upload_.request_id == event.request_id) failActiveUpload();
      else if (takeMutex(upload_mutex_)) {
        if (accepted_upload_request_ == event.request_id)
          accepted_upload_request_ = 0;
        giveMutex(upload_mutex_);
      }
      break;
  }
}

AdmissionResult NativePortalOwner::requestDisplay(size_t ordinal) {
  if (ordinal > std::numeric_limits<uint8_t>::max())
    return AdmissionResult::InvalidEnvelope;
  WorkEnvelope command{};
  command.generation = 1;
  command.request_id = nextRequestId();
  command.opcode = productOpcode(ProductOpcode::DisplayAlbumOrdinal);
  command.work_class = WorkClass::Display;
  command.kind = EnvelopeKind::Command;
  command.disposition = WorkDisposition::Accepted;
  command.flags = static_cast<uint8_t>(ordinal);
  return supervisor_.post(command);
}

void NativePortalOwner::serviceCommand(
    const portal::PortalCommand& command) {
  if (command.type == portal::PortalCommandType::UpdateSettings) {
    applySettings(command.settings);
    return;
  }
  if (command.type == portal::PortalCommandType::PreviewVolume) {
    voice_.enqueueVolumePreview(command.volume);
    return;
  }
  if (command.type == portal::PortalCommandType::StartMyAiPairing) {
    voice_.enqueueStartMyAiPairing();
    return;
  }
  if (command.type == portal::PortalCommandType::RebindMyAi) {
    voice_.enqueueRebindMyAi();
    return;
  }
  if (command.type == portal::PortalCommandType::GenerateImage) {
    voice_.enqueueImageGeneration(command.prompt);
    return;
  }
  if (command.type == portal::PortalCommandType::ClearLocalChat) {
    voice_.enqueueClearLocalChat();
    return;
  }
  if (command.type == portal::PortalCommandType::RequestFirmwareUpdate) {
    if (firmware_update_owner_) {
      (void)firmware_update_owner_->requestPortalFirmwareUpdate(
          command.request_id);
      refreshState();
    }
    return;
  }
  if (command.type == portal::PortalCommandType::DeleteAlbumItem) {
    if (album_mutation_owner_ &&
        album_mutation_owner_->deletePortalAlbumItem(command.asset_id) ==
            portal::PortalResult::Ok) {
      display_.reloadCatalog();
      refreshAlbum();
    }
    return;
  }
  storage::AlbumIndex index;
  if (!album_store_ || !album_store_->readCatalog(index).ok()) return;
  size_t ordinal = index.assets.size();
  for (size_t at = 0; at < index.assets.size(); ++at) {
    if (index.assets[at].id == command.asset_id) {
      ordinal = at;
      break;
    }
  }
  if (ordinal >= index.assets.size()) return;
  if (command.type == portal::PortalCommandType::DisplayAlbumItem) {
    requestDisplay(ordinal);
  } else if (command.type ==
             portal::PortalCommandType::SetAlbumRenderStrategy) {
    if (boardSupportsRenderStrategy(board_, command.render_strategy) &&
        album_store_->updateRenderStrategy(
            command.asset_id, command.render_strategy).ok()) {
      display_.reloadCatalog();
      refreshAlbum();
    }
  }
}

bool NativePortalOwner::loadSettings() {
  if (!settings_owner_) {
    return boardSupportsRenderStrategy(
        board_, state_cache_.settings.default_render_strategy);
  }
  portal::PortalSettingsSnapshot next;
  if (settings_owner_->readPortalSettings(next) != portal::PortalResult::Ok)
    return false;
  if (!boardSupportsRenderStrategy(board_, next.default_render_strategy)) {
    if (!boardSupportsRenderStrategy(
            board_, std::string(kOfficialQualityRenderStrategy))) {
      return false;
    }
    portal::PortalSettingsPatch fallback;
    fallback.has_default_render_strategy = true;
    fallback.default_render_strategy = kOfficialQualityRenderStrategy;
    portal::PortalSettingsSnapshot corrected;
    if (settings_owner_->applyPortalSettings(fallback, corrected) !=
            portal::PortalResult::Ok ||
        !boardSupportsRenderStrategy(
            board_, corrected.default_render_strategy)) {
      return false;
    }
    next = std::move(corrected);
    ESP_LOGW(kTag,
             "stored render strategy unsupported by selected board; "
             "persisted official-quality fallback");
  }
  state_cache_.settings = std::move(next);
  return true;
}

void NativePortalOwner::applySettings(
    const portal::PortalSettingsPatch& patch) {
  if (!settings_owner_) return;
  portal::PortalSettingsSnapshot next;
  if (settings_owner_->applyPortalSettings(patch, next) !=
      portal::PortalResult::Ok) return;
  if (!boardSupportsRenderStrategy(board_, next.default_render_strategy))
    return;
  if (takeMutex(cache_mutex_)) {
    state_cache_.settings = next;
    giveMutex(cache_mutex_);
  }
  leds_.setMaximumBrightnessPercent(next.led_maximum_brightness_percent);
  voice_.enqueuePersistedVoiceSettings(
      next.volume, next.voice_assistance_enabled, next.assistant_prompt);
}

void NativePortalOwner::refreshState() {
  portal::PortalStateSnapshot next;
  if (takeMutex(cache_mutex_)) {
    next = state_cache_;
    giveMutex(cache_mutex_);
  } else {
    return;
  }
  const NativeMyAiOnboardingSnapshot onboarding = voice_.onboardingSnapshot();
  const RuntimeTelemetrySnapshot runtime = supervisor_.telemetry();
  static_assert(kTaskLaneCount == portal::kPortalRuntimeLaneCount,
                "Portal telemetry lane contract must match RuntimeSupervisor");
  next.runtime = portal::PortalRuntimeTelemetry{};
  next.runtime.available = true;
  next.runtime.lane_count = static_cast<uint8_t>(runtime.lanes.size());
  next.runtime.sequence = runtime.sequence;
  next.runtime.last_managed_update_ms = runtime.last_managed_update_ms;
  next.runtime.internal_heap_min_free_bytes =
      runtime.internal_heap_min_free_bytes;
  next.runtime.psram_min_free_bytes = runtime.psram_min_free_bytes;
  next.runtime.resource_sample_count = runtime.resource_sample_count;
  next.runtime.internal_heap_sampled = runtime.internal_heap_sampled;
  next.runtime.psram_available = runtime.psram_available;
  for (size_t index = 0; index < runtime.lanes.size(); ++index) {
    const RuntimeLaneTelemetry& source = runtime.lanes[index];
    portal::PortalRuntimeLaneTelemetry& target = next.runtime.lanes[index];
    target.queue_capacity = source.queue_capacity;
    target.queue_depth = source.queue_depth;
    target.queue_high_water = source.queue_high_water;
    target.stack_low_water_bytes = source.stack_low_water_bytes;
    target.handler_count = source.handler_count;
    target.handler_max_us = source.handler_max_us;
    target.tick_count = source.tick_count;
    target.tick_max_us = source.tick_max_us;
    target.tick_late_count = source.tick_late_count;
    target.tick_missed = source.tick_missed;
    target.tick_late_max_us = source.tick_late_max_us;
    target.last_progress_ms = source.last_progress_ms;
    target.configured_core = source.configured_core;
    target.observed_core = source.observed_core;
    target.configured_priority = source.configured_priority;
    target.observed_priority = source.observed_priority;
    target.task_running = source.task_running;
    target.stack_sampled = source.stack_sampled;
  }
  next.wifi_online = wifi_.online();
  next.firmware_update = portal::PortalFirmwareUpdateSnapshot{};
  if (firmware_update_owner_) {
    portal::PortalFirmwareUpdateSnapshot update;
    if (firmware_update_owner_->readPortalFirmwareUpdate(update) ==
        portal::PortalResult::Ok) {
      next.firmware_update = update;
    }
  }
  next.storage_ready = false;
  struct statvfs capacity {};
  portENTER_CRITICAL(&activity_mux_);
  const bool storage_admitted = storage_available_;
  portEXIT_CRITICAL(&activity_mux_);
  if (storage_admitted && album_store_ && storage_root_ &&
      ::statvfs(storage_root_, &capacity) == 0 && capacity.f_frsize != 0U &&
      capacity.f_blocks <=
          std::numeric_limits<uint64_t>::max() / capacity.f_frsize &&
      capacity.f_bavail <=
          std::numeric_limits<uint64_t>::max() / capacity.f_frsize) {
    next.storage_ready = true;
    next.storage_total_bytes =
        static_cast<uint64_t>(capacity.f_blocks) * capacity.f_frsize;
    next.storage_free_bytes =
        static_cast<uint64_t>(capacity.f_bavail) * capacity.f_frsize;
  } else {
    next.storage_ready = false;
    next.storage_free_bytes = 0;
    next.storage_total_bytes = 0;
  }
  next.display_busy = display_.busy();
  const NativeDisplayDiagnostics display_diagnostics = display_.diagnostics();
  next.display_completed_refreshes =
      display_diagnostics.completed_album_refreshes;
  next.display_load_decode_ms = display_diagnostics.last_load_decode_ms;
  next.display_conversion_ms = display_diagnostics.last_conversion_ms;
  next.display_panel_refresh_ms =
      display_diagnostics.last_panel_refresh_ms;
  next.display_total_ms = display_diagnostics.last_album_total_ms;
  next.myai_state = portalMyAiState(onboarding.activation_state);
  next.pairing_code = onboarding.device_code.data();
  next.binding_url = onboarding.pairing_view_available
                         ? onboarding.binding_url.data() : std::string();
  if (takeMutex(cache_mutex_)) {
    state_cache_ = std::move(next);
    state_cache_ready_ = true;
    giveMutex(cache_mutex_);
  }
  const uint32_t now = nowMs();
  if (initialized_ &&
      (next_runtime_summary_ms_ == 0U ||
       due(now, next_runtime_summary_ms_))) {
    uint32_t queue_maximum = 0U;
    uint64_t late_total = 0U;
    uint64_t missed_total = 0U;
    std::array<uint8_t, 2> running_by_core{};
    for (const RuntimeLaneTelemetry& lane : runtime.lanes) {
      queue_maximum = std::max(queue_maximum, lane.queue_high_water);
      late_total += lane.tick_late_count;
      missed_total += lane.tick_missed;
      if (lane.task_running && lane.observed_core >= 0 &&
          lane.observed_core < static_cast<int8_t>(running_by_core.size())) {
        ++running_by_core[static_cast<size_t>(lane.observed_core)];
      }
    }
    ESP_LOGI(kTag,
             "runtime seq=%lu q_high=%lu tick_late=%lu tick_missed=%lu "
             "heap_sampled=%u heap_min=%lu psram=%u psram_min=%lu "
             "running_c0=%u running_c1=%u",
             static_cast<unsigned long>(runtime.sequence),
             static_cast<unsigned long>(queue_maximum),
             static_cast<unsigned long>(std::min<uint64_t>(
                 late_total, std::numeric_limits<uint32_t>::max())),
             static_cast<unsigned long>(std::min<uint64_t>(
                 missed_total, std::numeric_limits<uint32_t>::max())),
             static_cast<unsigned>(runtime.internal_heap_sampled),
             static_cast<unsigned long>(runtime.internal_heap_min_free_bytes),
             static_cast<unsigned>(runtime.psram_available),
             static_cast<unsigned long>(runtime.psram_min_free_bytes),
             static_cast<unsigned>(running_by_core[0]),
             static_cast<unsigned>(running_by_core[1]));
    next_runtime_summary_ms_ = now + kRuntimeSummaryMs;
  }
}

void NativePortalOwner::refreshAlbum() {
  portENTER_CRITICAL(&activity_mux_);
  const bool storage_available = storage_available_;
  portEXIT_CRITICAL(&activity_mux_);
  if (!storage_available || !album_store_ || album_store_->active()) return;
  storage::AlbumIndex next;
  if (!album_store_->readCatalog(next).ok()) return;
  if (takeMutex(cache_mutex_)) {
    album_cache_ = std::move(next);
    ++album_revision_;
    album_cache_ready_ = true;
    giveMutex(cache_mutex_);
  }
}

void NativePortalOwner::refreshChat() {
  // The Storage lane is the sole live JSONL owner. It publishes a bounded
  // snapshot through publishLocalChatSnapshot(); this task never scans the
  // file concurrently with an append.
}

esp_err_t NativePortalOwner::startServer() {
  if (server_ && server_->running()) return ESP_OK;
  const EspWifiStationSnapshot wifi = wifi_.snapshot();
  if (wifi.phase != WifiStationPhase::Online || wifi.ipv4[0] == '\0')
    return ESP_ERR_INVALID_STATE;
  portal::PortalAccessConfig access;
  const std::array<char, 64> local_access = wifi_.localAccessCode();
  access.access_code = local_access.data();
  access.session_id = bootToken(16U);
  access.csrf_token = bootToken(16U);
  const std::string ip = wifi.ipv4.data();
  access.allowed_hosts = {"inkloop.local", "inkloop.local:80", ip,
                          ip + ":80"};
  access.allowed_origins = {"http://inkloop.local",
                            "http://inkloop.local:80", "http://" + ip,
                            "http://" + ip + ":80"};
  core_.reset(new (std::nothrow) portal::PortalCore(access, *this, *this));
  if (!core_ || !core_->ready()) return ESP_ERR_NO_MEM;
  portal::EspPortalServerConfig config;
  config.preview_task_priority = 1U;
  server_.reset(new (std::nothrow)
                    portal::EspPortalServer(*core_, *this, *this, config));
  if (!server_) return ESP_ERR_NO_MEM;
  const esp_err_t started = server_->start();
  if (started != ESP_OK) {
    server_.reset();
    core_.reset();
    return started;
  }
  if (!mdns_started_) {
    const esp_err_t mdns = mdns_init();
    if ((mdns == ESP_OK || mdns == ESP_ERR_INVALID_STATE) &&
        mdns_hostname_set("inkloop") == ESP_OK &&
        mdns_instance_name_set(mdns_instance_name_.data()) == ESP_OK &&
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0) ==
            ESP_OK) {
      mdns_started_ = true;
    }
  }
  ESP_LOGI(kTag, "local portal ready at http://inkloop.local/ and http://%s/",
           ip.c_str());
  return ESP_OK;
}

esp_err_t NativePortalOwner::stopServer() {
  if (server_) {
    const esp_err_t stopped = server_->stop();
    if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) return stopped;
  }
  server_.reset();
  core_.reset();
  if (mdns_started_) {
    mdns_service_remove("_http", "_tcp");
    mdns_free();
    mdns_started_ = false;
  }
  return ESP_OK;
}

void NativePortalOwner::tick(bool wifi_online,
                             bool storage_mutation_allowed) {
  if (!initialized_) return;
  if (storageMaintenanceActive()) {
    stopServer();
    return;
  }
  if (!wifi_online) {
    stopServer();
    return;
  }
  if (restart_refresh_required_) {
    if (!storage_mutation_allowed) return;
    refreshState();
    refreshAlbum();
    next_state_refresh_ms_ = nowMs() + kStateRefreshMs;
    next_album_refresh_ms_ = nowMs() + kAlbumRefreshMs;
    restart_refresh_required_ = false;
  }
  if (!running()) {
    const esp_err_t started = startServer();
    if (started != ESP_OK && started != ESP_ERR_INVALID_STATE)
      ESP_LOGW(kTag, "portal start deferred: %s", esp_err_to_name(started));
  }

  const uint32_t now = nowMs();
  const uint32_t accessed = lastAccessMs();
  const bool recently_accessed = accessed != 0U &&
      static_cast<uint32_t>(now - accessed) < kRecentPortalWindowMs;
  if (recently_accessed && due(now, next_state_refresh_ms_)) {
    refreshState();
    next_state_refresh_ms_ = now + kStateRefreshMs;
  }
  if (recently_accessed && storage_mutation_allowed &&
      due(now, next_album_refresh_ms_)) {
    refreshAlbum();
    next_album_refresh_ms_ = now + kAlbumRefreshMs;
  }
  if (recently_accessed && storage_mutation_allowed &&
      due(now, next_chat_refresh_ms_)) {
    refreshChat();
    next_chat_refresh_ms_ = now + kChatRefreshMs;
  }
  if (!storage_mutation_allowed) return;
  for (size_t at = 0; at < 2U; ++at) {
    portal::PortalCommand command;
    if (!takeCommand(command)) break;
    serviceCommand(command);
  }
  for (size_t at = 0; at < 12U; ++at) {
    UploadEvent event;
    if (!takeUploadEvent(event)) break;
    serviceUpload(event);
  }
}

bool NativePortalOwner::mutationBusy() const {
  if (!initialized_) return false;
  if (active_upload_.request_id != 0U ||
      (upload_queue_ && uxQueueMessagesWaiting(upload_queue_) != 0U) ||
      (command_queue_ && uxQueueMessagesWaiting(command_queue_) != 0U)) {
    return true;
  }
  if (!takeMutex(upload_mutex_)) return true;
  const bool accepted = accepted_upload_request_ != 0U;
  giveMutex(upload_mutex_);
  return accepted;
}

bool NativePortalOwner::running() const {
  return server_ && server_->running();
}

portal::PortalResult NativePortalOwner::open(
    const std::string& asset_id, portal::PortalPreviewInfo& output) {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  noteAccess();
  storage::AlbumIndex index;
  if (!album_store_ || album_store_->active() ||
      !album_store_->readCatalog(index).ok()) {
    return portal::PortalResult::Busy;
  }
  const auto found = std::find_if(
      index.assets.begin(), index.assets.end(),
      [&](const storage::AlbumIndexAsset& asset) {
        return asset.id == asset_id;
      });
  if (found == index.assets.end()) return portal::PortalResult::InvalidData;
  std::string path;
  if (!album_store_->absoluteAssetPath(*found, path))
    return portal::PortalResult::InvalidData;
  PreviewHandle* slot = nullptr;
  for (PreviewHandle& candidate : preview_handles_) {
    if (!candidate.file) {
      slot = &candidate;
      break;
    }
  }
  if (!slot) return portal::PortalResult::Busy;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return portal::PortalResult::Unavailable;
  slot->handle = ++preview_sequence_;
  if (slot->handle == 0U) slot->handle = ++preview_sequence_;
  slot->file = file;
  slot->remaining = found->bytes;
  output.handle = slot->handle;
  output.bytes = found->bytes;
  output.content_type = "image/png";
  return portal::PortalResult::Ok;
}

portal::PortalResult NativePortalOwner::read(
    uint64_t handle, uint8_t* output, size_t capacity, size_t& bytes_read) {
  StorageHttpGuard storage_guard(*this);
  if (!storage_guard.active()) return portal::PortalResult::Busy;
  bytes_read = 0;
  if (handle == 0U || !output || capacity == 0U)
    return portal::PortalResult::InvalidRequest;
  for (PreviewHandle& slot : preview_handles_) {
    if (slot.handle != handle || !slot.file) continue;
    const size_t wanted = std::min(capacity, slot.remaining);
    if (wanted == 0U) return portal::PortalResult::Ok;
    bytes_read = std::fread(output, 1U, wanted, slot.file);
    if (bytes_read == 0U || bytes_read > slot.remaining)
      return portal::PortalResult::Unavailable;
    slot.remaining -= bytes_read;
    return portal::PortalResult::Ok;
  }
  return portal::PortalResult::InvalidData;
}

void NativePortalOwner::close(uint64_t handle) {
  StorageHttpGuard storage_guard(*this, true);
  if (!storage_guard.active()) return;
  for (PreviewHandle& slot : preview_handles_) {
    if (slot.handle != handle) continue;
    if (slot.file) std::fclose(slot.file);
    slot = PreviewHandle();
    return;
  }
}

uint64_t NativePortalOwner::nextRequestId() {
  ++request_sequence_;
  if (request_sequence_ == 0U) ++request_sequence_;
  return 0x504f525400000000ULL | request_sequence_;
}

}  // namespace inkloop
