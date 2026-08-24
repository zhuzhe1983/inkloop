#include "inkloop/storage/esp_storage_mount.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace inkloop {
namespace storage {
namespace {

constexpr char kTag[] = "ink-storage";

bool validAbsoluteRoot(const char* value) {
  if (!value || value[0] != '/' || value[1] == '\0') return false;
  size_t length = 0;
  for (; value[length] != '\0'; ++length) {
    if (length >= 63U) return false;
    if (value[length] == '/' && value[length + 1] == '.') return false;
  }
  return value[length - 1U] != '/';
}

bool applyCapacity(uint64_t total, uint64_t free, bool writable,
                   MountedBackendStatus& status) {
  if (total == 0 || free > total) return false;
  status.total_bytes = total;
  status.free_bytes = free;
  status.writable = writable;
  status.state = MountState::Mounted;
  return true;
}

// M5Stack's C151 factory ESP-IDF firmware deliberately probes removable media
// at 20/10/4 MHz (with a retry at each rate). Some otherwise healthy cards
// answer the initial SPI commands at 400 kHz but reject the CSD read when the
// driver switches directly to 25 MHz. Keep the same conservative ladder. A
// failed esp_vfs_fat_sdspi_mount() releases the SDSPI device/host state, so a
// later attempt cannot retain a stale card handle or VFS registration.
constexpr std::array<int, 3> kPaperColorSdProbeKhz{{20000, 10000, 4000}};
constexpr size_t kPaperColorSdAttemptsPerRate = 2U;
constexpr uint32_t kPaperColorSdRetryDelayMs = 500U;

bool retryableSdProbeError(esp_err_t status) {
  return status == ESP_ERR_INVALID_RESPONSE || status == ESP_ERR_TIMEOUT ||
         status == ESP_ERR_INVALID_CRC;
}

}  // namespace

EspStorageMountOwner::EspStorageMountOwner(StorageMountConfig config)
    : config_(config),
      internal_album_(config.internal_base_path ? config.internal_base_path : "",
                      false, AlbumCapacityBackend::EspLittleFs,
                      config.internal_partition_label
                          ? config.internal_partition_label
                          : ""),
      sd_album_(config.sd_base_path ? config.sd_base_path : "", true,
                AlbumCapacityBackend::EspFatFs,
                config.sd_base_path ? config.sd_base_path : "") {
  snapshot_.internal.removable = false;
  snapshot_.sd.removable = true;
  if (!validConfig()) {
    resetInternal(MountState::LayoutMismatch);
    resetSd(MountState::LayoutMismatch);
  }
}

EspStorageMountOwner::~EspStorageMountOwner() {
  unmountSd();
  unmountInternal();
}

bool EspStorageMountOwner::validConfig() const {
  return config_.internal_partition_label &&
         config_.internal_partition_label[0] != '\0' &&
         validAbsoluteRoot(config_.internal_base_path) &&
         config_.internal_partition_address == 0x00c90000U &&
         config_.internal_partition_size == 0x00360000U &&
         validAbsoluteRoot(config_.sd_base_path) &&
         config_.sd_spi_host == SPI2_HOST && config_.sd_sclk_gpio == 15 &&
         config_.sd_miso_gpio == 14 && config_.sd_mosi_gpio == 13 &&
         config_.sd_cs_gpio == 47 && config_.sd_max_frequency_khz > 0 &&
         config_.sd_max_frequency_khz <= 25000;
}

void EspStorageMountOwner::resetInternal(MountState state) {
  snapshot_.internal = MountedBackendStatus{};
  snapshot_.internal.state = state;
}

void EspStorageMountOwner::resetSd(MountState state) {
  snapshot_.sd = MountedBackendStatus{};
  snapshot_.sd.state = state;
  snapshot_.sd.removable = true;
}

esp_err_t EspStorageMountOwner::mountInternalReadOnly() {
  return mountInternalWithAccess(InternalMountAccess::ReadOnly);
}

esp_err_t EspStorageMountOwner::mountInternal() {
  if (recovery_mode_) return ESP_ERR_INVALID_STATE;
  return mountInternalWithAccess(InternalMountAccess::ReadWriteProduct);
}

esp_err_t EspStorageMountOwner::mountInternalWithAccess(
    InternalMountAccess access) {
  if (internal_registered_ ||
      internal_access_ != InternalMountAccess::Unmounted)
    return ESP_ERR_INVALID_STATE;
  if (access != InternalMountAccess::ReadOnly &&
      access != InternalMountAccess::ReadWriteProduct &&
      access != InternalMountAccess::ReadWriteRecovery)
    return ESP_ERR_INVALID_ARG;
  if (!validConfig() || !internal_album_.pathsValid()) {
    resetInternal(MountState::LayoutMismatch);
    return ESP_ERR_INVALID_ARG;
  }
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
      config_.internal_partition_label);
  if (!partition || partition->address != config_.internal_partition_address ||
      partition->size != config_.internal_partition_size) {
    resetInternal(MountState::LayoutMismatch);
    return ESP_ERR_NOT_FOUND;
  }

