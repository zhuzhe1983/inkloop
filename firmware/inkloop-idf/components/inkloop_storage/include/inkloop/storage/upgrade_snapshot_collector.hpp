#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/storage/upgrade_evidence_composer.hpp"

namespace inkloop {
namespace storage {

enum class UpgradeRecordDomain : std::uint8_t {
  NvsNamespace,
  File,
};

struct UpgradeRecordId {
  UpgradeRecordDomain domain = UpgradeRecordDomain::NvsNamespace;
  std::size_t index = 0U;
};

inline constexpr std::uint64_t kMaximumUpgradeNvsNamespaceBytes =
    512U * 1024U;

bool upgradeRecordIdValid(UpgradeRecordId record);
const char* upgradeRecordName(UpgradeRecordId record);
std::uint64_t upgradeRecordMaximumBytes(UpgradeRecordId record);

// Raw record bytes are pushed through this transient interface. Collectors and
// executors retain only bounded byte counts and SHA-256 digests.
class IUpgradeByteSink {
 public:
  virtual ~IUpgradeByteSink() = default;
  virtual bool write(const std::uint8_t* bytes, std::size_t length) = 0;
};

enum class UpgradeRecordStreamCode : std::uint8_t {
  Missing,
  Valid,
  Recoverable,
  Ambiguous,
  Invalid,
  Unvalidated,
  TooLarge,
  IoError,
};

struct UpgradeSnapshotMetadata {
  bool internal_mounted = false;
  UpgradeSourceLayout source_layout = UpgradeSourceLayout::Unknown;
  std::uint16_t source_layout_schema_version = 0U;
  bool legacy_source_durable = false;
  std::array<MigrationSlotEvidence, 2> native_slots{};
  MigrationMarkerJournalInspection marker_journal{};
};

// Implementations must be read-only. NVS implementations provide a stable,
// canonical byte stream for each complete namespace, including key names,
// types and values. File implementations stream the exact file bytes.
class IUpgradeSnapshotSource {
 public:
  virtual ~IUpgradeSnapshotSource() = default;
  virtual bool inspectMetadata(UpgradeSnapshotMetadata& output) const = 0;
  virtual UpgradeRecordStreamCode streamRecord(
      UpgradeRecordId record, std::uint64_t maximum_bytes,
      IUpgradeByteSink& sink) const = 0;
};

enum class UpgradeSnapshotCollectCode : std::uint8_t {
  Ok,
  Changed,
  Ambiguous,
  IoError,
  UnsupportedSchema,
  TooLarge,
  InvalidEvidence,
};

struct CollectedUpgradeRecovery {
  // This snapshot is safe to retain: it contains only classifications, byte
  // counts and digests, never source bytes.
  UpgradeEvidenceSnapshot snapshot{};
  UpgradeRecoveryEvidence evidence{};
  UpgradeRecoveryPlan plan{};
};

class UpgradeSnapshotCollector final {
 public:
  explicit UpgradeSnapshotCollector(const IUpgradeSnapshotSource& source)
      : source_(source) {}

  // Every fixed record is independently streamed twice. The composer/planner
  // are invoked only after both complete passes are bounded and coherent.
  UpgradeSnapshotCollectCode collect(
      CollectedUpgradeRecovery& output) const;

 private:
  const IUpgradeSnapshotSource& source_;
};

const char* upgradeRecordStreamCodeName(UpgradeRecordStreamCode code);
const char* upgradeSnapshotCollectCodeName(UpgradeSnapshotCollectCode code);

}  // namespace storage
}  // namespace inkloop
