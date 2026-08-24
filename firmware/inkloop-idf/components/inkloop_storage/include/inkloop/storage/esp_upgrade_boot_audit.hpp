#pragma once

#include "esp_err.h"
#include "esp_partition.h"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {

enum class NvsBootMountAccess : std::uint8_t {
  Unmounted,
  ReadOnlyAudit,
  ReadWriteProduct,
  RecoveryRequired,
};

// NVS initialization is not inherently read-only: IDF's normal init can
// repair duplicate entries, orphan blobs and interrupted page compaction.
// This owner initializes the same physical partition through a lifetime-safe
// descriptor copy whose readonly bit is forced on.  The partition layer then
// rejects every attempted write/erase, in addition to inventory handles using
// NVS_READONLY.  Product RW initialization is a separate deinit/init step.
class EspNvsBootMountOwner final {
 public:
  EspNvsBootMountOwner() = default;
  ~EspNvsBootMountOwner();

  EspNvsBootMountOwner(const EspNvsBootMountOwner&) = delete;
  EspNvsBootMountOwner& operator=(const EspNvsBootMountOwner&) = delete;

  esp_err_t mountReadOnlyAudit();
  esp_err_t promoteReadWriteProduct();
  // Every post-audit Recovery entry calls this, including failures that occur
  // after Product promoted NVS RW. It first proves that writer was torn down,
  // then attempts to restore the physical readonly descriptor. Failed writer
  // teardown preserves the live RW truth so the caller refuses networking;
  // a later failed RO remount remains deinitialized and RecoveryRequired.
  esp_err_t prepareRecoveryReadOnlyOrDeinit();
  NvsBootMountAccess access() const { return access_; }
  bool freshBlank() const { return fresh_blank_; }
  // Reports the owner's actual NVS API lifetime. Recovery must additionally
  // require recoveryReadOnlyReady(); it never reuses an RW instance or runs
  // generic initialization to repair a refused audit. A blank audit partition
  // deliberately has no NVS API instance yet.
  bool nvsApiReady() const { return nvs_initialized_; }
  bool recoveryReadOnlyReady() const {
    return access_ == NvsBootMountAccess::ReadOnlyAudit && nvs_initialized_;
  }
  // True only when no Product read-write NVS instance can still be live.
  // ReadOnlyAudit is safe both for an initialized readonly descriptor and a
  // fresh blank partition whose NVS API initialization was deliberately
  // deferred. Any inconsistent initialized non-RO state fails closed.
  bool recoveryWritesRevoked() const {
    if (access_ == NvsBootMountAccess::ReadWriteProduct) return false;
    return access_ == NvsBootMountAccess::ReadOnlyAudit || !nvs_initialized_;
  }

 private:
  esp_partition_t read_only_partition_{};
  NvsBootMountAccess access_ = NvsBootMountAccess::Unmounted;
  bool nvs_initialized_ = false;
  bool fresh_blank_ = false;
};

struct EspUpgradeBootAuditOutcome {
  esp_err_t initialization_status = ESP_FAIL;
  UpgradeAuditReport report{};

  bool allowsStartup() const {
    return initialization_status == ESP_OK && report.allowsInitialization();
  }
};

// Initializes NVS without any erase fallback, mounts the historical LittleFS
// partition without formatting/growing it, and inventories every protected
// NVS/file record read-only. Recovery and ambiguity are reported to the caller;
// this function never promotes, deletes, rewrites, formats, or resolves them.
EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit(
    EspStorageMountOwner& storage, EspNvsBootMountOwner& nvs);

// Audits a separately mounted asset backend (normally TF/SD) with the same
// exact file validators but no NVS probes. The caller chooses the backend only
// after loading the persisted storage preference; this function is read-only
// and never mounts, recovers, formats, or promotes transaction files.
UpgradeAuditReport runReadOnlyMountedFileUpgradeAudit(const char* root);

}  // namespace storage
}  // namespace inkloop
