#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "inkloop/board.hpp"
#include "inkloop/esp_ota_boot_health.hpp"
#include "inkloop/native_device_state_owner.hpp"
#include "inkloop/native_settings_migration_gate.hpp"
#include "inkloop/ota_runtime_health.hpp"
#include "inkloop/product_runtime.hpp"
#include "inkloop/recovery/recovery_network_owner.hpp"
#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/persistence_compatibility.hpp"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage/esp_upgrade_boot_audit.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "inkloop/task_topology.hpp"
#include "ota_outcome_journal.hpp"
#include "ota_update_owner.hpp"
#include "recovery_action_owner.hpp"

namespace {
constexpr char kTag[] = "inkloop";

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void logMainStackWatermark(const char* stage) {
  ESP_LOGI(kTag, "BOOT_STACK:%s free_min_bytes=%u", stage,
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

bool priorResetWasFatal() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

void logOtaObservation(const inkloop::EspOtaBootHealthObservation& value) {
  ESP_LOGI(kTag, "OTA_HEALTH:%s image=%s state=%s action=%s reason=%s",
           inkloop::espOtaBootHealthCodeName(value.code),
           inkloop::otaRunningImageStateName(value.observed_image),
           inkloop::otaBootHealthStateName(value.decision.state),
           inkloop::otaBootHealthActionName(value.decision.action),
           inkloop::otaBootHealthReasonName(value.decision.reason));
}

template <size_t Size>
void copyFixed(const char* value, std::array<char, Size>& output) {
  output.fill('\0');
  if (!value || output.empty()) return;
  const size_t length = strnlen(value, output.size() - 1U);
  std::copy(value, value + length, output.begin());
}

bool countableProbe(inkloop::storage::RecordProbe value) {
  return value != inkloop::storage::RecordProbe::Missing &&
      value != inkloop::storage::RecordProbe::IoError;
}

inkloop::recovery::RecoveryRecordCounts recoveryRecordCounts(
    const inkloop::storage::EspStorageMountOwner& storage) {
  inkloop::recovery::RecoveryRecordCounts output;
  const char* root = storage.taskRoot();
  if (!root) root = storage.auditInternalRoot();
  if (root) {
    const inkloop::storage::PosixUpgradeInventory files(root);
    const auto probes = files.inspectFiles();
    output.files = static_cast<uint32_t>(std::count_if(
        probes.begin(), probes.end(), countableProbe));
  }
  const inkloop::storage::EspNvsUpgradeInventory nvs;
  const auto probes = nvs.inspect();
  output.nvs_namespaces = static_cast<uint32_t>(std::count_if(
      probes.begin(), probes.end(), countableProbe));
  output.ota_slots = 2U;
  return output;
}

inkloop::recovery::RecoveryDiagnosticSnapshot recoveryDiagnostic(
    const inkloop::BoardDescriptor& board,
    inkloop::recovery::RecoveryReason reason,
    inkloop::recovery::RecoveryPhase phase,
    inkloop::recovery::RecoveryOutcome outcome,
    const inkloop::recovery::RecoveryRecordCounts& records = {}) {
  inkloop::recovery::RecoveryDiagnosticSnapshot output;
  output.reason = reason;
  output.phase = phase;
  output.outcome = outcome;
  const esp_app_desc_t* app = esp_app_get_description();
  copyFixed(app && app->version[0] != '\0' ? app->version : "inkloop-idf",
            output.firmware_id);
  copyFixed(board.id, output.board_id);
  output.records = records;
  output.normal_startup_refused = true;
  return output;
}

inkloop::recovery::RecoveryDiagnosticSnapshot auditRecoveryDiagnostic(
    const inkloop::BoardDescriptor& board,
    const inkloop::storage::UpgradeAuditReport& report,
    const inkloop::recovery::RecoveryRecordCounts& records) {
  using inkloop::recovery::RecoveryOutcome;
  using inkloop::recovery::RecoveryPhase;
  using inkloop::recovery::RecoveryReason;
  using inkloop::storage::UpgradeAuditResult;
  switch (report.result) {
    case UpgradeAuditResult::DisplayResolutionRequired:
      return recoveryDiagnostic(board, RecoveryReason::MigrationRefused,
                                RecoveryPhase::Migration,
                                RecoveryOutcome::RequiresOperator, records);
    case UpgradeAuditResult::RecoveryRequired:
    case UpgradeAuditResult::Ambiguous:
      return recoveryDiagnostic(board, RecoveryReason::StorageIntegrityRefused,
                                RecoveryPhase::StorageAudit,
                                RecoveryOutcome::RequiresOperator, records);
    case UpgradeAuditResult::SourceUnavailable:
      return recoveryDiagnostic(board, RecoveryReason::StorageIntegrityRefused,
                                RecoveryPhase::StorageAudit,
                                RecoveryOutcome::Failed, records);
    case UpgradeAuditResult::Fresh:
    case UpgradeAuditResult::Compatible:
      break;
  }
  return recoveryDiagnostic(board, RecoveryReason::BootAuditRefused,
                            RecoveryPhase::BootAudit,
                            RecoveryOutcome::Incomplete, records);
}

class FixedRecoveryDiagnosticCache final
    : public inkloop::recovery::IRecoveryDiagnosticCache {
 public:
  explicit FixedRecoveryDiagnosticCache(
      const inkloop::recovery::RecoveryDiagnosticSnapshot& snapshot)
      : snapshot_(snapshot) {}

  inkloop::recovery::RecoveryReadResult readRecoveryDiagnostic(
      inkloop::recovery::RecoveryDiagnosticSnapshot& output) const override {
    output = snapshot_;
    return snapshot_.normal_startup_refused
        ? inkloop::recovery::RecoveryReadResult::Ok
        : inkloop::recovery::RecoveryReadResult::InvalidData;
  }

 private:
  inkloop::recovery::RecoveryDiagnosticSnapshot snapshot_{};
};

inkloop::portal::PortalFirmwareUpdateCode portalOtaCode(
    inkloop::OtaUpdateCode code) {
  using inkloop::OtaUpdateCode;
  using inkloop::portal::PortalFirmwareUpdateCode;
  switch (code) {
    case OtaUpdateCode::Ok:
    case OtaUpdateCode::Ready:
    case OtaUpdateCode::ImageSelected:
      return PortalFirmwareUpdateCode::None;
    case OtaUpdateCode::ConfigurationMissing:
    case OtaUpdateCode::ManifestUrlRejected:
    case OtaUpdateCode::PlaceholderEndpointRejected:
    case OtaUpdateCode::PublicKeyRejected:
    case OtaUpdateCode::DeadlineRejected:
    case OtaUpdateCode::Disabled:
      return PortalFirmwareUpdateCode::ConfigurationInvalid;
    case OtaUpdateCode::DeadlineExceeded:
      return PortalFirmwareUpdateCode::TimedOut;
    case OtaUpdateCode::ManifestFetchFailed:
    case OtaUpdateCode::ImageFetchFailed:
      return PortalFirmwareUpdateCode::NetworkUnavailable;
    case OtaUpdateCode::ManifestRejected:
      return PortalFirmwareUpdateCode::ManifestRejected;
    case OtaUpdateCode::ImageOriginMismatch:
      return PortalFirmwareUpdateCode::ImageRejected;
    case OtaUpdateCode::StagingBeginFailed:
    case OtaUpdateCode::StagingFinishFailed:
      return PortalFirmwareUpdateCode::StagingFailed;
    case OtaUpdateCode::InvalidRequestId:
    case OtaUpdateCode::DuplicateRequest:
    case OtaUpdateCode::Busy:
    case OtaUpdateCode::NoRequest:
    case OtaUpdateCode::RequestMismatch:
    case OtaUpdateCode::InvalidTerminalCode:
    case OtaUpdateCode::QuiesceFailed:
    case OtaUpdateCode::PlatformUnavailable:
    case OtaUpdateCode::VerifierUnavailable:
    case OtaUpdateCode::AcquisitionInvalidState:
    case OtaUpdateCode::AcquisitionConfigurationRejected:
      return PortalFirmwareUpdateCode::InternalError;
  }
  return PortalFirmwareUpdateCode::InternalError;
}

class PortalOtaUpdateBridge final
    : public inkloop::IPortalFirmwareUpdateOwner {
 public:
  PortalOtaUpdateBridge(inkloop::OtaUpdateOwner& owner,
                        const std::atomic<bool>& boot_health_ready,
                        const inkloop::OtaOutcomeJournal& outcomes)
      : owner_(owner),
        boot_health_ready_(boot_health_ready),
        outcomes_(outcomes) {}

  inkloop::portal::PortalResult readPortalFirmwareUpdate(
      inkloop::portal::PortalFirmwareUpdateSnapshot& output) const override {
    using inkloop::OtaUpdateState;
    using inkloop::portal::PortalFirmwareUpdatePhase;
    output = inkloop::portal::PortalFirmwareUpdateSnapshot{};
    const inkloop::OtaUpdateSnapshot source = owner_.snapshot();
    if (!boot_health_ready_.load(std::memory_order_acquire)) {
      return inkloop::portal::PortalResult::Ok;
    }
    output.configured = source.state != OtaUpdateState::Disabled;
    output.code = portalOtaCode(source.code);
    switch (source.state) {
      case OtaUpdateState::Disabled:
        output.phase = PortalFirmwareUpdatePhase::Unavailable;
        break;
      case OtaUpdateState::Idle:
      case OtaUpdateState::Failed:
        output.phase = output.configured
            ? PortalFirmwareUpdatePhase::Ready
            : PortalFirmwareUpdatePhase::Unavailable;
        break;
      case OtaUpdateState::Requested:
      case OtaUpdateState::Running:
      case OtaUpdateState::Acquiring:
      case OtaUpdateState::ImageSelected:
        output.accepted_offline = true;
        output.phase = PortalFirmwareUpdatePhase::AcceptedOffline;
        output.code = inkloop::portal::PortalFirmwareUpdateCode::None;
        break;
    }
    if (source.state == OtaUpdateState::Disabled ||
        source.state == OtaUpdateState::Idle) {
      const inkloop::OtaOutcomeSnapshot outcome = outcomes_.snapshot();
      if (outcome.available) {
        switch (outcome.kind) {
          case inkloop::OtaOutcomeKind::AcquisitionFailed:
            output.code = portalOtaCode(outcome.code);
            break;
          case inkloop::OtaOutcomeKind::Confirmed:
            output.code =
                inkloop::portal::PortalFirmwareUpdateCode::UpdateConfirmed;
            break;
          case inkloop::OtaOutcomeKind::RollbackObserved:
            output.code =
                inkloop::portal::PortalFirmwareUpdateCode::UpdateRolledBack;
            break;
          case inkloop::OtaOutcomeKind::ImageSelected:
            output.code =
                inkloop::portal::PortalFirmwareUpdateCode::InternalError;
            break;
          case inkloop::OtaOutcomeKind::None:
            break;
        }
      }
    }
    return inkloop::portal::PortalResult::Ok;
  }

  inkloop::portal::PortalResult requestPortalFirmwareUpdate(
      uint64_t request_id) override {
    if (!boot_health_ready_.load(std::memory_order_acquire))
      return inkloop::portal::PortalResult::Busy;
    switch (owner_.request(request_id)) {
      case inkloop::OtaUpdateCode::Ok:
        return inkloop::portal::PortalResult::Ok;
      case inkloop::OtaUpdateCode::Disabled:
        return inkloop::portal::PortalResult::Unavailable;
      case inkloop::OtaUpdateCode::DuplicateRequest:
      case inkloop::OtaUpdateCode::Busy:
        return inkloop::portal::PortalResult::Busy;
      default:
        return inkloop::portal::PortalResult::InvalidData;
    }
  }

 private:
  inkloop::OtaUpdateOwner& owner_;
  const std::atomic<bool>& boot_health_ready_;
  const inkloop::OtaOutcomeJournal& outcomes_;
};

[[noreturn]] void runRecoveryNetworkWithNvsStatus(
    const inkloop::recovery::RecoveryDiagnosticSnapshot& diagnostic,
    esp_err_t nvs_status,
    inkloop::storage::EspStorageMountOwner* storage,
    const std::string& local_access_override,
    inkloop::recovery::RecoveryWifiStoragePolicy wifi_storage) {
  FixedRecoveryDiagnosticCache cache(diagnostic);
  std::unique_ptr<inkloop::EspRecoveryActionOwner> actions;
  if (storage) {
    actions.reset(new (std::nothrow) inkloop::EspRecoveryActionOwner(*storage));
    if (!actions || !actions->ready()) actions.reset();
  }
  inkloop::recovery::RecoveryNetworkModeOwner recovery(
      cache, actions.get(), actions.get(), wifi_storage);
  bool initialized = false;
  uint32_t next_log_ms = 0U;
  unsigned forced_restart_stop_attempts = 0U;
  constexpr unsigned kMaximumForcedRestartStopAttempts = 8U;
  for (;;) {
    const uint32_t now = nowMs();
    if (!initialized) {
      esp_err_t status = nvs_status;
      if (status == ESP_OK && !local_access_override.empty()) {
        status = recovery.setLocalAccessCodeOverride(local_access_override);
      }
      if (status == ESP_OK) status = recovery.initialize(now);
      initialized = status == ESP_OK;
      if (!initialized && static_cast<int32_t>(now - next_log_ms) >= 0) {
        ESP_LOGE(kTag,
                 "recovery network unavailable: %s; no erase fallback",
                 esp_err_to_name(status));
        next_log_ms = now + 5000U;
      }
    } else {
      recovery.tick(now);
      const bool forced_restart =
          actions && actions->forcedRestartReady(now);
      const bool clean_restart = actions && actions->restartReady(now);
      if (forced_restart || clean_restart) {
        const esp_err_t stopped = recovery.stop();
        if (stopped == ESP_OK) {
          if (forced_restart) {
            ESP_LOGI(kTag,
                     "recovery write revocation failed; network stopped; "
                     "forcing fresh boot audit");
          } else {
            ESP_LOGI(kTag,
                     "recovery actions passed fresh boot audit; restarting");
          }
          vTaskDelay(pdMS_TO_TICKS(100U));
          esp_restart();
        }
        if (forced_restart &&
            ++forced_restart_stop_attempts >=
                kMaximumForcedRestartStopAttempts) {
          ESP_LOGE(kTag,
                   "recovery network would not quiesce after write "
                   "revocation failure; forcing hardware restart");
          vTaskDelay(pdMS_TO_TICKS(100U));
          esp_restart();
        }
        if (static_cast<int32_t>(now - next_log_ms) >= 0) {
          ESP_LOGE(kTag, "recovery restart quiesce failed: %s",
                   esp_err_to_name(stopped));
          next_log_ms = now + 5000U;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(initialized ? 100U : 1000U));
  }
}

// After the boot audit starts, Recovery never retries generic NVS
// initialization, repair, or erase. Its Wi-Fi owner is explicitly RAM-only
// (nvs_enable=false), so even blank/corrupt/uninitialized NVS must not make the
// diagnostic Portal unreachable. A ready audit owner remains useful evidence
// to typed recovery actions, but is not a network prerequisite.
[[noreturn]] void runRecoveryNetwork(
    const inkloop::recovery::RecoveryDiagnosticSnapshot& diagnostic,
    inkloop::storage::EspNvsBootMountOwner* nvs_boot_mount,
    inkloop::storage::EspStorageMountOwner* storage = nullptr,
    bool recovery_actions_requested = false,
    const std::string& local_access_override = {}) {
  const esp_err_t nvs_read_only = nvs_boot_mount
      ? nvs_boot_mount->prepareRecoveryReadOnlyOrDeinit()
      : ESP_ERR_INVALID_STATE;
  const bool nvs_inventory_available = nvs_read_only == ESP_OK &&
      nvs_boot_mount && nvs_boot_mount->recoveryReadOnlyReady();
  const bool nvs_writes_revoked = !nvs_boot_mount ||
      nvs_boot_mount->recoveryWritesRevoked();
  const esp_err_t storage_read_only = storage
      ? storage->prepareRecoveryReadOnly()
      : ESP_ERR_INVALID_STATE;
  const bool storage_writes_revoked = !storage ||
      storage->recoveryWritesRevoked();
  const bool storage_inventory_available = storage_read_only == ESP_OK &&
      storage && storage->recoveryReadOnlyReady();
  if (!nvs_inventory_available) {
    ESP_LOGW(kTag,
             "NVS readonly inventory unavailable (%s); volatile Recovery "
             "requires confirmed writer revocation",
             esp_err_to_name(nvs_read_only));
  }
  if (!nvs_writes_revoked) {
    ESP_LOGE(kTag,
             "NVS Product writer could not be revoked; refusing Recovery "
             "network startup");
  }
  if (storage && !storage_inventory_available) {
    ESP_LOGW(kTag,
             "LittleFS readonly inventory unavailable (%s); Recovery "
             "actions disabled",
             esp_err_to_name(storage_read_only));
  }
  if (!storage_writes_revoked) {
    ESP_LOGE(kTag,
             "LittleFS Product writer could not be revoked; refusing "
             "Recovery network startup");
  }
  const bool all_product_writes_revoked =
      nvs_writes_revoked && storage_writes_revoked;
  const bool recovery_actions_available = recovery_actions_requested &&
      nvs_inventory_available && storage_inventory_available &&
      all_product_writes_revoked;
  runRecoveryNetworkWithNvsStatus(
      diagnostic,
      all_product_writes_revoked ? ESP_OK : ESP_ERR_INVALID_STATE,
      recovery_actions_available ? storage : nullptr,
      local_access_override,
      inkloop::recovery::RecoveryWifiStoragePolicy::VolatileRam);
}

// OTA running-image health is evaluated before any upgrade-audit owner exists.
// No Product NVS writer has been admitted at this point, and Recovery must not
// initialize, repair, or otherwise mutate NVS merely to become reachable.
// Volatile STA/AP credentials keep this path fail-closed and RAM-only.
[[noreturn]] void runEarlyOtaRecoveryNetwork(
    const inkloop::recovery::RecoveryDiagnosticSnapshot& diagnostic) {
  runRecoveryNetworkWithNvsStatus(
      diagnostic, ESP_OK, nullptr, {},
      inkloop::recovery::RecoveryWifiStoragePolicy::VolatileRam);
}

[[noreturn]] void runRecoveryAfterProductFailure(
    inkloop::EspProductRuntime& runtime,
    const inkloop::recovery::RecoveryDiagnosticSnapshot& diagnostic,
    inkloop::storage::EspNvsBootMountOwner& nvs_boot_mount,
    inkloop::storage::EspStorageMountOwner& storage,
    const std::string& local_access_override) {
  constexpr unsigned kMaximumQuiesceAttempts = 8U;
  for (unsigned attempt = 1U; attempt <= kMaximumQuiesceAttempts; ++attempt) {
    const esp_err_t quiesced = runtime.shutdownForRecovery();
    if (quiesced == ESP_OK) {
      ESP_LOGI(kTag,
               "normal product owners quiesced; entering sole-owner recovery");
      runRecoveryNetwork(
          diagnostic, &nvs_boot_mount, &storage, false,
          local_access_override);
    }
    ESP_LOGE(kTag, "product recovery quiesce attempt=%u/%u failed: %s",
             attempt, kMaximumQuiesceAttempts, esp_err_to_name(quiesced));
    vTaskDelay(pdMS_TO_TICKS(250U));
  }

  // Starting Recovery after an incomplete teardown could race the old STA,
  // provisioning HTTP, mDNS, WSS or storage writers. A clean reboot is the
  // only safe fallback and will repeat the audit before any writer starts.
  ESP_LOGE(kTag,
           "product owners could not quiesce; restarting without recovery "
           "network handoff");
  vTaskDelay(pdMS_TO_TICKS(100U));
  esp_restart();
  for (;;) vTaskDelay(portMAX_DELAY);
}
}

extern "C" void app_main(void) {
  const inkloop::BoardDescriptor board = inkloop::board_descriptor();

  // Observe a pending image before any storage, board or product work. This
  // starts the bounded rollback deadline at the earliest safe point instead of
  // accidentally granting a broken image extra time during initialization.
  const inkloop::OtaBootHealthConfig ota_health_config =
      inkloop::productionOtaBootHealthConfig();
  static inkloop::OtaBootHealthCore ota_health_core(ota_health_config);
  static inkloop::EspOtaBootHealthAdapter ota_health(
      ota_health_core, inkloop::systemEspOtaFunctions());
  inkloop::OtaBootStageState ota_stage;
  ota_stage.fatal_status_observed = true;
  ota_stage.fatal_status_clear = !priorResetWasFatal();
  const inkloop::RuntimeTelemetrySnapshot empty_runtime{};
  inkloop::EspOtaBootHealthObservation ota_observation = ota_health.tick(
      inkloop::composeOtaBootHealthEvidence(
          nowMs(), ota_stage, empty_runtime));
  logOtaObservation(ota_observation);
  bool ota_pending = ota_observation.observed_image ==
      inkloop::OtaRunningImageState::PendingVerify;
  static std::atomic<bool> ota_update_allowed{!ota_pending};
  if (ota_observation.code == inkloop::EspOtaBootHealthCode::InvalidFunctions ||
      ota_observation.code == inkloop::EspOtaBootHealthCode::StateReadFailed ||
      ota_observation.decision.state == inkloop::OtaBootHealthState::Refused) {
    ESP_LOGE(kTag, "OTA running-image state is unsafe; refusing startup");
    runEarlyOtaRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::OtaHealthRefused,
        inkloop::recovery::RecoveryPhase::OtaHealth,
        inkloop::recovery::RecoveryOutcome::Failed));
  }
  auto failPendingBoot = [&](const char* stage) {
    if (!ota_pending) return;
    ota_stage.explicit_fatal_health_failure = true;
    const inkloop::EspOtaBootHealthObservation failed = ota_health.tick(
        inkloop::composeOtaBootHealthEvidence(
            nowMs(), ota_stage, empty_runtime));
    ESP_LOGE(kTag, "pending image failed during %s", stage);
    logOtaObservation(failed);
  };

  if (!board.valid()) {
    ESP_LOGE(kTag, "board descriptor rejected before hardware initialization");
    failPendingBoot("board_descriptor");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed), nullptr);
  }
  ESP_LOGI(kTag, "ESP-IDF scaffold board=%s", board.id);

