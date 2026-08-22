#include "inkloop/storage/persistence_compatibility.hpp"

#include <cstring>

namespace inkloop {
namespace storage {
namespace {

constexpr UpgradeRecordId nvs(std::size_t index) {
  return {UpgradeRecordDomain::NvsNamespace, index};
}

constexpr UpgradeRecordId file(std::size_t index) {
  return {UpgradeRecordDomain::File, index};
}

const std::array<PersistenceCompatibilityEntry,
                 kPersistenceCompatibilityEntryCount> kContract{{
    {nvs(0), "inkloop-v2",
     PersistenceCompatibilityMode::LegacyRetained,
     "upgrade inventory only"},
    {nvs(1), "inkloop",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "EspNvsInkloopIdentityStore"},
    {nvs(2), "ink-myai-v1",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "EspNvsCredentialJournalStore"},
    {nvs(3), "ink-portal",
     PersistenceCompatibilityMode::ReadOnlyImportRetained,
     "EspNvsReadOnlyLegacyPortalSource -> ink-settings-v1"},
    {nvs(4), "ink-album-meta",
     PersistenceCompatibilityMode::LegacyRetained,
     "upgrade inventory only"},
    {nvs(5), "ink-pair-ui",
     PersistenceCompatibilityMode::LegacyRetained,
     "upgrade inventory only"},
    {nvs(6), "nvs.net80211",
     PersistenceCompatibilityMode::EspSystemShared,
     "ESP-IDF Wi-Fi station"},
    {nvs(7), "phy",
     PersistenceCompatibilityMode::EspSystemShared,
     "ESP-IDF PHY calibration"},
    {nvs(8), "cal_data",
     PersistenceCompatibilityMode::EspSystemShared,
     "ESP-IDF radio calibration"},
    {file(0), "/tasks.json",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixTaskStore"},
    {file(1), "/tasks.next",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixTaskStore transaction recovery"},
    {file(2), "/tasks.prev",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixTaskStore transaction recovery"},
    {file(3), "/display-txn.json",
     PersistenceCompatibilityMode::ExplicitPhysicalResolution,
     "recovery composer only"},
    {file(4), "/display-txn.next",
     PersistenceCompatibilityMode::ExplicitPhysicalResolution,
     "recovery composer only"},
    {file(5), "/display-txn.prev",
     PersistenceCompatibilityMode::ExplicitPhysicalResolution,
     "recovery composer only"},
    {file(6), "/inkloop-album/index.json",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixAtomicAlbumStore"},
    {file(7), "/inkloop-album/index.next",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixAtomicAlbumStore transaction recovery"},
    {file(8), "/inkloop-album/index.prev",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixAtomicAlbumStore transaction recovery"},
    {file(9), "/inkloop/myai-chat.txt",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixChatLineStore"},
    {file(10), "/inkloop/myai-chat.prev.txt",
     PersistenceCompatibilityMode::SharedRollbackCompatible,
     "PosixChatLineStore rotation"},
}};

bool recordEqual(UpgradeRecordId left, UpgradeRecordId right) {
  return left.domain == right.domain && left.index == right.index;
}

}  // namespace

const std::array<PersistenceCompatibilityEntry,
                 kPersistenceCompatibilityEntryCount>&
persistenceCompatibilityContract() {
  return kContract;
}

bool persistenceCompatibilityContractValid() {
  if (kContract.size() != kPersistenceCompatibilityEntryCount) return false;
  for (std::size_t at = 0U; at < kContract.size(); ++at) {
    const UpgradeRecordId expected = at < kProtectedNvsNamespaces.size()
        ? nvs(at)
        : file(at - kProtectedNvsNamespaces.size());
    const PersistenceCompatibilityEntry& entry = kContract[at];
    const char* expected_name = upgradeRecordName(expected);
    if (!recordEqual(entry.record, expected) || !entry.name ||
        !entry.native_consumer || !expected_name ||
        std::strcmp(entry.name, expected_name) != 0) {
      return false;
    }
    const bool display = expected.domain == UpgradeRecordDomain::File &&
        expected.index >= 3U && expected.index <= 5U;
    if (display != (entry.mode ==
                    PersistenceCompatibilityMode::ExplicitPhysicalResolution)) {
      return false;
    }
  }
  return true;
}

const PersistenceCompatibilityEntry* persistenceCompatibilityEntry(
    UpgradeRecordId record) {
  if (!upgradeRecordIdValid(record)) return nullptr;
  const std::size_t at = record.domain == UpgradeRecordDomain::NvsNamespace
      ? record.index
      : kProtectedNvsNamespaces.size() + record.index;
  const PersistenceCompatibilityEntry& entry = kContract[at];
  return recordEqual(entry.record, record) ? &entry : nullptr;
}

const char* persistenceCompatibilityModeName(
    PersistenceCompatibilityMode mode) {
  switch (mode) {
    case PersistenceCompatibilityMode::SharedRollbackCompatible:
      return "SHARED_ROLLBACK_COMPATIBLE";
    case PersistenceCompatibilityMode::ReadOnlyImportRetained:
      return "READ_ONLY_IMPORT_RETAINED";
    case PersistenceCompatibilityMode::LegacyRetained:
      return "LEGACY_RETAINED";
    case PersistenceCompatibilityMode::EspSystemShared:
      return "ESP_SYSTEM_SHARED";
    case PersistenceCompatibilityMode::ExplicitPhysicalResolution:
      return "EXPLICIT_PHYSICAL_RESOLUTION";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
