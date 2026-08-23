#include "inkloop/native_voice_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "inkloop/board_prompt_policy.hpp"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/storage/album_index.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-native-voice";
constexpr uint32_t kResponsiveDeadlineMs = 150;
constexpr uint32_t kNetworkDeadlineMs = 1000;
constexpr uint32_t kPairingPollMs = 2000;
constexpr uint32_t kPairingStartRetryMs = 30000;
constexpr uint32_t kAuthorizationRefreshMs = 10U * 60U * 1000U;
constexpr uint32_t kHeartbeatMs = 30000;
constexpr uint32_t kMinimumReconnectMs = 1000;
constexpr uint32_t kAigcPollMs = 5000;
constexpr uint32_t kAigcPromptMaximum = 1024;
constexpr uint32_t kLocalToolDeadlineMs = 5000;

bool terminalImageSuccess(const std::string& status) {
  return status == "completed" || status == "complete" ||
      status == "succeeded";
}

std::string safeAigcFailureState(const myai::Status& status) {
  std::string output = "aigc.failed code=" +
      std::to_string(static_cast<unsigned>(status.code));
  if (status.httpStatus != 0) {
    output += " http=" + std::to_string(status.httpStatus);
  }

  // MyAiClient normally emits fixed, credential-free diagnostics. A gateway
  // status message is less trusted, however, so retain it only when it is
  // bounded, printable and cannot carry an auth secret or provider-local URL.
  if (status.detail.empty() || status.detail.size() > 192U) return output;
  std::string lowercase_detail = status.detail;
  std::transform(lowercase_detail.begin(), lowercase_detail.end(),
                 lowercase_detail.begin(), [](unsigned char ch) {
                   return static_cast<char>(
                       ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
                 });
  static const char* const sensitive[] = {
      "token", "authorization", "bearer", "http://", "https://", "ws://",
      "wss://", "provider_url", "local_url"};
  for (const char* marker : sensitive) {
    if (lowercase_detail.find(marker) != std::string::npos) return output;
  }
  for (unsigned char ch : status.detail) {
    if (ch < 0x20U || ch == 0x7fU) return output;
  }
  output += " detail=" + status.detail;
  return output;
}

bool sixDigits(const std::string& value) {
  if (value.size() != 6U) return false;
  for (char ch : value)
    if (ch < '0' || ch > '9') return false;
  return true;
}

size_t boundedUtf8Prefix(const std::string& text, size_t maximum) {
  if (text.size() <= maximum) return text.size();
  size_t end = maximum;
  while (end > 0U &&
         (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) {
    --end;
  }
  return end;
}

NativeLocalChatRole nativeChatRole(storage::ChatRecordKind kind) {
  switch (kind) {
    case storage::ChatRecordKind::AsrFinal:
      return NativeLocalChatRole::User;
    case storage::ChatRecordKind::AssistantFinal:
      return NativeLocalChatRole::Assistant;
    case storage::ChatRecordKind::ToolState:
    case storage::ChatRecordKind::AigcState:
      return NativeLocalChatRole::Tool;
  }
  return NativeLocalChatRole::Tool;
}

template <size_t Size>
bool copyBounded(const std::string& value, std::array<char, Size>& output) {
  static_assert(Size > 1U, "bounded text requires a terminator");
  output.fill('\0');
  if (value.empty() || value.size() >= Size) return false;
  std::memcpy(output.data(), value.data(), value.size());
  return true;
}

std::string composeImagePrompt(const BoardDescriptor& board,
                               const std::string& subject,
                               const std::string& configured_template) {
  if (subject.empty() || subject.size() > kAigcPromptMaximum)
    return std::string();
  const std::string prompt_template =
      configured_template.empty() ? defaultImagePromptTemplate(board)
                                  : configured_template;
  if (!local_tools::LocalCommandParser::validStoredPrompt(prompt_template))
    return std::string();
  for (const char* marker : {"{{prompt}}", "{prompt}", "{{subject}}",
                             "{subject}"}) {
    const size_t at = prompt_template.find(marker);
    if (at == std::string::npos) continue;
    const size_t marker_size = std::strlen(marker);
    if (prompt_template.size() - marker_size >
        kAigcPromptMaximum - subject.size()) {
      return std::string();
    }
    std::string output = prompt_template;
    output.replace(at, marker_size, subject);
    return output;
  }
  if (prompt_template.size() + 1U >
      kAigcPromptMaximum - subject.size())
    return std::string();
  return prompt_template + " " + subject;
}

diagnostics::SerialDiagnosticVoiceState serialVoiceState(
    myai::VoiceState state) {
  using diagnostics::SerialDiagnosticVoiceState;
  switch (state) {
    case myai::VoiceState::Idle:
      return SerialDiagnosticVoiceState::Idle;
    case myai::VoiceState::Connecting:
      return SerialDiagnosticVoiceState::Connecting;
    case myai::VoiceState::Ready:
      return SerialDiagnosticVoiceState::Ready;
    case myai::VoiceState::Listening:
      return SerialDiagnosticVoiceState::Listening;
    case myai::VoiceState::Thinking:
      return SerialDiagnosticVoiceState::Thinking;
    case myai::VoiceState::Speaking:
      return SerialDiagnosticVoiceState::Speaking;
    case myai::VoiceState::Error:
      return SerialDiagnosticVoiceState::Error;
  }
  return SerialDiagnosticVoiceState::Error;
}

}  // namespace

NativeVoiceService::NativeVoiceService(IBoardAdapter& board,
                                       RuntimeSupervisor& supervisor,
                                       const char* storage_root,
                                       storage::IAlbumStagingStore* album_store,
                                       diagnostics::ISerialDiagnosticEventSink*
                                           serial_diagnostics)
    : board_(board), supervisor_(supervisor), storage_root_(storage_root),
      album_store_(album_store), serial_diagnostics_(serial_diagnostics),
      wire_codec_(board.descriptor().id ? board.descriptor().id
                                        : "inkloop-device") {}

myai::Status NativeVoiceService::DisabledAudioSink::begin(uint32_t, uint8_t) {
  return myai::Status(myai::ErrorCode::InvalidState, 0,
                      "selected board has no duplex audio");
}

myai::Status NativeVoiceService::DisabledAudioSink::write(
    const uint8_t*, size_t) {
  return myai::Status(myai::ErrorCode::InvalidState, 0,
                      "selected board has no duplex audio");
}

myai::Status NativeVoiceService::DisabledAudioSink::end() {
  return myai::Status(myai::ErrorCode::InvalidState, 0,
                      "selected board has no duplex audio");
}

bool NativeVoiceService::EspConfirmationTokens::issue(std::string& token) {
  std::array<uint32_t, 4> random{};
  for (uint32_t& word : random) word = esp_random();
  char encoded[33]{};
  const int written = std::snprintf(
      encoded, sizeof(encoded), "%08lx%08lx%08lx%08lx",
      static_cast<unsigned long>(random[0]),
      static_cast<unsigned long>(random[1]),
      static_cast<unsigned long>(random[2]),
      static_cast<unsigned long>(random[3]));
  if (written != 32) {
    token.clear();
    return false;
  }
  token.assign(encoded, 32U);
  std::fill(random.begin(), random.end(), 0U);
  std::fill(encoded, encoded + sizeof(encoded), '\0');
  return true;
}

uint32_t NativeVoiceService::nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool NativeVoiceService::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void NativeVoiceService::emitSerialDiagnostic(
    const diagnostics::SerialDiagnosticEvent& event) const {
  if (serial_diagnostics_)
    (void)serial_diagnostics_->postSerialDiagnosticEvent(event);
}

void NativeVoiceService::emitSerialAigcPhase(
    diagnostics::SerialDiagnosticAigcPhase phase) const {
  diagnostics::SerialDiagnosticEvent event;
  event.kind = diagnostics::SerialDiagnosticEventKind::AigcPhase;
  event.code = static_cast<uint8_t>(phase);
  emitSerialDiagnostic(event);
}

uint64_t NativeVoiceService::nextRequestId() {
  portENTER_CRITICAL(&sequence_mux_);
  do {
    ++sequence_;
  } while (sequence_ == 0);
  const uint64_t value = sequence_;
  portEXIT_CRITICAL(&sequence_mux_);
  return value;
}

bool NativeVoiceService::maintenanceBlocksInteractiveWork() const {
  portENTER_CRITICAL(&maintenance_mux_);
  const bool blocked = storage_maintenance_active_;
  portEXIT_CRITICAL(&maintenance_mux_);
  return blocked;
}

bool NativeVoiceService::beginTrackedStorageWork() {
  portENTER_CRITICAL(&maintenance_mux_);
  if (storage_maintenance_active_ || !storage_available_ ||
      tracked_storage_work_ == std::numeric_limits<uint32_t>::max()) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  ++tracked_storage_work_;
  portEXIT_CRITICAL(&maintenance_mux_);
  return true;
}

void NativeVoiceService::finishTrackedStorageWork() {
  portENTER_CRITICAL(&maintenance_mux_);
  if (tracked_storage_work_ != 0U) --tracked_storage_work_;
  portEXIT_CRITICAL(&maintenance_mux_);
}

void NativeVoiceService::noteVoiceTurnActive(bool active) {
  portENTER_CRITICAL(&maintenance_mux_);
  voice_turn_active_ = active;
  portEXIT_CRITICAL(&maintenance_mux_);
}

bool NativeVoiceService::voiceTurnActive() const {
  portENTER_CRITICAL(&maintenance_mux_);
  const bool active = voice_turn_active_;
  portEXIT_CRITICAL(&maintenance_mux_);
  return active;
}

void NativeVoiceService::noteLocalAudioActive(bool active) {
  portENTER_CRITICAL(&maintenance_mux_);
  local_audio_active_ = active;
  portEXIT_CRITICAL(&maintenance_mux_);
}

bool NativeVoiceService::beginStorageMaintenance() {
  portENTER_CRITICAL(&maintenance_mux_);
  if (storage_maintenance_active_) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return true;
  }
  storage_maintenance_active_ = true;
  storage_reset_required_ = false;
  storage_reset_pending_ = false;
  storage_reset_complete_ = false;
  const bool local_busy = tracked_storage_work_ != 0U ||
      voice_turn_active_ || local_audio_active_;
  if (local_busy) {
    storage_maintenance_active_ = false;
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  // Keep the admission gate locked while inspecting AIGC; acceptAigcPrompt()
  // takes locks in this same order, closing the check/admit race.
  portENTER_CRITICAL(&aigc_mux_);
  const bool aigc_busy = aigc_admission_pending_ ||
      aigc_phase_ != AigcPhase::Idle || aigc_exclusive_;
  portEXIT_CRITICAL(&aigc_mux_);
  if (aigc_busy || audio_bridge_->captureBusy() ||
      audio_bridge_->playbackBusy()) {
    storage_maintenance_active_ = false;
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  portEXIT_CRITICAL(&maintenance_mux_);
  return true;
}

bool NativeVoiceService::finishStorageMaintenance(bool storage_changed,
                                                   bool storage_available) {
  portENTER_CRITICAL(&maintenance_mux_);
  if (!storage_maintenance_active_) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  if (!storage_changed) {
    storage_reset_required_ = false;
    storage_reset_complete_ = true;
    portEXIT_CRITICAL(&maintenance_mux_);
    return true;
  }
  if (!storage_available) {
    if (storage_reset_pending_) {
      portEXIT_CRITICAL(&maintenance_mux_);
      return false;
    }
    portEXIT_CRITICAL(&maintenance_mux_);
    if (!chat_snapshot_mutex_ ||
        xSemaphoreTake(chat_snapshot_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
      return false;
    }
    chat_snapshot_mailbox_ = NativeLocalChatSnapshot{};
    portENTER_CRITICAL(&chat_snapshot_state_mux_);
    chat_snapshot_request_pending_ = false;
    chat_snapshot_ready_ = true;
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
    xSemaphoreGive(chat_snapshot_mutex_);
    portENTER_CRITICAL(&maintenance_mux_);
    if (!storage_maintenance_active_) {
      portEXIT_CRITICAL(&maintenance_mux_);
      return false;
    }
    storage_available_ = false;
    storage_reset_required_ = false;
    storage_reset_complete_ = true;
    portEXIT_CRITICAL(&maintenance_mux_);
    return true;
  }
  storage_reset_required_ = true;
  storage_available_ = false;
  if (storage_reset_complete_) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return true;
  }
  if (storage_reset_pending_) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  storage_reset_pending_ = true;
  portEXIT_CRITICAL(&maintenance_mux_);

  const AdmissionResult admitted = post(
      WorkClass::Storage,
      ProductOpcode::StorageRecoverLocalChatAfterFormat, 0, 0);
  if (admitted != AdmissionResult::Admitted) {
    portENTER_CRITICAL(&maintenance_mux_);
    storage_reset_pending_ = false;
    portEXIT_CRITICAL(&maintenance_mux_);
  }
  return false;
}

void NativeVoiceService::endStorageMaintenance() {
  portENTER_CRITICAL(&maintenance_mux_);
  if (storage_reset_pending_ ||
      (storage_reset_required_ && !storage_reset_complete_)) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return;
  }
  storage_maintenance_active_ = false;
  storage_reset_required_ = false;
  storage_reset_complete_ = false;
  portEXIT_CRITICAL(&maintenance_mux_);
}

bool NativeVoiceService::storageMaintenanceActive() const {
  return maintenanceBlocksInteractiveWork();
}

myai::ClientConfig NativeVoiceService::makeConfig() const {
  uint8_t mac[6]{};
  myai::ClientConfig config;
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return config;
  const std::string fingerprint_prefix =
      myAiInstallationFingerprintPrefix(board_.descriptor());
  char fingerprint[128]{};
  std::snprintf(fingerprint, sizeof(fingerprint),
                "%s-%02x%02x%02x%02x%02x%02x", fingerprint_prefix.c_str(),
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  char wire_mac[18]{};
  // Preserve the public Center record's established eFuse display order.
  std::snprintf(wire_mac, sizeof(wire_mac),
                "%02X:%02X:%02X:%02X:%02X:%02X", mac[5], mac[4], mac[3],
                mac[2], mac[1], mac[0]);
  config.installationFingerprint = fingerprint;
  config.macAddress = wire_mac;
  config.deviceLabel = myAiDeviceLabel(board_.descriptor());
  config.clientRegion = "cn";
  config.systemPrompt = defaultAssistantPrompt(board_.descriptor());
  return config;
}

esp_err_t NativeVoiceService::initialize() {
  if (initialized_ || !storage_root_ || storage_root_[0] != '/')
    return ESP_ERR_INVALID_STATE;
  const BoardDescriptor& descriptor = board_.descriptor();
  voice_hardware_available_ =
      descriptor.has_microphone && descriptor.has_speaker;
  IAudioCodecControl* codec = nullptr;
  esp_err_t status = ESP_OK;
  audio_bridge_.reset(new (std::nothrow) EspCrossCoreAudioBridge());
  if (!audio_bridge_) return ESP_ERR_NO_MEM;
  if (voice_hardware_available_) {
    codec = board_.audioCodec();
    if (!codec) return ESP_ERR_NOT_SUPPORTED;
    status = audio_bridge_->initialize();
    if (status != ESP_OK) return status;
  }
  chat_snapshot_mutex_ =
      xSemaphoreCreateMutexStatic(&chat_snapshot_mutex_storage_);
  if (!chat_snapshot_mutex_) return ESP_ERR_NO_MEM;
  if (voice_hardware_available_) {
    audio_device_.reset(
        new (std::nothrow) EspI2sAudioDevice(board_.audioConfig(), *codec));
    if (!audio_device_) return ESP_ERR_NO_MEM;
  }

  local_tools::ILocalToolsAdapter* settings = nullptr;
  portENTER_CRITICAL(&local_tools_mux_);
  settings = local_tools_adapter_;
  portEXIT_CRITICAL(&local_tools_mux_);
  uint8_t saved_volume = 0;
  if (settings && settings->queryVolume(saved_volume).ok() &&
      saved_volume <= 100U) {
    saved_volume_percent_ = saved_volume;
    hardware_volume_percent_ = saved_volume;
    if (audio_device_) audio_device_->setVolumePercent(saved_volume);
  }

  const std::string directory = std::string(storage_root_) + "/inkloop";
  if (::mkdir(directory.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0 &&
      errno != EEXIST) {
    return ESP_FAIL;
  }
  chat_store_.reset(new (std::nothrow) storage::PosixChatLineStore(
      directory + "/myai-chat.txt",
      directory + "/myai-chat.prev.txt"));
  if (!chat_store_ || !chat_store_->pathsValid()) return ESP_ERR_NO_MEM;
  chat_log_.reset(new (std::nothrow) storage::LocalChatLog(*chat_store_));
  if (!chat_log_) return ESP_ERR_NO_MEM;
  storage::ChatRecovery recovery;
  const storage::ChatLogResult recovered = chat_log_->recover(recovery);
  if (!recovered.ok()) return ESP_FAIL;

  myai::ClientConfig config = makeConfig();
  std::string saved_system_prompt;
  if (settings && settings->queryAssistantPrompt(saved_system_prompt).ok() &&
      !saved_system_prompt.empty() &&
      local_tools::LocalCommandParser::validStoredPrompt(saved_system_prompt)) {
    config.systemPrompt = saved_system_prompt;
  }
  if (config.installationFingerprint.empty() || config.macAddress.empty())
    return ESP_FAIL;
  myai::IAudioSink& audio_sink = voice_hardware_available_
      ? static_cast<myai::IAudioSink&>(*audio_bridge_)
      : static_cast<myai::IAudioSink&>(disabled_audio_sink_);
  client_.reset(new (std::nothrow) myai::MyAiClient(
      config, http_, gateway_probes_, wss_, aigc_output_, endpoint_security_,
      credentials_, wire_codec_, clock_, audio_sink, *this, *this));
  if (!client_) return ESP_ERR_NO_MEM;
  if (!album_store_) return ESP_ERR_NOT_FOUND;
  if (voice_hardware_available_) {
    wss_.setIngressReadyGate(&EspCrossCoreAudioBridge::ingressReady,
                             audio_bridge_.get());
  }
  initialized_ = true;
  return ESP_OK;
}

void NativeVoiceService::shutdown() {
  if (audio_device_) {
    if (local_prompts_.busy()) local_prompts_.cancel(*audio_device_);
    audio_device_->abort();
  }
  if (audio_bridge_) audio_bridge_->abort();
  client_.reset();
  wss_.close(1001U, "runtime_shutdown");
  wss_.setIngressReadyGate(nullptr, nullptr);
  audio_device_.reset();
  audio_bridge_.reset();
  if (album_store_) album_store_->abort();
  chat_log_.reset();
  chat_store_.reset();
  if (chat_snapshot_mutex_) {
    vSemaphoreDelete(chat_snapshot_mutex_);
    chat_snapshot_mutex_ = nullptr;
  }
  local_prompts_ = LocalPromptPlayer();
  text_pool_.clear();
  local_tools_session_.cancelConfirmation();
  std::fill(local_confirmation_token_.begin(),
            local_confirmation_token_.end(), '\0');
  local_confirmation_token_.clear();
  std::fill(assistant_text_.begin(), assistant_text_.end(), '\0');
  assistant_text_.clear();
  std::fill(aigc_prompt_.begin(), aigc_prompt_.end(), '\0');
  aigc_prompt_.clear();
  std::fill(pending_system_prompt_.begin(), pending_system_prompt_.end(),
            '\0');

  portENTER_CRITICAL(&sequence_mux_);
  sequence_ = 0U;
  portEXIT_CRITICAL(&sequence_mux_);
  portENTER_CRITICAL(&aigc_mux_);
  aigc_admission_pending_ = false;
  aigc_admission_ticket_ = 0U;
  aigc_phase_ = AigcPhase::Idle;
  aigc_request_ = myai::ImageRequest{};
  aigc_generated_ = myai::AigcGenerateResponse{};
  aigc_status_ = myai::AigcStatusResponse{};
  aigc_exclusive_ = false;
  aigc_serial_diagnostic_ = false;
  portEXIT_CRITICAL(&aigc_mux_);
  portENTER_CRITICAL(&onboarding_mux_);
  onboarding_ = NativeMyAiOnboardingSnapshot{};
  portEXIT_CRITICAL(&onboarding_mux_);
  portENTER_CRITICAL(&chat_snapshot_state_mux_);
  chat_snapshot_mailbox_ = NativeLocalChatSnapshot{};
  chat_snapshot_request_pending_ = false;
  chat_snapshot_ready_ = false;
  portEXIT_CRITICAL(&chat_snapshot_state_mux_);
  portENTER_CRITICAL(&local_tools_mux_);
  local_confirmation_pending_ = false;
  local_confirmation_format_pending_ = false;
  portEXIT_CRITICAL(&local_tools_mux_);
  portENTER_CRITICAL(&maintenance_mux_);
  tracked_storage_work_ = 0U;
  storage_maintenance_active_ = false;
  voice_turn_active_ = false;
  local_audio_active_ = false;
  storage_available_ = true;
  storage_reset_required_ = false;
  storage_reset_pending_ = false;
  storage_reset_complete_ = false;
  portEXIT_CRITICAL(&maintenance_mux_);

  next_pairing_poll_ms_ = 0U;
  next_pairing_start_ms_ = 0U;
  next_authorization_check_ms_ = 0U;
  next_voice_reconnect_ms_ = 0U;
  last_heartbeat_ms_ = 0U;
  next_aigc_poll_ms_ = 0U;
  activation_state_ = myai::ActivationState::Unconfigured;
  network_voice_state_ = myai::VoiceState::Idle;
  voice_task_state_ = myai::VoiceState::Idle;
  serial_voice_state_.store(static_cast<uint8_t>(myai::VoiceState::Idle),
                            std::memory_order_release);
  client_initialized_ = false;
  authorization_verified_ = false;
  voice_begin_pending_ = false;
  assistant_finalized_ = false;
  ready_deferred_for_audio_ = false;
  reconnect_cleanup_pending_ = false;
  heartbeat_audio_deferred_ = false;
  wifi_was_online_ = false;
  system_prompt_pending_ = false;
  volume_preview_active_ = false;
  pairing_start_requested_ = false;
  rebind_requested_ = false;
  voice_hardware_available_ = false;
  initialized_ = false;
}

esp_err_t NativeVoiceService::attachLocalTools(
    local_tools::ILocalToolsAdapter& adapter) {
  if (initialized_ || supervisor_.started()) return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&local_tools_mux_);
  local_tools_adapter_ = &adapter;
  local_confirmation_pending_ = false;
  local_confirmation_format_pending_ = false;
  portEXIT_CRITICAL(&local_tools_mux_);
  local_tools_session_.cancelConfirmation();
  std::fill(local_confirmation_token_.begin(),
            local_confirmation_token_.end(), '\0');
  local_confirmation_token_.clear();
  return ESP_OK;
}

esp_err_t NativeVoiceService::configureHandlers() {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = supervisor_.registerHandler(
      TaskLane::Voice, &NativeVoiceService::voiceHandler, this);
  if (status == ESP_OK) {
    status = supervisor_.registerTickHandler(
        TaskLane::Voice, &NativeVoiceService::voiceTick, this, 5);
  }
  if (status == ESP_OK) {
    status = supervisor_.registerHandler(
        TaskLane::Storage, &NativeVoiceService::storageHandler, this);
  }
  return status;
}

AdmissionResult NativeVoiceService::post(WorkClass work_class,
                                         ProductOpcode opcode, uint8_t flags,
                                         uint32_t deadline_ms) {
  WorkEnvelope envelope{};
  envelope.generation = 1;
  envelope.request_id = nextRequestId();
  envelope.deadline_ms = deadline_ms == 0 ? 0 : nowMs() + deadline_ms;
  envelope.opcode = productOpcode(opcode);
  envelope.work_class = work_class;
  envelope.kind = EnvelopeKind::Command;
  envelope.disposition = WorkDisposition::Accepted;
  envelope.flags = flags;
  return supervisor_.post(envelope);
}

AdmissionResult NativeVoiceService::enqueueTopButton() {
  if (!voice_hardware_available_) return AdmissionResult::NotReady;
  if (maintenanceBlocksInteractiveWork()) return AdmissionResult::QueueFull;
  portENTER_CRITICAL(&local_tools_mux_);
  const bool confirm_local_tool = local_confirmation_pending_;
  const bool confirm_format = local_confirmation_format_pending_;
  portEXIT_CRITICAL(&local_tools_mux_);
  if (confirm_local_tool) {
    if (!confirm_format && !beginTrackedStorageWork())
      return AdmissionResult::QueueFull;
    const AdmissionResult admitted = post(
        WorkClass::Portal, ProductOpcode::PortalConfirmLocalTool,
        confirm_format ? 0U : 1U,
        kLocalToolDeadlineMs);
    if (!confirm_format && admitted != AdmissionResult::Admitted)
      finishTrackedStorageWork();
    return admitted;
  }
  return post(WorkClass::Voice, ProductOpcode::VoiceTopButton, 0,
              kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueAlbumOrdinal(
    size_t one_based_ordinal, bool refresh_start) {
  if (!voice_hardware_available_) return AdmissionResult::NotReady;
  if (maintenanceBlocksInteractiveWork()) return AdmissionResult::QueueFull;
  if (one_based_ordinal == 0 || one_based_ordinal > 99U)
    return AdmissionResult::InvalidEnvelope;
  return post(WorkClass::Voice,
              refresh_start ? ProductOpcode::VoicePromptRefreshOrdinal
                            : ProductOpcode::VoicePromptOrdinal,
              static_cast<uint8_t>(one_based_ordinal), kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueLocalPrompt(LocalPrompt prompt) {
  if (!voice_hardware_available_) return AdmissionResult::NotReady;
  if (maintenanceBlocksInteractiveWork()) return AdmissionResult::QueueFull;
  ProductOpcode opcode = ProductOpcode::None;
  switch (prompt) {
    case LocalPrompt::PleaseWait:
      opcode = ProductOpcode::VoicePromptPleaseWait;
      break;
    case LocalPrompt::AlbumEmpty:
      opcode = ProductOpcode::VoicePromptAlbumEmpty;
      break;
    case LocalPrompt::DeviceRestored:
      opcode = ProductOpcode::VoicePromptDeviceRestored;
      break;
  }
  return post(WorkClass::Voice, opcode, 0, kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueVolumePreview(
    uint8_t temporary_percent) {
  if (!voice_hardware_available_) return AdmissionResult::NotReady;
  if (!initialized_ || temporary_percent > 100U)
    return AdmissionResult::InvalidEnvelope;
  if (maintenanceBlocksInteractiveWork()) return AdmissionResult::QueueFull;
  return post(WorkClass::Voice, ProductOpcode::VoicePreviewVolume,
              temporary_percent, kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueStartMyAiPairing() {
  if (!initialized_) return AdmissionResult::NotReady;
  return post(WorkClass::MyAiNetwork,
              ProductOpcode::NetworkStartMyAiPairing, 0,
              kNetworkDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueRebindMyAi() {
  if (!initialized_) return AdmissionResult::NotReady;
  return post(WorkClass::MyAiNetwork, ProductOpcode::NetworkRebindMyAi, 0,
              kNetworkDeadlineMs);
}

AdmissionResult NativeVoiceService::enqueueImageGeneration(
    const std::string& prompt) {
  return enqueueImageGenerationImpl(prompt, false);
}

AdmissionResult NativeVoiceService::enqueueDiagnosticImageGeneration(
    const std::string& prompt) {
  return enqueueImageGenerationImpl(prompt, true);
}

AdmissionResult NativeVoiceService::enqueueImageGenerationImpl(
    const std::string& prompt, bool serial_diagnostic) {
  if (!initialized_ || prompt.empty() ||
      prompt.size() > kAigcPromptMaximum) {
    return AdmissionResult::InvalidEnvelope;
  }
  const uint64_t ticket = text_pool_.put(ProductTextKind::AigcState, prompt);
  if (ticket == 0U) return AdmissionResult::QueueFull;
  // Reserve the AIGC slot before posting to Network. The consumer can run as
  // soon as post() returns, and power admission must see accepted queued work
  // even before Network turns it into PendingHandoff. Keep the established
  // maintenance -> AIGC lock order so storage maintenance cannot cross it.
  portENTER_CRITICAL(&maintenance_mux_);
  portENTER_CRITICAL(&aigc_mux_);
  const bool available = !storage_maintenance_active_ && storage_available_ &&
      !aigc_admission_pending_ && aigc_phase_ == AigcPhase::Idle &&
      !aigc_exclusive_;
  if (available) {
    aigc_admission_pending_ = true;
    aigc_admission_ticket_ = ticket;
  }
  portEXIT_CRITICAL(&aigc_mux_);
  portEXIT_CRITICAL(&maintenance_mux_);
  if (!available) {
    text_pool_.release(ticket);
    return AdmissionResult::QueueFull;
  }
  WorkEnvelope envelope{};
  envelope.generation = 1;
  envelope.request_id = ticket;
  envelope.deadline_ms = nowMs() + kNetworkDeadlineMs;
  envelope.opcode = productOpcode(ProductOpcode::NetworkQueueAigc);
  envelope.work_class = WorkClass::MyAiNetwork;
  envelope.kind = EnvelopeKind::Command;
  envelope.disposition = WorkDisposition::Accepted;
  envelope.flags = serial_diagnostic ? 1U : 0U;
  const AdmissionResult admitted = supervisor_.post(envelope);
  if (admitted != AdmissionResult::Admitted) {
    text_pool_.release(ticket);
    cancelQueuedAigcAdmission(ticket);
  }
  return admitted;
}

AdmissionResult NativeVoiceService::enqueueClearLocalChat() {
  if (!initialized_) return AdmissionResult::NotReady;
  if (!beginTrackedStorageWork()) return AdmissionResult::QueueFull;
  const AdmissionResult admitted = post(
      WorkClass::Storage, ProductOpcode::StorageClearLocalChat, 0,
      kLocalToolDeadlineMs);
  if (admitted != AdmissionResult::Admitted) finishTrackedStorageWork();
  return admitted;
}

esp_err_t NativeVoiceService::seedPersistedVoiceSettings(
    uint8_t volume_percent, bool voice_assistance_enabled,
    const std::string& assistant_prompt) {
  if (!initialized_ || supervisor_.started() || volume_percent > 100U ||
      !local_tools::LocalCommandParser::validStoredPrompt(assistant_prompt)) {
    return ESP_ERR_INVALID_STATE;
  }
  portENTER_CRITICAL(&settings_mux_);
  saved_volume_percent_ = volume_percent;
  hardware_volume_percent_ = volume_percent;
  voice_assistance_enabled_ = voice_assistance_enabled;
  portEXIT_CRITICAL(&settings_mux_);
  if (audio_device_) audio_device_->setVolumePercent(volume_percent);
  stageSystemPrompt(assistant_prompt);
  return ESP_OK;
}

AdmissionResult NativeVoiceService::enqueuePersistedVoiceSettings(
    uint8_t volume_percent, bool voice_assistance_enabled,
    const std::string& assistant_prompt) {
  if (!initialized_ || volume_percent > 100U ||
      !local_tools::LocalCommandParser::validStoredPrompt(assistant_prompt)) {
    return AdmissionResult::InvalidEnvelope;
  }

  // Publish the persisted target first. The Voice tick reconciles volume even
  // if its bounded queue is transiently full; no HTTP or Portal task touches
  // I2S. The explicit opcodes keep task/result accounting observable.
  portENTER_CRITICAL(&settings_mux_);
  saved_volume_percent_ = volume_percent;
  voice_assistance_enabled_ = voice_assistance_enabled;
  portEXIT_CRITICAL(&settings_mux_);
  stageSystemPrompt(assistant_prompt);

  AdmissionResult result = post(WorkClass::Voice,
                                ProductOpcode::VoiceApplyVolume,
                                volume_percent, kResponsiveDeadlineMs);
  const AdmissionResult assistance = post(
      WorkClass::Voice, ProductOpcode::VoiceApplyAssistance,
      voice_assistance_enabled ? 1U : 0U, kResponsiveDeadlineMs);
  if (result == AdmissionResult::Admitted) result = assistance;
  const AdmissionResult prompt = post(
      WorkClass::MyAiNetwork, ProductOpcode::NetworkApplySystemPrompt, 0,
      kNetworkDeadlineMs);
  if (result == AdmissionResult::Admitted) result = prompt;
  return result;
}

AdmissionResult NativeVoiceService::requestLocalChatSnapshot(
    uint64_t after_sequence, uint8_t limit) {
  if (!initialized_ ||
      after_sequence == std::numeric_limits<uint64_t>::max() ||
      limit == 0U ||
      limit > kNativeLocalChatPageItems) {
    return AdmissionResult::InvalidEnvelope;
  }
  if (!beginTrackedStorageWork()) return AdmissionResult::QueueFull;
  portENTER_CRITICAL(&chat_snapshot_state_mux_);
  if (chat_snapshot_request_pending_ || chat_snapshot_ready_) {
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
    finishTrackedStorageWork();
    return AdmissionResult::QueueFull;
  }
  chat_snapshot_request_pending_ = true;
  portEXIT_CRITICAL(&chat_snapshot_state_mux_);

  WorkEnvelope envelope{};
  envelope.generation = 1;
  envelope.request_id = after_sequence + 1U;
  envelope.opcode = productOpcode(ProductOpcode::StorageReadLocalChat);
  envelope.work_class = WorkClass::Storage;
  envelope.kind = EnvelopeKind::Command;
  envelope.disposition = WorkDisposition::Accepted;
  envelope.flags = limit;
  const AdmissionResult admitted = supervisor_.post(envelope);
  if (admitted != AdmissionResult::Admitted) {
    portENTER_CRITICAL(&chat_snapshot_state_mux_);
    chat_snapshot_request_pending_ = false;
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
    finishTrackedStorageWork();
  }
  return admitted;
}

bool NativeVoiceService::tryConsumeLocalChatSnapshot(
    INativeLocalChatSnapshotConsumer& consumer) {
  if (!chat_snapshot_mutex_) return false;
  portENTER_CRITICAL(&chat_snapshot_state_mux_);
  const bool ready = chat_snapshot_ready_;
  portEXIT_CRITICAL(&chat_snapshot_state_mux_);
  if (!ready || xSemaphoreTake(chat_snapshot_mutex_, 0) != pdTRUE) return false;
  portENTER_CRITICAL(&chat_snapshot_state_mux_);
  const bool still_ready = chat_snapshot_ready_;
  portEXIT_CRITICAL(&chat_snapshot_state_mux_);
  bool consumed = false;
  if (still_ready) consumed = consumer.accept(chat_snapshot_mailbox_);
  if (consumed) {
    portENTER_CRITICAL(&chat_snapshot_state_mux_);
    chat_snapshot_ready_ = false;
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
  }
  xSemaphoreGive(chat_snapshot_mutex_);
  return consumed;
}

AdmissionResult NativeVoiceService::postVoiceState(myai::VoiceState state) {
  return post(WorkClass::Voice, ProductOpcode::VoiceStateChanged,
              static_cast<uint8_t>(state), kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::postVoiceLed(VoiceLedMode mode) {
  return post(WorkClass::LedStatus, ProductOpcode::SetVoiceLed,
              static_cast<uint8_t>(mode), kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::postImageLed(ImageLedMode mode) {
  return post(WorkClass::LedStatus, ProductOpcode::SetImageLed,
              static_cast<uint8_t>(mode), kResponsiveDeadlineMs);
}

AdmissionResult NativeVoiceService::postLedMaximumBrightness(uint8_t percent) {
  if (percent == 0U || percent > 100U)
    return AdmissionResult::InvalidEnvelope;
  return post(WorkClass::LedStatus,
              ProductOpcode::SetLedMaximumBrightness, percent,
              kResponsiveDeadlineMs);
}

WorkDisposition NativeVoiceService::voiceHandler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<NativeVoiceService*>(context)->handleVoice(envelope)
             : WorkDisposition::Failed;
}

WorkDisposition NativeVoiceService::storageHandler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<NativeVoiceService*>(context)->handleStorage(envelope)
             : WorkDisposition::Failed;
}

void NativeVoiceService::voiceTick(void* context) {
  if (context) static_cast<NativeVoiceService*>(context)->serviceVoice();
}

WorkDisposition NativeVoiceService::handleVoice(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Voice)
    return WorkDisposition::Failed;
  if (envelope.opcode == productOpcode(ProductOpcode::VoiceTopButton))
    return handleTopButton();
  if (envelope.opcode == productOpcode(ProductOpcode::VoicePromptOrdinal) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::VoicePromptRefreshOrdinal) ||
      envelope.opcode == productOpcode(ProductOpcode::VoicePromptPleaseWait) ||
      envelope.opcode == productOpcode(ProductOpcode::VoicePromptAlbumEmpty) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::VoicePromptDeviceRestored)) {
    return handleLocalPrompt(envelope);
  }
  if (envelope.opcode == productOpcode(ProductOpcode::VoiceStartCapture))
    return startCapture();
  if (envelope.opcode == productOpcode(ProductOpcode::VoicePreviewVolume))
    return handleVolumePreview(envelope);
  if (envelope.opcode == productOpcode(ProductOpcode::VoiceApplyVolume) &&
      envelope.flags <= 100U && audio_device_) {
    portENTER_CRITICAL(&settings_mux_);
    saved_volume_percent_ = envelope.flags;
    const bool preview_active = volume_preview_active_;
    portEXIT_CRITICAL(&settings_mux_);
    if (!preview_active) {
      audio_device_->setVolumePercent(envelope.flags);
      hardware_volume_percent_ = envelope.flags;
    }
    return WorkDisposition::Complete;
  }
  if (envelope.opcode ==
          productOpcode(ProductOpcode::VoiceApplyAssistance) &&
      envelope.flags <= 1U) {
    portENTER_CRITICAL(&settings_mux_);
    voice_assistance_enabled_ = envelope.flags != 0U;
    portEXIT_CRITICAL(&settings_mux_);
    return WorkDisposition::Complete;
  }
  if (envelope.opcode == productOpcode(ProductOpcode::VoiceStateChanged) &&
      envelope.flags <= static_cast<uint8_t>(myai::VoiceState::Error)) {
    reconcileVoiceState(static_cast<myai::VoiceState>(envelope.flags));
    return WorkDisposition::Complete;
  }
  return WorkDisposition::Failed;
}

WorkDisposition NativeVoiceService::handleLocalPrompt(
    const WorkEnvelope& envelope) {
  if (!audio_device_) return WorkDisposition::Failed;
  portENTER_CRITICAL(&settings_mux_);
  const bool assistance_enabled = voice_assistance_enabled_;
  portEXIT_CRITICAL(&settings_mux_);
  // This preference suppresses routine navigation/status speech only. The
  // explicit volume-preview action deliberately bypasses it so the slider is
  // still testable after an experienced user disables assistance.
  if (!assistance_enabled) return WorkDisposition::Complete;
  if (local_prompts_.busy()) local_prompts_.cancel(*audio_device_);
  restoreVolumeAfterPreview();
  const bool network_turn_active = voice_begin_pending_ ||
      network_voice_state_ == myai::VoiceState::Listening ||
      network_voice_state_ == myai::VoiceState::Thinking ||
      network_voice_state_ == myai::VoiceState::Speaking ||
      audio_bridge_->captureBusy() || audio_bridge_->playbackBusy();
  voice_begin_pending_ = false;
  if (audio_bridge_->captureBusy())
    audio_bridge_->cancelCapture(*audio_device_);
  if (audio_bridge_->playbackBusy()) {
    audio_bridge_->abort();
    if (audio_bridge_->servicePlayback(*audio_device_) != ESP_OK) {
      failVoiceHardware();
      return WorkDisposition::Failed;
    }
  }
  if (network_turn_active) {
    post(WorkClass::MyAiNetwork, ProductOpcode::NetworkVoiceCancel, 0,
         kNetworkDeadlineMs);
  }

  bool accepted = false;
  if (envelope.opcode == productOpcode(ProductOpcode::VoicePromptOrdinal) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::VoicePromptRefreshOrdinal)) {
    accepted = local_prompts_.requestOrdinal(
        envelope.flags,
        envelope.opcode ==
            productOpcode(ProductOpcode::VoicePromptRefreshOrdinal),
        *audio_device_);
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::VoicePromptPleaseWait)) {
    accepted = local_prompts_.request(LocalPrompt::PleaseWait, *audio_device_);
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::VoicePromptAlbumEmpty)) {
    accepted = local_prompts_.request(LocalPrompt::AlbumEmpty, *audio_device_);
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::VoicePromptDeviceRestored)) {
    accepted =
        local_prompts_.request(LocalPrompt::DeviceRestored, *audio_device_);
  }
  if (!accepted) {
    noteLocalAudioActive(false);
    postVoiceLed(VoiceLedMode::Error);
    return WorkDisposition::Failed;
  }
  noteLocalAudioActive(true);
  postVoiceLed(VoiceLedMode::Speaking);
  return WorkDisposition::Complete;
}

WorkDisposition NativeVoiceService::handleVolumePreview(
    const WorkEnvelope& envelope) {
  if (!audio_device_ || envelope.flags > 100U)
    return WorkDisposition::Failed;

  if (local_prompts_.busy()) local_prompts_.cancel(*audio_device_);
  restoreVolumeAfterPreview();
  const bool network_turn_active = voice_begin_pending_ ||
      network_voice_state_ == myai::VoiceState::Listening ||
      network_voice_state_ == myai::VoiceState::Thinking ||
      network_voice_state_ == myai::VoiceState::Speaking ||
      audio_bridge_->captureBusy() || audio_bridge_->playbackBusy();
  voice_begin_pending_ = false;
  if (audio_bridge_->captureBusy())
    audio_bridge_->cancelCapture(*audio_device_);
  if (audio_bridge_->playbackBusy()) {
    audio_bridge_->abort();
    if (audio_bridge_->servicePlayback(*audio_device_) != ESP_OK) {
      failVoiceHardware();
      return WorkDisposition::Failed;
    }
  }
  if (network_turn_active) {
    post(WorkClass::MyAiNetwork, ProductOpcode::NetworkVoiceCancel, 0,
         kNetworkDeadlineMs);
  }

  audio_device_->setVolumePercent(envelope.flags);
  hardware_volume_percent_ = envelope.flags;
  portENTER_CRITICAL(&settings_mux_);
  volume_preview_active_ = true;
  portEXIT_CRITICAL(&settings_mux_);
  if (!local_prompts_.request(LocalPrompt::DeviceRestored, *audio_device_)) {
    restoreVolumeAfterPreview();
    noteLocalAudioActive(false);
    postVoiceLed(VoiceLedMode::Error);
    return WorkDisposition::Failed;
  }
  noteLocalAudioActive(true);
  postVoiceLed(VoiceLedMode::Speaking);
  return WorkDisposition::Complete;
}

void NativeVoiceService::restoreVolumeAfterPreview() {
  portENTER_CRITICAL(&settings_mux_);
  const bool active = volume_preview_active_;
  const uint8_t saved = saved_volume_percent_;
  volume_preview_active_ = false;
  portEXIT_CRITICAL(&settings_mux_);
  if (!active || !audio_device_) return;
  audio_device_->setVolumePercent(saved);
  hardware_volume_percent_ = saved;
}

WorkDisposition NativeVoiceService::handleTopButton() {
  if (!client_ || !audio_device_) return WorkDisposition::Failed;
  if (local_prompts_.busy()) {
    local_prompts_.cancel(*audio_device_);
    noteLocalAudioActive(false);
  }
  restoreVolumeAfterPreview();
  if (voice_task_state_ == myai::VoiceState::Ready &&
      !voice_begin_pending_ && !audio_bridge_->playbackBusy() &&
      !audio_bridge_->captureBusy()) {
    voice_begin_pending_ = true;
    noteVoiceTurnActive(true);
    postVoiceLed(VoiceLedMode::Thinking);
    const AdmissionResult queued = post(
        WorkClass::MyAiNetwork, ProductOpcode::NetworkVoiceBegin, 0,
        kNetworkDeadlineMs);
    if (queued != AdmissionResult::Admitted) {
      voice_begin_pending_ = false;
      noteVoiceTurnActive(false);
      postVoiceLed(VoiceLedMode::Error);
      return WorkDisposition::Busy;
    }
    return WorkDisposition::Complete;
  }
  if (voice_task_state_ == myai::VoiceState::Listening &&
      !voice_begin_pending_ && audio_bridge_->captureBusy()) {
    const esp_err_t stopped = audio_bridge_->finishCapture(*audio_device_);
    if (stopped != ESP_OK) {
      failVoiceHardware();
      return WorkDisposition::Failed;
    }
    voice_task_state_ = myai::VoiceState::Thinking;
    postVoiceLed(VoiceLedMode::Thinking);
    return WorkDisposition::Complete;
  }
  if (voice_begin_pending_ ||
      voice_task_state_ == myai::VoiceState::Listening ||
      voice_task_state_ == myai::VoiceState::Thinking ||
      voice_task_state_ == myai::VoiceState::Speaking) {
    voice_begin_pending_ = false;
    audio_bridge_->cancelCapture(*audio_device_);
    audio_bridge_->abort();
    const AdmissionResult queued = post(
        WorkClass::MyAiNetwork, ProductOpcode::NetworkVoiceCancel, 0,
        kNetworkDeadlineMs);
    postVoiceLed(VoiceLedMode::Blocked);
    return queued == AdmissionResult::Admitted ? WorkDisposition::Complete
                                                : WorkDisposition::Busy;
  }
  postVoiceLed(VoiceLedMode::Blocked);
  return WorkDisposition::Busy;
}

WorkDisposition NativeVoiceService::startCapture() {
  voice_begin_pending_ = false;
  if (!audio_device_ || voice_task_state_ != myai::VoiceState::Listening ||
      audio_bridge_->captureBusy() || audio_bridge_->playbackBusy()) {
    postVoiceLed(VoiceLedMode::Error);
    return WorkDisposition::Failed;
  }
  const esp_err_t started = audio_bridge_->beginCapture(*audio_device_);
  if (started != ESP_OK) {
    failVoiceHardware();
    return WorkDisposition::Failed;
  }
  postVoiceLed(VoiceLedMode::Listening);
  return WorkDisposition::Complete;
}

void NativeVoiceService::reconcileVoiceState(myai::VoiceState state) {
  if (state == myai::VoiceState::Ready && audio_bridge_->playbackBusy()) {
    ready_deferred_for_audio_ = true;
    return;
  }
  voice_task_state_ = state;
  switch (state) {
    case myai::VoiceState::Ready:
      ready_deferred_for_audio_ = false;
      postVoiceLed(VoiceLedMode::Ready);
      break;
    case myai::VoiceState::Listening:
      postVoiceLed(VoiceLedMode::Listening);
      break;
    case myai::VoiceState::Thinking:
      postVoiceLed(VoiceLedMode::Thinking);
      break;
    case myai::VoiceState::Connecting:
      // The client also reconnects proactively while idle. Only a connection
      // made on behalf of a button-initiated turn is an interactive Thinking
      // state; an unavailable background gateway is visibly blocked and must
      // not keep the device awake.
      postVoiceLed(voiceTurnActive() ? VoiceLedMode::Thinking
                                     : VoiceLedMode::Blocked);
      break;
    case myai::VoiceState::Speaking:
      postVoiceLed(VoiceLedMode::Speaking);
      break;
    case myai::VoiceState::Idle:
      voice_begin_pending_ = false;
      if (audio_device_) audio_bridge_->cancelCapture(*audio_device_);
      audio_bridge_->abort();
      postVoiceLed(VoiceLedMode::Blocked);
      break;
    case myai::VoiceState::Error:
      voice_begin_pending_ = false;
      if (audio_device_) audio_bridge_->cancelCapture(*audio_device_);
      audio_bridge_->abort();
      postVoiceLed(VoiceLedMode::Error);
      break;
  }
}

void NativeVoiceService::failVoiceHardware() {
  portENTER_CRITICAL(&diagnostics_mux_);
  ++diagnostics_.audio_failures;
  portEXIT_CRITICAL(&diagnostics_mux_);
  voice_begin_pending_ = false;
  noteVoiceTurnActive(false);
  noteLocalAudioActive(false);
  restoreVolumeAfterPreview();
  voice_task_state_ = myai::VoiceState::Error;
  diagnostics::SerialDiagnosticEvent event;
  event.kind = diagnostics::SerialDiagnosticEventKind::VoiceError;
  event.code = 1U;
  emitSerialDiagnostic(event);
  postVoiceLed(VoiceLedMode::Error);
  post(WorkClass::MyAiNetwork, ProductOpcode::NetworkVoiceCancel, 0,
       kNetworkDeadlineMs);
}

void NativeVoiceService::serviceVoice() {
  if (!audio_device_) return;
  portENTER_CRITICAL(&settings_mux_);
  const uint8_t saved_volume = saved_volume_percent_;
  const bool preview_active = volume_preview_active_;
  portEXIT_CRITICAL(&settings_mux_);
  if (!preview_active && hardware_volume_percent_ != saved_volume) {
    audio_device_->setVolumePercent(saved_volume);
    hardware_volume_percent_ = saved_volume;
  }
  if (local_prompts_.busy()) {
    const esp_err_t local = local_prompts_.service(*audio_device_);
    if (local != ESP_OK) {
      restoreVolumeAfterPreview();
      noteLocalAudioActive(false);
      failVoiceHardware();
      return;
    }
    if (!local_prompts_.busy()) {
      restoreVolumeAfterPreview();
      noteLocalAudioActive(false);
      postVoiceLed(network_voice_state_ == myai::VoiceState::Ready
                       ? VoiceLedMode::Ready
                       : VoiceLedMode::Blocked);
    }
    return;
  }
  const esp_err_t playback = audio_bridge_->servicePlayback(*audio_device_);
  if (playback != ESP_OK) {
    failVoiceHardware();
    return;
  }
  if (audio_bridge_->captureBusy()) {
    const esp_err_t captured = audio_bridge_->captureStep(*audio_device_, 2);
    if (captured != ESP_OK) {
      failVoiceHardware();
      return;
    }
  }
  if (ready_deferred_for_audio_ && !audio_bridge_->playbackBusy()) {
    ready_deferred_for_audio_ = false;
    voice_task_state_ = myai::VoiceState::Ready;
    postVoiceLed(VoiceLedMode::Ready);
  }
}

bool NativeVoiceService::handleControlResult(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Result) return false;
  if (envelope.work_class == WorkClass::Voice &&
      (envelope.opcode ==
           productOpcode(ProductOpcode::VoiceApplyVolume) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::VoiceApplyAssistance) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::VoicePreviewVolume)))
    return true;
  if (envelope.work_class == WorkClass::Storage &&
      (envelope.opcode == productOpcode(ProductOpcode::StorageAppendChat) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::StorageReadLocalChat) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::StorageClearLocalChat)))
    return true;
  if (envelope.work_class == WorkClass::Storage &&
      envelope.opcode == productOpcode(
          ProductOpcode::StorageRecoverLocalChatAfterFormat))
    return true;
  if (envelope.work_class == WorkClass::Portal &&
      (envelope.opcode == productOpcode(ProductOpcode::PortalRunAigc) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::PortalRunLocalTool) ||
       envelope.opcode ==
           productOpcode(ProductOpcode::PortalConfirmLocalTool)))
    return true;
  if (envelope.work_class != WorkClass::MyAiNetwork) return false;
  if (envelope.opcode == productOpcode(ProductOpcode::NetworkVoiceBegin)) {
    if (envelope.disposition == WorkDisposition::Complete) {
      post(WorkClass::Voice, ProductOpcode::VoiceStartCapture, 0,
           kResponsiveDeadlineMs);
    } else {
      noteVoiceTurnActive(false);
      postVoiceState(myai::VoiceState::Error);
    }
    return true;
  }
  if (envelope.opcode == productOpcode(ProductOpcode::NetworkVoiceCancel) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::NetworkApplySystemPrompt) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::NetworkStartMyAiPairing) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::NetworkRebindMyAi) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::NetworkQueueAigc))
    return true;
  return false;
}

