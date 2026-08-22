#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inkloop/recovery/recovery_portal.hpp"
#include "inkloop/storage/esp_legacy_display_recovery.hpp"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage/legacy_file_transaction_recovery.hpp"

namespace inkloop {

// Recovery-mode-only composition adapter. It owns no normal product writer and
// accepts only the fixed typed actions exposed by RecoveryPortalCore.
class EspRecoveryActionOwner final
    : public recovery::IRecoveryActionOwner {
 public:
  explicit EspRecoveryActionOwner(storage::EspStorageMountOwner& storage);
  ~EspRecoveryActionOwner() override;

  EspRecoveryActionOwner(const EspRecoveryActionOwner&) = delete;
  EspRecoveryActionOwner& operator=(const EspRecoveryActionOwner&) = delete;

  bool ready() const { return mutex_ != nullptr; }
  recovery::RecoveryActionReadResult inspectRecoveryActions(
      recovery::RecoveryActionInventory& output) override;
  recovery::RecoveryActionResolveResult resolveRecoveryAction(
      const recovery::RecoveryActionRequest& request) override;

  // Successful resolution requests a reboot only when a fresh complete boot
  // audit is clean. The grace period lets the HTTP response leave the device
  // before the recovery network is stopped.
  bool restartReady(std::uint32_t now_ms) const;

 private:
  static constexpr std::size_t kFileSnapshotCount = 3U;

  bool lock();
  void unlock();
  bool postActionAuditClean() const;
  bool inspectDisplay(recovery::RecoveryActionSnapshot& output);
  bool inspectFile(std::size_t cache_index,
                   storage::LegacyFileTransactionTarget target,
                   recovery::RecoveryActionDomain domain,
                   recovery::RecoveryActionBackend backend,
                   recovery::RecoveryActionSnapshot& output);
  const recovery::RecoveryActionSnapshot* findCached(
      recovery::RecoveryActionDomain domain,
      recovery::RecoveryActionBackend backend) const;

  storage::EspStorageMountOwner& storage_;
  storage::EspLegacyDisplayRecovery display_;
  storage::PosixLegacyFileTransactionRecovery files_;
  SemaphoreHandle_t mutex_ = nullptr;
  recovery::RecoveryActionInventory cached_inventory_{};
  storage::LegacyDisplayRecoverySnapshot display_snapshot_{};
  std::array<storage::LegacyFileTransactionSnapshot,
             kFileSnapshotCount> file_snapshots_{};
  std::array<bool, kFileSnapshotCount> file_snapshot_valid_{};
  bool display_snapshot_valid_ = false;
  std::atomic<std::uint32_t> restart_not_before_ms_{0U};
};

}  // namespace inkloop
