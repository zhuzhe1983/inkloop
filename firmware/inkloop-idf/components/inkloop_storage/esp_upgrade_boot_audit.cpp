#include "inkloop/storage/esp_upgrade_boot_audit.hpp"

#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "nvs_flash.h"

namespace inkloop {
namespace storage {

EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit(
    EspStorageMountOwner& storage) {
  EspUpgradeBootAuditOutcome outcome;

  // No-free-pages and newer-schema outcomes are terminal here. Existing
  // device credentials and Wi-Fi settings are protected upgrade input; this
  // path never applies the destructive recovery suggested by generic samples.
  outcome.initialization_status = nvs_flash_init();
  if (outcome.initialization_status != ESP_OK) return outcome;

  outcome.initialization_status = storage.mountInternal();
  if (outcome.initialization_status != ESP_OK) return outcome;

  const EspNvsUpgradeInventory nvs;
  const PosixUpgradeInventory files(storage.taskRoot());
  outcome.report = auditUpgrade(files.inspect(nvs.inspect()));
  return outcome;
}

UpgradeAuditReport runReadOnlyMountedFileUpgradeAudit(const char* root) {
  if (!root) return UpgradeAuditReport{};
  const PosixUpgradeInventory files(root);
  std::array<RecordProbe, kProtectedNvsNamespaces.size()> no_nvs{};
  no_nvs.fill(RecordProbe::Missing);
  return auditUpgrade(files.inspect(no_nvs));
}

}  // namespace storage
}  // namespace inkloop
