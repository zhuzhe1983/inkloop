#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/spi_common.h"
#include "esp_err.h"
#include "inkloop/storage/posix_atomic_album_store.hpp"
#include "sdmmc_cmd.h"

namespace inkloop {
namespace storage {

enum class MountState : uint8_t {
  Unmounted,
  Mounted,
  Absent,
  NotPowered,
  LayoutMismatch,
  RecoveryRequired,
  IoError,
};

enum class AssetStoragePreference : uint8_t {
  Automatic,
  Internal,
  SdCard,
};

struct MountedBackendStatus {
  MountState state = MountState::Unmounted;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  bool removable = false;
  bool writable = false;

  bool healthy() const {
    return state == MountState::Mounted && writable && total_bytes > 0 &&
           free_bytes <= total_bytes;
  }
};

struct StorageMountSnapshot {
  MountedBackendStatus internal;
  MountedBackendStatus sd;
};

struct StorageMountConfig {
  const char* internal_partition_label = "spiffs";
  const char* internal_base_path = "/littlefs";
  uint32_t internal_partition_address = 0x00c90000U;
  uint32_t internal_partition_size = 0x00360000U;
  const char* sd_base_path = "/sd";
  // PaperColor ED2208 and TF share the FSPI/SPI2 bus on GPIO 13/14/15.
  // The board owner initializes it once; storage only adds/removes the SD
  // device and must never free the shared bus.
  spi_host_device_t sd_spi_host = SPI2_HOST;
  bool sd_bus_already_initialized = true;
  int sd_sclk_gpio = 15;
  int sd_miso_gpio = 14;
  int sd_mosi_gpio = 13;
  int sd_cs_gpio = 47;
  int sd_max_frequency_khz = 25000;
};

// Sole native filesystem mount owner. It never formats, erases, grows, or
// silently changes the selected backend. Board code must enable PM1 TF power
// and debounce card detect before calling mountSd().
class EspStorageMountOwner final {
 public:
  explicit EspStorageMountOwner(StorageMountConfig config = {});
  ~EspStorageMountOwner();

  EspStorageMountOwner(const EspStorageMountOwner&) = delete;
  EspStorageMountOwner& operator=(const EspStorageMountOwner&) = delete;

  esp_err_t mountInternal();
  esp_err_t mountSd(bool power_ready, bool card_inserted);
  // Destructive maintenance entry point. The caller must already have
  // obtained explicit physical confirmation and serialized the operation on
  // the Portal/storage slow lane. It can only target the mounted removable TF
  // card; internal LittleFS is deliberately unreachable.
  esp_err_t formatSdCardConfirmed();
  void unmountSd();
  void unmountInternal();

  StorageMountSnapshot snapshot() const { return snapshot_; }
  const char* taskRoot() const;
  const char* internalRoot() const;
  const char* removableRoot() const;
  const char* selectedAssetRoot(AssetStoragePreference preference) const;
  PosixAtomicAlbumStore* selectedAlbumStore(
      AssetStoragePreference preference);
  PosixAtomicAlbumStore* albumStoreForLegacyIdentity(const char* identity);

 private:
  bool validConfig() const;
  void resetInternal(MountState state);
  void resetSd(MountState state);

  StorageMountConfig config_;
  StorageMountSnapshot snapshot_{};
  PosixAtomicAlbumStore internal_album_;
  PosixAtomicAlbumStore sd_album_;
  sdmmc_card_t* sd_card_ = nullptr;
  bool internal_registered_ = false;
  bool sd_registered_ = false;
  bool sd_bus_owned_ = false;
};

const char* mountStateName(MountState state);

}  // namespace storage
}  // namespace inkloop