WorkDisposition NativeVoiceService::handleNetworkCommand(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::MyAiNetwork || !client_) {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.command_rejections;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }

  if (envelope.opcode ==
      productOpcode(ProductOpcode::NetworkQueueAigc)) {
    ProductTextKind kind = ProductTextKind::ToolState;
    std::string prompt;
    const bool serial_diagnostic = envelope.flags == 1U;
    if (envelope.flags > 1U ||
        !text_pool_.take(envelope.request_id, kind, prompt) ||
        kind != ProductTextKind::AigcState ||
        !acceptAigcPrompt(std::move(prompt), serial_diagnostic,
                          envelope.request_id)) {
      cancelQueuedAigcAdmission(envelope.request_id);
      queueChat(ProductTextKind::AigcState,
                "aigc.rejected_busy_or_invalid source=portal");
      postImageLed(ImageLedMode::Error);
      if (serial_diagnostic) {
        diagnostics::SerialDiagnosticEvent event;
        event.kind = diagnostics::SerialDiagnosticEventKind::AigcError;
        event.code = 1U;
        emitSerialDiagnostic(event);
      }
      return WorkDisposition::Failed;
    }
    postImageLed(ImageLedMode::Generating);
    return WorkDisposition::Complete;
  }
  if (envelope.opcode ==
      productOpcode(ProductOpcode::NetworkStartMyAiPairing)) {
    pairing_start_requested_ = true;
    return WorkDisposition::Complete;
  }
  if (envelope.opcode ==
      productOpcode(ProductOpcode::NetworkRebindMyAi)) {
    rebind_requested_ = true;
    return WorkDisposition::Complete;
  }
  if (!client_initialized_) {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.command_rejections;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }

  myai::Status status;
  if (envelope.opcode ==
      productOpcode(ProductOpcode::NetworkVoiceBegin)) {
    status = client_->beginVoiceTurn(std::to_string(envelope.request_id));
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::NetworkVoiceCancel)) {
    status = client_->disconnectVoice("top_button_cancel");
    reconnect_cleanup_pending_ = false;
    next_voice_reconnect_ms_ = nowMs() + kMinimumReconnectMs;
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::NetworkApplySystemPrompt)) {
    return applyPendingSystemPrompt() ? WorkDisposition::Complete
                                      : WorkDisposition::Failed;
  } else {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.command_rejections;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }

  if (!status.ok()) {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.network_failures;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }
  return WorkDisposition::Complete;
}

