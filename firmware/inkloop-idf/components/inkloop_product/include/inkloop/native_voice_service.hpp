#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inkloop/board.hpp"
#include "inkloop/bounded_text_pool.hpp"
#include "inkloop/diagnostics/serial_diagnostic_events.hpp"
#include "inkloop/esp_cross_core_audio_bridge.hpp"
#include "inkloop/local_tools/local_tools.hpp"
#include "inkloop/local_prompt_player.hpp"
#include "inkloop/myai/CanonicalJsonCodec.h"
#include "inkloop/myai/CredentialPersistence.h"
#include "inkloop/myai/MyAiClient.h"
#include "inkloop/myai/esp_aigc_output_transport.hpp"
#include "inkloop/myai/esp_http_adapters.hpp"
#include "inkloop/myai/esp_nvs_credential_store.hpp"
#include "inkloop/myai/esp_wss_transport.hpp"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/status_led_core.hpp"
#include "inkloop/storage/local_chat_log.hpp"
#include "inkloop/storage/aigc_album_sink.hpp"
#include "inkloop/storage/posix_chat_store.hpp"

namespace inkloop {

struct NativeVoiceDiagnostics {
  uint32_t command_rejections = 0;
  uint32_t audio_failures = 0;
  uint32_t network_failures = 0;
  uint32_t chat_queue_drops = 0;
  uint32_t chat_write_failures = 0;
  uint32_t chat_read_failures = 0;
  uint32_t chat_snapshot_drops = 0;
  uint32_t reconnects = 0;
  uint32_t heartbeat_audio_deferrals = 0;
};

inline constexpr size_t kNativeLocalChatPageItems = 24U;
inline constexpr size_t kNativeLocalChatItemBytes = 2048U;
inline constexpr size_t kNativeLocalChatPageTextBytes = 16U * 1024U;

enum class NativeLocalChatRole : uint8_t { User, Assistant, Tool };

struct NativeLocalChatItem {
  uint64_t sequence = 0;
  uint16_t text_offset = 0;
  uint16_t text_bytes = 0;
  NativeLocalChatRole role = NativeLocalChatRole::User;
  bool truncated = false;
};

// Fixed, credential-free Storage->Portal DTO. Text is packed into one bounded
// blob so the mailbox consumes ~16 KiB rather than 24 independent 2 KiB slots.
// The consumer must use offsets/lengths and never assume a trailing NUL.
struct NativeLocalChatSnapshot {
  std::array<NativeLocalChatItem, kNativeLocalChatPageItems> items{};
  std::array<char, kNativeLocalChatPageTextBytes> text{};
  uint64_t after_sequence = 0;
  uint64_t next_after_sequence = 0;
  size_t item_count = 0;
  size_t text_bytes = 0;
  bool has_more = false;
  bool corruption_observed = false;
};

class INativeLocalChatSnapshotConsumer {
 public:
  virtual ~INativeLocalChatSnapshotConsumer() = default;
  // Called only from Portal lane while the bounded mailbox is held. The
  // implementation may copy/convert locally but must not call MyAI or storage.
  virtual bool accept(const NativeLocalChatSnapshot& snapshot) = 0;
};

struct NativeMyAiOnboardingSnapshot {
  std::array<char, 7> device_code{};
  std::array<char, 257> binding_url{};
  std::array<char, 129> expires_at{};
  myai::ActivationState activation_state =
      myai::ActivationState::Unconfigured;
  bool pairing_view_available = false;
  // A persisted device token only establishes Bound.  This bit is published
  // after a successful /devices/check and lets the local Portal distinguish
  // an actually authorized device without exposing any credential material.
  bool authorization_verified = false;
};

struct NativeVoiceSerialDiagnosticSnapshot {
  myai::ActivationState activation_state =
      myai::ActivationState::Unconfigured;
  myai::VoiceState voice_state = myai::VoiceState::Idle;
  bool authorization_verified = false;
};

// Cross-core MyAI voice composition. All HTTP/WSS/client methods run on the
// slow Network owner; only I2S capture/playback touches the high-priority Voice
// owner. Audio travels through bounded PSRAM rings and is never persisted.
class NativeVoiceService final : public myai::ILocalTranscriptInterceptor,
                                 public myai::IMyAiEvents {
 public:
  NativeVoiceService(IBoardAdapter& board, RuntimeSupervisor& supervisor,
                     const char* storage_root,
                     storage::IAlbumStagingStore* album_store,
                     diagnostics::ISerialDiagnosticEventSink*
                         serial_diagnostics = nullptr);

  esp_err_t initialize();
  // Supervisor must be stopped first. Closes WSS, aborts I2S and any album
  // transaction, and releases every heap-backed Voice owner allocation.
  void shutdown();
  // Installs the SKU/storage settings execution boundary. The adapter is
  // invoked by the low-priority Portal owner after a bounded command has been
  // admitted, plus a single-threaded boot read for saved prompt/volume;
  // final-ASR, Network and Voice callbacks never call it. Composition must
  // attach it before initialize() and before RuntimeSupervisor starts.
  esp_err_t attachLocalTools(local_tools::ILocalToolsAdapter& adapter);
  esp_err_t configureHandlers();
  AdmissionResult enqueueTopButton();
  AdmissionResult enqueueAlbumOrdinal(size_t one_based_ordinal,
                                       bool refresh_start);
  AdmissionResult enqueueLocalPrompt(LocalPrompt prompt);
  // Portal callbacks only enqueue bounded typed work. Audio, MyAI and file
  // operations remain owned by Voice, Network and Storage respectively.
  AdmissionResult enqueueVolumePreview(uint8_t temporary_percent);
  AdmissionResult enqueueStartMyAiPairing();
  AdmissionResult enqueueRebindMyAi();
  AdmissionResult enqueueImageGeneration(const std::string& prompt);
  AdmissionResult enqueueDiagnosticImageGeneration(
      const std::string& prompt);
  AdmissionResult enqueueClearLocalChat();
  // Portal settings are loaded after the Voice owner is constructed but
  // before the supervisor starts. This seed does no I/O and never exposes a
  // MyAI credential. Runtime changes are then applied through owner queues.
  esp_err_t seedPersistedVoiceSettings(uint8_t volume_percent,
                                       bool voice_assistance_enabled,
                                       const std::string& assistant_prompt);
  AdmissionResult enqueuePersistedVoiceSettings(
      uint8_t volume_percent, bool voice_assistance_enabled,
      const std::string& assistant_prompt);
  AdmissionResult requestLocalChatSnapshot(uint64_t after_sequence,
                                           uint8_t limit);
  bool tryConsumeLocalChatSnapshot(
      INativeLocalChatSnapshotConsumer& consumer);
  bool handleControlResult(const WorkEnvelope& envelope);
  WorkDisposition handleNetworkCommand(const WorkEnvelope& envelope);
  WorkDisposition handlePortalCommand(const WorkEnvelope& envelope);
  void networkTick(bool wifi_online);
  void portalTick(bool album_mutation_allowed = true);
  bool portalBusy() const;
  // True from accepted PendingHandoff through terminal cleanup. Power uses
  // this live signal instead of waiting for the presentation LEDs so a newly
  // queued image cannot race the final deep-sleep snapshot.
  bool aigcBusy() const;
  // Presentation state is not an execution lock. This reports only a
  // button-initiated MyAI turn or local/capture/playback audio so power
  // admission cannot be held awake by proactive gateway reconnect LEDs.
  bool interactiveAudioBusy() const;
  // Called from the Portal coordinator before an explicitly confirmed TF
  // maintenance operation. A successful begin blocks new AIGC/chat/audio
  // admissions and proves all already-admitted Voice storage work drained.
  // It never formats or mounts storage. Failure rolls the admission gate back.
  bool beginStorageMaintenance();
  bool finishStorageMaintenance(bool storage_changed,
                                bool storage_available);
  void endStorageMaintenance();
  bool storageMaintenanceActive() const;
  NativeVoiceDiagnostics diagnostics() const;
  NativeMyAiOnboardingSnapshot onboardingSnapshot() const;
  NativeVoiceSerialDiagnosticSnapshot serialDiagnosticSnapshot() const;

  myai::LocalTranscriptDecision inspect(
      const std::string& transcript) override;
  void onActivationState(myai::ActivationState state,
                         const myai::Status& status) override;
  void onPairingReady(const myai::PairingView& pairing) override;
  void onVoiceState(myai::VoiceState state) override;
  void onTranscript(const std::string& text, bool final) override;
  void onAssistantText(const std::string& text, bool final) override;
  void onLocalCommand(const std::string& command_name,
                      const std::string& transcript) override;
  void onVoiceAction(const myai::VoiceEvent& action) override;
  void onAigcState(myai::AigcState state,
                   const std::string& detail) override;
  void onError(const myai::Status& status) override;

 private:
  static WorkDisposition voiceHandler(const WorkEnvelope& envelope,
                                      void* context);
  static WorkDisposition storageHandler(const WorkEnvelope& envelope,
                                        void* context);
  static void voiceTick(void* context);
  WorkDisposition handleVoice(const WorkEnvelope& envelope);
  WorkDisposition handleStorage(const WorkEnvelope& envelope);
  WorkDisposition readLocalChatSnapshot(const WorkEnvelope& envelope);
  void serviceVoice();
  void serviceAigc(bool album_mutation_allowed);
  void handoffAigcIfReady();
  void finishAigc(bool success, const char* state);
  WorkDisposition handleLocalToolCommand(const WorkEnvelope& envelope);
  bool queueLocalTool(const std::string& transcript,
                      local_tools::CommandKind command);
  void publishLocalToolOutcome(const local_tools::ToolOutcome& outcome);
  static std::string describeLocalToolOutcome(
      const local_tools::ToolOutcome& outcome);
  WorkDisposition handleTopButton();
  WorkDisposition handleLocalPrompt(const WorkEnvelope& envelope);
  WorkDisposition handleVolumePreview(const WorkEnvelope& envelope);
  WorkDisposition startCapture();
  AdmissionResult post(WorkClass work_class, ProductOpcode opcode,
                       uint8_t flags, uint32_t deadline_ms);
  AdmissionResult postVoiceState(myai::VoiceState state);
  AdmissionResult postVoiceLed(VoiceLedMode mode);
  AdmissionResult postImageLed(ImageLedMode mode);
  AdmissionResult postLedMaximumBrightness(uint8_t percent);
  bool queueChat(ProductTextKind kind, const std::string& text);
  void reconcileVoiceState(myai::VoiceState state);
  void failVoiceHardware();
  void scheduleReconnect(uint32_t delay_ms);
  void stageSystemPrompt(const std::string& prompt);
  bool applyPendingSystemPrompt();
  void publishOnboarding(const myai::PairingView* pairing);
  void startPairingIfNeeded(uint32_t now_ms);
  myai::Status startPairingNow(uint32_t now_ms);
  void serviceRequestedPairingActions(uint32_t now_ms);
  bool acceptAigcPrompt(std::string prompt,
                        bool serial_diagnostic = false,
                        uint64_t queued_ticket = 0U);
  void cancelQueuedAigcAdmission(uint64_t queued_ticket);
  void restoreVolumeAfterPreview();
  bool beginTrackedStorageWork();
  void finishTrackedStorageWork();
  bool maintenanceBlocksInteractiveWork() const;
  bool voiceTurnActive() const;
  void noteVoiceTurnActive(bool active);
  void noteLocalAudioActive(bool active);
  uint64_t nextRequestId();
  static uint32_t nowMs();
  static bool due(uint32_t now_ms, uint32_t deadline_ms);
  myai::ClientConfig makeConfig() const;
  AdmissionResult enqueueImageGenerationImpl(const std::string& prompt,
                                              bool serial_diagnostic);
  void emitSerialDiagnostic(
      const diagnostics::SerialDiagnosticEvent& event) const;
  void emitSerialAigcPhase(
      diagnostics::SerialDiagnosticAigcPhase phase) const;

  class EspConfirmationTokens final
      : public local_tools::IConfirmationTokenSource {
   public:
    bool issue(std::string& token) override;
  };

  class DisabledAudioSink final : public myai::IAudioSink {
   public:
    myai::Status begin(uint32_t, uint8_t) override;
    myai::Status write(const uint8_t*, size_t) override;
    myai::Status end() override;
    void abort() override {}
  };

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  const char* storage_root_;
  storage::IAlbumStagingStore* album_store_;
  diagnostics::ISerialDiagnosticEventSink* serial_diagnostics_ = nullptr;
  myai::EspEndpointSecurity endpoint_security_{};
  myai::EspHttpTransport http_{endpoint_security_};
  myai::EspClock clock_{};
  myai::EspGatewayProbeSet gateway_probes_{clock_};
  myai::EspWssTransport wss_{endpoint_security_};
  myai::EspAigcOutputTransport aigc_output_{endpoint_security_};
  myai::EspNvsCredentialJournalStore credential_journal_{};
  myai::JsonSha256CredentialCodec credential_codec_{};
  myai::CredentialPersistenceCore credentials_{credential_journal_,
                                                credential_codec_};
  myai::CanonicalJsonCodec wire_codec_{};
  std::unique_ptr<EspCrossCoreAudioBridge> audio_bridge_;
  DisabledAudioSink disabled_audio_sink_{};
  LocalPromptPlayer local_prompts_{};
  std::unique_ptr<EspI2sAudioDevice> audio_device_;
  std::unique_ptr<storage::PosixChatLineStore> chat_store_;
  std::unique_ptr<storage::LocalChatLog> chat_log_;
  std::unique_ptr<myai::MyAiClient> client_;
  BoundedTextPool text_pool_{};
  local_tools::LocalCommandParser local_tool_parser_{};
  local_tools::LocalToolsSession local_tools_session_{};
  EspConfirmationTokens local_confirmation_tokens_{};
  local_tools::ILocalToolsAdapter* local_tools_adapter_ = nullptr;
  // Portal-owner only. Never copied into WorkEnvelope, diagnostics or chat.
  std::string local_confirmation_token_;
  std::string assistant_text_;
  portMUX_TYPE sequence_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE diagnostics_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE aigc_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE onboarding_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE local_tools_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE settings_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE chat_snapshot_state_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE maintenance_mux_ = portMUX_INITIALIZER_UNLOCKED;
  StaticSemaphore_t chat_snapshot_mutex_storage_{};
  SemaphoreHandle_t chat_snapshot_mutex_ = nullptr;
  NativeLocalChatSnapshot chat_snapshot_mailbox_{};
  NativeVoiceDiagnostics diagnostics_{};
  NativeMyAiOnboardingSnapshot onboarding_{};
  uint64_t sequence_ = 0;
  uint32_t next_pairing_poll_ms_ = 0;
  uint32_t next_pairing_start_ms_ = 0;
  uint32_t next_authorization_check_ms_ = 0;
  uint32_t next_voice_reconnect_ms_ = 0;
  uint32_t last_heartbeat_ms_ = 0;
  uint32_t next_aigc_poll_ms_ = 0;
  enum class AigcPhase : uint8_t { Idle, PendingHandoff, Start, Poll, Download };
  AigcPhase aigc_phase_ = AigcPhase::Idle;
  bool aigc_admission_pending_ = false;
  uint64_t aigc_admission_ticket_ = 0U;
  std::string aigc_prompt_;
  std::string aigc_render_strategy_ = "official-quality";
  std::array<char, local_tools::kMaximumStoredPromptBytes + 1U>
      pending_system_prompt_{};
  uint32_t system_prompt_generation_ = 0;
  uint8_t saved_volume_percent_ = 60U;
  uint8_t hardware_volume_percent_ = 60U;
  myai::ImageRequest aigc_request_{};
  myai::AigcGenerateResponse aigc_generated_{};
  myai::AigcStatusResponse aigc_status_{};
  myai::ActivationState activation_state_ =
      myai::ActivationState::Unconfigured;
  myai::VoiceState network_voice_state_ = myai::VoiceState::Idle;
  myai::VoiceState voice_task_state_ = myai::VoiceState::Idle;
  bool client_initialized_ = false;
  bool authorization_verified_ = false;
  bool voice_begin_pending_ = false;
  bool assistant_finalized_ = false;
  bool ready_deferred_for_audio_ = false;
  bool reconnect_cleanup_pending_ = false;
  bool heartbeat_audio_deferred_ = false;
  bool aigc_exclusive_ = false;
  bool aigc_serial_diagnostic_ = false;
  bool wifi_was_online_ = false;
  bool local_confirmation_pending_ = false;
  bool local_confirmation_format_pending_ = false;
  bool system_prompt_pending_ = false;
  bool voice_assistance_enabled_ = true;
  bool volume_preview_active_ = false;
  bool pairing_start_requested_ = false;
  bool rebind_requested_ = false;
  uint32_t tracked_storage_work_ = 0;
  bool storage_maintenance_active_ = false;
  bool voice_turn_active_ = false;
  bool local_audio_active_ = false;
  bool storage_available_ = true;
  bool storage_reset_required_ = false;
  bool storage_reset_pending_ = false;
  bool storage_reset_complete_ = false;
  bool voice_hardware_available_ = false;
  bool chat_snapshot_request_pending_ = false;
  bool chat_snapshot_ready_ = false;
  bool initialized_ = false;
  std::atomic<uint8_t> serial_voice_state_{
      static_cast<uint8_t>(myai::VoiceState::Idle)};
};

}  // namespace inkloop
