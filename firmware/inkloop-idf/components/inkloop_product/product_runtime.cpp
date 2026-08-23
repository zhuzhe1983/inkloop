#include "inkloop/product_runtime.hpp"

#include <cerrno>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/slow_io_arbitration.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-product";
constexpr uint32_t kStorageFinalizationDeadlineMs = 10000U;
constexpr char kSerialAigcPrompt[] =
    "红色灯塔与深蓝色大海，鲜艳六色墨水屏配色，强对比，大色块，"
    "简洁竖向构图，无文字";

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

uint64_t displayFingerprint(const char* first, const char* second,
                            const char* third = nullptr,
                            const char* fourth = nullptr) {
  uint64_t value = 1469598103934665603ULL;
  const char* fields[] = {first, second, third, fourth};
  for (const char* field : fields) {
    value ^= 0xffU;
    value *= 1099511628211ULL;
    if (!field) continue;
    for (const unsigned char* at =
             reinterpret_cast<const unsigned char*>(field); *at; ++at) {
      value = (value ^ *at) * 1099511628211ULL;
    }
  }
  return value == 0U ? 1U : value;
}

}  // namespace

EspProductRuntime::EspProductRuntime(IBoardAdapter& board,
    storage::EspStorageMountOwner& storage,
    storage::AssetStoragePreference asset_preference)
    : storage_(storage),
      selected_album_store_(storage.selectedAlbumStore(asset_preference)),
      sd_album_store_(storage.selectedAlbumStore(
          storage::AssetStoragePreference::SdCard)),
      selected_album_is_sd_(selected_album_store_ &&
                            selected_album_store_ == sd_album_store_),
      buttons_(board, supervisor_), leds_(board, supervisor_),
      display_(board, supervisor_, selected_album_store_,
               &serial_diagnostics_),
      voice_(board, supervisor_, storage.selectedAssetRoot(
                 asset_preference),
             selected_album_store_, &serial_diagnostics_),
      inkloop_(supervisor_, storage.taskRoot(),
               selected_album_store_,
               display_),
      portal_(board, supervisor_, wifi_, leds_, display_, voice_,
              storage.selectedAssetRoot(
                  asset_preference),
              storage.selectedAlbumStore(
                  asset_preference)),
      power_(board, supervisor_, buttons_, leds_, wifi_, display_, voice_,
             inkloop_, portal_) {}

EspProductRuntime::~EspProductRuntime() { (void)shutdownForRecovery(); }

esp_err_t EspProductRuntime::setBeginFaultInjector(
    ProductRuntimeBeginFaultInjector injector, void* context) {
  if (started_ || supervisor_.initialized() || shutdown_incomplete_)
    return ESP_ERR_INVALID_STATE;
  begin_fault_injector_ = injector;
  begin_fault_context_ = context;
  return ESP_OK;
}

