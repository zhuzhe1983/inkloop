#include "inkloop/storage/esp_storage_mount.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstring>
#include <limits>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"

namespace inkloop {
namespace storage {
namespace {

bool validAbsoluteRoot(const char* value) {
  if (!value || value[0] != '/' || value[1] == '\0') return false;
  size_t length = 0;
  for (; value[length] != '\0'; ++length) {
    if (length >= 63U) return false;
    if (value[length] == '/' && value[length + 1] == '.') return false;
  }
  return value[length - 1U] != '/';
}

bool checkedCapacity(const struct statvfs& capacity, uint64_t& total,
                     uint64_t& free) {
  if (capacity.f_frsize == 0 ||
      capacity.f_blocks >
          std::numeric_limits<uint64_t>::max() / capacity.f_frsize ||
      capacity.f_bavail >
          std::numeric_limits<uint64_t>::max() / capacity.f_frsize) {
    return false;
  }
  total = static_cast<uint64_t>(capacity.f_blocks) * capacity.f_frsize;
  free = static_cast<uint64_t>(capacity.f_bavail) * capacity.f_frsize;
  return total > 0 && free <= total;
}

}  // namespace

EspStorageMountOwner::EspStorageMountOwner(StorageMountConfig config)
    : config_(config),
      internal_album_(config.internal_base_path ? config.internal_base_path : "",
                      false),
      sd_album_(config.sd_base_path ? config.sd_base_path : "", true) {
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

bool EspStorageMountOwner::updateCapacity(
    const char* root, MountedBackendStatus& status) const {
  struct stat root_status {};
  struct statvfs capacity {};
  uint64_t total = 0;
  uint64_t free = 0;
  if (::stat(root, &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
      ::statvfs(root, &capacity) != 0 ||
      !checkedCapacity(capacity, total, free)) {
    return false;
  }
  status.total_bytes = total;
  status.free_bytes = free;
  status.writable = true;
  status.state = MountState::Mounted;
  return true;
}

esp_err_t EspStorageMountOwner::mountInternal() {
  if (internal_registered_) return ESP_ERR_INVALID_STATE;
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
  mount_config.partition_label = config_.internal_partition_label;
  mount_config.partition = nullptr;
  mount_config.format_if_mount_failed = false;
  mount_config.read_only = false;
  mount_config.dont_mount = false;
  mount_config.grow_on_mount = false;
  const esp_err_t mounted = esp_vfs_littlefs_register(&mount_config);
  if (mounted != ESP_OK) {
    resetInternal(mounted == ESP_FAIL ? MountState::RecoveryRequired
                                      : MountState::IoError);
    return mounted;
  }
  internal_registered_ = true;
  size_t total = 0;
  size_t used = 0;
  if (esp_littlefs_info(config_.internal_partition_label, &total, &used) !=
          ESP_OK ||
      used > total || !updateCapacity(config_.internal_base_path,
                                      snapshot_.internal)) {
    unmountInternal();
    resetInternal(MountState::RecoveryRequired);
    return ESP_FAIL;
  }
  snapshot_.internal.removable = false;
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::mountSd(bool power_ready,
                                        bool card_inserted) {
  if (sd_registered_ || sd_bus_owned_) return ESP_ERR_INVALID_STATE;
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
  host.max_freq_khz = config_.sd_max_frequency_khz;
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
  result = esp_vfs_fat_sdspi_mount(config_.sd_base_path, &host, &device, &fat,
                                   &sd_card_);
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
  if (!updateCapacity(config_.sd_base_path, snapshot_.sd)) {
    unmountSd();
    resetSd(MountState::RecoveryRequired);
    return ESP_FAIL;
  }
  snapshot_.sd.removable = true;
  return ESP_OK;
}

esp_err_t EspStorageMountOwner::formatSdCardConfirmed() {
  if (!validConfig() || !sd_registered_ || !sd_card_ ||
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
  if (!updateCapacity(config_.sd_base_path, snapshot_.sd)) {
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

void EspStorageMountOwner::unmountInternal() {
  internal_album_.abort();
  if (internal_registered_) {
    esp_vfs_littlefs_unregister(config_.internal_partition_label);
  }
  internal_registered_ = false;
  resetInternal(MountState::Unmounted);
}

const char* EspStorageMountOwner::taskRoot() const {
  return snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::internalRoot() const {
  return snapshot_.internal.healthy() ? config_.internal_base_path : nullptr;
}

const char* EspStorageMountOwner::removableRoot() const {
  return snapshot_.sd.healthy() ? config_.sd_base_path : nullptr;
}

const char* EspStorageMountOwner::selectedAssetRoot(
    AssetStoragePreference preference) const {
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
  if (!identity) return nullptr;
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
