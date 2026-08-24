#pragma once

#include <cstdint>

#include "inkloop/board.hpp"
#include "inkloop/settings/esp_nvs_settings_store.hpp"
#include "inkloop/settings/settings_journal.hpp"
#include "inkloop/storage/esp_nvs_upgrade_marker_journal.hpp"
#include "inkloop/storage/esp_upgrade_boot_audit.hpp"

namespace inkloop {

enum class NativeSettingsAuthorityKind : std::uint8_t {
  None,
  FreshDefaults,
  NativeJournal,
};

// Capability handed to NativeDeviceStateOwner only after read-only evidence
// selection and any durable migration phases have completed.  It contains no
// settings values or credentials and is revalidated against the target
// generation by the consumer.
struct NativeSettingsMigrationAuthorization {
  NativeSettingsAuthorityKind kind = NativeSettingsAuthorityKind::None;
  std::uint32_t observed_generation = 0U;
  std::uint64_t migration_generation = 0U;
  storage::MigrationFingerprint source_fingerprint{};

  bool valid() const;
};

enum class NativeSettingsMigrationPlanKind : std::uint8_t {
  None,
  FreshNoMigration,
  NativeNoMigration,
  Start,
  Resume,
  RecoverPreparedHead,
  Complete,
};

// Immutable read-only audit result. execute() re-collects every source and
// target record after RO->RW promotion and requires this identity to match
// before the first NVS commit.
struct NativeSettingsMigrationPlan {
  NativeSettingsMigrationPlanKind kind =
      NativeSettingsMigrationPlanKind::None;
  storage::MigrationFingerprint source_fingerprint{};
  std::uint16_t source_schema = 0U;
  std::uint32_t observed_generation = 0U;
  std::uint64_t target_generation = 0U;
  std::uint64_t marker_sequence = 0U;
  storage::MigrationPhase marker_phase = storage::MigrationPhase::None;
  // A fresh import rolls back to the untouched Arduino snapshot.  A
  // beta27/beta29 completion import rolls back to the previous native journal
  // slot, which is deliberately the opposite slot from target_generation.
  storage::MigrationRollbackSource rollback_source =
      storage::MigrationRollbackSource::None;
};

enum class NativeSettingsMigrationGateCode : std::uint8_t {
  Ok,
  InvalidState,
  SourceCorrupt,
  SourceChanged,
  TargetCorrupt,
  MarkerCorrupt,
  MarkerMismatch,
  MarkerWriteFailed,
  TargetWriteFailed,
};

class NativeSettingsMigrationGate final {
 public:
  NativeSettingsMigrationGate(
      const BoardDescriptor& board,
      const storage::EspNvsBootMountOwner& nvs_boot_mount);

  // Must run while NVS is physically mounted through the read-only descriptor
  // (or is a verified all-0xFF fresh partition) and internal LittleFS is RO.
  NativeSettingsMigrationGateCode auditReadOnly(
      NativeSettingsMigrationPlan& output);

  // Must run only after both NVS and LittleFS promotion.  It performs the
  // selected marker/target phases and yields the sole Product authorization.
  NativeSettingsMigrationGateCode execute(
      const NativeSettingsMigrationPlan& authorized_plan,
      NativeSettingsMigrationAuthorization& authorization);

 private:
  struct Evidence;

  NativeSettingsMigrationGateCode collect(Evidence& output);
  NativeSettingsMigrationGateCode compose(
      const Evidence& evidence, NativeSettingsMigrationPlan& output) const;
  NativeSettingsMigrationGateCode advance(
      NativeSettingsMigrationAuthorization& authorization);
  NativeSettingsMigrationGateCode recoverPreparedHead(
      const NativeSettingsMigrationPlan& plan);

  const storage::EspNvsBootMountOwner& nvs_boot_mount_;
  settings::DeviceSettings defaults_;
  settings::EspNvsSettingsJournalStore settings_journal_{};
  settings::SettingsStoreCore settings_store_;
  settings::EspNvsReadOnlyLegacyPortalSource legacy_{};
  settings::EspPsaLegacySha256Verifier legacy_sha_{};
  storage::EspNvsMigrationMarkerJournalStore marker_store_{};
};

const char* nativeSettingsMigrationGateCodeName(
    NativeSettingsMigrationGateCode code);

}  // namespace inkloop