StorageMaintenanceResult EspProductRuntime::formatTfCardConfirmed() {
  if (!started_ ||
      storage_maintenance_phase_ != StorageMaintenancePhase::Idle) {
    return {StorageMaintenanceCode::Busy};
  }
  const storage::StorageMountSnapshot before = storage_.snapshot();
  if (!before.sd.healthy() || !sd_album_store_) {
    return {StorageMaintenanceCode::NotReady};
  }

  bool portal_gate = false;
  bool voice_gate = false;
  bool inkloop_gate = false;
  bool display_gate = false;
  bool store_gate = false;
  auto rollback = [&]() {
    if (store_gate) sd_album_store_->endMaintenance();
    if (display_gate) display_.finishStorageMaintenance(false);
    if (inkloop_gate) inkloop_.endStorageMaintenance(true);
    if (voice_gate) {
      voice_.finishStorageMaintenance(false, true);
      voice_.endStorageMaintenance();
    }
    if (portal_gate) {
      portal_.finishStorageMaintenance(false, true);
      portal_.endStorageMaintenance();
    }
  };

  portal_gate = portal_.beginStorageMaintenance();
  if (!portal_gate) return {StorageMaintenanceCode::Busy};
  voice_gate = voice_.beginStorageMaintenance();
  if (!voice_gate) {
    rollback();
    return {StorageMaintenanceCode::Busy};
  }
  inkloop_gate = inkloop_.beginStorageMaintenance();
  if (!inkloop_gate) {
    rollback();
    return {StorageMaintenanceCode::Busy};
  }
  display_gate = display_.beginStorageMaintenance();
  if (!display_gate) {
    rollback();
    return {StorageMaintenanceCode::Busy};
  }
  store_gate = sd_album_store_->beginMaintenance();
  if (!store_gate) {
    rollback();
    return {StorageMaintenanceCode::Busy};
  }

  const esp_err_t formatted = storage_.formatSdCardConfirmed();
  const bool storage_changed = formatted != ESP_ERR_INVALID_STATE;
  sd_album_store_->endMaintenance();
  store_gate = false;
  if (!storage_changed) {
    rollback();
    return {StorageMaintenanceCode::NotReady};
  }

  const storage::StorageMountSnapshot after = storage_.snapshot();
  bool selected_storage_available =
      !selected_album_is_sd_ || after.sd.healthy();
  if (selected_album_is_sd_ && selected_storage_available) {
    const char* root = storage_.selectedAssetRoot(
        storage::AssetStoragePreference::SdCard);
    const std::string chat_directory = root
        ? std::string(root) + "/inkloop" : std::string();
    if (chat_directory.empty() ||
        (::mkdir(chat_directory.c_str(), 0700) != 0 && errno != EEXIST)) {
      selected_storage_available = false;
    }
  }

  storage_maintenance_changed_ = selected_album_is_sd_;
  storage_maintenance_available_ = selected_storage_available;
  storage_maintenance_display_pending_ =
      !display_.finishStorageMaintenance(storage_maintenance_changed_);
  storage_maintenance_portal_pending_ = !portal_.finishStorageMaintenance(
      storage_maintenance_changed_, storage_maintenance_available_);
  storage_maintenance_voice_pending_ = !voice_.finishStorageMaintenance(
      storage_maintenance_changed_, storage_maintenance_available_);
  storage_maintenance_deadline_ms_ =
      nowMs() + kStorageFinalizationDeadlineMs;
  storage_maintenance_phase_ = StorageMaintenancePhase::Finalizing;
  serviceStorageMaintenanceFinalization(nowMs());

  return {formatted == ESP_OK && selected_storage_available
              ? StorageMaintenanceCode::Ok
              : StorageMaintenanceCode::IoError};
}

void EspProductRuntime::releaseStorageMaintenanceOwners() {
  voice_.endStorageMaintenance();
  portal_.endStorageMaintenance();
  inkloop_.endStorageMaintenance(storage_maintenance_available_);
  storage_maintenance_phase_ = StorageMaintenancePhase::Idle;
  storage_maintenance_deadline_ms_ = 0U;
  storage_maintenance_changed_ = false;
  storage_maintenance_available_ = true;
  storage_maintenance_display_pending_ = false;
  storage_maintenance_portal_pending_ = false;
  storage_maintenance_voice_pending_ = false;
}

void EspProductRuntime::serviceStorageMaintenanceFinalization(
    uint32_t now_ms) {
  if (storage_maintenance_phase_ != StorageMaintenancePhase::Finalizing)
    return;
  if (storage_maintenance_display_pending_) {
    storage_maintenance_display_pending_ = !display_.finishStorageMaintenance(
        storage_maintenance_changed_);
  }
  if (storage_maintenance_portal_pending_) {
    storage_maintenance_portal_pending_ = !portal_.finishStorageMaintenance(
        storage_maintenance_changed_, storage_maintenance_available_);
  }
  if (storage_maintenance_voice_pending_) {
    const bool before_deadline =
        static_cast<int32_t>(now_ms - storage_maintenance_deadline_ms_) < 0;
    storage_maintenance_voice_pending_ = !voice_.finishStorageMaintenance(
        storage_maintenance_changed_,
        before_deadline && storage_maintenance_available_);
  }
  if (!storage_maintenance_portal_pending_ &&
      !storage_maintenance_voice_pending_) {
    if (storage_maintenance_display_pending_) {
      ESP_LOGE(kTag,
               "storage maintenance left Display fail-closed until reboot");
    }
    releaseStorageMaintenanceOwners();
  }
}