bool NativeVoiceService::acceptAigcPrompt(std::string prompt,
                                          bool serial_diagnostic,
                                          uint64_t queued_ticket) {
  if (prompt.empty() || prompt.size() > kAigcPromptMaximum) return false;
  bool accepted = false;
  portENTER_CRITICAL(&maintenance_mux_);
  if (storage_maintenance_active_ || !storage_available_) {
    portEXIT_CRITICAL(&maintenance_mux_);
    return false;
  }
  portENTER_CRITICAL(&aigc_mux_);
  const bool queued_admission = queued_ticket != 0U;
  const bool reservation_matches = queued_admission
      ? aigc_admission_pending_ && aigc_admission_ticket_ == queued_ticket
      : !aigc_admission_pending_;
  if (reservation_matches && aigc_phase_ == AigcPhase::Idle &&
      !aigc_exclusive_) {
    // Moving a pre-allocated string is bounded and allocation-free. The
    // queued reservation and protected phase change under the same lock, so
    // power never observes a false idle gap between them.
    aigc_prompt_.swap(prompt);
    aigc_admission_pending_ = false;
    aigc_admission_ticket_ = 0U;
    aigc_phase_ = AigcPhase::PendingHandoff;
    aigc_serial_diagnostic_ = serial_diagnostic;
    accepted = true;
  }
  portEXIT_CRITICAL(&aigc_mux_);
  portEXIT_CRITICAL(&maintenance_mux_);
  return accepted;
}

