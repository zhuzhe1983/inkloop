#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/storage/upgrade_marker_journal.hpp"

namespace inkloop {
namespace storage {

struct FingerprintedUpgradeRecord {
  // Unvalidated is the fail-closed default; callers must classify every fixed
  // protected record explicitly, including absence.
  RecordProbe probe = RecordProbe::Unvalidated;
  std::uint64_t logical_bytes = 0U;
  MigrationFingerprint content_fingerprint{};
};

struct UpgradeEvidenceSnapshot {
  bool internal_mounted = false;
  UpgradeSourceLayout source_layout = UpgradeSourceLayout::Unknown;
  std::uint16_t source_layout_schema_version = 0U;
  bool legacy_source_durable = false;
  std::array<FingerprintedUpgradeRecord,
             kProtectedNvsNamespaces.size()> nvs{};
  std::array<FingerprintedUpgradeRecord,
             kProtectedFilePaths.size()> files{};
  std::array<MigrationSlotEvidence, 2> native_slots{};
  MigrationMarkerJournalInspection marker_journal{};
};

enum class UpgradeEvidenceComposeCode : std::uint8_t {
  Ok,
  Changed,
  Ambiguous,
  IoError,
  UnsupportedSchema,
  InvalidEvidence,
};

// Callers obtain two independently read, bounded snapshots. Only byte-logical
// equality across both reads can produce planner evidence. The composer hashes
// fixed record identities, probes, sizes, and precomputed content digests; it
// never receives or returns secret/user record contents.
UpgradeEvidenceComposeCode composeUpgradeRecoveryEvidence(
    const UpgradeEvidenceSnapshot& first,
    const UpgradeEvidenceSnapshot& second,
    UpgradeRecoveryEvidence& output);

const char* upgradeEvidenceComposeCodeName(UpgradeEvidenceComposeCode code);

}  // namespace storage
}  // namespace inkloop