uint32_t EspProductRuntime::nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

esp_err_t EspProductRuntime::begin() {
  if (started_ || shutdown_incomplete_ || ota_acquisition_quiesced_)
    return ESP_ERR_INVALID_STATE;
  esp_err_t status = ESP_OK;
  const auto acquire = [&](ProductRuntimeBeginStage stage,
                           auto&& operation) {
    if (status != ESP_OK) return;
    status = operation();
    if (status == ESP_OK && begin_fault_injector_) {
      status = begin_fault_injector_(stage, begin_fault_context_);
    }
  };
  acquire(ProductRuntimeBeginStage::SupervisorInitialize,
          [&] { return supervisor_.initialize(); });
  acquire(ProductRuntimeBeginStage::ButtonsConfigure,
          [&] { return buttons_.configure(); });
  acquire(ProductRuntimeBeginStage::LedsConfigure,
          [&] { return leds_.configure(); });
  acquire(ProductRuntimeBeginStage::SerialDiagnosticsConfigure, [&] {
    return serial_diagnostics_.configure(
        &EspProductRuntime::serialDiagnosticCommand, this);
  });
  acquire(ProductRuntimeBeginStage::DisplayConfigure,
          [&] { return display_.configure(); });
  acquire(ProductRuntimeBeginStage::VoiceInitialize,
          [&] { return voice_.initialize(); });
  acquire(ProductRuntimeBeginStage::InkloopInitialize,
          [&] { return inkloop_.initialize(); });
  acquire(ProductRuntimeBeginStage::PortalInitialize,
          [&] { return portal_.initialize(); });
  acquire(ProductRuntimeBeginStage::PowerInitialize,
          [&] { return power_.initialize(nowMs()); });
  acquire(ProductRuntimeBeginStage::VoiceHandlersConfigure,
          [&] { return voice_.configureHandlers(); });
  acquire(ProductRuntimeBeginStage::ControlHandlerRegister, [&] {
    return supervisor_.registerHandler(
        TaskLane::Control, &EspProductRuntime::controlHandler, this);
  });
  acquire(ProductRuntimeBeginStage::NetworkHandlerRegister, [&] {
    return supervisor_.registerHandler(
        TaskLane::Network, &EspProductRuntime::networkHandler, this);
  });
  acquire(ProductRuntimeBeginStage::NetworkTickRegister, [&] {
    return supervisor_.registerTickHandler(
        TaskLane::Network, &EspProductRuntime::networkTick, this, 10);
  });
  acquire(ProductRuntimeBeginStage::PortalHandlerRegister, [&] {
    return supervisor_.registerHandler(
        TaskLane::Portal, &EspProductRuntime::portalHandler, this);
  });
  acquire(ProductRuntimeBeginStage::PortalTickRegister, [&] {
    return supervisor_.registerTickHandler(
        TaskLane::Portal, &EspProductRuntime::portalTick, this, 100);
  });
  acquire(ProductRuntimeBeginStage::WifiInitialize,
          [&] { return wifi_.initialize(nowMs()); });
  acquire(ProductRuntimeBeginStage::SupervisorStart,
          [&] { return supervisor_.start(); });
  acquire(ProductRuntimeBeginStage::PowerAfterSupervisorStarted,
          [&] { return power_.afterSupervisorStarted(nowMs()); });
  if (status != ESP_OK) {
    const esp_err_t rollback = shutdownForRecovery();
    if (rollback != ESP_OK) {
      ESP_LOGE(kTag, "begin failed=%s rollback failed=%s",
               esp_err_to_name(status), esp_err_to_name(rollback));
      return rollback;
    }
    return status;
  }
  started_ = true;
  const EspWifiStationSnapshot wifi = wifi_.snapshot();
  ESP_LOGI(kTag, "runtime started; saved_wifi=%u phase=%s",
           static_cast<unsigned>(wifi.saved_credentials),
           wifiStationPhaseName(wifi.phase));
  return ESP_OK;
}