  const esp_err_t topology = inkloop::validate_task_topology();
  if (topology != ESP_OK) {
    ESP_LOGE(kTag, "task topology rejected: %s", esp_err_to_name(topology));
    failPendingBoot("task_topology");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed), nullptr);
  }

  // Static lifetime is intentional: the mount owner becomes part of the
  // product runtime after this guard succeeds. A stack owner would unmount the
  // filesystems when app_main starts the long-lived worker tasks and returns.
  static inkloop::storage::EspStorageMountOwner storage;
  static inkloop::storage::EspNvsBootMountOwner nvs_boot_mount;
  const inkloop::storage::EspUpgradeBootAuditOutcome upgrade =
      inkloop::storage::runReadOnlyUpgradeBootAudit(storage, nvs_boot_mount);
  if (upgrade.initialization_status != ESP_OK) {
    ESP_LOGE(kTag, "read-only upgrade inventory unavailable: %s",
             esp_err_to_name(upgrade.initialization_status));
    failPendingBoot("upgrade_inventory");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed), &nvs_boot_mount,
        &storage);
  }
  ESP_LOGI(kTag, "UPGRADE_AUDIT:%s protected=%u tasks=%s album=%s",
           inkloop::storage::upgradeAuditResultName(upgrade.report.result),
           static_cast<unsigned>(upgrade.report.protected_records_present),
           inkloop::storage::transactionAuditName(upgrade.report.tasks),
           inkloop::storage::transactionAuditName(upgrade.report.album));
  if (!inkloop::storage::persistenceCompatibilityContractValid()) {
    ESP_LOGE(kTag,
             "protected persistence compatibility contract is incomplete; "
             "refusing all writers");
    failPendingBoot("persistence_contract");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::MigrationRefused,
        inkloop::recovery::RecoveryPhase::Migration,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }

  // The compatibility decision above is the only authorization for an RW
  // internal mount.  Promotion always unregisters the real read-only VFS and
  // registers it again as RW; no Product owner is constructed before this
  // succeeds.  Refused/ambiguous audits remain read-only in Recovery.
  if (!upgrade.allowsStartup()) {
    ESP_LOGE(kTag,
             "upgrade state needs explicit recovery; refusing RW promotion");
    ota_stage.storage_gate_observed = true;
    failPendingBoot("upgrade_gate");
    runRecoveryNetwork(auditRecoveryDiagnostic(
        board, upgrade.report, recoveryRecordCounts(storage)),
        &nvs_boot_mount, &storage, true);
  }
  static inkloop::NativeSettingsMigrationGate settings_migration(
      board, nvs_boot_mount);
  inkloop::NativeSettingsMigrationPlan settings_migration_plan;
  const inkloop::NativeSettingsMigrationGateCode settings_audit =
      settings_migration.auditReadOnly(settings_migration_plan);
  if (settings_audit != inkloop::NativeSettingsMigrationGateCode::Ok) {
    ESP_LOGE(kTag, "read-only settings migration audit refused: %s",
             inkloop::nativeSettingsMigrationGateCodeName(settings_audit));
    failPendingBoot("settings_migration_audit");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::MigrationRefused,
        inkloop::recovery::RecoveryPhase::Migration,
        inkloop::recovery::RecoveryOutcome::RequiresOperator,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  const esp_err_t internal_promotion = storage.promoteInternalReadWrite();
  if (internal_promotion != ESP_OK) {
    ESP_LOGE(kTag, "internal RO-to-RW promotion failed: %s",
             esp_err_to_name(internal_promotion));
    failPendingBoot("internal_rw_promotion");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::StorageIntegrityRefused,
        inkloop::recovery::RecoveryPhase::StorageAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  const esp_err_t nvs_promotion = nvs_boot_mount.promoteReadWriteProduct();
  if (nvs_promotion != ESP_OK) {
    ESP_LOGE(kTag, "NVS RO-to-RW promotion failed: %s",
             esp_err_to_name(nvs_promotion));
    failPendingBoot("nvs_rw_promotion");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::StorageIntegrityRefused,
        inkloop::recovery::RecoveryPhase::StorageAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  inkloop::NativeSettingsMigrationAuthorization settings_authorization;
  const inkloop::NativeSettingsMigrationGateCode settings_execution =
      settings_migration.execute(
          settings_migration_plan, settings_authorization);
  if (settings_execution != inkloop::NativeSettingsMigrationGateCode::Ok ||
      !settings_authorization.valid()) {
    ESP_LOGE(kTag, "settings migration execution refused: %s",
             inkloop::nativeSettingsMigrationGateCodeName(settings_execution));
    failPendingBoot("settings_migration_execution");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::MigrationRefused,
        inkloop::recovery::RecoveryPhase::Migration,
        inkloop::recovery::RecoveryOutcome::RequiresOperator,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }

  const esp_err_t initialized = inkloop::board_initialize();
  if (initialized != ESP_OK) {
    ESP_LOGE(
        kTag,
        "board adapter is not implementation-ready: %s; refusing startup",
        esp_err_to_name(initialized));
    ota_stage.board_observed = true;
    failPendingBoot("board_initialize");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  ota_stage.board_observed = true;
  ota_stage.board_healthy = true;

  // SD is optional and never formatted automatically. When present it becomes
  // the automatic album/chat backend before the long-lived product services
  // capture their storage pointers; otherwise internal LittleFS remains the
  // safe fallback for this boot.
  inkloop::IBoardAdapter& board_adapter = inkloop::board_adapter();
  if (board.has_sd) {
    const esp_err_t sd_power = board_adapter.prepareSdCard();
    if (sd_power == ESP_OK) {
      const bool inserted = board_adapter.sdCardInserted();
      const esp_err_t sd_mount = storage.mountSd(true, inserted);
      ESP_LOGI(kTag, "SD:%s inserted=%u status=%s",
               inkloop::storage::mountStateName(storage.snapshot().sd.state),
               static_cast<unsigned>(inserted), esp_err_to_name(sd_mount));
    } else {
      ESP_LOGW(kTag, "SD power/card detection unavailable: %s; using internal",
               esp_err_to_name(sd_power));
    }
  } else {
    ESP_LOGI(kTag, "SD:UNSUPPORTED board=%s; using internal", board.id);
  }

  // Operational settings are loaded before long-lived services capture their
  // storage pointers. This makes the persisted backend choice authoritative
  // for the whole boot and prevents Voice, Portal, Display and Inkloop from
  // accidentally operating on different albums. A missing requested TF card
  // falls back to Automatic without rewriting the user's saved preference.
  inkloop::IBoardRenderer* const renderer = board_adapter.renderer();
  if (!renderer) {
    ESP_LOGE(kTag,
             "board renderer unavailable; refusing SKU render-setting owner");
    failPendingBoot("board_renderer");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  static inkloop::NativeDeviceStateOwner device_state(
      board, storage, *renderer);
  const esp_err_t settings_status =
      device_state.initialize(settings_authorization);
  if (settings_status != ESP_OK) {
    ESP_LOGE(kTag, "native settings unavailable: %s; refusing writers",
             esp_err_to_name(settings_status));
    failPendingBoot("native_settings");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::StorageIntegrityRefused,
        inkloop::recovery::RecoveryPhase::StorageAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }

  const char* selected_asset_root = storage.selectedAssetRoot(
      device_state.effectiveAssetPreference());
  const char* internal_root = storage.internalRoot();
  if (!selected_asset_root || !internal_root) {
    ESP_LOGE(kTag, "selected persistent backend unavailable; refusing writers");
    failPendingBoot("asset_backend");
    runRecoveryNetwork(recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::StorageIntegrityRefused,
        inkloop::recovery::RecoveryPhase::StorageAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)), &nvs_boot_mount, &storage);
  }
  if (std::strcmp(selected_asset_root, internal_root) != 0) {
    const inkloop::storage::UpgradeAuditReport asset_upgrade =
        inkloop::storage::runReadOnlyMountedFileUpgradeAudit(
            selected_asset_root);
    ESP_LOGI(kTag,
             "ASSET_UPGRADE_AUDIT:%s protected=%u album=%s root=removable",
             inkloop::storage::upgradeAuditResultName(asset_upgrade.result),
             static_cast<unsigned>(asset_upgrade.protected_records_present),
             inkloop::storage::transactionAuditName(asset_upgrade.album));
    if (!asset_upgrade.allowsInitialization()) {
      ESP_LOGE(kTag,
               "selected removable backend needs explicit recovery; "
               "refusing all product writers");
      ota_stage.storage_gate_observed = true;
      failPendingBoot("removable_upgrade_gate");
      runRecoveryNetwork(auditRecoveryDiagnostic(
          board, asset_upgrade, recoveryRecordCounts(storage)),
          &nvs_boot_mount, &storage, true);
    }
  }
  ota_stage.storage_gate_observed = true;
  ota_stage.storage_gate_healthy = true;

  // This journal may commit NVS. It is therefore consumed only after all
  // read-only compatibility and selected-backend audits have passed, while
  // still preceding every long-lived Product writer.
  static inkloop::OtaOutcomeJournal& ota_outcomes =
      inkloop::systemOtaOutcomeJournal();
  const esp_app_desc_t* audited_app = esp_app_get_description();
  const char* audited_version = audited_app && audited_app->version[0] != '\0'
      ? audited_app->version : "invalid";
  const inkloop::OtaOutcomeJournalCode outcome_boot = ota_outcomes.beginBoot(
      {audited_version, std::strlen(audited_version)}, ota_pending);
  ESP_LOGI(kTag, "OTA outcome boot=%s kind=%s",
           inkloop::otaOutcomeJournalCodeName(outcome_boot),
           inkloop::otaOutcomeKindName(ota_outcomes.snapshot().kind));
  logMainStackWatermark("before_product_construct");

  static inkloop::EspProductRuntime runtime(
      board_adapter, storage, device_state.effectiveAssetPreference());
  logMainStackWatermark("after_product_construct");
  static inkloop::OtaUpdateOwner& ota_update = inkloop::systemOtaUpdateOwner();
  static PortalOtaUpdateBridge ota_portal_bridge(
      ota_update, ota_update_allowed, ota_outcomes);
  esp_err_t runtime_status =
      device_state.attachStorageMaintenanceCoordinator(runtime);
  if (runtime_status == ESP_OK) {
    runtime_status = runtime.attachPortalSettingsOwner(device_state);
  }
  if (runtime_status == ESP_OK) {
    runtime_status = runtime.attachPortalAlbumMutationOwner(device_state);
  }
  if (runtime_status == ESP_OK) {
    runtime_status = runtime.attachPortalFirmwareUpdateOwner(
        ota_portal_bridge);
  }
  if (runtime_status == ESP_OK) {
    runtime_status = runtime.attachLocalTools(device_state);
  }
  const inkloop::settings::SettingsSnapshot operational_settings =
      device_state.snapshot();
  if (runtime_status == ESP_OK &&
      !operational_settings.values.local_management_password_override.empty()) {
    runtime_status = runtime.setLocalAccessCodeOverride(
        operational_settings.values.local_management_password_override);
  }
  if (runtime_status != ESP_OK) {
    ESP_LOGE(kTag, "native device-state composition rejected: %s",
             esp_err_to_name(runtime_status));
    ota_stage.runtime_observed = true;
    failPendingBoot("product_composition");
    runRecoveryAfterProductFailure(runtime, recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)),
        nvs_boot_mount,
        storage,
        operational_settings.values.local_management_password_override);
  }
  runtime_status = runtime.begin();
  if (runtime_status != ESP_OK) {
    ESP_LOGE(kTag, "native product runtime rejected startup: %s",
             esp_err_to_name(runtime_status));
    ota_stage.runtime_observed = true;
    failPendingBoot("product_runtime");
    runRecoveryAfterProductFailure(runtime, recoveryDiagnostic(
        board, inkloop::recovery::RecoveryReason::BootAuditRefused,
        inkloop::recovery::RecoveryPhase::BootAudit,
        inkloop::recovery::RecoveryOutcome::Failed,
        recoveryRecordCounts(storage)),
        nvs_boot_mount,
        storage,
        operational_settings.values.local_management_password_override);
  }
  logMainStackWatermark("after_product_begin");
  ota_stage.runtime_observed = true;
  ota_stage.runtime_healthy = true;

  inkloop::OtaBootHealthState logged_state =
      inkloop::OtaBootHealthState::Uninitialized;
  for (;;) {
    if (ota_pending) {
      const inkloop::SupervisorDiagnostics diagnostics = runtime.diagnostics();
      ota_stage.explicit_fatal_health_failure =
          diagnostics.startup_failed != 0U;
      ota_observation = ota_health.tick(
          inkloop::composeOtaBootHealthEvidence(
              nowMs(), ota_stage, runtime.telemetry()));
      if (ota_observation.decision.state != logged_state ||
          ota_observation.action_attempted) {
        logged_state = ota_observation.decision.state;
        logOtaObservation(ota_observation);
      }
      if (ota_observation.code ==
              inkloop::EspOtaBootHealthCode::ConfirmationSucceeded ||
          ota_observation.decision.state ==
              inkloop::OtaBootHealthState::ConfirmedBoot ||
          ota_observation.decision.state ==
              inkloop::OtaBootHealthState::OrdinaryBoot) {
        const inkloop::OtaOutcomeJournalCode confirmed =
            ota_outcomes.recordConfirmed();
        ESP_LOGI(kTag, "OTA outcome confirmation=%s",
                 inkloop::otaOutcomeJournalCodeName(confirmed));
        ota_pending = false;
        ota_update_allowed.store(true, std::memory_order_release);
      }
    }

    inkloop::OtaUpdateRequest ota_request;
    if (ota_update.take(ota_request) == inkloop::OtaUpdateCode::Ok) {
      ESP_LOGI(kTag, "OTA request accepted; quiescing normal writers");
      const esp_app_desc_t* source_app = esp_app_get_description();
      const char* source_version =
          source_app && source_app->version[0] != '\0'
          ? source_app->version : "invalid";
      const esp_err_t quiesced = runtime.shutdownForOtaAcquisition();
      inkloop::OtaUpdateCode result = inkloop::OtaUpdateCode::QuiesceFailed;
      if (quiesced == ESP_OK) {
        result = ota_update.acquire(
            ota_request, {board.id, std::strlen(board.id)},
            {source_version, std::strlen(source_version)});
      } else {
        result = ota_update.fail(
            ota_request, inkloop::OtaUpdateCode::QuiesceFailed);
      }
      ESP_LOGI(kTag, "OTA acquisition terminal=%s quiesce=%s",
               inkloop::otaUpdateCodeName(result),
               esp_err_to_name(quiesced));

      constexpr unsigned kOtaOutcomeJournalAttempts = 3U;
      inkloop::OtaOutcomeJournalCode journaled =
          inkloop::OtaOutcomeJournalCode::IoError;
      for (unsigned attempt = 0U;
           attempt < kOtaOutcomeJournalAttempts &&
               journaled != inkloop::OtaOutcomeJournalCode::Ok;
           ++attempt) {
        journaled = ota_outcomes.recordTerminal(
            ota_request, result,
            {source_version, std::strlen(source_version)});
      }
      ESP_LOGI(kTag, "OTA outcome terminal=%s",
               inkloop::otaOutcomeJournalCodeName(journaled));

      // Success selected the new inactive slot; failure did not. In both
      // cases close the retained STA and reboot into a fresh audited boot.
      constexpr unsigned kOtaShutdownAttempts = 8U;
      esp_err_t stopped = ESP_FAIL;
      for (unsigned attempt = 1U;
           attempt <= kOtaShutdownAttempts && stopped != ESP_OK; ++attempt) {
        stopped = runtime.shutdownForRecovery();
        if (stopped != ESP_OK) vTaskDelay(pdMS_TO_TICKS(250U));
      }
      ESP_LOGI(kTag, "OTA reboot handoff network_stop=%s",
               esp_err_to_name(stopped));
      vTaskDelay(pdMS_TO_TICKS(100U));
      esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(100U));
  }
}