  esp_vfs_littlefs_conf_t mount_config{};
  mount_config.base_path = config_.internal_base_path;
  if (access == InternalMountAccess::ReadOnly) {
    // Both enforcement layers are intentional. LittleFS/VFS rejects writable
    // opens, while the lifetime-owned descriptor makes the partition API
    // reject any accidental program/erase call below the filesystem.
    internal_readonly_partition_ = *partition;
    internal_readonly_partition_.readonly = true;
    internal_readonly_partition_valid_ = true;
    mount_config.partition_label = nullptr;
    mount_config.partition = &internal_readonly_partition_;
  } else {
    internal_readonly_partition_valid_ = false;
    mount_config.partition_label = config_.internal_partition_label;
    mount_config.partition = nullptr;
  }
  mount_config.format_if_mount_failed = false;
  mount_config.read_only = access == InternalMountAccess::ReadOnly;
  mount_config.dont_mount = false;
  mount_config.grow_on_mount = false;
  const esp_err_t mounted = esp_vfs_littlefs_register(&mount_config);
  if (mounted != ESP_OK) {
    internal_readonly_partition_valid_ = false;
    resetInternal(mounted == ESP_FAIL ? MountState::RecoveryRequired
                                      : MountState::IoError);
    return mounted;
  }
  internal_registered_ = true;
  internal_access_ = access;
  size_t total = 0;
  size_t used = 0;
  if (esp_littlefs_info(config_.internal_partition_label, &total, &used) !=
          ESP_OK ||
      used > total ||
      !applyCapacity(total, total - used,
                     access != InternalMountAccess::ReadOnly,
                     snapshot_.internal)) {
    unmountInternal();
    resetInternal(MountState::RecoveryRequired);
    return ESP_FAIL;
  }
  snapshot_.internal.removable = false;
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::promoteInternalReadWrite() {
  if (recovery_mode_) return ESP_ERR_INVALID_STATE;
  if (!internal_registered_ ||
      internal_access_ != InternalMountAccess::ReadOnly ||
      snapshot_.internal.state != MountState::Mounted ||
      snapshot_.internal.writable)
    return ESP_ERR_INVALID_STATE;
  internal_album_.abort();
  const esp_err_t unmounted = unregisterInternal();
  if (unmounted != ESP_OK) {
    resetInternal(MountState::RecoveryRequired);
    return unmounted;
  }
  const esp_err_t mounted =
      mountInternalWithAccess(InternalMountAccess::ReadWriteProduct);
  if (mounted != ESP_OK) {
    resetInternal(MountState::RecoveryRequired);
    return mounted;
  }
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::prepareRecoveryReadOnly() {
  if (recovery_mutation_domain_ != RecoveryMutationDomain::None)
    return ESP_ERR_INVALID_STATE;
  recovery_mode_ = true;  // Revoke Product roots before touching the VFS.
  if (internal_registered_ &&
      internal_access_ == InternalMountAccess::ReadOnly &&
      auditInternalRoot()) {
    return ESP_OK;
  }
  if (internal_registered_) {
    internal_album_.abort();
    const esp_err_t unmounted = unregisterInternal();
    if (unmounted != ESP_OK) {
      // One bounded second unregister attempt handles a transient VFS owner
      // release. If it still fails, recoveryWritesRevoked() remains false and
      // app_main refuses to start the Recovery network on a Product RW mount.
      unmountInternal();
      resetInternal(MountState::RecoveryRequired);
      return unmounted;
    }
  }
  const esp_err_t mounted =
      mountInternalWithAccess(InternalMountAccess::ReadOnly);
  if (mounted != ESP_OK) resetInternal(MountState::RecoveryRequired);
  return mounted;
}

esp_err_t EspStorageMountOwner::beginRecoveryMutation(
    RecoveryMutationDomain domain) {
  if (!recovery_mode_ ||
      recovery_mutation_domain_ != RecoveryMutationDomain::None ||
      domain == RecoveryMutationDomain::None || !auditInternalRoot()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (domain == RecoveryMutationDomain::RemovableAlbum) {
    if (!snapshot_.sd.healthy()) return ESP_ERR_NOT_FOUND;
    recovery_mutation_domain_ = domain;
    return ESP_OK;
  }
  internal_album_.abort();
  const esp_err_t unmounted = unregisterInternal();
  if (unmounted != ESP_OK) {
    resetInternal(MountState::RecoveryRequired);
    return unmounted;
  }
  const esp_err_t mounted =
      mountInternalWithAccess(InternalMountAccess::ReadWriteRecovery);
  if (mounted != ESP_OK) {
    // A failed recovery promotion is never allowed to strand a writable
    // registration. Best-effort hard-RO remount preserves inspection access;
    // failure remains fail-closed and leaves no Product root.
    (void)mountInternalWithAccess(InternalMountAccess::ReadOnly);
    return mounted;
  }
  recovery_mutation_domain_ = domain;
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::endRecoveryMutationAndRemountReadOnly() {
  const RecoveryMutationDomain domain = recovery_mutation_domain_;
  if (!recovery_mode_ || domain == RecoveryMutationDomain::None)
    return ESP_ERR_INVALID_STATE;
  recovery_mutation_domain_ = RecoveryMutationDomain::None;
  if (domain == RecoveryMutationDomain::RemovableAlbum) {
    return auditInternalRoot() ? ESP_OK : ESP_ERR_INVALID_STATE;
  }
  if (!internal_registered_ ||
      internal_access_ != InternalMountAccess::ReadWriteRecovery) {
    resetInternal(MountState::RecoveryRequired);
    return ESP_ERR_INVALID_STATE;
  }
  internal_album_.abort();
  const esp_err_t unmounted = unregisterInternal();
  if (unmounted != ESP_OK) {
    resetInternal(MountState::RecoveryRequired);
    return unmounted;
  }
  const esp_err_t mounted =
      mountInternalWithAccess(InternalMountAccess::ReadOnly);
  if (mounted != ESP_OK) resetInternal(MountState::RecoveryRequired);
  return mounted;
}

esp_err_t EspStorageMountOwner::mountSd(bool power_ready,
                                        bool card_inserted) {
  if (recovery_mode_ || sd_registered_ || sd_bus_owned_)
    return ESP_ERR_INVALID_STATE;
  if (!validConfig() || !sd_album_.pathsValid()) {
    resetSd(MountState::LayoutMismatch);
    return ESP_ERR_INVALID_ARG;
  }
  if (!power_ready) {
    resetSd(MountState::NotPowered);
    return ESP_ERR_INVALID_STATE;
  }
  if (!card_inserted) {
    resetSd(MountState::Absent);
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t result = ESP_OK;
  if (!config_.sd_bus_already_initialized) {
    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = config_.sd_mosi_gpio;
    bus_config.miso_io_num = config_.sd_miso_gpio;
    bus_config.sclk_io_num = config_.sd_sclk_gpio;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    bus_config.data_io_default_level = false;
    bus_config.max_transfer_sz = 4096;
    bus_config.flags = SPICOMMON_BUSFLAG_MASTER;
    bus_config.isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO;
    bus_config.intr_flags = 0;
    result = spi_bus_initialize(config_.sd_spi_host, &bus_config,
                                SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
      resetSd(MountState::IoError);
      return result;
    }
    sd_bus_owned_ = true;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = config_.sd_spi_host;
  sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id = config_.sd_spi_host;
  device.gpio_cs = static_cast<gpio_num_t>(config_.sd_cs_gpio);
  device.gpio_cd = GPIO_NUM_NC;
  device.gpio_wp = GPIO_NUM_NC;
  device.gpio_int = GPIO_NUM_NC;
  const esp_vfs_fat_mount_config_t fat = {
      .format_if_mount_failed = false,
      .max_files = 8,
      .allocation_unit_size = 0,
      .disk_status_check_enable = true,
      .use_one_fat = false,
  };
  int previous_frequency_khz = 0;
  for (const int official_frequency_khz : kPaperColorSdProbeKhz) {
    const int frequency_khz =
        std::min(config_.sd_max_frequency_khz, official_frequency_khz);
    if (frequency_khz == previous_frequency_khz) continue;
    previous_frequency_khz = frequency_khz;
    host.max_freq_khz = frequency_khz;
    for (size_t attempt = 0; attempt < kPaperColorSdAttemptsPerRate;
         ++attempt) {
      sd_card_ = nullptr;
      result = esp_vfs_fat_sdspi_mount(config_.sd_base_path, &host, &device,
                                       &fat, &sd_card_);
      if (result == ESP_OK) {
        ESP_LOGI(kTag, "SD mount succeeded at %d KHz attempt=%u",
                 frequency_khz, static_cast<unsigned>(attempt + 1U));
        break;
      }
      sd_card_ = nullptr;
      if (!retryableSdProbeError(result)) break;
      if (attempt + 1U < kPaperColorSdAttemptsPerRate) {
        vTaskDelay(pdMS_TO_TICKS(kPaperColorSdRetryDelayMs));
      }
    }
    if (result == ESP_OK || !retryableSdProbeError(result)) break;
    vTaskDelay(pdMS_TO_TICKS(kPaperColorSdRetryDelayMs));
  }
  if (result != ESP_OK) {
    if (sd_bus_owned_) {
      spi_bus_free(config_.sd_spi_host);
      sd_bus_owned_ = false;
    }
    sd_card_ = nullptr;
    resetSd(result == ESP_FAIL ? MountState::RecoveryRequired
                               : MountState::IoError);
    return result;
  }
  sd_registered_ = true;
  uint64_t total = 0;
  uint64_t free = 0;
  if (esp_vfs_fat_info(config_.sd_base_path, &total, &free) != ESP_OK ||
      !applyCapacity(total, free, true, snapshot_.sd)) {
    unmountSd();
    resetSd(MountState::RecoveryRequired);
    return ESP_FAIL;
  }
  snapshot_.sd.removable = true;
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::formatSdCardConfirmed() {
  if (recovery_mode_ || !validConfig() || !sd_registered_ || !sd_card_ ||
      snapshot_.sd.state != MountState::Mounted) {
    return ESP_ERR_INVALID_STATE;
  }
  // Never invalidate an in-flight atomic album transaction. The confirmation
  // remains a product-layer concern; this owner only enforces the exact TF
  // target and storage serialization invariant.
  if (sd_album_.active()) return ESP_ERR_INVALID_STATE;
  const esp_err_t formatted =
      esp_vfs_fat_sdcard_format(config_.sd_base_path, sd_card_);
  if (formatted != ESP_OK) {
    resetSd(MountState::RecoveryRequired);
    return formatted;
  }
  uint64_t total = 0;
  uint64_t free = 0;
  if (esp_vfs_fat_info(config_.sd_base_path, &total, &free) != ESP_OK ||
      !applyCapacity(total, free, true, snapshot_.sd)) {
    // IDF's formatter unmounts internally and, if mounting the new FAT volume
    // fails, releases the card/driver before returning. Some IDF releases can
    // still return ESP_OK from that remount-failure path. Do not retain or
    // later pass that potentially freed card pointer to sdcard_unmount(). The
    // owner stays fail-closed until reboot; an owned SPI bus is released by
    // unmountSd()/the destructor without dereferencing the card.
    sd_registered_ = false;
    sd_card_ = nullptr;
    resetSd(MountState::RecoveryRequired);
    return ESP_FAIL;
  }
  snapshot_.sd.removable = true;
  return ESP_OK;
}

void EspStorageMountOwner::unmountSd() {
  sd_album_.abort();
  if (sd_registered_ && sd_card_) {
    esp_vfs_fat_sdcard_unmount(config_.sd_base_path, sd_card_);
  }
  sd_registered_ = false;
  sd_card_ = nullptr;
  if (sd_bus_owned_) spi_bus_free(config_.sd_spi_host);
  sd_bus_owned_ = false;
  resetSd(MountState::Unmounted);
}

esp_err_t EspStorageMountOwner::unregisterInternal() {
  if (internal_registered_) {
    const esp_err_t status =
        esp_vfs_littlefs_unregister(config_.internal_partition_label);
    if (status != ESP_OK) return status;
  }
  internal_registered_ = false;
  internal_access_ = InternalMountAccess::Unmounted;
  internal_readonly_partition_valid_ = false;
  resetInternal(MountState::Unmounted);
  return ESP_OK;
}

void EspStorageMountOwner::unmountInternal() {
  internal_album_.abort();
  recovery_mutation_domain_ = RecoveryMutationDomain::None;
  if (unregisterInternal() != ESP_OK)
    resetInternal(MountState::RecoveryRequired);
}

const char* EspStorageMountOwner::auditInternalRoot() const {
  return internal_registered_ &&
      internal_access_ == InternalMountAccess::ReadOnly &&
      internal_readonly_partition_valid_ &&
      internal_readonly_partition_.readonly &&
      snapshot_.internal.state == MountState::Mounted &&
      !snapshot_.internal.writable
      ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::taskRoot() const {
  return !recovery_mode_ &&
      internal_access_ == InternalMountAccess::ReadWriteProduct &&
      snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::internalRoot() const {
  return !recovery_mode_ &&
      internal_access_ == InternalMountAccess::ReadWriteProduct &&
      snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::removableRoot() const {
  return !recovery_mode_ && snapshot_.sd.healthy()
      ? config_.sd_base_path : nullptr;
}

const char* EspStorageMountOwner::recoveryReadInternalRoot() const {
  return recovery_mode_ &&
      recovery_mutation_domain_ == RecoveryMutationDomain::None
      ? auditInternalRoot() : nullptr;
}

const char* EspStorageMountOwner::recoveryReadTaskRoot() const {
  return recoveryReadInternalRoot();
}

const char* EspStorageMountOwner::recoveryReadRemovableRoot() const {
  return recovery_mode_ &&
      recovery_mutation_domain_ == RecoveryMutationDomain::None &&
      snapshot_.sd.healthy() ? config_.sd_base_path : nullptr;
}

const char* EspStorageMountOwner::recoveryMutationInternalRoot(
    RecoveryMutationDomain domain) const {
  const bool internal_domain = domain == RecoveryMutationDomain::Display ||
      domain == RecoveryMutationDomain::Tasks ||
      domain == RecoveryMutationDomain::InternalAlbum;
  return recovery_mode_ && internal_domain &&
      recovery_mutation_domain_ == domain && internal_registered_ &&
      internal_access_ == InternalMountAccess::ReadWriteRecovery &&
      snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::recoveryMutationTaskRoot(
    RecoveryMutationDomain domain) const {
  const bool task_domain = domain == RecoveryMutationDomain::Display ||
      domain == RecoveryMutationDomain::Tasks;
  return recovery_mode_ && task_domain &&
      recovery_mutation_domain_ == domain && internal_registered_ &&
      internal_access_ == InternalMountAccess::ReadWriteRecovery &&
      snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::recoveryMutationRemovableRoot(
    RecoveryMutationDomain domain) const {
  const bool removable_domain = domain == RecoveryMutationDomain::Display ||
      domain == RecoveryMutationDomain::RemovableAlbum;
  return recovery_mode_ && removable_domain &&
      recovery_mutation_domain_ == domain && snapshot_.sd.healthy()
      ? config_.sd_base_path : nullptr;
}

PosixAtomicAlbumStore* EspStorageMountOwner::recoveryMutationAlbumStore(
    RecoveryMutationDomain domain, const char* identity) {
  if (!identity) return nullptr;
  if (std::strcmp(identity, "littlefs") == 0)
    return recoveryMutationInternalRoot(domain) ? &internal_album_ : nullptr;
  if (std::strcmp(identity, "sd") == 0)
    return recoveryMutationRemovableRoot(domain) ? &sd_album_ : nullptr;
  return nullptr;
}

const char* EspStorageMountOwner::selectedAssetRoot(
    AssetStoragePreference preference) const {
  if (recovery_mode_ ||
      internal_access_ != InternalMountAccess::ReadWriteProduct) {
    return nullptr;
  }
  if (preference == AssetStoragePreference::Internal)
    return snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
  if (preference == AssetStoragePreference::SdCard)
    return snapshot_.sd.healthy() ? config_.sd_base_path : nullptr;
  if (snapshot_.sd.healthy()) return config_.sd_base_path;
  return snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

PosixAtomicAlbumStore* EspStorageMountOwner::selectedAlbumStore(
    AssetStoragePreference preference) {
  const char* root = selectedAssetRoot(preference);
  if (!root) return nullptr;
  if (root == config_.sd_base_path) return &sd_album_;
  return &internal_album_;
}

PosixAtomicAlbumStore* EspStorageMountOwner::albumStoreForLegacyIdentity(
    const char* identity) {
  if (recovery_mode_ ||
      internal_access_ != InternalMountAccess::ReadWriteProduct || !identity) {
    return nullptr;
  }
  if (std::strcmp(identity, "littlefs") == 0)
    return snapshot_.internal.healthy() ? &internal_album_ : nullptr;
  if (std::strcmp(identity, "sd") == 0)
    return snapshot_.sd.healthy() ? &sd_album_ : nullptr;
  return nullptr;
}

const char* mountStateName(MountState state) {
  switch (state) {
    case MountState::Unmounted:
      return "UNMOUNTED";
    case MountState::Mounted:
      return "MOUNTED";
    case MountState::Absent:
      return "ABSENT";
    case MountState::NotPowered:
      return "NOT_POWERED";
    case MountState::LayoutMismatch:
      return "LAYOUT_MISMATCH";
    case MountState::RecoveryRequired:
      return "RECOVERY_REQUIRED";
    case MountState::IoError:
      return "IO_ERROR";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