esp_err_t EspProductRuntime::shutdownForOtaAcquisition() {
  if (ota_acquisition_quiesced_)
    return wifi_.online() ? ESP_OK : ESP_ERR_INVALID_STATE;
  if (!started_ || shutdown_incomplete_) return ESP_ERR_INVALID_STATE;

  // This is deliberately a two-phase shutdown. All producer/tasks and normal
  // remote/local clients go first, while the single connected STA/netif stays
  // alive solely for EspOtaHttpsTransport. shutdownForRecovery() owns phase 2
  // and tears that final network owner down before reboot or Recovery.
  buttons_.disarm();
  const esp_err_t stopped = supervisor_.stop();
  if (stopped != ESP_OK) {
    shutdown_incomplete_ = true;
    return stopped;
  }

  esp_err_t first = ESP_OK;
  const auto record = [&](esp_err_t value) {
    if (value != ESP_OK && first == ESP_OK) first = value;
  };
  record(portal_.shutdown());
  voice_.shutdown();
  serial_diagnostics_.shutdown();
  power_.shutdown();
  inkloop_.shutdown();
  display_.shutdown();
  leds_.shutdown();
  buttons_.disarm();
  record(supervisor_.shutdown());
  if (first == ESP_OK && !wifi_.online()) first = ESP_ERR_INVALID_STATE;

  visible_provisioning_fingerprint_ = 0U;
  visible_pairing_fingerprint_ = 0U;
  next_chat_snapshot_ms_ = 0U;
  storage_maintenance_phase_ = StorageMaintenancePhase::Idle;
  storage_maintenance_deadline_ms_ = 0U;
  storage_maintenance_changed_ = false;
  storage_maintenance_available_ = true;
  storage_maintenance_display_pending_ = false;
  storage_maintenance_portal_pending_ = false;
  storage_maintenance_voice_pending_ = false;
  started_ = false;
  ota_acquisition_quiesced_ = first == ESP_OK;
  shutdown_incomplete_ = first != ESP_OK;
  return first;
}

esp_err_t EspProductRuntime::shutdownForRecovery() {
  // Runtime-only resources acquired after task start are released before the
  // Wi-Fi driver: buttons/normal tasks can no longer produce work, then HTTP,
  // mDNS, WSS and I2S are joined/closed while their underlying netif exists.
  buttons_.disarm();
  esp_err_t first = ESP_OK;
  const auto record = [&](esp_err_t value) {
    if (value != ESP_OK && first == ESP_OK) first = value;
  };
  if (supervisor_.started()) {
    const esp_err_t stopped = supervisor_.stop();
    record(stopped);
    if (stopped != ESP_OK && supervisor_.started()) {
      shutdown_incomplete_ = true;
      return stopped;
    }
  }

  record(portal_.shutdown());
  voice_.shutdown();
  serial_diagnostics_.shutdown();
  record(wifi_.shutdown());
  power_.shutdown();
  inkloop_.shutdown();
  display_.shutdown();
  leds_.shutdown();
  buttons_.disarm();
  record(supervisor_.shutdown());

  visible_provisioning_fingerprint_ = 0U;
  visible_pairing_fingerprint_ = 0U;
  next_chat_snapshot_ms_ = 0U;
  storage_maintenance_phase_ = StorageMaintenancePhase::Idle;
  storage_maintenance_deadline_ms_ = 0U;
  storage_maintenance_changed_ = false;
  storage_maintenance_available_ = true;
  storage_maintenance_display_pending_ = false;
  storage_maintenance_portal_pending_ = false;
  storage_maintenance_voice_pending_ = false;
  started_ = false;
  ota_acquisition_quiesced_ = false;
  shutdown_incomplete_ = first != ESP_OK;
  return first;
}

WorkDisposition EspProductRuntime::controlHandler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<EspProductRuntime*>(context)->handleControl(envelope)
             : WorkDisposition::Failed;
}

