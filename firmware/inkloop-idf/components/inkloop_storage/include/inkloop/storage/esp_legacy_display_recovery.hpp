#pragma once

#include <string>

#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage/legacy_display_recovery.hpp"

namespace inkloop {
namespace storage {

// Native sole-storage-owner adapter for the released Arduino display journal.
// Inspection is side-effect-free. Resolution is only available through the
// explicit portable executor, which re-inspects before any mutation.
class EspLegacyDisplayRecovery final : public ILegacyDisplayRecoverySource,
                                       public ILegacyDisplayResolutionAdapter {
 public:
  explicit EspLegacyDisplayRecovery(EspStorageMountOwner& storage);

  LegacyDisplayRecoveryProbe inspect(
      LegacyDisplayRecoverySnapshot& output) const override;
  LegacyDisplayResolutionCode resolve(
      const LegacyDisplayRecoverySnapshot& expected,
      LegacyDisplayResolutionChoice choice);

  LegacyDisplayResolutionAdapterCode applyTargetCurrent(
      const LegacyDisplayJournal& journal) override;
  LegacyDisplayResolutionAdapterCode acknowledgeTask(
      const LegacyDisplayJournal& journal) override;
  LegacyDisplayResolutionAdapterCode clearJournalSet() override;

 private:
  static bool readRecord(const std::string& path,
                         RawLegacyDisplayRecord& output);
  static LegacyDisplayResolutionAdapterCode albumResult(
      const myai::Status& status);

  EspStorageMountOwner& storage_;
};

}  // namespace storage
}  // namespace inkloop
