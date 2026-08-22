#include "inkloop/storage/upgrade_evidence_composer.hpp"

#include <algorithm>
#include <cstring>

#include "inkloop/storage/sha256.hpp"

namespace inkloop {
namespace storage {
namespace {

bool fingerprintPresent(const MigrationFingerprint& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0U; });
}

bool markerEqual(const MigrationMarker& left,
                 const MigrationMarker& right) {
  return left.schema_version == right.schema_version &&
      left.source_layout_schema_version ==
          right.source_layout_schema_version &&
      left.generation == right.generation &&
      left.source_fingerprint == right.source_fingerprint &&
      left.phase == right.phase && left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source &&
      left.checksum == right.checksum;
}

bool recordEqual(const FingerprintedUpgradeRecord& left,
                 const FingerprintedUpgradeRecord& right) {
  return left.probe == right.probe &&
      left.logical_bytes == right.logical_bytes &&
      left.content_fingerprint == right.content_fingerprint;
}

bool slotEqual(const MigrationSlotEvidence& left,
               const MigrationSlotEvidence& right) {
  return left.probe == right.probe &&
      left.migration_generation == right.migration_generation &&
      left.source_fingerprint == right.source_fingerprint;
}

bool journalEqual(const MigrationMarkerJournalInspection& left,
                  const MigrationMarkerJournalInspection& right) {
  return left.probe == right.probe && left.sequence == right.sequence &&
      markerEqual(left.marker, right.marker);
}

bool snapshotEqual(const UpgradeEvidenceSnapshot& left,
                   const UpgradeEvidenceSnapshot& right) {
  if (left.internal_mounted != right.internal_mounted ||
      left.source_layout != right.source_layout ||
      left.source_layout_schema_version !=
          right.source_layout_schema_version ||
      left.legacy_source_durable != right.legacy_source_durable ||
      !journalEqual(left.marker_journal, right.marker_journal))
    return false;
  for (std::size_t at = 0U; at < left.nvs.size(); ++at)
    if (!recordEqual(left.nvs[at], right.nvs[at])) return false;
  for (std::size_t at = 0U; at < left.files.size(); ++at)
    if (!recordEqual(left.files[at], right.files[at])) return false;
  for (std::size_t at = 0U; at < left.native_slots.size(); ++at)
    if (!slotEqual(left.native_slots[at], right.native_slots[at])) return false;
  return true;
}

UpgradeEvidenceComposeCode validateRecord(
    const FingerprintedUpgradeRecord& record) {
  switch (record.probe) {
    case RecordProbe::Missing:
      return record.logical_bytes == 0U &&
              !fingerprintPresent(record.content_fingerprint)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case RecordProbe::Valid:
    case RecordProbe::Recoverable:
      return fingerprintPresent(record.content_fingerprint)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case RecordProbe::Ambiguous:
    case RecordProbe::Invalid:
    case RecordProbe::Unvalidated:
      return UpgradeEvidenceComposeCode::Ambiguous;
    case RecordProbe::IoError:
      return UpgradeEvidenceComposeCode::IoError;
  }
  return UpgradeEvidenceComposeCode::InvalidEvidence;
}

UpgradeEvidenceComposeCode validateNativeSlot(
    const MigrationSlotEvidence& slot) {
  switch (slot.probe) {
    case MigrationSlotProbe::Missing:
      return slot.migration_generation == 0U &&
              !fingerprintPresent(slot.source_fingerprint)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case MigrationSlotProbe::Valid:
      return slot.migration_generation != 0U &&
              fingerprintPresent(slot.source_fingerprint)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case MigrationSlotProbe::Unknown:
    case MigrationSlotProbe::Invalid:
      return UpgradeEvidenceComposeCode::Ambiguous;
    case MigrationSlotProbe::IoError:
      return UpgradeEvidenceComposeCode::IoError;
  }
  return UpgradeEvidenceComposeCode::InvalidEvidence;
}

RecordProbe collapseDisplay(
    const std::array<FingerprintedUpgradeRecord,
                     kProtectedFilePaths.size()>& files) {
  for (std::size_t at = 3U; at <= 5U; ++at)
    if (files[at].probe != RecordProbe::Missing)
      return RecordProbe::Unvalidated;
  return RecordProbe::Missing;
}

UpgradeAuditInput auditInput(const UpgradeEvidenceSnapshot& snapshot) {
  UpgradeAuditInput input;
  input.internal_mounted = snapshot.internal_mounted;
  for (std::size_t at = 0U; at < snapshot.nvs.size(); ++at)
    input.application_nvs[at] = snapshot.nvs[at].probe;
  input.tasks = {snapshot.files[0].probe, snapshot.files[1].probe,
                 snapshot.files[2].probe};
  input.display_transaction = collapseDisplay(snapshot.files);
  input.album = {snapshot.files[6].probe, snapshot.files[7].probe,
                 snapshot.files[8].probe};
  input.chat_current = snapshot.files[9].probe;
  input.chat_previous = snapshot.files[10].probe;
  return input;
}

bool update(Sha256& hash, const void* bytes, std::size_t length) {
  return hash.update(static_cast<const std::uint8_t*>(bytes), length);
}

bool update8(Sha256& hash, std::uint8_t value) {
  return update(hash, &value, sizeof(value));
}

bool update16(Sha256& hash, std::uint16_t value) {
  const std::uint8_t bytes[] = {
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8U)};
  return update(hash, bytes, sizeof(bytes));
}