WorkDisposition EspProductRuntime::unavailableHandler(
    const WorkEnvelope& envelope, void* context) {
  (void)context;
  ESP_LOGW(kTag, "unavailable lane rejected class=%u opcode=%u request=%llu",
           static_cast<unsigned>(envelope.work_class),
           static_cast<unsigned>(envelope.opcode),
           static_cast<unsigned long long>(envelope.request_id));
  return WorkDisposition::Failed;
}

WorkDisposition EspProductRuntime::networkHandler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<EspProductRuntime*>(context)->handleNetwork(envelope)
             : WorkDisposition::Failed;
}

WorkDisposition EspProductRuntime::portalHandler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<EspProductRuntime*>(context)->handlePortal(envelope)
             : WorkDisposition::Failed;
}

void EspProductRuntime::networkTick(void* context) {
  if (context) static_cast<EspProductRuntime*>(context)->serviceNetwork();
}

void EspProductRuntime::portalTick(void* context) {
  if (context) static_cast<EspProductRuntime*>(context)->servicePortal();
}

void EspProductRuntime::serialDiagnosticCommand(
    diagnostics::SerialCommand command, void* context) {
  if (context) {
    static_cast<EspProductRuntime*>(context)->handleSerialDiagnosticCommand(
        command);
  }
}

void EspProductRuntime::handleSerialDiagnosticCommand(
    diagnostics::SerialCommand command) {
  diagnostics::SerialDiagnosticEvent event;
  switch (command) {
    case diagnostics::SerialCommand::Status: {
      portal::PortalStateSnapshot state;
      const bool state_ready = portal_.readSerialDiagnosticState(state) ==
          portal::PortalResult::Ok;
      const NativeVoiceSerialDiagnosticSnapshot voice =
          voice_.serialDiagnosticSnapshot();
      event.kind = diagnostics::SerialDiagnosticEventKind::Status;
      if (started_) event.flags |= diagnostics::StatusRuntimeStarted;
      if (wifi_.online()) event.flags |= diagnostics::StatusWifiOnline;
      if (state_ready && state.storage_ready)
        event.flags |= diagnostics::StatusStorageReady;
      if (display_.busy()) event.flags |= diagnostics::StatusDisplayBusy;
      if (voice.authorization_verified)
        event.flags |= diagnostics::StatusMyAiAuthorized;
      event.first = static_cast<uint8_t>(voice.activation_state);
      event.second = static_cast<uint8_t>(serialVoiceState(voice.voice_state));
      (void)serial_diagnostics_.postSerialDiagnosticEvent(event);
      return;
    }
    case diagnostics::SerialCommand::AlbumStatus: {
      const NativePortalAlbumDiagnosticSnapshot album =
          portal_.serialDiagnosticAlbum();
      event.kind = diagnostics::SerialDiagnosticEventKind::Album;
      event.flags = album.ready ? 1U : 0U;
      event.first = static_cast<uint32_t>(album.total_items);
      event.second = static_cast<uint32_t>(album.current_one_based);
      (void)serial_diagnostics_.postSerialDiagnosticEvent(event);
      return;
    }
    case diagnostics::SerialCommand::VoiceTap: {
      const AdmissionResult admitted = voice_.enqueueTopButton();
      if (admitted != AdmissionResult::Admitted) {
        event.kind = diagnostics::SerialDiagnosticEventKind::VoiceError;
        event.code = static_cast<uint8_t>(admitted);
        (void)serial_diagnostics_.postSerialDiagnosticEvent(event);
      }
      return;
    }
    case diagnostics::SerialCommand::AigcTest: {
      const AdmissionResult admitted =
          voice_.enqueueDiagnosticImageGeneration(kSerialAigcPrompt);
      event.kind = diagnostics::SerialDiagnosticEventKind::AigcDiagnostic;
      event.flags = admitted == AdmissionResult::Admitted ? 1U : 0U;
      (void)serial_diagnostics_.postSerialDiagnosticEvent(event);
      if (admitted != AdmissionResult::Admitted) {
        event.kind = diagnostics::SerialDiagnosticEventKind::AigcError;
        event.code = static_cast<uint8_t>(admitted);
        (void)serial_diagnostics_.postSerialDiagnosticEvent(event);
      }
      return;
    }
    case diagnostics::SerialCommand::None:
      return;
  }
}