void NativeVoiceService::cancelQueuedAigcAdmission(uint64_t queued_ticket) {
  portENTER_CRITICAL(&aigc_mux_);
  if (queued_ticket != 0U && aigc_admission_pending_ &&
      aigc_admission_ticket_ == queued_ticket) {
    aigc_admission_pending_ = false;
    aigc_admission_ticket_ = 0U;
  }
  portEXIT_CRITICAL(&aigc_mux_);
}

void NativeVoiceService::scheduleReconnect(uint32_t delay_ms) {
  next_voice_reconnect_ms_ = nowMs() +
      std::max<uint32_t>(delay_ms, kMinimumReconnectMs);
  reconnect_cleanup_pending_ = true;
}

void NativeVoiceService::stageSystemPrompt(const std::string& prompt) {
  if (!local_tools::LocalCommandParser::validStoredPrompt(prompt)) return;
  portENTER_CRITICAL(&settings_mux_);
  pending_system_prompt_.fill('\0');
  std::memcpy(pending_system_prompt_.data(), prompt.data(), prompt.size());
  do {
    ++system_prompt_generation_;
  } while (system_prompt_generation_ == 0U);
  system_prompt_pending_ = true;
  portEXIT_CRITICAL(&settings_mux_);
}

bool NativeVoiceService::applyPendingSystemPrompt() {
  if (!client_ || !client_initialized_) return false;
  std::array<char, local_tools::kMaximumStoredPromptBytes + 1U> prompt{};
  uint32_t generation = 0;
  portENTER_CRITICAL(&settings_mux_);
  const bool pending = system_prompt_pending_;
  if (pending) {
    prompt = pending_system_prompt_;
    generation = system_prompt_generation_;
  }
  portEXIT_CRITICAL(&settings_mux_);
  if (!pending) return true;

  // A lease can remain valid after the socket has already closed. Always let
  // the Network owner clear it before updating the client policy. The client
  // clears the local lease even when the best-effort disconnect report fails.
  client_->disconnectVoice("system_prompt_changed");
  next_voice_reconnect_ms_ = nowMs() + kMinimumReconnectMs;
  const myai::Status applied = client_->setSystemPrompt(prompt.data());
  if (!applied.ok()) return false;
  portENTER_CRITICAL(&settings_mux_);
  if (system_prompt_pending_ && system_prompt_generation_ == generation) {
    pending_system_prompt_.fill('\0');
    system_prompt_pending_ = false;
  }
  portEXIT_CRITICAL(&settings_mux_);
  std::fill(prompt.begin(), prompt.end(), '\0');
  return true;
}

