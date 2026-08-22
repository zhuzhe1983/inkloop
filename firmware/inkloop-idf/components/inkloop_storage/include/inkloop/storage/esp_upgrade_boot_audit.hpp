#pragma once

#include "esp_err.h"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {

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
    EspStorageMountOwner& storage);

// Audits a separately mounted asset backend (normally TF/SD) with the same
// exact file validators but no NVS probes. The caller chooses the backend only
// after loading the persisted storage preference; this function is read-only
// and never mounts, recovers, formats, or promotes transaction files.
UpgradeAuditReport runReadOnlyMountedFileUpgradeAudit(const char* root);

}  // namespace storage
}  // namespace inkloop