WorkDisposition EspProductRuntime::handleControl(
    const WorkEnvelope& envelope) {
  if (envelope.kind == EnvelopeKind::Command &&
      envelope.work_class == WorkClass::Control &&
      envelope.opcode == productOpcode(ProductOpcode::AlbumRefreshStarting)) {
    const AdmissionResult announced = voice_.enqueueAlbumOrdinal(
        static_cast<size_t>(envelope.flags) + 1U, true);
    return announced == AdmissionResult::Admitted ||
                   announced == AdmissionResult::NotReady
               ? WorkDisposition::Complete
               : WorkDisposition::Busy;
  }
  // AIGC and Inkloop asset commits occur outside NativePortalOwner. Any
  // Display result means the persisted album/current selection may have
  // changed, so invalidate the browser-facing cache before the owning service
  // consumes the result. This call is atomic only; Portal performs the read.
  if (envelope.kind == EnvelopeKind::Result &&
      envelope.work_class == WorkClass::Display) {
    portal_.requestAlbumRefresh();
  }
  if (voice_.handleControlResult(envelope))
    return WorkDisposition::Complete;
  if (inkloop_.handleControlResult(envelope))
    return WorkDisposition::Complete;
  if (envelope.kind == EnvelopeKind::Result &&
      envelope.work_class == WorkClass::LedStatus)
    return WorkDisposition::Complete;
  if (envelope.kind == EnvelopeKind::Result &&
      envelope.work_class == WorkClass::Display)
    return WorkDisposition::Complete;
  if (envelope.kind != EnvelopeKind::Result ||
      envelope.work_class != WorkClass::Button)
    return WorkDisposition::Failed;
  if (envelope.disposition == WorkDisposition::Cancelled)
    return WorkDisposition::Complete;
  if (envelope.disposition != WorkDisposition::Complete)
    return WorkDisposition::Failed;

  power_.noteButtonActivity(nowMs());

  if (envelope.opcode == productOpcode(ProductOpcode::RawButtonVoice)) {
    return voice_.enqueueTopButton() == AdmissionResult::Admitted
               ? WorkDisposition::Complete
               : WorkDisposition::Busy;
  }
  if (envelope.opcode == productOpcode(ProductOpcode::RawButtonPrevious) ||
      envelope.opcode == productOpcode(ProductOpcode::RawButtonNext)) {
    size_t ordinal = 0;
    const AlbumStepResult selected = display_.selectRelative(
        envelope.opcode == productOpcode(ProductOpcode::RawButtonNext) ? 1
                                                                       : -1,
        ordinal);
    if (selected == AlbumStepResult::Selected) {
      const AdmissionResult announced =
          voice_.enqueueAlbumOrdinal(ordinal + 1U, false);
      return announced == AdmissionResult::Admitted ||
                     announced == AdmissionResult::NotReady
                 ? WorkDisposition::Complete
                 : WorkDisposition::Busy;
    }
    if (selected == AlbumStepResult::Empty) {
      voice_.enqueueLocalPrompt(LocalPrompt::AlbumEmpty);
      return WorkDisposition::Complete;
    }
    voice_.enqueueLocalPrompt(LocalPrompt::PleaseWait);
    return selected == AlbumStepResult::Busy ? WorkDisposition::Busy
                                              : WorkDisposition::Failed;
  }
  return WorkDisposition::Failed;
}

WorkDisposition EspProductRuntime::handleNetwork(
    const WorkEnvelope& envelope) {
  return voice_.handleNetworkCommand(envelope);
}

WorkDisposition EspProductRuntime::handlePortal(
    const WorkEnvelope& envelope) {
  return voice_.handlePortalCommand(envelope);
}

void EspProductRuntime::serviceNetwork() {
  wifi_.tick(nowMs());
  voice_.networkTick(wifi_.online());
}