void NativeVoiceService::networkTick(bool wifi_online) {
  if (!initialized_ || !client_) return;
  portENTER_CRITICAL(&aigc_mux_);
  const bool aigc_exclusive = aigc_exclusive_;
  portEXIT_CRITICAL(&aigc_mux_);
  if (aigc_exclusive) {
    wifi_was_online_ = wifi_online;
    return;
  }
  if (!wifi_online) {
    if (wifi_was_online_ && client_initialized_) {
      client_->disconnectVoice("wifi_offline");
      reconnect_cleanup_pending_ = false;
      next_voice_reconnect_ms_ = nowMs() + kMinimumReconnectMs;
    }
    wifi_was_online_ = false;
    return;
  }
  wifi_was_online_ = true;
  const uint32_t now = nowMs();
  if (!client_initialized_) {
    const myai::Status initialized = client_->initialize();
    client_initialized_ = initialized.ok();
    if (!initialized.ok()) {
      portENTER_CRITICAL(&diagnostics_mux_);
      ++diagnostics_.network_failures;
      portEXIT_CRITICAL(&diagnostics_mux_);
      return;
    }
    publishOnboarding(nullptr);
    if (activation_state_ == myai::ActivationState::Pairing) {
      myai::PairingView pending;
      if (client_->pendingPairing(pending).ok()) publishOnboarding(&pending);
    }
  }

  if (!applyPendingSystemPrompt()) return;

  serviceRequestedPairingActions(now);
  startPairingIfNeeded(now);

  if (activation_state_ == myai::ActivationState::Pairing &&
      due(now, next_pairing_poll_ms_)) {
    bool bound = false;
    const myai::Status polled = client_->pollPairing(bound);
    next_pairing_poll_ms_ = now + kPairingPollMs;
    if (!polled.ok() && polled.code != myai::ErrorCode::PaymentRequired &&
        polled.code != myai::ErrorCode::PairingExpired) {
      portENTER_CRITICAL(&diagnostics_mux_);
      ++diagnostics_.network_failures;
      portEXIT_CRITICAL(&diagnostics_mux_);
    }
    if (bound) authorization_verified_ = false;
  }

  if (activation_state_ != myai::ActivationState::Bound) return;
  if (!authorization_verified_ || due(now, next_authorization_check_ms_)) {
    bool authorized = false;
    const myai::Status checked = client_->checkAuthorization(authorized);
    authorization_verified_ = checked.ok() && authorized;
    next_authorization_check_ms_ = now + kAuthorizationRefreshMs;
    publishOnboarding(nullptr);
    if (!authorization_verified_) return;
  }

  // A manually submitted or voice-authored image request does not depend on
  // an already-open voice socket. Hand it to the exclusive Portal pipeline
  // before spending this tick reconnecting realtime voice.
  handoffAigcIfReady();
  portENTER_CRITICAL(&aigc_mux_);
  const bool handed_to_aigc = aigc_exclusive_;
  portEXIT_CRITICAL(&aigc_mux_);
  if (handed_to_aigc) return;

  // Pairing, authorization, local history and image generation remain
  // available on display-only SKUs. They must never open a realtime voice
  // socket or allocate DMA when the board declares no duplex audio.
  if (!voice_hardware_available_) return;

  if (reconnect_cleanup_pending_) {
    client_->disconnectVoice("native_reconnect_cleanup");
    reconnect_cleanup_pending_ = false;
    return;
  }
  if (!wss_.connected() && due(now, next_voice_reconnect_ms_)) {
    const myai::Status connected = client_->connectVoice();
    if (!connected.ok()) {
      scheduleReconnect(client_->suggestedVoiceReconnectDelayMs());
      portENTER_CRITICAL(&diagnostics_mux_);
      ++diagnostics_.network_failures;
      portEXIT_CRITICAL(&diagnostics_mux_);
      return;
    }
    last_heartbeat_ms_ = now;
    heartbeat_audio_deferred_ = false;
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.reconnects;
    portEXIT_CRITICAL(&diagnostics_mux_);
  }
  if (!wss_.connected()) return;
  const myai::Status ingress = wss_.pollIngress();
  if (!ingress.ok()) {
    scheduleReconnect(client_->suggestedVoiceReconnectDelayMs());
    return;
  }
  if (audio_bridge_->captureBusy()) {
    const myai::Status pumped = audio_bridge_->pumpCaptureToNetwork(*client_);
    if (!pumped.ok()) {
      scheduleReconnect(client_->suggestedVoiceReconnectDelayMs());
      return;
    }
  }
  if (due(now, last_heartbeat_ms_ + kHeartbeatMs)) {
    // Lease maintenance is low-priority control traffic. Never contend with
    // microphone uplink or TTS DMA; leaving last_heartbeat_ms_ unchanged makes
    // the first audio-idle Network tick retry immediately.
    if (audio_bridge_->captureBusy() || audio_bridge_->playbackBusy()) {
      if (!heartbeat_audio_deferred_) {
        heartbeat_audio_deferred_ = true;
        portENTER_CRITICAL(&diagnostics_mux_);
        ++diagnostics_.heartbeat_audio_deferrals;
        portEXIT_CRITICAL(&diagnostics_mux_);
      }
      return;
    }
    heartbeat_audio_deferred_ = false;
    const myai::Status heartbeat = client_->heartbeatVoice();
    if (!heartbeat.ok()) {
      scheduleReconnect(client_->suggestedVoiceReconnectDelayMs());
    } else {
      last_heartbeat_ms_ = now;
    }
  }
}

void NativeVoiceService::startPairingIfNeeded(uint32_t now_ms) {
  if (!client_ || !client_initialized_ ||
      activation_state_ != myai::ActivationState::Unconfigured ||
      !due(now_ms, next_pairing_start_ms_)) {
    return;
  }
  startPairingNow(now_ms);
}

myai::Status NativeVoiceService::startPairingNow(uint32_t now_ms) {
  if (!client_ || !client_initialized_ ||
      activation_state_ != myai::ActivationState::Unconfigured) {
    return myai::Status(myai::ErrorCode::InvalidState, 0,
                        "MyAI pairing start is not currently legal");
  }
  char candidate[7]{};
  const uint32_t value = esp_random() % 1000000U;
  std::snprintf(candidate, sizeof(candidate), "%06lu",
                static_cast<unsigned long>(value));
  myai::PairingView pairing;
  const myai::Status started = client_->startPairing(candidate, pairing);
  next_pairing_start_ms_ = now_ms + kPairingStartRetryMs;
  if (started.ok()) {
    publishOnboarding(&pairing);
    next_pairing_poll_ms_ = now_ms + kPairingPollMs;
    return started;
  }
  portENTER_CRITICAL(&diagnostics_mux_);
  ++diagnostics_.network_failures;
  portEXIT_CRITICAL(&diagnostics_mux_);
  return started;
}