bool update64(Sha256& hash, std::uint64_t value) {
  std::uint8_t bytes[8]{};
  for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
    bytes[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
  return update(hash, bytes, sizeof(bytes));
}

bool updateName(Sha256& hash, const char* name) {
  if (!name) return false;
  const std::size_t length = std::strlen(name);
  return length <= 0xFFFFU &&
      update16(hash, static_cast<std::uint16_t>(length)) &&
      update(hash, name, length);
}

bool updateRecord(Sha256& hash, std::uint8_t domain, const char* name,
                  const FingerprintedUpgradeRecord& record) {
  return update8(hash, domain) && updateName(hash, name) &&
      update8(hash, static_cast<std::uint8_t>(record.probe)) &&
      update64(hash, record.logical_bytes) &&
      update(hash, record.content_fingerprint.data(),
             record.content_fingerprint.size());
}

bool sourceFingerprint(const UpgradeEvidenceSnapshot& snapshot,
                       MigrationFingerprint& output) {
  static constexpr char kDomain[] = "INKLOOP-UPGRADE-SOURCE-V1";
  Sha256 hash;
  if (!update(hash, kDomain, sizeof(kDomain) - 1U) ||
      !update8(hash, static_cast<std::uint8_t>(snapshot.source_layout)) ||
      !update16(hash, snapshot.source_layout_schema_version))
    return false;
  for (std::size_t at = 0U; at < snapshot.nvs.size(); ++at)
    if (!updateRecord(hash, static_cast<std::uint8_t>('N'),
                      kProtectedNvsNamespaces[at], snapshot.nvs[at]))
      return false;
  for (std::size_t at = 0U; at < snapshot.files.size(); ++at)
    if (!updateRecord(hash, static_cast<std::uint8_t>('F'),
                      kProtectedFilePaths[at], snapshot.files[at]))
      return false;
  return hash.finish(output) && fingerprintPresent(output);
}

bool allProtectedMissing(const UpgradeEvidenceSnapshot& snapshot) {
  for (const FingerprintedUpgradeRecord& record : snapshot.nvs)
    if (record.probe != RecordProbe::Missing) return false;
  for (const FingerprintedUpgradeRecord& record : snapshot.files)
    if (record.probe != RecordProbe::Missing) return false;
  return true;
}

bool allNativeSlotsMissing(const UpgradeEvidenceSnapshot& snapshot) {
  return snapshot.native_slots[0].probe == MigrationSlotProbe::Missing &&
      snapshot.native_slots[1].probe == MigrationSlotProbe::Missing;
}

UpgradeEvidenceComposeCode validateJournal(
    const MigrationMarkerJournalInspection& journal) {
  const MigrationMarker empty_marker{};
  switch (journal.probe) {
    case MigrationMarkerJournalProbe::Missing:
      return journal.sequence == 0U && markerEqual(journal.marker, empty_marker)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case MigrationMarkerJournalProbe::Valid:
      return journal.sequence != 0U && migrationMarkerValid(journal.marker)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case MigrationMarkerJournalProbe::Torn:
    case MigrationMarkerJournalProbe::Corrupt:
      return journal.sequence == 0U && markerEqual(journal.marker, empty_marker)
          ? UpgradeEvidenceComposeCode::Ok
          : UpgradeEvidenceComposeCode::InvalidEvidence;
    case MigrationMarkerJournalProbe::IoError:
      return UpgradeEvidenceComposeCode::IoError;
  }
  return UpgradeEvidenceComposeCode::InvalidEvidence;
}

MigrationMarkerProbe plannerMarkerProbe(
    MigrationMarkerJournalProbe probe) {
  switch (probe) {
    case MigrationMarkerJournalProbe::Missing:
      return MigrationMarkerProbe::Missing;
    case MigrationMarkerJournalProbe::Valid:
      return MigrationMarkerProbe::Valid;
    case MigrationMarkerJournalProbe::Torn:
      return MigrationMarkerProbe::Torn;
    case MigrationMarkerJournalProbe::Corrupt:
      return MigrationMarkerProbe::Corrupt;
    case MigrationMarkerJournalProbe::IoError:
      return MigrationMarkerProbe::IoError;
  }
  return MigrationMarkerProbe::IoError;
}

}  // namespace