void EspProductRuntime::serviceStableDisplayPages(
    const EspWifiStationSnapshot& wifi,
    const NativeMyAiOnboardingSnapshot& onboarding) {
  if (wifi.provisioning_ap) {
    visible_pairing_fingerprint_ = 0U;
    const std::array<char, 64> access = wifi_.localAccessCode();
    const uint64_t fingerprint = displayFingerprint(
        wifi.provisioning_ssid.data(), access.data(), "inkloop.local",
        "192.168.4.1");
    if (visible_provisioning_fingerprint_ != fingerprint) {
      const NativeDisplayPageRequestResult result =
          display_.requestProvisioningPage({
              wifi.provisioning_ssid.data(), access.data(),
              "inkloop.local", "192.168.4.1"});
      // Only Unchanged proves that the complete stable page is already on the
      // e-paper. Accepted/pending work is retried until Display confirms that
      // state, so allocation/panel failures do not strand a blank screen.
      if (result == NativeDisplayPageRequestResult::Unchanged)
        visible_provisioning_fingerprint_ = fingerprint;
    }
    return;
  }
  visible_provisioning_fingerprint_ = 0U;
  if (!onboarding.pairing_view_available) {
    visible_pairing_fingerprint_ = 0U;
    // This is deliberately retried on every stable Portal tick. Busy,
    // maintenance and transient decode/storage failures therefore cannot
    // strand a stale provisioning or pairing page on the e-paper.
    display_.requestAlbumRestore();
    return;
  }
  const uint64_t fingerprint = displayFingerprint(
      onboarding.device_code.data(), onboarding.binding_url.data());
  if (visible_pairing_fingerprint_ == fingerprint) return;
  const NativeDisplayPageRequestResult result =
      display_.requestMyAiPairingPage(
          {onboarding.device_code.data(), onboarding.binding_url.data()});
  if (result == NativeDisplayPageRequestResult::Unchanged)
    visible_pairing_fingerprint_ = fingerprint;
}

void EspProductRuntime::servicePortal() {
  serial_diagnostics_.service();
  const uint32_t now = nowMs();
  serviceStorageMaintenanceFinalization(now);
  if (storage_maintenance_phase_ != StorageMaintenancePhase::Idle) {
    power_.tick(now);
    return;
  }
  if (power_.recovering()) {
    power_.tick(now);
    return;
  }
  const EspWifiStationSnapshot wifi = wifi_.snapshot();
  const NativeMyAiOnboardingSnapshot onboarding =
      voice_.onboardingSnapshot();
  if (!power_.deferBackgroundPanel(now))
    serviceStableDisplayPages(wifi, onboarding);
  voice_.tryConsumeLocalChatSnapshot(portal_);
  const uint32_t portal_access = portal_.lastAccessMs();
  const bool portal_recent = portal_access != 0U &&
      static_cast<uint32_t>(now - portal_access) < 30000U;
  if (portal_.running() && portal_recent &&
      static_cast<int32_t>(now - next_chat_snapshot_ms_) >= 0) {
    if (voice_.requestLocalChatSnapshot(0, kNativeLocalChatPageItems) ==
        AdmissionResult::Admitted) {
      next_chat_snapshot_ms_ = now + 2000U;
    } else {
      next_chat_snapshot_ms_ = now + 500U;
    }
  }
  // Portal, AIGC and Inkloop storage work execute serially on this task.  Let
  // Portal first drain already-admitted work even while AIGC is active; using
  // voice_.portalBusy() as this gate created a circular wait where AIGC could
  // not download until the Portal queue emptied, while Portal was forbidden
  // from emptying that queue.  Re-sample every owner between stages because a
  // drained Portal command can enqueue Display work.
  portal_.tick(
      wifi_.online(),
      SlowIoArbitration::portalMayDrain(display_.busy(), inkloop_.busy()));
  voice_.portalTick(SlowIoArbitration::aigcMayMutate(
      display_.busy(), portal_.mutationBusy(), inkloop_.busy()));
  inkloop_.portalTick(wifi_.online(),
                      SlowIoArbitration::inkloopMayRun(
                          voice_.portalBusy(), display_.busy(),
                          portal_.mutationBusy()),
                      !power_.deferBackgroundPanel(now), onboarding);
  power_.tick(now);
}

}  // namespace inkloop
