#include "inkloop/storage/esp_upgrade_boot_audit.hpp"

#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace inkloop {
namespace storage {
namespace {

constexpr char kDefaultNvsLabel[] = "nvs";
constexpr std::uint32_t kDefaultNvsAddress = 0x00009000U;
constexpr std::uint32_t kDefaultNvsSize = 0x00005000U;

esp_err_t partitionIsBlank(const esp_partition_t& partition, bool& blank) {
  blank = true;
  std::array<std::uint8_t, 256> buffer{};
  for (std::size_t offset = 0U; offset < partition.size;
       offset += buffer.size()) {
    const std::size_t length =
        std::min(buffer.size(),
                 static_cast<std::size_t>(partition.size) - offset);
    const esp_err_t read =
        esp_partition_read(&partition, offset, buffer.data(), length);
    if (read != ESP_OK) return read;
    if (!std::all_of(buffer.begin(), buffer.begin() + length,
                     [](std::uint8_t value) { return value == 0xFFU; })) {
      blank = false;
      return ESP_OK;
    }
  }
  return ESP_OK;
}

}  // namespace

EspNvsBootMountOwner::~EspNvsBootMountOwner() {
  if (nvs_initialized_)
    (void)nvs_flash_deinit_partition(kDefaultNvsLabel);
}

esp_err_t EspNvsBootMountOwner::mountReadOnlyAudit() {
  if (access_ != NvsBootMountAccess::Unmounted)
    return ESP_ERR_INVALID_STATE;
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
  // A secure read-only descriptor adapter must preserve the configured key
  // scheme.  This product intentionally has NVS encryption disabled; fail the
  // build-time variant closed instead of silently opening ciphertext as plain.
  return ESP_ERR_NOT_SUPPORTED;
#else
  // Detect an unexpected earlier initializer. Deinitializing only releases
  // RAM ownership/handles; it never writes flash. Its presence proves that
  // this boot can no longer establish a mutation-free audit boundary.
  const esp_err_t prior = nvs_flash_deinit_partition(kDefaultNvsLabel);
  if (prior == ESP_OK) {
    access_ = NvsBootMountAccess::RecoveryRequired;
    return ESP_ERR_INVALID_STATE;
  }
  if (prior != ESP_ERR_NVS_NOT_INITIALIZED) {
    access_ = NvsBootMountAccess::RecoveryRequired;
    return prior;
  }

  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
      kDefaultNvsLabel);
  if (!partition || partition->address != kDefaultNvsAddress ||
      partition->size != kDefaultNvsSize || partition->encrypted) {
    access_ = NvsBootMountAccess::RecoveryRequired;
    return ESP_ERR_NOT_FOUND;
  }
  read_only_partition_ = *partition;
  read_only_partition_.readonly = true;
  const esp_err_t blank_status =
      partitionIsBlank(read_only_partition_, fresh_blank_);
  if (blank_status != ESP_OK) {
    std::memset(&read_only_partition_, 0, sizeof(read_only_partition_));
    access_ = NvsBootMountAccess::RecoveryRequired;
    return blank_status;
  }
  // IDF must activate a page when a completely erased NVS partition is first
  // initialized.  That is necessarily a write, so defer initialization until
  // post-audit promotion and represent every namespace as Missing meanwhile.
  if (fresh_blank_) {
    access_ = NvsBootMountAccess::ReadOnlyAudit;
    return ESP_OK;
  }
  const esp_err_t mounted =
      nvs_flash_init_partition_ptr(&read_only_partition_);
  if (mounted != ESP_OK) {
    std::memset(&read_only_partition_, 0, sizeof(read_only_partition_));
    access_ = NvsBootMountAccess::RecoveryRequired;
    return mounted;
  }
  nvs_initialized_ = true;
  access_ = NvsBootMountAccess::ReadOnlyAudit;
  return ESP_OK;
#endif
}

esp_err_t EspNvsBootMountOwner::promoteReadWriteProduct() {
  if (access_ != NvsBootMountAccess::ReadOnlyAudit)
    return ESP_ERR_INVALID_STATE;
  if (nvs_initialized_) {
    const esp_err_t unmounted =
        nvs_flash_deinit_partition(kDefaultNvsLabel);
    if (unmounted != ESP_OK) {
      access_ = NvsBootMountAccess::RecoveryRequired;
      return unmounted;
    }
    nvs_initialized_ = false;
  }
  access_ = NvsBootMountAccess::Unmounted;
  std::memset(&read_only_partition_, 0, sizeof(read_only_partition_));
  const esp_err_t mounted = nvs_flash_init();
  if (mounted != ESP_OK) {
    access_ = NvsBootMountAccess::RecoveryRequired;
    return mounted;
  }
  fresh_blank_ = false;
  nvs_initialized_ = true;
  access_ = NvsBootMountAccess::ReadWriteProduct;
  return ESP_OK;
}

esp_err_t EspNvsBootMountOwner::prepareRecoveryReadOnlyOrDeinit() {
  if (access_ == NvsBootMountAccess::ReadOnlyAudit) return ESP_OK;

  // The only initialized non-RO state this owner permits is Product RW.  Tear
  // it down before doing anything else.  nvs_flash_deinit_partition releases
  // the in-RAM NVS owner and performs no flash mutation.
  if (nvs_initialized_) {
    const esp_err_t unmounted =
        nvs_flash_deinit_partition(kDefaultNvsLabel);
    if (unmounted != ESP_OK) {
      // Deinitialization did not prove that the Product writer was revoked.
      // Preserve both the initialized flag and the real access state so
      // Recovery cannot mistake a possibly-live writer for a safe failure.
      return unmounted;
    }
    nvs_initialized_ = false;
  }

  access_ = NvsBootMountAccess::Unmounted;
  fresh_blank_ = false;
  std::memset(&read_only_partition_, 0, sizeof(read_only_partition_));
  const esp_err_t mounted = mountReadOnlyAudit();
  if (mounted != ESP_OK) {
    // mountReadOnlyAudit already fails closed, but keep this invariant local:
    // no caller may infer an NVS API writer from a failed Recovery demotion.
    nvs_initialized_ = false;
    access_ = NvsBootMountAccess::RecoveryRequired;
  }
  return mounted;
}

EspUpgradeBootAuditOutcome runReadOnlyUpgradeBootAudit(
    EspStorageMountOwner& storage, EspNvsBootMountOwner& nvs) {
  EspUpgradeBootAuditOutcome outcome;

  // No-free-pages and newer-schema outcomes are terminal here. Existing
  // device credentials and Wi-Fi settings are protected upgrade input; this
  // path never applies the destructive recovery suggested by generic samples.
  outcome.initialization_status = nvs.mountReadOnlyAudit();
  if (outcome.initialization_status != ESP_OK) return outcome;

  outcome.initialization_status = storage.mountInternalReadOnly();
  if (outcome.initialization_status != ESP_OK) return outcome;

  const EspNvsUpgradeInventory nvs_inventory;
  const PosixUpgradeInventory files(storage.auditInternalRoot());
  std::array<RecordProbe, kProtectedNvsNamespaces.size()> nvs_probes{};
  nvs_probes.fill(RecordProbe::Missing);
  if (!nvs.freshBlank()) nvs_probes = nvs_inventory.inspect();
  outcome.report = auditUpgrade(files.inspect(nvs_probes));
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
