#pragma once

#include <cstdint>
#include <string>

#include "inkloop/board.hpp"
#include "inkloop/button_input.hpp"
#include "inkloop/esp_wifi_station.hpp"
#include "inkloop/native_display_service.hpp"
#include "inkloop/native_inkloop_service.hpp"
#include "inkloop/native_portal_owner.hpp"
#include "inkloop/native_power_owner.hpp"
#include "inkloop/native_voice_service.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/status_led_owner.hpp"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage_maintenance.hpp"

namespace inkloop {

enum class ProductRuntimeBeginStage : uint8_t {
  SupervisorInitialize,
  ButtonsConfigure,
  LedsConfigure,
  DisplayConfigure,
  VoiceInitialize,
  InkloopInitialize,
  PortalInitialize,
  PowerInitialize,
  VoiceHandlersConfigure,
  ControlHandlerRegister,
  NetworkHandlerRegister,
  NetworkTickRegister,
  PortalHandlerRegister,
  PortalTickRegister,
  WifiInitialize,
  SupervisorStart,
  PowerAfterSupervisorStarted,
  Count,
};

using ProductRuntimeBeginFaultInjector = esp_err_t (*)(
    ProductRuntimeBeginStage stage, void* context);

// Incremental native product composition root. Every unavailable operation is
// rejected explicitly; successful dispositions are never used as placeholders.
// As feature owners are migrated they replace the corresponding typed handler
// without changing board adapters or cross-task contracts.
class EspProductRuntime final : public IStorageMaintenanceCoordinator {
 public:
  EspProductRuntime(IBoardAdapter& board,
                    storage::EspStorageMountOwner& storage,
                    storage::AssetStoragePreference asset_preference);
  ~EspProductRuntime();

  esp_err_t begin();
  // Stops every normal product writer/worker/network owner. ESP_OK is the
  // sole handoff condition under which a recovery network owner may start.
  // The method is bounded, idempotent, and also backs begin() rollback.
  esp_err_t shutdownForRecovery();
  // Quiesces every normal task, HTTP/audio/display/storage writer but retains
  // the already-connected sole STA owner for one bounded verified OTA fetch.
  // The caller must finish with shutdownForRecovery() before rebooting, whether
  // acquisition succeeds or fails. Never call this from a managed lane.
  esp_err_t shutdownForOtaAcquisition();
  bool otaAcquisitionQuiesced() const { return ota_acquisition_quiesced_; }
  // Deterministic begin-stage fault seam for lifecycle tests. It is rejected
  // after supervisor acquisition and is never installed by production code.
  esp_err_t setBeginFaultInjector(ProductRuntimeBeginFaultInjector injector,
                                  void* context);
  esp_err_t attachPortalSettingsOwner(IPortalSettingsOwner& owner) {
    return portal_.attachSettingsOwner(owner);
  }
  esp_err_t attachPortalAlbumMutationOwner(
      IPortalAlbumMutationOwner& owner) {
    return portal_.attachAlbumMutationOwner(owner);
  }
  esp_err_t attachPortalFirmwareUpdateOwner(
      IPortalFirmwareUpdateOwner& owner) {
    return portal_.attachFirmwareUpdateOwner(owner);
  }
  esp_err_t attachLocalTools(local_tools::ILocalToolsAdapter& adapter) {
    return voice_.attachLocalTools(adapter);
  }
  esp_err_t setLocalAccessCodeOverride(const std::string& value) {
    return wifi_.setLocalAccessCodeOverride(value);
  }
  StorageMaintenanceResult formatTfCardConfirmed() override;
  bool started() const { return started_; }
  EspWifiStationSnapshot wifiSnapshot() const { return wifi_.snapshot(); }
  SupervisorDiagnostics diagnostics() const {
    return supervisor_.diagnostics();
  }
  RuntimeTelemetrySnapshot telemetry() const {
    return supervisor_.telemetry();
  }

 private:
  static WorkDisposition controlHandler(const WorkEnvelope& envelope,
                                        void* context);
  static WorkDisposition unavailableHandler(const WorkEnvelope& envelope,
                                            void* context);
  static WorkDisposition networkHandler(const WorkEnvelope& envelope,
                                        void* context);
  static WorkDisposition portalHandler(const WorkEnvelope& envelope,
                                       void* context);
  static void networkTick(void* context);
  static void portalTick(void* context);
  WorkDisposition handleControl(const WorkEnvelope& envelope);
  WorkDisposition handleNetwork(const WorkEnvelope& envelope);
  WorkDisposition handlePortal(const WorkEnvelope& envelope);
  void serviceNetwork();
  void servicePortal();
  void serviceStableDisplayPages(
      const EspWifiStationSnapshot& wifi,
      const NativeMyAiOnboardingSnapshot& onboarding);
  void serviceStorageMaintenanceFinalization(uint32_t now_ms);
  void releaseStorageMaintenanceOwners();
  static uint32_t nowMs();

  storage::EspStorageMountOwner& storage_;
  storage::PosixAtomicAlbumStore* selected_album_store_ = nullptr;
  storage::PosixAtomicAlbumStore* sd_album_store_ = nullptr;
  bool selected_album_is_sd_ = false;
  RuntimeSupervisor supervisor_{};
  EspButtonInputOwner buttons_;
  EspStatusLedOwner leds_;
  EspWifiStationOwner wifi_{};
  NativeDisplayService display_;
  NativeVoiceService voice_;
  NativeInkloopService inkloop_;
  NativePortalOwner portal_;
  NativePowerOwner power_;
  uint64_t visible_provisioning_fingerprint_ = 0;
  uint64_t visible_pairing_fingerprint_ = 0;
  uint32_t next_chat_snapshot_ms_ = 0;
  enum class StorageMaintenancePhase : uint8_t { Idle, Finalizing };
  StorageMaintenancePhase storage_maintenance_phase_ =
      StorageMaintenancePhase::Idle;
  uint32_t storage_maintenance_deadline_ms_ = 0;
  bool storage_maintenance_changed_ = false;
  bool storage_maintenance_available_ = true;
  bool storage_maintenance_display_pending_ = false;
  bool storage_maintenance_portal_pending_ = false;
  bool storage_maintenance_voice_pending_ = false;
  bool started_ = false;
  bool shutdown_incomplete_ = false;
  bool ota_acquisition_quiesced_ = false;
  ProductRuntimeBeginFaultInjector begin_fault_injector_ = nullptr;
  void* begin_fault_context_ = nullptr;
};

}  // namespace inkloop