UpgradeEvidenceComposeCode composeUpgradeRecoveryEvidence(
    const UpgradeEvidenceSnapshot& first,
    const UpgradeEvidenceSnapshot& second,
    UpgradeRecoveryEvidence& output) {
  output = UpgradeRecoveryEvidence{};
  if (!snapshotEqual(first, second))
    return UpgradeEvidenceComposeCode::Changed;
  if (!first.internal_mounted) return UpgradeEvidenceComposeCode::IoError;

  for (const FingerprintedUpgradeRecord& record : first.nvs) {
    const UpgradeEvidenceComposeCode code = validateRecord(record);
    if (code != UpgradeEvidenceComposeCode::Ok) return code;
  }
  for (const FingerprintedUpgradeRecord& record : first.files) {
    const UpgradeEvidenceComposeCode code = validateRecord(record);
    if (code != UpgradeEvidenceComposeCode::Ok) return code;
  }
  for (const MigrationSlotEvidence& slot : first.native_slots) {
    const UpgradeEvidenceComposeCode code = validateNativeSlot(slot);
    if (code != UpgradeEvidenceComposeCode::Ok) return code;
  }
  const UpgradeEvidenceComposeCode journal_code =
      validateJournal(first.marker_journal);
  if (journal_code != UpgradeEvidenceComposeCode::Ok) return journal_code;

  const UpgradeAuditReport audit = auditUpgrade(auditInput(first));
  if (audit.result == UpgradeAuditResult::SourceUnavailable)
    return UpgradeEvidenceComposeCode::IoError;
  if (audit.result == UpgradeAuditResult::Ambiguous ||
      audit.result == UpgradeAuditResult::DisplayResolutionRequired)
    return UpgradeEvidenceComposeCode::Ambiguous;

  output.audit = audit;
  output.source_layout = first.source_layout;
  output.source_layout_schema_version =
      first.source_layout_schema_version;
  output.marker_probe = plannerMarkerProbe(first.marker_journal.probe);
  if (first.marker_journal.probe == MigrationMarkerJournalProbe::Valid)
    output.marker = first.marker_journal.marker;
  output.native_slots = first.native_slots;

  switch (first.source_layout) {
    case UpgradeSourceLayout::Fresh:
      if (first.source_layout_schema_version != 0U ||
          first.legacy_source_durable || !allProtectedMissing(first) ||
          !allNativeSlotsMissing(first) ||
          audit.result != UpgradeAuditResult::Fresh)
        return UpgradeEvidenceComposeCode::InvalidEvidence;
      return UpgradeEvidenceComposeCode::Ok;
    case UpgradeSourceLayout::Legacy:
      if (first.source_layout_schema_version !=
          kSupportedLegacyLayoutSchemaVersion)
        return UpgradeEvidenceComposeCode::UnsupportedSchema;
      break;
    case UpgradeSourceLayout::Native:
      if (first.source_layout_schema_version !=
              kSupportedNativeLayoutSchemaVersion ||
          first.legacy_source_durable)
        return first.source_layout_schema_version !=
                kSupportedNativeLayoutSchemaVersion
            ? UpgradeEvidenceComposeCode::UnsupportedSchema
            : UpgradeEvidenceComposeCode::InvalidEvidence;
      break;
    case UpgradeSourceLayout::Unsupported:
      return UpgradeEvidenceComposeCode::UnsupportedSchema;
    case UpgradeSourceLayout::Unknown:
      return UpgradeEvidenceComposeCode::Ambiguous;
    default:
      return UpgradeEvidenceComposeCode::InvalidEvidence;
  }
  if (audit.result == UpgradeAuditResult::Fresh || allProtectedMissing(first))
    return UpgradeEvidenceComposeCode::InvalidEvidence;
  if (!sourceFingerprint(first, output.source_fingerprint))
    return UpgradeEvidenceComposeCode::InvalidEvidence;
  if (first.source_layout == UpgradeSourceLayout::Legacy) {
    output.legacy_source.durable = first.legacy_source_durable;
    if (first.legacy_source_durable)
      output.legacy_source.fingerprint = output.source_fingerprint;
  }
  return UpgradeEvidenceComposeCode::Ok;
}

const char* upgradeEvidenceComposeCodeName(UpgradeEvidenceComposeCode code) {
  switch (code) {
    case UpgradeEvidenceComposeCode::Ok: return "OK";
    case UpgradeEvidenceComposeCode::Changed: return "CHANGED";
    case UpgradeEvidenceComposeCode::Ambiguous: return "AMBIGUOUS";
    case UpgradeEvidenceComposeCode::IoError: return "IO_ERROR";
    case UpgradeEvidenceComposeCode::UnsupportedSchema:
      return "UNSUPPORTED_SCHEMA";
    case UpgradeEvidenceComposeCode::InvalidEvidence:
      return "INVALID_EVIDENCE";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