void NativeVoiceService::serviceRequestedPairingActions(uint32_t now_ms) {
  if (!client_ || !client_initialized_) return;

  if (rebind_requested_) {
    portENTER_CRITICAL(&aigc_mux_);
    const bool image_busy = aigc_admission_pending_ ||
        aigc_phase_ != AigcPhase::Idle || aigc_exclusive_;
    portEXIT_CRITICAL(&aigc_mux_);
    if (image_busy) return;

    // Rebinding is explicit and local. It clears only MyAI's device-scoped
    // credential; the Inkloop album, cloud-device identity and task journal
    // are outside this client and remain untouched.
    client_->disconnectVoice("portal_rebind");
    reconnect_cleanup_pending_ = false;
    const myai::Status reset = client_->resetCredentialForRebind();
    rebind_requested_ = false;
    if (!reset.ok()) {
      portENTER_CRITICAL(&diagnostics_mux_);
      ++diagnostics_.network_failures;
      portEXIT_CRITICAL(&diagnostics_mux_);
      return;
    }
    authorization_verified_ = false;
    publishOnboarding(nullptr);
    pairing_start_requested_ = true;
    next_pairing_start_ms_ = now_ms;
  }

  if (!pairing_start_requested_) return;
  if (activation_state_ == myai::ActivationState::Pairing) {
    // Refresh only the already persisted public view. Never invent or rotate a
    // second code, and never expose the opaque pairing token.
    myai::PairingView pending;
    if (client_->pendingPairing(pending).ok()) {
      publishOnboarding(&pending);
      next_pairing_poll_ms_ = now_ms;
    }
    pairing_start_requested_ = false;
    return;
  }
  if (activation_state_ != myai::ActivationState::Unconfigured) {
    pairing_start_requested_ = false;
    return;
  }
  const myai::Status started = startPairingNow(now_ms);
  // Automatic retry remains responsible for transient failures; keeping an
  // extra explicit request bit would otherwise issue duplicate starts.
  pairing_start_requested_ = false;
  (void)started;
}

void NativeVoiceService::publishOnboarding(
    const myai::PairingView* pairing) {
  NativeMyAiOnboardingSnapshot next;
  next.activation_state = activation_state_;
  next.authorization_verified = authorization_verified_;
  const std::string device_id = client_ ? client_->authoritativeDeviceId()
                                         : std::string();
  if (sixDigits(device_id)) copyBounded(device_id, next.device_code);
  if (pairing && sixDigits(pairing->onboardingCode) &&
      copyBounded(pairing->onboardingCode, next.device_code) &&
      copyBounded(pairing->bindingUrl, next.binding_url) &&
      copyBounded(pairing->expiresAt, next.expires_at)) {
    next.pairing_view_available = true;
  }
  portENTER_CRITICAL(&onboarding_mux_);
  onboarding_ = next;
  portEXIT_CRITICAL(&onboarding_mux_);
}

void NativeVoiceService::handoffAigcIfReady() {
  portENTER_CRITICAL(&aigc_mux_);
  const bool pending = aigc_phase_ == AigcPhase::PendingHandoff;
  portEXIT_CRITICAL(&aigc_mux_);
  const bool voice_turn_active =
      network_voice_state_ == myai::VoiceState::Listening ||
      network_voice_state_ == myai::VoiceState::Thinking ||
      network_voice_state_ == myai::VoiceState::Speaking ||
      voice_begin_pending_;
  if (!pending || !client_ ||
      activation_state_ != myai::ActivationState::Bound ||
      !authorization_verified_ || voice_turn_active ||
      audio_bridge_->playbackBusy() || audio_bridge_->captureBusy()) {
    return;
  }
  // AIGC has its own gateway lease. Close any idle/ready voice lease first so
  // the slow Portal lane is the sole MyAiClient caller during generation.
  client_->disconnectVoice("aigc_exclusive_handoff");
  portENTER_CRITICAL(&aigc_mux_);
  aigc_exclusive_ = true;
  aigc_phase_ = AigcPhase::Start;
  portEXIT_CRITICAL(&aigc_mux_);
  const AdmissionResult admitted = post(
      WorkClass::Portal, ProductOpcode::PortalRunAigc, 0, kNetworkDeadlineMs);
  if (admitted != AdmissionResult::Admitted) {
    finishAigc(false, "aigc.portal_queue_busy");
  }
}

WorkDisposition NativeVoiceService::handlePortalCommand(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Portal) {
    return WorkDisposition::Failed;
  }
  if (envelope.opcode == productOpcode(ProductOpcode::PortalRunLocalTool) ||
      envelope.opcode ==
          productOpcode(ProductOpcode::PortalConfirmLocalTool)) {
    const WorkDisposition disposition = handleLocalToolCommand(envelope);
    if (envelope.opcode ==
            productOpcode(ProductOpcode::PortalRunLocalTool) ||
        envelope.flags != 0U) {
      finishTrackedStorageWork();
    }
    return disposition;
  }
  if (envelope.opcode != productOpcode(ProductOpcode::PortalRunAigc))
    return WorkDisposition::Failed;
  portENTER_CRITICAL(&aigc_mux_);
  const bool valid = aigc_exclusive_ && aigc_phase_ == AigcPhase::Start;
  portEXIT_CRITICAL(&aigc_mux_);
  return valid ? WorkDisposition::Complete : WorkDisposition::Failed;
}

void NativeVoiceService::portalTick(bool album_mutation_allowed) {
  serviceAigc(album_mutation_allowed);
}

bool NativeVoiceService::portalBusy() const {
  return aigcBusy() || storageMaintenanceActive();
}

bool NativeVoiceService::aigcBusy() const {
  portENTER_CRITICAL(&aigc_mux_);
  const bool busy = aigc_admission_pending_ ||
      aigc_phase_ != AigcPhase::Idle || aigc_exclusive_;
  portEXIT_CRITICAL(&aigc_mux_);
  return busy;
}

bool NativeVoiceService::interactiveAudioBusy() const {
  portENTER_CRITICAL(&maintenance_mux_);
  const bool tracked_busy = voice_turn_active_ || local_audio_active_;
  portEXIT_CRITICAL(&maintenance_mux_);
  return tracked_busy ||
         (audio_bridge_ &&
          (audio_bridge_->captureBusy() || audio_bridge_->playbackBusy()));
}

void NativeVoiceService::serviceAigc(bool album_mutation_allowed) {
  portENTER_CRITICAL(&aigc_mux_);
  const AigcPhase phase = aigc_phase_;
  const bool exclusive = aigc_exclusive_;
  const bool serial_diagnostic = aigc_serial_diagnostic_;
  portEXIT_CRITICAL(&aigc_mux_);
  if (!exclusive || phase == AigcPhase::Idle ||
      phase == AigcPhase::PendingHandoff || !client_ || !album_store_) {
    return;
  }

  myai::Status status;
  if (phase == AigcPhase::Start) {
    if (serial_diagnostic) {
      emitSerialAigcPhase(
          diagnostics::SerialDiagnosticAigcPhase::Starting);
    }
    aigc_request_ = myai::ImageRequest();
    std::string configured_template;
    std::string configured_negative;
    std::string configured_render_strategy;
    local_tools::ILocalToolsAdapter* settings = nullptr;
    portENTER_CRITICAL(&local_tools_mux_);
    settings = local_tools_adapter_;
    portEXIT_CRITICAL(&local_tools_mux_);
    if (settings &&
        !settings->queryAigcPrompt(configured_template).ok()) {
      configured_template.clear();
    }
    if (!settings ||
        !settings->queryAigcNegativePrompt(configured_negative).ok()) {
      // Empty is a valid, fail-safe negative prompt. Never silently replace a
      // persisted user setting with an unrelated hard-coded policy.
      configured_negative.clear();
    }
    if (!settings ||
        !settings->queryDefaultRenderStrategy(
             configured_render_strategy).ok()) {
      configured_render_strategy = kOfficialQualityRenderStrategy;
    }
    IBoardRenderer* renderer = board_.renderer();
    const BoardRenderStrategyCatalog catalog =
        renderer ? renderer->renderStrategyCatalog()
                 : BoardRenderStrategyCatalog{};
    if (!renderer || !catalog.valid()) {
      finishAigc(false, "aigc.renderer_catalog_unavailable");
      return;
    }
    if (!catalog.contains(configured_render_strategy) ||
        !renderer->supportsRenderStrategy(configured_render_strategy)) {
      configured_render_strategy = kOfficialQualityRenderStrategy;
      if (!catalog.contains(configured_render_strategy) ||
          !renderer->supportsRenderStrategy(configured_render_strategy)) {
        finishAigc(false, "aigc.official_strategy_unavailable");
        return;
      }
      queueChat(ProductTextKind::AigcState,
                "aigc.render_strategy_fallback selected=official-quality");
    }
    aigc_request_.prompt = composeImagePrompt(
        board_.descriptor(), aigc_prompt_, configured_template);
    aigc_request_.negativePrompt = configured_negative;
    aigc_render_strategy_ = configured_render_strategy;
    aigc_request_.size = aigcImageSize(board_.descriptor());
    aigc_request_.steps = 20;
    aigc_request_.maxEncodedBytes = 2U * 1024U * 1024U;
    aigc_request_.maxDecodedBytes = storage::kMaximumAlbumAssetBytes;
    if (aigc_request_.prompt.empty()) {
      finishAigc(false, "aigc.invalid_prompt");
      return;
    }
    queueChat(ProductTextKind::AigcState,
              std::string("aigc.request size=") + aigc_request_.size +
                  " prompt=" + aigc_request_.prompt);
    status = client_->startImage(aigc_request_, aigc_generated_);
    if (status.ok()) {
      portENTER_CRITICAL(&aigc_mux_);
      aigc_phase_ = AigcPhase::Poll;
      next_aigc_poll_ms_ = nowMs() + kAigcPollMs;
      portEXIT_CRITICAL(&aigc_mux_);
      if (serial_diagnostic) {
        emitSerialAigcPhase(
            diagnostics::SerialDiagnosticAigcPhase::Submitted);
      }
      queueChat(ProductTextKind::AigcState, "aigc.submitted poll=5s");
      return;
    }
  } else if (phase == AigcPhase::Poll) {
    if (!due(nowMs(), next_aigc_poll_ms_)) return;
    status = client_->pollImage(aigc_generated_.promptId, aigc_status_);
    if (status.ok() && terminalImageSuccess(aigc_status_.status) &&
        !aigc_status_.outputs.empty()) {
      portENTER_CRITICAL(&aigc_mux_);
      aigc_phase_ = AigcPhase::Download;
      portEXIT_CRITICAL(&aigc_mux_);
      if (serial_diagnostic) {
        emitSerialAigcPhase(
            diagnostics::SerialDiagnosticAigcPhase::GenerationComplete);
      }
      queueChat(ProductTextKind::AigcState, "aigc.generated downloading");
      return;
    }
    if (status.ok()) {
      next_aigc_poll_ms_ = nowMs() + kAigcPollMs;
      return;
    }
  } else if (phase == AigcPhase::Download) {
    if (!album_mutation_allowed) return;
    storage::AigcAlbumSink sink(*album_store_,
                                storage::kMaximumAlbumAssetBytes,
                                aigc_render_strategy_);
    myai::AigcOutputMetadata metadata;
    status = client_->downloadImage(
        aigc_generated_.promptId, aigc_status_.outputs.front(), aigc_request_,
        sink, metadata);
    storage::AlbumCommitResult committed;
    if (status.ok() && sink.takeCommittedAsset(committed)) {
      queueChat(ProductTextKind::AigcState,
                std::string("aigc.cached asset=") + committed.asset_id +
                    " ordinal=" + std::to_string(committed.ordinal + 1U));
      if (serial_diagnostic) {
        emitSerialAigcPhase(
            diagnostics::SerialDiagnosticAigcPhase::Cached);
      }
      if (committed.ordinal <= 255U &&
          post(WorkClass::Display,
               serial_diagnostic
                   ? ProductOpcode::DisplayDiagnosticAigcOrdinal
                   : ProductOpcode::DisplayAlbumOrdinal,
               static_cast<uint8_t>(committed.ordinal), 0) ==
              AdmissionResult::Admitted) {
        finishAigc(true, "aigc.cached display_queued");
        return;
      }
      status = myai::Status(myai::ErrorCode::InvalidState, 0,
                            "AIGC display queue is busy");
    }
    if (status.ok()) {
      status = myai::Status(myai::ErrorCode::Storage, 0,
                            "AIGC output did not commit to album");
    }
  }

  if (!status.ok()) {
    const std::string failure = safeAigcFailureState(status);
    finishAigc(false, failure.c_str());
  }
}

