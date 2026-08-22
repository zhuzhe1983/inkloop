#pragma once

#include "inkloop/storage/upgrade_marker_journal.hpp"

namespace inkloop {
namespace storage {

// Dedicated `ink-migrate-v1` owner. Inspection never creates the namespace;
// writes are limited to its initialized/head/slot0/slot1 keys.
class EspNvsMigrationMarkerJournalStore final
    : public IMigrationMarkerJournalStore {
 public:
  MigrationJournalStoreCode inspectRaw(
      RawMigrationMarkerJournal& state) const override;
  MigrationJournalStoreCode writeSlotAndCommit(
      std::uint8_t slot,
      const EncodedMigrationJournalSlot& encoded) override;
  MigrationJournalStoreCode writeHeadAndMarkerAndCommit(
      std::uint64_t sequence) override;
};

}  // namespace storage
}  // namespace inkloop
