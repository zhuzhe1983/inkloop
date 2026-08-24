#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "inkloop/storage/posix_atomic_album_store.hpp"
#include "sdmmc_cmd.h"

namespace inkloop {

class EspRecoveryActionOwner;

namespace storage {

class EspLegacyDisplayRecovery;

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

// Internal LittleFS has two deliberately separate mount capabilities.  A
// read-only audit mount is not considered a healthy Product backend and can
// therefore never leak through taskRoot()/internalRoot()/albumStore().
enum class InternalMountAccess : uint8_t {
  Unmounted,
  ReadOnly,
  ReadWriteProduct,
  ReadWriteRecovery,
};

// Recovery mutations are deliberately narrower than a Product RW mount.  The
// mount owner never exposes Product roots while one of these capabilities is
// active, and only the recovery composition friends below can acquire one.
enum class RecoveryMutationDomain : uint8_t {
  None,
  Display,
  Tasks,
  InternalAlbum,
  RemovableAlbum,
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

  // Boot compatibility inventory must use this entry point.  It sets the
  // LittleFS driver's real read_only flag and never formats or grows media.
  esp_err_t mountInternalReadOnly();
  // Normal Product callers use an RW mount only after the read-only boot audit
  // has explicitly authorized promotion.
  esp_err_t mountInternal();
  // The only RO -> RW transition: unregister the audit VFS first, then create
  // a fresh RW registration.  Any failure leaves the backend unavailable and
  // marked RecoveryRequired.
  esp_err_t promoteInternalReadWrite();
  // Every post-audit Recovery entry uses this before starting any network
  // owner. Product-facing roots are revoked first; a Product RW registration
  // is then unregistered and replaced by a physical read-only descriptor. If
  // the RO remount fails, the partition stays unmounted. A caller must refuse
  // Recovery networking if recoveryWritesRevoked() is false.
  esp_err_t prepareRecoveryReadOnly();
  bool recoveryWritesRevoked() const {
    return recovery_mode_ &&
        internal_access_ != InternalMountAccess::ReadWriteProduct &&
        internal_access_ != InternalMountAccess::ReadWriteRecovery;
  }
  bool recoveryReadOnlyReady() const {
    return recoveryWritesRevoked() && auditInternalRoot() != nullptr;
  }
  esp_err_t mountSd(bool power_ready, bool card_inserted);
  // Destructive maintenance entry point. The caller must already have
  // obtained explicit physical confirmation and serialized the operation on
  // the Portal/storage slow lane. It can only target the mounted removable TF
  // card; internal LittleFS is deliberately unreachable.
  esp_err_t formatSdCardConfirmed();
  void unmountSd();
  void unmountInternal();

  StorageMountSnapshot snapshot() const { return snapshot_; }
  InternalMountAccess internalMountAccess() const { return internal_access_; }
  // Only exposed while the actual driver registration is read-only.  This is
  // for bounded inventory code; all writer-facing roots remain null.
  const char* auditInternalRoot() const;
  const char* taskRoot() const;
  const char* internalRoot() const;
  const char* removableRoot() const;
  const char* selectedAssetRoot(AssetStoragePreference preference) const;
  PosixAtomicAlbumStore* selectedAlbumStore(
      AssetStoragePreference preference);
  PosixAtomicAlbumStore* albumStoreForLegacyIdentity(const char* identity);

 private:
  friend class ::inkloop::EspRecoveryActionOwner;
  friend class EspLegacyDisplayRecovery;

  bool validConfig() const;
  esp_err_t mountInternalWithAccess(InternalMountAccess access);
  esp_err_t unregisterInternal();
  esp_err_t beginRecoveryMutation(RecoveryMutationDomain domain);
  esp_err_t endRecoveryMutationAndRemountReadOnly();
  const char* recoveryReadTaskRoot() const;
  const char* recoveryReadInternalRoot() const;
  const char* recoveryReadRemovableRoot() const;
  const char* recoveryMutationTaskRoot(
      RecoveryMutationDomain domain) const;
  const char* recoveryMutationInternalRoot(
      RecoveryMutationDomain domain) const;
  const char* recoveryMutationRemovableRoot(
      RecoveryMutationDomain domain) const;
  PosixAtomicAlbumStore* recoveryMutationAlbumStore(
      RecoveryMutationDomain domain, const char* identity);
  void resetInternal(MountState state);
  void resetSd(MountState state);

  StorageMountConfig config_;
  StorageMountSnapshot snapshot_{};
  PosixAtomicAlbumStore internal_album_;
  PosixAtomicAlbumStore sd_album_;
  sdmmc_card_t* sd_card_ = nullptr;
  bool internal_registered_ = false;
  InternalMountAccess internal_access_ = InternalMountAccess::Unmounted;
  // Lifetime-owned copy used by the RO LittleFS registration.  Its readonly
  // bit makes esp_partition_write/erase reject mutation below VFS/LittleFS.
  esp_partition_t internal_readonly_partition_{};
  bool internal_readonly_partition_valid_ = false;
  bool recovery_mode_ = false;
  RecoveryMutationDomain recovery_mutation_domain_ =
      RecoveryMutationDomain::None;
  bool sd_registered_ = false;
  bool sd_bus_owned_ = false;
};

const char* mountStateName(MountState state);

}  // namespace storage
}  // namespace inkloop