void NativeVoiceService::finishAigc(bool success, const char* state) {
  portENTER_CRITICAL(&aigc_mux_);
  const bool serial_diagnostic = aigc_serial_diagnostic_;
  portEXIT_CRITICAL(&aigc_mux_);
  if (client_) client_->disconnectImage(success ? "complete" : "failed");
  queueChat(ProductTextKind::AigcState, state ? state : "aigc.failed");
  if (!success) postImageLed(ImageLedMode::Error);
  aigc_prompt_.clear();
  aigc_render_strategy_ = kOfficialQualityRenderStrategy;
  aigc_generated_ = myai::AigcGenerateResponse();
  aigc_status_ = myai::AigcStatusResponse();
  portENTER_CRITICAL(&aigc_mux_);
  aigc_admission_pending_ = false;
  aigc_admission_ticket_ = 0U;
  aigc_phase_ = AigcPhase::Idle;
  aigc_exclusive_ = false;
  aigc_serial_diagnostic_ = false;
  portEXIT_CRITICAL(&aigc_mux_);
  if (!success && serial_diagnostic) {
    diagnostics::SerialDiagnosticEvent event;
    event.kind = diagnostics::SerialDiagnosticEventKind::AigcError;
    event.code = 2U;
    emitSerialDiagnostic(event);
  }
  next_voice_reconnect_ms_ = nowMs() + kMinimumReconnectMs;
}

