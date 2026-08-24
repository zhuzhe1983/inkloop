#include "inkloop/native_settings_migration_gate.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "inkloop/board_prompt_policy.hpp"
#include "inkloop/settings/legacy_portal_import.hpp"
#include "inkloop/storage/upgrade_marker_journal.hpp"

namespace inkloop {
namespace {

bool fingerprintValid(const storage::MigrationFingerprint& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0U; });
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool decodeFingerprint(const std::string& encoded,
                       storage::MigrationFingerprint& output) {
  output.fill(0U);
  if (encoded.size() != output.size() * 2U) return false;
  for (std::size_t index = 0U; index < output.size(); ++index) {
    const int high = hexDigit(encoded[index * 2U]);
    const int low = hexDigit(encoded[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      output.fill(0U);
      return false;
    }
    output[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return fingerprintValid(output);
}

settings::DeviceSettings defaultsFor(const BoardDescriptor& board) {
  settings::DeviceSettings output = settings::makeGenericDeviceDefaults();
  output.assistant_prompt = defaultAssistantPrompt(board);
  output.aigc_prompt_template = defaultImagePromptTemplate(board);
  output.negative_prompt = defaultNegativePrompt(board);
  if (!settings::validDeviceSettings(output))
    output = settings::makeGenericDeviceDefaults();
  return output;
}

bool sameSettingsAtGeneration(const settings::SettingsSnapshot& target,
                              const settings::DeviceSettings& candidate,
                              std::uint32_t generation) {
  if (generation == 0U || target.generation != generation)
    return false;
  // Every migration target written by beta31 must be a beta27-readable schema
  // 2 main record. collect() has already overlaid the exact-generation
  // extension, so full semantic equality also proves LED/steps persistence.
  return target.decoded_record_schema == settings::kSettingsRecordSchema &&
      target.values == candidate;
}

bool supportedNativeAuthoritySchema(std::uint16_t schema) {
  return schema >= 1U &&
      schema <= settings::kMaximumReadableSettingsRecordSchema;
}

bool completedWireAuthority(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const storage::MigrationMarker& marker) {
  if (target.decoded_record_schema >= 3U)
    return true;
  return target.decoded_record_schema == settings::kSettingsRecordSchema &&
      target_extension.sequence != 0U &&
      target_extension.settings_generation == marker.generation;
}

bool sameMarker(const storage::MigrationMarker& left,
                const storage::MigrationMarker& right) {
  return left.schema_version == right.schema_version &&
      left.source_layout_schema_version ==
          right.source_layout_schema_version &&
      left.generation == right.generation &&
      left.source_fingerprint == right.source_fingerprint &&
      left.phase == right.phase && left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source &&
      left.checksum == right.checksum;
}

bool rawMatchesInspection(
    const storage::RawMigrationMarkerJournal& raw,
    const storage::MigrationMarkerJournalInspection& inspection) {
  if (!raw.namespace_available) return false;
  if (inspection.probe == storage::MigrationMarkerJournalProbe::Missing) {
    return !raw.initialized_present && !raw.head_present &&
        !raw.slots[0].present && !raw.slots[1].present;
  }
  if (inspection.probe != storage::MigrationMarkerJournalProbe::Valid ||
      !raw.initialized_present ||
      raw.initialized != storage::kMigrationJournalInitializedMarker ||
      !raw.head_present || raw.head_sequence != inspection.sequence ||
      inspection.sequence == 0U)
    return false;
  const std::size_t selected =
      static_cast<std::size_t>(inspection.sequence & 1U);
  std::uint64_t decoded_sequence = 0U;
  storage::MigrationMarker decoded;
  return storage::decodeMigrationJournalSlotV1(
             raw.slots[selected], decoded_sequence, decoded) ==
          storage::MigrationMarkerCodecCode::Ok &&
      decoded_sequence == inspection.sequence &&
      sameMarker(decoded, inspection.marker);
}

bool decodePreviousCommittedMarker(
    const storage::RawMigrationMarkerJournal& raw,
    const storage::MigrationMarkerJournalInspection& current,
    storage::MigrationMarker& previous) {
  previous = storage::MigrationMarker{};
  if (current.probe != storage::MigrationMarkerJournalProbe::Valid ||
      current.sequence <= 1U || !rawMatchesInspection(raw, current))
    return false;
  const std::size_t previous_slot =
      static_cast<std::size_t>((current.sequence & 1U) ^ 1U);
  std::uint64_t previous_sequence = 0U;
  return storage::decodeMigrationJournalSlotV1(
             raw.slots[previous_slot], previous_sequence, previous) ==
          storage::MigrationMarkerCodecCode::Ok &&
      previous_sequence == current.sequence - 1U;
}

storage::MigrationSlot slotFor(std::uint64_t generation) {
  return (generation & 1U) == 0U ? storage::MigrationSlot::SlotA
                                 : storage::MigrationSlot::SlotB;
}

storage::MigrationRollbackSource nativeRollbackFor(
    std::uint64_t generation) {
  return (generation & 1U) == 0U
      ? storage::MigrationRollbackSource::NativeSlotA
      : storage::MigrationRollbackSource::NativeSlotB;
}

bool generationFitsSettings(std::uint64_t generation) {
  return generation != 0U &&
      generation <= std::numeric_limits<std::uint32_t>::max();
}

bool markerSequenceHasRoom(std::uint64_t sequence,
                           std::uint8_t commits_remaining) {
  return commits_remaining <=
      std::numeric_limits<std::uint64_t>::max() - sequence;
}

std::uint8_t markerCommitsRemaining(storage::MigrationPhase phase) {
  switch (phase) {
    case storage::MigrationPhase::Prepared: return 4U;
    case storage::MigrationPhase::TargetWritten: return 3U;
    case storage::MigrationPhase::TargetVerified: return 2U;
    case storage::MigrationPhase::CommitRecorded: return 1U;
    case storage::MigrationPhase::Complete:
    case storage::MigrationPhase::RollbackRequired:
    case storage::MigrationPhase::None:
      return 0U;
  }
  return 0U;
}

std::uint16_t normalizedSourceSchema(
    const settings::LegacySettingsImport& legacy) {
  return legacy.source_schema == 0U ? 1U : legacy.source_schema;
}

bool freshMigrationIdentity(const storage::MigrationMarker& marker) {
  return marker.generation == 1U &&
      marker.rollback_source ==
          storage::MigrationRollbackSource::LegacySnapshot;
}

bool historicalMigrationIdentity(const storage::MigrationMarker& marker) {
  return marker.generation > 1U && generationFitsSettings(marker.generation) &&
      marker.rollback_source == nativeRollbackFor(marker.generation - 1U);
}

bool targetIsCompletedMigrationBase(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& complete) {
  return complete.phase == storage::MigrationPhase::Complete &&
      generationFitsSettings(complete.generation) &&
      complete.generation < std::numeric_limits<std::uint32_t>::max() &&
      complete.source_layout_schema_version >= 1U &&
      complete.source_layout_schema_version <= 2U &&
      fingerprintValid(complete.source_fingerprint) &&
      complete.target_slot == slotFor(complete.generation) &&
      (freshMigrationIdentity(complete) ||
       historicalMigrationIdentity(complete)) &&
      legacy.state == settings::LegacyImportState::Candidate &&
      !legacy.used_fallback_slot &&
      target.generation == complete.generation &&
      supportedNativeAuthoritySchema(target.decoded_record_schema) &&
      completedWireAuthority(target, target_extension, complete);
}

bool targetCanResumePostCompleteRollover(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& prepared,
    const storage::MigrationMarkerJournalInspection& journal,
    const storage::RawMigrationMarkerJournal& raw) {
  if (prepared.phase != storage::MigrationPhase::Prepared ||
      !generationFitsSettings(prepared.generation) ||
      prepared.generation <= 1U)
    return false;
  storage::MigrationMarker previous;
  return decodePreviousCommittedMarker(raw, journal, previous) &&
      generationFitsSettings(previous.generation) &&
      previous.generation < std::numeric_limits<std::uint32_t>::max() &&
      previous.generation + 1U == prepared.generation &&
      previous.source_fingerprint != prepared.source_fingerprint &&
      prepared.rollback_source == nativeRollbackFor(previous.generation) &&
      targetIsCompletedMigrationBase(
          target, target_extension, legacy, previous);
}

bool targetIsHistoricalBase(
    const settings::SettingsSnapshot& target,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& marker) {
  return historicalMigrationIdentity(marker) &&
      target.generation == marker.generation - 1U &&
      settings::matchesHistoricalIncompleteImport(target, legacy);
}

bool targetIsMigrationResult(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& marker) {
  return generationFitsSettings(marker.generation) &&
      sameSettingsAtGeneration(
          target, legacy.values,
          static_cast<std::uint32_t>(marker.generation)) &&
      completedWireAuthority(target, target_extension, marker);
}

bool targetIsCompatibleCompletedMigrationResult(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& marker) {
  return generationFitsSettings(marker.generation) &&
      target.generation == static_cast<std::uint32_t>(marker.generation) &&
      supportedNativeAuthoritySchema(target.decoded_record_schema) &&
      target.values == legacy.values &&
      completedWireAuthority(target, target_extension, marker);
}

settings::DeviceSettings mainSettingsProjection(
    const settings::DeviceSettings& values) {
  settings::DeviceSettings output = values;
  output.led_roles_swapped = false;
  output.aigc_steps = settings::kDefaultAigcSteps;
  return output;
}

bool targetHasMigrationMainProjection(
    const settings::SettingsSnapshot& target,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& marker) {
  return generationFitsSettings(marker.generation) &&
      target.generation == static_cast<std::uint32_t>(marker.generation) &&
      target.decoded_record_schema == settings::kSettingsRecordSchema &&
      mainSettingsProjection(target.values) ==
          mainSettingsProjection(legacy.values);
}

bool targetCanResumePrepared(
    const settings::SettingsSnapshot& target,
    const settings::SettingsExtensionSnapshot& target_extension,
    const settings::LegacySettingsImport& legacy,
    const storage::MigrationMarker& marker) {
  if (freshMigrationIdentity(marker)) {
    return target.generation == 0U ||
        targetIsMigrationResult(
            target, target_extension, legacy, marker);
  }
  return targetIsHistoricalBase(target, legacy, marker) ||
      targetIsMigrationResult(
          target, target_extension, legacy, marker);
}

storage::MigrationMarker markerFor(
    const storage::MigrationFingerprint& fingerprint,
    std::uint16_t source_schema, std::uint64_t generation,
    storage::MigrationPhase phase,
    storage::MigrationRollbackSource rollback_source) {
  storage::MigrationMarker output;
  output.source_layout_schema_version = source_schema == 0U ? 1U : source_schema;
  output.generation = generation;
  output.source_fingerprint = fingerprint;
  output.phase = phase;
  output.target_slot = slotFor(generation);
  output.rollback_source = rollback_source;
  output.checksum = storage::migrationMarkerChecksum(output);
  return output;
}

bool samePlan(const NativeSettingsMigrationPlan& left,
              const NativeSettingsMigrationPlan& right) {
  return left.kind == right.kind &&
      left.source_fingerprint == right.source_fingerprint &&
      left.source_schema == right.source_schema &&
      left.observed_generation == right.observed_generation &&
      left.target_generation == right.target_generation &&
      left.marker_sequence == right.marker_sequence &&
      left.marker_phase == right.marker_phase &&
      left.rollback_source == right.rollback_source;
}

NativeSettingsMigrationGateCode markerFailure(
    storage::MigrationMarkerJournalCode code) {
  switch (code) {
    case storage::MigrationMarkerJournalCode::Torn:
    case storage::MigrationMarkerJournalCode::Corrupt:
      return NativeSettingsMigrationGateCode::MarkerCorrupt;
    case storage::MigrationMarkerJournalCode::Ok:
      return NativeSettingsMigrationGateCode::Ok;
    case storage::MigrationMarkerJournalCode::InvalidArgument:
    case storage::MigrationMarkerJournalCode::Conflict:
    case storage::MigrationMarkerJournalCode::Exhausted:
    case storage::MigrationMarkerJournalCode::IoError:
    case storage::MigrationMarkerJournalCode::ReadBackFailed:
      return NativeSettingsMigrationGateCode::MarkerWriteFailed;
  }
  return NativeSettingsMigrationGateCode::MarkerWriteFailed;
}

}  // namespace

struct NativeSettingsMigrationGate::Evidence {
  settings::SettingsSnapshot target;
  settings::SettingsExtensionSnapshot target_extension;
  settings::LegacySettingsImport legacy;
  storage::MigrationMarkerJournalCode marker_code =
      storage::MigrationMarkerJournalCode::IoError;
  storage::MigrationMarkerJournalInspection marker;
  storage::RawMigrationMarkerJournal raw_marker{};
};

bool NativeSettingsMigrationAuthorization::valid() const {
  if (kind == NativeSettingsAuthorityKind::FreshDefaults)
    return observed_generation == 0U && migration_generation == 0U &&
        !fingerprintValid(source_fingerprint);
  if (kind != NativeSettingsAuthorityKind::NativeJournal ||
      observed_generation == 0U || migration_generation > observed_generation)
    return false;
  return migration_generation == 0U || fingerprintValid(source_fingerprint);
}

NativeSettingsMigrationGate::NativeSettingsMigrationGate(
    const BoardDescriptor& board,
    const storage::EspNvsBootMountOwner& nvs_boot_mount)
    : nvs_boot_mount_(nvs_boot_mount), defaults_(defaultsFor(board)),
      settings_store_(settings_journal_, defaults_),
      settings_extension_store_(settings_extension_journal_) {}

NativeSettingsMigrationGateCode NativeSettingsMigrationGate::collect(
    Evidence& output) {
  output = Evidence{};
  // A physically verified blank NVS partition cannot be initialized by IDF
  // without activating (writing) its first page.  During RO audit all NVS
  // namespaces and both journals are therefore known Missing without API
  // opens; promotion will initialize them later.
  if (nvs_boot_mount_.freshBlank()) {
    output.target.values = defaults_;
    output.legacy.values = defaults_;
    output.marker_code = storage::MigrationMarkerJournalCode::Ok;
    output.marker.probe = storage::MigrationMarkerJournalProbe::Missing;
    output.raw_marker.namespace_available = true;
    return NativeSettingsMigrationGateCode::Ok;
  }

  if (!settings::loadRollbackCompatibleSettings(
           settings_store_, settings_extension_store_, output.target,
           &output.target_extension).ok())
    return NativeSettingsMigrationGateCode::TargetCorrupt;
  const settings::SettingsStatus legacy_status =
      settings::inspectLegacyPortalSettings(
          legacy_, legacy_sha_, defaults_, output.legacy);
  if (!legacy_status.ok())
    return NativeSettingsMigrationGateCode::SourceCorrupt;

  storage::MigrationMarkerJournalCore marker_core(marker_store_);
  output.marker_code = marker_core.inspect(output.marker);
  if (marker_store_.inspectRaw(output.raw_marker) !=
      storage::MigrationJournalStoreCode::Ok)
    return NativeSettingsMigrationGateCode::MarkerCorrupt;
  if (output.marker_code == storage::MigrationMarkerJournalCode::Ok &&
      !rawMatchesInspection(output.raw_marker, output.marker))
    return NativeSettingsMigrationGateCode::MarkerCorrupt;
  return NativeSettingsMigrationGateCode::Ok;
}

NativeSettingsMigrationGateCode NativeSettingsMigrationGate::compose(
    const Evidence& evidence, NativeSettingsMigrationPlan& output) const {
  output = NativeSettingsMigrationPlan{};
  storage::MigrationFingerprint fingerprint{};
  const bool candidate = evidence.legacy.state ==
      settings::LegacyImportState::Candidate;
  if (candidate &&
      !decodeFingerprint(evidence.legacy.source_fingerprint, fingerprint))
    return NativeSettingsMigrationGateCode::SourceCorrupt;

  if (evidence.marker_code == storage::MigrationMarkerJournalCode::Torn) {
    if (!candidate) return NativeSettingsMigrationGateCode::MarkerCorrupt;
    const storage::RawMigrationMarkerJournal& raw = evidence.raw_marker;
    if (!raw.namespace_available || raw.slots[0].present ||
        !raw.slots[1].present ||
        (raw.initialized_present &&
         raw.initialized != storage::kMigrationJournalInitializedMarker) ||
        (raw.head_present && raw.head_sequence != 1U))
      return NativeSettingsMigrationGateCode::MarkerCorrupt;
    std::uint64_t sequence = 0U;
    storage::MigrationMarker marker;
    if (storage::decodeMigrationJournalSlotV1(
            raw.slots[1], sequence, marker) !=
            storage::MigrationMarkerCodecCode::Ok ||
        sequence != 1U || marker.phase != storage::MigrationPhase::Prepared ||
        !generationFitsSettings(marker.generation) ||
        marker.source_fingerprint != fingerprint ||
        marker.source_layout_schema_version !=
            normalizedSourceSchema(evidence.legacy) ||
        marker.target_slot != slotFor(marker.generation) ||
        (!freshMigrationIdentity(marker) &&
         !historicalMigrationIdentity(marker)) ||
        !targetCanResumePrepared(
            evidence.target, evidence.target_extension,
            evidence.legacy, marker))
      return NativeSettingsMigrationGateCode::MarkerMismatch;
    output.kind = NativeSettingsMigrationPlanKind::RecoverPreparedHead;
    output.source_fingerprint = fingerprint;
    output.source_schema = marker.source_layout_schema_version;
    output.observed_generation = evidence.target.generation;
    output.target_generation = marker.generation;
    output.marker_sequence = 0U;
    output.marker_phase = storage::MigrationPhase::Prepared;
    output.rollback_source = marker.rollback_source;
    return NativeSettingsMigrationGateCode::Ok;
  }
  if (evidence.marker_code != storage::MigrationMarkerJournalCode::Ok)
    return NativeSettingsMigrationGateCode::MarkerCorrupt;

  if (evidence.marker.probe ==
      storage::MigrationMarkerJournalProbe::Missing) {
    output.observed_generation = evidence.target.generation;
    if (!candidate) {
      output.kind = evidence.target.generation == 0U
          ? NativeSettingsMigrationPlanKind::FreshNoMigration
          : NativeSettingsMigrationPlanKind::NativeNoMigration;
      return NativeSettingsMigrationGateCode::Ok;
    }
    output.kind = NativeSettingsMigrationPlanKind::Start;
    output.source_fingerprint = fingerprint;
    output.source_schema = normalizedSourceSchema(evidence.legacy);
    if (evidence.target.generation == 0U) {
      output.target_generation = 1U;
      output.rollback_source =
          storage::MigrationRollbackSource::LegacySnapshot;
      return NativeSettingsMigrationGateCode::Ok;
    }
    if (!settings::matchesHistoricalIncompleteImport(
            evidence.target, evidence.legacy)) {
      // Any native user edit, schema-3 native save, or merely similar record
      // remains authoritative.  Only the exact beta27/beta29 projection may
      // receive the fields that the historical auto-importer omitted.
      output.kind = NativeSettingsMigrationPlanKind::NativeNoMigration;
      output.source_fingerprint = {};
      output.source_schema = 0U;
      return NativeSettingsMigrationGateCode::Ok;
    }
    // The matcher itself requires the old auto-importer's unique generation
    // one, schema 1/2, exact old projection and verified live legacy identity.
    // Later native generations return NativeNoMigration above and are never
    // overwritten, even if their values happen to collide.
    output.target_generation =
        static_cast<std::uint64_t>(evidence.target.generation) + 1U;
    output.rollback_source = nativeRollbackFor(evidence.target.generation);
    return NativeSettingsMigrationGateCode::Ok;
  }
  if (evidence.marker.probe != storage::MigrationMarkerJournalProbe::Valid)
    return NativeSettingsMigrationGateCode::MarkerCorrupt;

  const storage::MigrationMarker& marker = evidence.marker.marker;
  if (!generationFitsSettings(marker.generation) ||
      marker.source_layout_schema_version < 1U ||
      marker.source_layout_schema_version > 2U ||
      marker.target_slot != slotFor(marker.generation) ||
      (!freshMigrationIdentity(marker) &&
       !historicalMigrationIdentity(marker)))
    return NativeSettingsMigrationGateCode::MarkerMismatch;

  if (!candidate) {
    // A rollback-era factory reset can legitimately remove ink-portal after a
    // completed migration. The current native journal remains authoritative;
    // absence is distinct from corrupt legacy evidence, which collect()
    // rejects before composition. An in-flight migration still requires its
    // source and therefore cannot take this no-op path.
    if (marker.phase != storage::MigrationPhase::Complete)
      return NativeSettingsMigrationGateCode::MarkerCorrupt;
    if (evidence.target.generation < marker.generation ||
        !supportedNativeAuthoritySchema(
            evidence.target.decoded_record_schema))
      return NativeSettingsMigrationGateCode::TargetCorrupt;
    output.kind = NativeSettingsMigrationPlanKind::NativeNoMigration;
    output.observed_generation = evidence.target.generation;
    return NativeSettingsMigrationGateCode::Ok;
  }

  if (marker.source_fingerprint != fingerprint) {
    if (marker.phase != storage::MigrationPhase::Complete)
      return NativeSettingsMigrationGateCode::MarkerMismatch;
    if (evidence.target.generation < marker.generation)
      return NativeSettingsMigrationGateCode::TargetCorrupt;
    if (evidence.target.generation > marker.generation) {
      // A schema-3 native save after completion is newer authority. Preserve
      // it and ignore rollback-era legacy edits; never rewrite native gen2+.
      if (!supportedNativeAuthoritySchema(
              evidence.target.decoded_record_schema))
        return NativeSettingsMigrationGateCode::TargetCorrupt;
      output.kind = NativeSettingsMigrationPlanKind::NativeNoMigration;
      output.observed_generation = evidence.target.generation;
      return NativeSettingsMigrationGateCode::Ok;
    }
    if (marker.generation ==
        std::numeric_limits<std::uint32_t>::max()) {
      // Native is a valid completed authority but there is no representable
      // next settings generation. Preserve it without entering Recovery or
      // attempting a partial migration.
      if (!supportedNativeAuthoritySchema(
              evidence.target.decoded_record_schema))
        return NativeSettingsMigrationGateCode::TargetCorrupt;
      output.kind = NativeSettingsMigrationPlanKind::NativeNoMigration;
      output.observed_generation = evidence.target.generation;
      return NativeSettingsMigrationGateCode::Ok;
    }
    if (!targetIsCompletedMigrationBase(
            evidence.target, evidence.target_extension,
            evidence.legacy, marker))
      return NativeSettingsMigrationGateCode::TargetCorrupt;
    if (!markerSequenceHasRoom(evidence.marker.sequence, 5U))
      return NativeSettingsMigrationGateCode::MarkerWriteFailed;

    // A rollback-era Arduino commit may roll a completed migration forward
    // when the valid preferred legacy source changed, the prior Complete
    // marker is coherent, and native has not advanced beyond that completed
    // generation. Native generation is the user-edit guard: a newer native
    // save wins above. Missing-marker historical inference remains gen1-only.
    output.kind = NativeSettingsMigrationPlanKind::Start;
    output.source_fingerprint = fingerprint;
    output.source_schema = normalizedSourceSchema(evidence.legacy);
    output.observed_generation = evidence.target.generation;
    output.target_generation = marker.generation + 1U;
    output.marker_sequence = evidence.marker.sequence;
    output.marker_phase = marker.phase;
    output.rollback_source = nativeRollbackFor(evidence.target.generation);
    return NativeSettingsMigrationGateCode::Ok;
  }
  if (marker.source_layout_schema_version !=
      normalizedSourceSchema(evidence.legacy))
    return NativeSettingsMigrationGateCode::MarkerMismatch;
  output.source_fingerprint = fingerprint;
  output.source_schema = marker.source_layout_schema_version;
  output.observed_generation = evidence.target.generation;
  output.target_generation = marker.generation;
  output.marker_sequence = evidence.marker.sequence;
  output.marker_phase = marker.phase;
  output.rollback_source = marker.rollback_source;

  if (marker.phase == storage::MigrationPhase::Complete) {
    if (evidence.target.generation < marker.generation)
      return NativeSettingsMigrationGateCode::TargetCorrupt;
    if (evidence.target.generation == marker.generation) {
      if (!targetIsCompatibleCompletedMigrationResult(
              evidence.target, evidence.target_extension,
              evidence.legacy, marker))
        return NativeSettingsMigrationGateCode::TargetCorrupt;
    } else if (!supportedNativeAuthoritySchema(
                   evidence.target.decoded_record_schema)) {
      return NativeSettingsMigrationGateCode::TargetCorrupt;
    }
    output.kind = NativeSettingsMigrationPlanKind::Complete;
    return NativeSettingsMigrationGateCode::Ok;
  }
  if (marker.phase == storage::MigrationPhase::RollbackRequired ||
      marker.phase == storage::MigrationPhase::None)
    return NativeSettingsMigrationGateCode::TargetCorrupt;
  if (!markerSequenceHasRoom(
          evidence.marker.sequence, markerCommitsRemaining(marker.phase)))
    return NativeSettingsMigrationGateCode::MarkerWriteFailed;
  if (marker.phase == storage::MigrationPhase::Prepared) {
    const bool target_already_written = targetIsMigrationResult(
        evidence.target, evidence.target_extension,
        evidence.legacy, marker);
    const bool target_main_projection_written =
        targetHasMigrationMainProjection(
            evidence.target, evidence.legacy, marker);
    const bool bootstrap_base = evidence.marker.sequence == 1U &&
        targetCanResumePrepared(
            evidence.target, evidence.target_extension,
            evidence.legacy, marker);
    const bool rollover_base = evidence.marker.sequence > 1U &&
        targetCanResumePostCompleteRollover(
            evidence.target, evidence.target_extension,
            evidence.legacy, marker, evidence.marker,
            evidence.raw_marker);
    if (!target_already_written && !target_main_projection_written &&
        !bootstrap_base && !rollover_base)
      return NativeSettingsMigrationGateCode::TargetCorrupt;
  } else if (!targetIsMigrationResult(
                 evidence.target, evidence.target_extension,
                 evidence.legacy, marker)) {
    return NativeSettingsMigrationGateCode::TargetCorrupt;
  }
  output.kind = NativeSettingsMigrationPlanKind::Resume;
  return NativeSettingsMigrationGateCode::Ok;
}

NativeSettingsMigrationGateCode NativeSettingsMigrationGate::auditReadOnly(
    NativeSettingsMigrationPlan& output) {
  output = NativeSettingsMigrationPlan{};
  if (nvs_boot_mount_.access() != storage::NvsBootMountAccess::ReadOnlyAudit)
    return NativeSettingsMigrationGateCode::InvalidState;
  Evidence evidence;
  const NativeSettingsMigrationGateCode collected = collect(evidence);
  return collected == NativeSettingsMigrationGateCode::Ok
      ? compose(evidence, output) : collected;
}

NativeSettingsMigrationGateCode
NativeSettingsMigrationGate::recoverPreparedHead(
    const NativeSettingsMigrationPlan& plan) {
  Evidence evidence;
  NativeSettingsMigrationPlan fresh;
  NativeSettingsMigrationGateCode code = collect(evidence);
  if (code != NativeSettingsMigrationGateCode::Ok) return code;
  code = compose(evidence, fresh);
  if (code != NativeSettingsMigrationGateCode::Ok || !samePlan(plan, fresh))
    return NativeSettingsMigrationGateCode::SourceChanged;
  if (marker_store_.writeHeadAndMarkerAndCommit(1U) !=
      storage::MigrationJournalStoreCode::Ok)
    return NativeSettingsMigrationGateCode::MarkerWriteFailed;
  storage::MigrationMarkerJournalCore core(marker_store_);
  storage::MigrationMarkerJournalInspection repaired;
  if (core.inspect(repaired) != storage::MigrationMarkerJournalCode::Ok ||
      repaired.probe != storage::MigrationMarkerJournalProbe::Valid ||
      repaired.sequence != 1U ||
      repaired.marker.phase != storage::MigrationPhase::Prepared ||
      repaired.marker.source_fingerprint != plan.source_fingerprint)
    return NativeSettingsMigrationGateCode::MarkerWriteFailed;
  return NativeSettingsMigrationGateCode::Ok;
}

NativeSettingsMigrationGateCode NativeSettingsMigrationGate::advance(
    NativeSettingsMigrationAuthorization& authorization) {
  authorization = NativeSettingsMigrationAuthorization{};
  for (std::uint8_t step = 0U; step < 5U; ++step) {
    Evidence evidence;
    NativeSettingsMigrationPlan plan;
    NativeSettingsMigrationGateCode code = collect(evidence);
    if (code != NativeSettingsMigrationGateCode::Ok) return code;
    code = compose(evidence, plan);
    if (code != NativeSettingsMigrationGateCode::Ok) return code;
    if (plan.kind == NativeSettingsMigrationPlanKind::Complete) {
      authorization.kind = NativeSettingsAuthorityKind::NativeJournal;
      authorization.observed_generation = evidence.target.generation;
      authorization.migration_generation = plan.target_generation;
      authorization.source_fingerprint = plan.source_fingerprint;
      return authorization.valid() ? NativeSettingsMigrationGateCode::Ok
                                   : NativeSettingsMigrationGateCode::InvalidState;
    }
    if (plan.kind != NativeSettingsMigrationPlanKind::Resume ||
        evidence.marker.probe !=
            storage::MigrationMarkerJournalProbe::Valid)
      return NativeSettingsMigrationGateCode::MarkerMismatch;

    const storage::MigrationMarker current = evidence.marker.marker;
    storage::MigrationPhase next = storage::MigrationPhase::None;
    switch (current.phase) {
      case storage::MigrationPhase::Prepared: {
        if (!targetIsMigrationResult(
                evidence.target, evidence.target_extension,
                evidence.legacy, current)) {
          std::uint32_t expected_generation = 0U;
          const bool main_projection_written =
              targetHasMigrationMainProjection(
                  evidence.target, evidence.legacy, current);
          if (main_projection_written) {
            // Power failed after schema-2 main publication but before the
            // single ext-head selector write. Retry only the extension at the
            // already-authoritative main generation.
            expected_generation = evidence.target.generation;
          } else if (evidence.marker.sequence == 1U &&
              freshMigrationIdentity(current) &&
              evidence.target.generation == 0U) {
            expected_generation = 0U;
          } else if (evidence.marker.sequence == 1U &&
                     targetIsHistoricalBase(
                         evidence.target, evidence.legacy, current)) {
            expected_generation = evidence.target.generation;
          } else if (evidence.marker.sequence > 1U &&
                     targetCanResumePostCompleteRollover(
                         evidence.target, evidence.target_extension,
                         evidence.legacy, current,
                         evidence.marker, evidence.raw_marker)) {
            expected_generation = evidence.target.generation;
          } else {
            return NativeSettingsMigrationGateCode::TargetCorrupt;
          }
          settings::SettingsSnapshot committed;
          if (!settings::saveRollbackCompatibleSettings(
                  settings_store_, settings_extension_store_,
                  evidence.legacy.values, expected_generation, committed,
                  !main_projection_written, true).ok())
            return NativeSettingsMigrationGateCode::TargetWriteFailed;
          settings::SettingsExtensionSnapshot committed_extension;
          if (!settings::loadRollbackCompatibleSettings(
                  settings_store_, settings_extension_store_, committed,
                  &committed_extension).ok() ||
              committed.generation != current.generation ||
              !targetIsMigrationResult(
                  committed, committed_extension,
                  evidence.legacy, current))
            return NativeSettingsMigrationGateCode::TargetWriteFailed;
        }
        next = storage::MigrationPhase::TargetWritten;
        break;
      }
      case storage::MigrationPhase::TargetWritten:
        if (!targetIsMigrationResult(
                evidence.target, evidence.target_extension,
                evidence.legacy, current))
          return NativeSettingsMigrationGateCode::TargetCorrupt;
        next = storage::MigrationPhase::TargetVerified;
        break;
      case storage::MigrationPhase::TargetVerified:
        if (!targetIsMigrationResult(
                evidence.target, evidence.target_extension,
                evidence.legacy, current))
          return NativeSettingsMigrationGateCode::TargetCorrupt;
        next = storage::MigrationPhase::CommitRecorded;
        break;
      case storage::MigrationPhase::CommitRecorded:
        if (!targetIsMigrationResult(
                evidence.target, evidence.target_extension,
                evidence.legacy, current))
          return NativeSettingsMigrationGateCode::TargetCorrupt;
        next = storage::MigrationPhase::Complete;
        break;
      case storage::MigrationPhase::None:
      case storage::MigrationPhase::RollbackRequired:
      case storage::MigrationPhase::Complete:
        return NativeSettingsMigrationGateCode::MarkerMismatch;
    }
    storage::MigrationMarker next_marker = current;
    next_marker.phase = next;
    next_marker.checksum = storage::migrationMarkerChecksum(next_marker);
    storage::MigrationMarkerJournalInspection committed;
    const storage::MigrationMarkerJournalCode marker_code =
        storage::MigrationMarkerJournalCore(marker_store_).commit(
            next_marker, evidence.marker.sequence, committed);
    code = markerFailure(marker_code);
    if (code != NativeSettingsMigrationGateCode::Ok) return code;
  }
  return NativeSettingsMigrationGateCode::InvalidState;
}

NativeSettingsMigrationGateCode NativeSettingsMigrationGate::execute(
    const NativeSettingsMigrationPlan& authorized_plan,
    NativeSettingsMigrationAuthorization& authorization) {
  authorization = NativeSettingsMigrationAuthorization{};
  if (nvs_boot_mount_.access() !=
          storage::NvsBootMountAccess::ReadWriteProduct ||
      authorized_plan.kind == NativeSettingsMigrationPlanKind::None)
    return NativeSettingsMigrationGateCode::InvalidState;

  Evidence evidence;
  NativeSettingsMigrationPlan fresh;
  NativeSettingsMigrationGateCode code = collect(evidence);
  if (code != NativeSettingsMigrationGateCode::Ok) return code;
  code = compose(evidence, fresh);
  if (code != NativeSettingsMigrationGateCode::Ok ||
      !samePlan(authorized_plan, fresh))
    return NativeSettingsMigrationGateCode::SourceChanged;

  if (fresh.kind == NativeSettingsMigrationPlanKind::FreshNoMigration) {
    authorization.kind = NativeSettingsAuthorityKind::FreshDefaults;
    return NativeSettingsMigrationGateCode::Ok;
  }
  if (fresh.kind == NativeSettingsMigrationPlanKind::NativeNoMigration ||
      fresh.kind == NativeSettingsMigrationPlanKind::Complete) {
    authorization.kind = NativeSettingsAuthorityKind::NativeJournal;
    authorization.observed_generation = evidence.target.generation;
    if (fresh.kind == NativeSettingsMigrationPlanKind::Complete) {
      authorization.migration_generation = fresh.target_generation;
      authorization.source_fingerprint = fresh.source_fingerprint;
    }
    return authorization.valid() ? NativeSettingsMigrationGateCode::Ok
                                 : NativeSettingsMigrationGateCode::InvalidState;
  }
  if (fresh.kind == NativeSettingsMigrationPlanKind::RecoverPreparedHead) {
    code = recoverPreparedHead(fresh);
    if (code != NativeSettingsMigrationGateCode::Ok) return code;
    return advance(authorization);
  }
  if (fresh.kind == NativeSettingsMigrationPlanKind::Start) {
    storage::MigrationMarker prepared = markerFor(
        fresh.source_fingerprint, fresh.source_schema,
        fresh.target_generation, storage::MigrationPhase::Prepared,
        fresh.rollback_source);
    storage::MigrationMarkerJournalInspection committed;
    code = markerFailure(storage::MigrationMarkerJournalCore(marker_store_)
                             .commit(prepared, fresh.marker_sequence,
                                     committed));
    if (code != NativeSettingsMigrationGateCode::Ok) return code;
    return advance(authorization);
  }
  if (fresh.kind == NativeSettingsMigrationPlanKind::Resume)
    return advance(authorization);
  return NativeSettingsMigrationGateCode::InvalidState;
}

const char* nativeSettingsMigrationGateCodeName(
    NativeSettingsMigrationGateCode code) {
  switch (code) {
    case NativeSettingsMigrationGateCode::Ok: return "OK";
    case NativeSettingsMigrationGateCode::InvalidState: return "INVALID_STATE";
    case NativeSettingsMigrationGateCode::SourceCorrupt: return "SOURCE_CORRUPT";
    case NativeSettingsMigrationGateCode::SourceChanged: return "SOURCE_CHANGED";
    case NativeSettingsMigrationGateCode::TargetCorrupt: return "TARGET_CORRUPT";
    case NativeSettingsMigrationGateCode::MarkerCorrupt: return "MARKER_CORRUPT";
    case NativeSettingsMigrationGateCode::MarkerMismatch: return "MARKER_MISMATCH";
    case NativeSettingsMigrationGateCode::MarkerWriteFailed:
      return "MARKER_WRITE_FAILED";
    case NativeSettingsMigrationGateCode::TargetWriteFailed:
      return "TARGET_WRITE_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
