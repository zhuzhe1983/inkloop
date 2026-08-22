#pragma once

#include <array>
#include <string>

#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"
#include "inkloop/storage/upgrade_snapshot_collector.hpp"

namespace inkloop {
namespace storage {

// Marker/slot/layout ownership belongs to the boot composer and target store.
// This narrow seam lets the concrete byte source refresh that metadata before
// each full collector pass without inventing an authority policy here.
class IUpgradeSnapshotMetadataProvider {
 public:
  virtual ~IUpgradeSnapshotMetadataProvider() = default;
  virtual bool inspectUpgradeSnapshotMetadata(
      UpgradeSnapshotMetadata& output) const = 0;
};

// Native read-only source for the existing default NVS partition and mounted
// internal filesystem. NVS namespaces are serialized in a canonical key/type
// order; files are streamed verbatim in bounded chunks. The object is a boot-
// phase single-owner adapter: inspectMetadata() begins one pass and every
// subsequent streamRecord() uses the semantic classifications from that pass.
class EspUpgradeSnapshotSource final : public IUpgradeSnapshotSource {
 public:
  EspUpgradeSnapshotSource(
      std::string internal_root,
      const IUpgradeSnapshotMetadataProvider& metadata_provider);

  bool inspectMetadata(UpgradeSnapshotMetadata& output) const override;
  UpgradeRecordStreamCode streamRecord(
      UpgradeRecordId record, std::uint64_t maximum_bytes,
      IUpgradeByteSink& sink) const override;

  bool ready() const { return paths_.pathsValid(); }

 private:
  UpgradeRecordStreamCode streamNvsNamespace(
      std::size_t index, std::uint64_t maximum_bytes,
      IUpgradeByteSink& sink) const;
  UpgradeRecordStreamCode streamFile(
      std::size_t index, std::uint64_t maximum_bytes,
      IUpgradeByteSink& sink) const;

  std::string internal_root_;
  const IUpgradeSnapshotMetadataProvider& metadata_provider_;
  EspNvsUpgradeInventory nvs_{};
  PosixUpgradeInventory paths_;
  mutable std::array<RecordProbe, kProtectedNvsNamespaces.size()>
      nvs_classifications_{};
  mutable std::array<RecordProbe, kProtectedFilePaths.size()>
      file_classifications_{};
  mutable bool pass_ready_ = false;
};

}  // namespace storage
}  // namespace inkloop