WorkDisposition NativeVoiceService::handleStorage(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Storage || !chat_log_)
    return WorkDisposition::Failed;
  if (envelope.opcode == productOpcode(
          ProductOpcode::StorageRecoverLocalChatAfterFormat)) {
    const std::string directory = std::string(storage_root_) + "/inkloop";
    bool recovered = (::mkdir(directory.c_str(),
                              S_IRUSR | S_IWUSR | S_IXUSR) == 0 ||
                      errno == EEXIST);
    if (recovered) recovered = chat_log_->clear().ok();
    storage::ChatRecovery chat_recovery;
    if (recovered) recovered = chat_log_->recover(chat_recovery).ok();
    if (recovered && chat_snapshot_mutex_ &&
        xSemaphoreTake(chat_snapshot_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
      chat_snapshot_mailbox_ = NativeLocalChatSnapshot{};
      portENTER_CRITICAL(&chat_snapshot_state_mux_);
      chat_snapshot_request_pending_ = false;
      chat_snapshot_ready_ = true;
      portEXIT_CRITICAL(&chat_snapshot_state_mux_);
      xSemaphoreGive(chat_snapshot_mutex_);
    } else {
      recovered = false;
    }
    portENTER_CRITICAL(&maintenance_mux_);
    storage_reset_pending_ = false;
    storage_reset_complete_ = recovered;
    storage_available_ = recovered;
    portEXIT_CRITICAL(&maintenance_mux_);
    return recovered ? WorkDisposition::Complete : WorkDisposition::Failed;
  }
  const bool tracked_opcode =
      envelope.opcode == productOpcode(ProductOpcode::StorageReadLocalChat) ||
      envelope.opcode == productOpcode(ProductOpcode::StorageClearLocalChat) ||
      envelope.opcode == productOpcode(ProductOpcode::StorageAppendChat);
  if (!tracked_opcode) return WorkDisposition::Failed;
  auto finish = [this](WorkDisposition disposition) {
    finishTrackedStorageWork();
    return disposition;
  };
  if (envelope.opcode ==
      productOpcode(ProductOpcode::StorageReadLocalChat)) {
    return finish(readLocalChatSnapshot(envelope));
  }
  if (envelope.opcode ==
      productOpcode(ProductOpcode::StorageClearLocalChat)) {
    const storage::ChatLogResult cleared = chat_log_->clear();
    if (!cleared.ok() || !chat_snapshot_mutex_ ||
        xSemaphoreTake(chat_snapshot_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
      portENTER_CRITICAL(&diagnostics_mux_);
      ++diagnostics_.chat_write_failures;
      portEXIT_CRITICAL(&diagnostics_mux_);
      return finish(WorkDisposition::Failed);
    }
    // Publish the empty snapshot from the same sole Storage owner that
    // unlinked both JSONL generations. Portal never guesses that clear won.
    chat_snapshot_mailbox_ = NativeLocalChatSnapshot{};
    portENTER_CRITICAL(&chat_snapshot_state_mux_);
    chat_snapshot_request_pending_ = false;
    chat_snapshot_ready_ = true;
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
    xSemaphoreGive(chat_snapshot_mutex_);
    return finish(WorkDisposition::Complete);
  }
  ProductTextKind kind = ProductTextKind::ToolState;
  std::string text;
  if (!text_pool_.take(envelope.request_id, kind, text))
    return finish(WorkDisposition::Failed);
  const std::string utc = clock_.utcIso8601();
  storage::ChatLogResult result;
  switch (kind) {
    case ProductTextKind::AsrFinal:
      result = chat_log_->appendAsr(text, true, utc);
      break;
    case ProductTextKind::AssistantFinal:
      result = chat_log_->appendAssistant(text, true, utc);
      break;
    case ProductTextKind::ToolState:
      result = chat_log_->appendToolState(text, utc);
      break;
    case ProductTextKind::AigcState:
      result = chat_log_->appendAigcState(text, utc);
      break;
  }
  if (!result.ok() &&
      result.code != storage::ChatLogCode::IgnoredBlankAudio &&
      result.code != storage::ChatLogCode::IgnoredEmpty) {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_write_failures;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return finish(WorkDisposition::Failed);
  }
  return finish(WorkDisposition::Complete);
}

WorkDisposition NativeVoiceService::readLocalChatSnapshot(
    const WorkEnvelope& envelope) {
  auto finishRequest = [this](bool ready) {
    portENTER_CRITICAL(&chat_snapshot_state_mux_);
    chat_snapshot_request_pending_ = false;
    chat_snapshot_ready_ = ready;
    portEXIT_CRITICAL(&chat_snapshot_state_mux_);
  };
  if (!chat_log_ || !chat_snapshot_mutex_ || envelope.request_id == 0U ||
      envelope.flags == 0U ||
      envelope.flags > kNativeLocalChatPageItems) {
    finishRequest(false);
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_read_failures;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }

  const uint64_t after_sequence = envelope.request_id - 1U;
  storage::ChatPage page;
  const storage::ChatLogResult read =
      chat_log_->readPage(after_sequence, envelope.flags, page);
  if (!read.ok()) {
    finishRequest(false);
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_read_failures;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Failed;
  }

  // The file scan and heap-backed ChatPage stay on Storage. Only this bounded,
  // credential-free DTO crosses to Portal, and the Portal side never opens the
  // chat file or asks MyAI for history.
  if (xSemaphoreTake(chat_snapshot_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
    finishRequest(false);
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_snapshot_drops;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return WorkDisposition::Busy;
  }

  chat_snapshot_mailbox_ = NativeLocalChatSnapshot{};
  chat_snapshot_mailbox_.after_sequence = after_sequence;
  chat_snapshot_mailbox_.next_after_sequence = after_sequence;
  chat_snapshot_mailbox_.has_more = page.has_more;
  chat_snapshot_mailbox_.corruption_observed = page.corruption_observed;
  for (const storage::ChatRecord& record : page.records) {
    if (chat_snapshot_mailbox_.item_count >= kNativeLocalChatPageItems) {
      chat_snapshot_mailbox_.has_more = true;
      break;
    }
    const size_t text_bytes =
        boundedUtf8Prefix(record.text, kNativeLocalChatItemBytes);
    if (text_bytes == 0U) continue;
    if (text_bytes > kNativeLocalChatPageTextBytes -
                         chat_snapshot_mailbox_.text_bytes) {
      chat_snapshot_mailbox_.has_more = true;
      break;
    }
    NativeLocalChatItem& item =
        chat_snapshot_mailbox_.items[chat_snapshot_mailbox_.item_count];
    item.sequence = record.sequence;
    item.text_offset =
        static_cast<uint16_t>(chat_snapshot_mailbox_.text_bytes);
    item.text_bytes = static_cast<uint16_t>(text_bytes);
    item.role = nativeChatRole(record.kind);
    item.truncated = text_bytes < record.text.size();
    std::memcpy(chat_snapshot_mailbox_.text.data() +
                    chat_snapshot_mailbox_.text_bytes,
                record.text.data(), text_bytes);
    chat_snapshot_mailbox_.text_bytes += text_bytes;
    ++chat_snapshot_mailbox_.item_count;
    chat_snapshot_mailbox_.next_after_sequence = record.sequence;
  }
  finishRequest(true);
  xSemaphoreGive(chat_snapshot_mutex_);
  return WorkDisposition::Complete;
}

bool NativeVoiceService::queueChat(ProductTextKind kind,
                                   const std::string& text) {
  if (!beginTrackedStorageWork()) {
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_queue_drops;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return false;
  }
  const uint64_t ticket = text_pool_.put(kind, text);
  if (ticket == 0) {
    finishTrackedStorageWork();
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_queue_drops;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return false;
  }
  WorkEnvelope envelope{};
  envelope.generation = 1;
  envelope.request_id = ticket;
  envelope.opcode = productOpcode(ProductOpcode::StorageAppendChat);
  envelope.work_class = WorkClass::Storage;
  envelope.kind = EnvelopeKind::Command;
  envelope.disposition = WorkDisposition::Accepted;
  if (supervisor_.post(envelope) != AdmissionResult::Admitted) {
    text_pool_.release(ticket);
    finishTrackedStorageWork();
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.chat_queue_drops;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return false;
  }
  return true;
}

bool NativeVoiceService::queueLocalTool(
    const std::string& transcript, local_tools::CommandKind command) {
  if (!beginTrackedStorageWork()) return false;
  const uint64_t ticket = text_pool_.put(ProductTextKind::ToolState, transcript);
  if (ticket == 0) {
    finishTrackedStorageWork();
    portENTER_CRITICAL(&diagnostics_mux_);
    ++diagnostics_.command_rejections;
    portEXIT_CRITICAL(&diagnostics_mux_);
    return false;
  }
  WorkEnvelope envelope{};
  envelope.generation = 1;
  envelope.request_id = ticket;
  // No deadline: if Portal is briefly occupied by AIGC/album work, the fixed
  // queue provides backpressure without leaking the separately ticketed text.
  envelope.deadline_ms = 0;
  envelope.opcode = productOpcode(ProductOpcode::PortalRunLocalTool);
  envelope.work_class = WorkClass::Portal;
  envelope.kind = EnvelopeKind::Command;
  envelope.disposition = WorkDisposition::Accepted;
  envelope.flags = static_cast<uint8_t>(command);
  if (supervisor_.post(envelope) == AdmissionResult::Admitted) return true;
  text_pool_.release(ticket);
  finishTrackedStorageWork();
  portENTER_CRITICAL(&diagnostics_mux_);
  ++diagnostics_.command_rejections;
  portEXIT_CRITICAL(&diagnostics_mux_);
  return false;
}

std::string NativeVoiceService::describeLocalToolOutcome(
    const local_tools::ToolOutcome& outcome) {
  const char* command = local_tools::commandName(outcome.command);
  switch (outcome.code) {
    case local_tools::ExecutionCode::ConfirmationRequired:
      return std::string("local_tool.confirmation_required command=") + command +
          " press_top_button_within=30s";
    case local_tools::ExecutionCode::Executed:
      break;
    case local_tools::ExecutionCode::AdapterFailure:
      return std::string("local_tool.failed command=") + command +
          " adapter=" +
          std::to_string(static_cast<unsigned>(outcome.adapter_code));
    case local_tools::ExecutionCode::AdapterContractViolation:
      return std::string("local_tool.contract_violation command=") + command;
    case local_tools::ExecutionCode::ConfirmationMissing:
      return std::string("local_tool.confirmation_missing command=") + command;
    case local_tools::ExecutionCode::ConfirmationMismatch:
      return std::string("local_tool.confirmation_mismatch command=") + command;
    case local_tools::ExecutionCode::ConfirmationExpired:
      return std::string("local_tool.confirmation_expired command=") + command;
    case local_tools::ExecutionCode::TokenUnavailable:
      return std::string("local_tool.confirmation_unavailable command=") +
          command;
    case local_tools::ExecutionCode::Ignored:
      return "local_tool.ignored";
    case local_tools::ExecutionCode::Rejected:
      return std::string("local_tool.rejected parse=") +
          local_tools::parseCodeName(outcome.parse_code);
  }

  switch (outcome.command) {
    case local_tools::CommandKind::QueryStorage:
      return std::string("local_tool.ok command=") + command +
          " remaining_bytes=" +
          std::to_string(outcome.storage.remaining_bytes) +
          " total_bytes=" + std::to_string(outcome.storage.total_bytes);
    case local_tools::CommandKind::QueryVolume:
    case local_tools::CommandKind::SetVolume:
    case local_tools::CommandKind::SetLedMaximumBrightness:
      return std::string("local_tool.ok command=") + command +
          " percent=" + std::to_string(outcome.percent);
    case local_tools::CommandKind::QueryAssistantPrompt:
    case local_tools::CommandKind::SetAssistantPrompt:
    case local_tools::CommandKind::QueryAigcPrompt:
    case local_tools::CommandKind::SetAigcPrompt:
      return std::string("local_tool.ok command=") + command +
          " value=" + outcome.text;
    case local_tools::CommandKind::DeleteImageOrdinal:
    case local_tools::CommandKind::DeleteImageId:
    case local_tools::CommandKind::ClearAlbum:
    case local_tools::CommandKind::FormatTfCard:
      return std::string("local_tool.ok command=") + command;
    case local_tools::CommandKind::None:
      break;
  }
  return "local_tool.rejected";
}

void NativeVoiceService::publishLocalToolOutcome(
    const local_tools::ToolOutcome& outcome) {
  portENTER_CRITICAL(&local_tools_mux_);
  local_confirmation_pending_ = local_tools_session_.confirmationPending();
  local_confirmation_format_pending_ = local_confirmation_pending_ &&
      outcome.command == local_tools::CommandKind::FormatTfCard;
  portEXIT_CRITICAL(&local_tools_mux_);
  queueChat(ProductTextKind::ToolState, describeLocalToolOutcome(outcome));
  if (outcome.command == local_tools::CommandKind::QueryStorage) {
    diagnostics::SerialDiagnosticEvent event;
    event.kind = diagnostics::SerialDiagnosticEventKind::VoiceToolStorage;
    event.flags = outcome.code == local_tools::ExecutionCode::Executed ? 1U
                                                                       : 0U;
    emitSerialDiagnostic(event);
  }
  if (outcome.code != local_tools::ExecutionCode::Executed) return;

  AdmissionResult applied = AdmissionResult::Admitted;
  if (outcome.command == local_tools::CommandKind::SetVolume) {
    applied = post(WorkClass::Voice, ProductOpcode::VoiceApplyVolume,
                   outcome.percent, kResponsiveDeadlineMs);
  } else if (outcome.command ==
             local_tools::CommandKind::SetLedMaximumBrightness) {
    applied = postLedMaximumBrightness(outcome.percent);
  } else if (outcome.command ==
             local_tools::CommandKind::SetAssistantPrompt) {
    stageSystemPrompt(outcome.text);
    applied = post(WorkClass::MyAiNetwork,
                   ProductOpcode::NetworkApplySystemPrompt, 0,
                   kNetworkDeadlineMs);
  }
  if (applied != AdmissionResult::Admitted) {
    queueChat(ProductTextKind::ToolState,
              std::string("local_tool.effect_queue_busy command=") +
                  local_tools::commandName(outcome.command));
  }
}

WorkDisposition NativeVoiceService::handleLocalToolCommand(
    const WorkEnvelope& envelope) {
  local_tools::ILocalToolsAdapter* adapter = nullptr;
  portENTER_CRITICAL(&local_tools_mux_);
  adapter = local_tools_adapter_;
  portEXIT_CRITICAL(&local_tools_mux_);
  if (!adapter) return WorkDisposition::Failed;

  local_tools::ToolOutcome outcome;
  if (envelope.opcode == productOpcode(ProductOpcode::PortalRunLocalTool)) {
    ProductTextKind kind = ProductTextKind::ToolState;
    std::string transcript;
    if (!text_pool_.take(envelope.request_id, kind, transcript) ||
        kind != ProductTextKind::ToolState) {
      return WorkDisposition::Failed;
    }
    const local_tools::ParseResult parsed =
        local_tool_parser_.parseFinalAsr(transcript);
    if (!parsed.matched() ||
        envelope.flags != static_cast<uint8_t>(parsed.command.kind)) {
      queueChat(ProductTextKind::ToolState, "local_tool.rejected_stale_command");
      return WorkDisposition::Complete;
    }
    outcome = local_tools_session_.handleFinalAsr(
        transcript, nowMs(), *adapter, local_confirmation_tokens_);
    std::fill(local_confirmation_token_.begin(),
              local_confirmation_token_.end(), '\0');
    local_confirmation_token_.clear();
    if (outcome.code == local_tools::ExecutionCode::ConfirmationRequired) {
      local_confirmation_token_ = outcome.confirmation_token;
      std::fill(outcome.confirmation_token.begin(),
                outcome.confirmation_token.end(), '\0');
      outcome.confirmation_token.clear();
    }
  } else if (envelope.opcode ==
             productOpcode(ProductOpcode::PortalConfirmLocalTool)) {
    // The opaque token never crosses a task queue or enters logs. A physical
    // top-button press merely asks the same Portal owner to consume it.
    outcome = local_tools_session_.confirm(local_confirmation_token_, nowMs(),
                                           *adapter);
    std::fill(local_confirmation_token_.begin(),
              local_confirmation_token_.end(), '\0');
    local_confirmation_token_.clear();
  } else {
    return WorkDisposition::Failed;
  }
  publishLocalToolOutcome(outcome);
  return WorkDisposition::Complete;
}

myai::LocalTranscriptDecision NativeVoiceService::inspect(
    const std::string& transcript) {
  const local_tools::ParseResult parsed =
      local_tool_parser_.parseFinalAsr(transcript);
  if (parsed.ignored()) {
    // Empty/blank-audio artifacts are terminal locally: do not persist them
    // and do not ask MyAI to fabricate a response to missing speech.
    return myai::LocalTranscriptDecision(
        true, parsed.code == local_tools::ParseCode::IgnoredBlankAudio
                  ? "local.ignore_blank_audio"
                  : "local.ignore_empty");
  }
  diagnostics::SerialDiagnosticEvent asr_event;
  asr_event.kind = diagnostics::SerialDiagnosticEventKind::VoiceAsrFinal;
  asr_event.first = static_cast<uint32_t>(
      std::min<size_t>(transcript.size(),
                       std::numeric_limits<uint32_t>::max()));
  if (!parsed.matched()) {
    asr_event.code = static_cast<uint8_t>(
        diagnostics::SerialDiagnosticAsrRoute::Remote);
    emitSerialDiagnostic(asr_event);
    return myai::LocalTranscriptDecision(false);
  }
  asr_event.code = static_cast<uint8_t>(
      diagnostics::SerialDiagnosticAsrRoute::Local);
  emitSerialDiagnostic(asr_event);
  portENTER_CRITICAL(&local_tools_mux_);
  const bool attached = local_tools_adapter_ != nullptr;
  portEXIT_CRITICAL(&local_tools_mux_);
  if (!attached) {
    queueChat(ProductTextKind::ToolState,
              std::string("local_tool.unavailable command=") +
                  local_tools::commandName(parsed.command.kind));
    return myai::LocalTranscriptDecision(
        true, local_tools::commandName(parsed.command.kind));
  }
  if (!queueLocalTool(transcript, parsed.command.kind)) {
    queueChat(ProductTextKind::ToolState,
              std::string("local_tool.queue_busy command=") +
                  local_tools::commandName(parsed.command.kind));
  }
  // Recognized device-control phrases fail closed locally even under queue
  // pressure. Sending one to the remote LLM could turn a rejected destructive
  // operation into an unsafe or misleading assistant response.
  return myai::LocalTranscriptDecision(
      true, local_tools::commandName(parsed.command.kind));
}

void NativeVoiceService::onActivationState(myai::ActivationState state,
                                            const myai::Status&) {
  activation_state_ = state;
  if (state == myai::ActivationState::Pairing)
    next_pairing_poll_ms_ = nowMs();
  if (state != myai::ActivationState::Bound) authorization_verified_ = false;
  publishOnboarding(nullptr);
  if (state == myai::ActivationState::Unconfigured ||
      state == myai::ActivationState::PaymentRequired ||
      state == myai::ActivationState::RecoveryRequired ||
      state == myai::ActivationState::Error) {
    postVoiceState(myai::VoiceState::Error);
  }
}

void NativeVoiceService::onPairingReady(const myai::PairingView& pairing) {
  publishOnboarding(&pairing);
  next_pairing_poll_ms_ = nowMs();
}

void NativeVoiceService::onVoiceState(myai::VoiceState state) {
  network_voice_state_ = state;
  serial_voice_state_.store(static_cast<uint8_t>(state),
                            std::memory_order_release);
  diagnostics::SerialDiagnosticEvent serial_event;
  serial_event.kind = diagnostics::SerialDiagnosticEventKind::VoiceState;
  serial_event.code = static_cast<uint8_t>(serialVoiceState(state));
  emitSerialDiagnostic(serial_event);
  // Connecting is used both for a real button-initiated turn and for the
  // client's idle gateway preconnection/retry loop. Preserve the existing
  // turn authority through Connecting: handleTopButton() has already raised
  // it for a real turn, while a proactive reconnect begins false. Terminal
  // and active protocol states remain authoritative.
  if (state != myai::VoiceState::Connecting) {
    noteVoiceTurnActive(state == myai::VoiceState::Listening ||
                        state == myai::VoiceState::Thinking ||
                        state == myai::VoiceState::Speaking);
  }
  postVoiceState(state);
  if (state == myai::VoiceState::Error && client_) {
    scheduleReconnect(client_->suggestedVoiceReconnectDelayMs());
  }
}

void NativeVoiceService::onTranscript(const std::string& text, bool final) {
  if (!final) return;
  if (text.empty() || storage::LocalChatLog::isBlankAudioArtifact(text))
    return;
  queueChat(ProductTextKind::AsrFinal, text);
  assistant_text_.clear();
  assistant_finalized_ = false;
}

void NativeVoiceService::onAssistantText(const std::string& text,
                                         bool final) {
  if (!final) {
    if (assistant_finalized_) {
      assistant_text_.clear();
      assistant_finalized_ = false;
    }
    const size_t remaining = assistant_text_.size() <
            BoundedTextPool::kMaximumTextBytes
        ? BoundedTextPool::kMaximumTextBytes - assistant_text_.size()
        : 0;
    assistant_text_.append(text.data(), std::min(remaining, text.size()));
    return;
  }
  if (assistant_finalized_) return;
  const std::string completed = assistant_text_.empty() ? text : assistant_text_;
  queueChat(ProductTextKind::AssistantFinal, completed);
  assistant_text_.clear();
  assistant_finalized_ = true;
}

void NativeVoiceService::onLocalCommand(const std::string& command_name,
                                        const std::string&) {
  (void)command_name;
  // The slow Portal owner records the actual outcome. Logging recognition
  // here would create a misleading success row before any device action ran.
}

void NativeVoiceService::onVoiceAction(const myai::VoiceEvent& action) {
  if (action.kind != "aigc.generate") return;
  const bool accepted = acceptAigcPrompt(action.prompt);
  if (!accepted) {
    queueChat(ProductTextKind::AigcState, "aigc.rejected_busy_or_invalid");
    postImageLed(ImageLedMode::Error);
  }
}

void NativeVoiceService::onAigcState(myai::AigcState state,
                                     const std::string&) {
  ImageLedMode mode = ImageLedMode::Off;
  switch (state) {
    case myai::AigcState::Generating:
    case myai::AigcState::Polling:
      mode = ImageLedMode::Generating;
      break;
    case myai::AigcState::Downloading:
      mode = ImageLedMode::Downloading;
      break;
    case myai::AigcState::Complete:
      // MyAI completion only means the remote artifact is ready. The product
      // still has to validate, atomically cache, convert and refresh it.
      mode = ImageLedMode::Converting;
      break;
    case myai::AigcState::Error:
      mode = ImageLedMode::Error;
      break;
    case myai::AigcState::Idle:
      mode = ImageLedMode::Off;
      break;
  }
  postImageLed(mode);
}

void NativeVoiceService::onError(const myai::Status& status) {
  diagnostics::SerialDiagnosticEvent event;
  event.kind = diagnostics::SerialDiagnosticEventKind::MyAiError;
  event.code = static_cast<uint8_t>(status.code);
  emitSerialDiagnostic(event);
  ESP_LOGW(kTag, "MyAI error code=%u http=%d retry_ms=%lu",
           static_cast<unsigned>(status.code), status.httpStatus,
           static_cast<unsigned long>(status.retryAfterMs));
}

NativeVoiceDiagnostics NativeVoiceService::diagnostics() const {
  portENTER_CRITICAL(&diagnostics_mux_);
  const NativeVoiceDiagnostics value = diagnostics_;
  portEXIT_CRITICAL(&diagnostics_mux_);
  return value;
}

NativeMyAiOnboardingSnapshot NativeVoiceService::onboardingSnapshot() const {
  portENTER_CRITICAL(&onboarding_mux_);
  const NativeMyAiOnboardingSnapshot value = onboarding_;
  portEXIT_CRITICAL(&onboarding_mux_);
  return value;
}

NativeVoiceSerialDiagnosticSnapshot
NativeVoiceService::serialDiagnosticSnapshot() const {
  const NativeMyAiOnboardingSnapshot onboarding = onboardingSnapshot();
  NativeVoiceSerialDiagnosticSnapshot output;
  output.activation_state = onboarding.activation_state;
  output.authorization_verified = onboarding.authorization_verified;
  const uint8_t state = serial_voice_state_.load(std::memory_order_acquire);
  output.voice_state = state <= static_cast<uint8_t>(myai::VoiceState::Error)
      ? static_cast<myai::VoiceState>(state)
      : myai::VoiceState::Error;
  return output;
}

}  // namespace inkloop
