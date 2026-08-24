import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf/components");

const board = String.raw`#pragma once
namespace inkloop {
struct BoardDescriptor { const char* id = "test-board"; };
}
`;

const prompts = String.raw`#pragma once
#include <string>
#include "inkloop/board.hpp"
namespace inkloop {
inline std::string defaultAssistantPrompt(const BoardDescriptor&) {
  return "default assistant";
}
inline std::string defaultImagePromptTemplate(const BoardDescriptor&) {
  return "default image {prompt}";
}
inline std::string defaultNegativePrompt(const BoardDescriptor&) {
  return "default negative";
}
}
`;

const nvsBoot = String.raw`#pragma once
#include <cstdint>
namespace inkloop::storage {
enum class NvsBootMountAccess : std::uint8_t {
  Unmounted, ReadOnlyAudit, ReadWriteProduct, RecoveryRequired,
};
class EspNvsBootMountOwner final {
 public:
  NvsBootMountAccess access() const { return access_; }
  bool freshBlank() const { return fresh_blank_; }
  void setAccess(NvsBootMountAccess value) { access_ = value; }
  void setFreshBlank(bool value) { fresh_blank_ = value; }
 private:
  NvsBootMountAccess access_ = NvsBootMountAccess::ReadOnlyAudit;
  bool fresh_blank_ = false;
};
}
`;

const settingsAdapters = String.raw`#pragma once
#include <cstdint>
#include <string>
#include "inkloop/settings/legacy_portal_import.hpp"
#include "inkloop/settings/settings_journal.hpp"
namespace inkloop::settings {
inline SettingsJournalState test_settings_state{};
inline LegacyPortalJournalState test_legacy_state{};
inline unsigned test_settings_slot_writes = 0;
inline unsigned test_settings_head_writes = 0;
inline bool test_fail_settings_head_once = false;

inline std::string testDigest(const std::string& payload) {
  std::uint32_t value = 2166136261U;
  for (unsigned char ch : payload) value = (value ^ ch) * 16777619U;
  static const char hex[] = "0123456789abcdef";
  std::string output(64, '0');
  for (std::size_t at = 0; at < output.size(); ++at) {
    output[at] = hex[(value >> ((at & 7U) * 4U)) & 15U];
    value = value * 1103515245U + 12345U;
  }
  return output;
}

class EspNvsSettingsJournalStore final : public ISettingsJournalStore {
 public:
  SettingsStatus inspect(SettingsJournalState& output) override {
    output = test_settings_state;
    return SettingsStatus::success();
  }
  SettingsStatus writeSlotAndCommit(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) override {
    ++test_settings_slot_writes;
    if (slot > 1U) return {SettingsError::InvalidArgument, "slot"};
    test_settings_state.namespace_available = true;
    test_settings_state.slot_present[slot] = true;
    test_settings_state.slot[slot] = encoded;
    return SettingsStatus::success();
  }
  SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) override {
    ++test_settings_head_writes;
    if (test_fail_settings_head_once) {
      test_fail_settings_head_once = false;
      return {SettingsError::Storage, "injected head failure"};
    }
    test_settings_state.namespace_available = true;
    test_settings_state.head_present = true;
    test_settings_state.head_generation = generation;
    test_settings_state.marker_present = true;
    test_settings_state.marker_valid = true;
    return SettingsStatus::success();
  }
};

class EspNvsReadOnlyLegacyPortalSource final
    : public IReadOnlyLegacyPortalSource {
 public:
  SettingsStatus inspect(LegacyPortalJournalState& output) const override {
    output = test_legacy_state;
    return SettingsStatus::success();
  }
};

class EspPsaLegacySha256Verifier final : public ILegacySha256Verifier {
 public:
  bool matches(const std::string& payload,
               const std::string& expected) const override {
    return testDigest(payload) == expected;
  }
  bool digest(const std::string& payload,
              std::string& output) const override {
    output = testDigest(payload);
    return true;
  }
};
}
`;

const markerAdapter = String.raw`#pragma once
#include <cstdint>
#include "inkloop/storage/upgrade_marker_journal.hpp"
namespace inkloop::storage {
inline RawMigrationMarkerJournal test_marker_state{true};
inline unsigned test_marker_slot_writes = 0;
inline unsigned test_marker_head_writes = 0;
inline unsigned test_fail_marker_slot_call = 0;
inline unsigned test_fail_marker_slot_after_commit_call = 0;
inline bool test_fail_marker_head_once = false;
inline bool test_fail_marker_head_after_commit_once = false;

class EspNvsMigrationMarkerJournalStore final
    : public IMigrationMarkerJournalStore {
 public:
  MigrationJournalStoreCode inspectRaw(
      RawMigrationMarkerJournal& output) const override {
    output = test_marker_state;
    return MigrationJournalStoreCode::Ok;
  }
  MigrationJournalStoreCode writeSlotAndCommit(
      std::uint8_t slot,
      const EncodedMigrationJournalSlot& encoded) override {
    ++test_marker_slot_writes;
    if (slot > 1U ||
        (test_fail_marker_slot_call != 0U &&
         test_marker_slot_writes == test_fail_marker_slot_call)) {
      return MigrationJournalStoreCode::IoError;
    }
    test_marker_state.namespace_available = true;
    test_marker_state.slots[slot].present = true;
    test_marker_state.slots[slot].length = encoded.size();
    test_marker_state.slots[slot].bytes = encoded;
    if (test_fail_marker_slot_after_commit_call != 0U &&
        test_marker_slot_writes == test_fail_marker_slot_after_commit_call)
      return MigrationJournalStoreCode::IoError;
    return MigrationJournalStoreCode::Ok;
  }
  MigrationJournalStoreCode writeHeadAndMarkerAndCommit(
      std::uint64_t sequence) override {
    ++test_marker_head_writes;
    if (test_fail_marker_head_once) {
      test_fail_marker_head_once = false;
      return MigrationJournalStoreCode::IoError;
    }
    test_marker_state.namespace_available = true;
    test_marker_state.head_present = true;
    test_marker_state.head_sequence = sequence;
    test_marker_state.initialized_present = true;
    test_marker_state.initialized = kMigrationJournalInitializedMarker;
    if (test_fail_marker_head_after_commit_once) {
      test_fail_marker_head_after_commit_once = false;
      return MigrationJournalStoreCode::IoError;
    }
    return MigrationJournalStoreCode::Ok;
  }
};
}
`;

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "inkloop/native_settings_migration_gate.hpp"
#include "inkloop/settings/legacy_portal_import.hpp"
#include "inkloop/storage/upgrade_marker_journal.hpp"

using namespace inkloop;

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t value = 0xFFFFFFFFU;
  for (std::size_t at = 0U; at < length; ++at) {
    value ^= bytes[at];
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(value & 1U);
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return value ^ 0xFFFFFFFFU;
}

void put32(std::uint32_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

void put64(std::uint64_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

storage::MigrationMarker currentMarker() {
  const auto& raw = storage::test_marker_state;
  assert(raw.head_present && raw.head_sequence != 0U);
  const std::size_t selected =
      static_cast<std::size_t>(raw.head_sequence & 1U);
  std::uint64_t sequence = 0U;
  storage::MigrationMarker marker;
  assert(storage::decodeMigrationJournalSlotV1(
             raw.slots[selected], sequence, marker) ==
         storage::MigrationMarkerCodecCode::Ok);
  assert(sequence == raw.head_sequence);
  return marker;
}

void installCurrentMarker(const storage::MigrationMarker& marker,
                          std::uint64_t sequence) {
  assert(sequence != 0U && storage::migrationMarkerValid(marker));
  auto& raw = storage::test_marker_state;
  raw.namespace_available = true;
  raw.initialized_present = true;
  raw.initialized = storage::kMigrationJournalInitializedMarker;
  raw.head_present = true;
  raw.head_sequence = sequence;
  const std::size_t selected = static_cast<std::size_t>(sequence & 1U);
  auto& slot = raw.slots[selected];
  slot.present = true;
  slot.length = storage::kMigrationMarkerJournalSlotBytes;
  slot.bytes.fill(0U);
  slot.bytes[0] = 'I';
  slot.bytes[1] = 'M';
  slot.bytes[2] = 'J';
  slot.bytes[3] = '1';
  put64(sequence, slot.bytes.data() + 4U);
  storage::EncodedMigrationMarker encoded_marker{};
  assert(storage::encodeMigrationMarkerV1(marker, encoded_marker) ==
         storage::MigrationMarkerCodecCode::Ok);
  std::copy(encoded_marker.begin(), encoded_marker.end(),
            slot.bytes.begin() + 12U);
  put32(crc32(slot.bytes.data(), 68U), slot.bytes.data() + 68U);
}

void corruptPreviousCompleteTargetParity() {
  auto& raw = storage::test_marker_state;
  assert(raw.head_present && raw.head_sequence > 1U);
  const std::size_t previous_slot =
      static_cast<std::size_t>((raw.head_sequence & 1U) ^ 1U);
  auto& encoded_slot = raw.slots[previous_slot];
  std::uint64_t sequence = 0U;
  storage::MigrationMarker marker;
  assert(storage::decodeMigrationJournalSlotV1(
             encoded_slot, sequence, marker) ==
         storage::MigrationMarkerCodecCode::Ok);
  assert(sequence == raw.head_sequence - 1U &&
         marker.phase == storage::MigrationPhase::Complete);
  marker.target_slot = marker.target_slot == storage::MigrationSlot::SlotA
      ? storage::MigrationSlot::SlotB : storage::MigrationSlot::SlotA;
  marker.checksum = storage::migrationMarkerChecksum(marker);
  storage::EncodedMigrationMarker encoded_marker{};
  assert(storage::encodeMigrationMarkerV1(marker, encoded_marker) ==
         storage::MigrationMarkerCodecCode::Ok);
  std::copy(encoded_marker.begin(), encoded_marker.end(),
            encoded_slot.bytes.begin() + 12U);
  put32(crc32(encoded_slot.bytes.data(), 68U),
        encoded_slot.bytes.data() + 68U);
}

std::string quote(const std::string& value) {
  std::string output = "\"";
  for (char ch : value) {
    if (ch == '\"' || ch == '\\') output.push_back('\\');
    output.push_back(ch);
  }
  output.push_back('\"');
  return output;
}

std::string legacyPayload(unsigned volume = 77U,
                          std::uint64_t revision = 19U) {
  return "{\"schema\":2,\"revision\":" + std::to_string(revision) +
      ",\"settings\":{"
      "\"storage\":1,\"volume\":" + std::to_string(volume) +
      ",\"voice_assistance\":false,\"prompt\":\"agent\","
      "\"image_prompt\":\"image {prompt}\",\"negative\":\"\","
      "\"led_brightness\":31,\"led_swap\":true,"
      "\"local_password\":\"wifi-pass\",\"refresh\":1}}";
}

std::string envelope(const std::string& payload) {
  return "{\"payload\":" + quote(payload) + ",\"sha256\":\"" +
      settings::testDigest(payload) + "\"}";
}

settings::DeviceSettings candidate(unsigned volume = 77U) {
  settings::DeviceSettings value = settings::makeGenericDeviceDefaults();
  value.volume_percent = static_cast<std::uint8_t>(volume);
  value.led_maximum_brightness_percent = 31U;
  value.led_roles_swapped = true;
  value.voice_assistance_enabled = false;
  value.assistant_prompt = "agent";
  value.aigc_prompt_template = "image {prompt}";
  value.negative_prompt.clear();
  value.asset_storage_preference = settings::AssetStoragePreference::Internal;
  value.default_render_strategy = "classic-six-color";
  value.local_management_password_override = "wifi-pass";
  assert(settings::validDeviceSettings(value));
  return value;
}

void resetStores() {
  settings::test_settings_state = settings::SettingsJournalState{};
  settings::test_settings_state.namespace_available = true;
  settings::test_legacy_state = settings::LegacyPortalJournalState{};
  settings::test_legacy_state.namespace_available = true;
  settings::test_settings_slot_writes = 0U;
  settings::test_settings_head_writes = 0U;
  settings::test_fail_settings_head_once = false;
  storage::test_marker_state = storage::RawMigrationMarkerJournal{};
  storage::test_marker_state.namespace_available = true;
  storage::test_marker_slot_writes = 0U;
  storage::test_marker_head_writes = 0U;
  storage::test_fail_marker_slot_call = 0U;
  storage::test_fail_marker_slot_after_commit_call = 0U;
  storage::test_fail_marker_head_once = false;
  storage::test_fail_marker_head_after_commit_once = false;
}

void installLegacy(unsigned volume = 77U, std::uint64_t revision = 19U) {
  const std::string payload = legacyPayload(volume, revision);
  settings::test_legacy_state.marker_present = true;
  settings::test_legacy_state.marker_valid = true;
  settings::test_legacy_state.head_present = true;
  settings::test_legacy_state.head = 1U;
  settings::test_legacy_state.slot_present[0] = true;
  settings::test_legacy_state.slot[0] = envelope(payload);
}

void installLegacySuccessor(unsigned volume, std::uint64_t revision) {
  assert(settings::test_legacy_state.head_present);
  assert(settings::test_legacy_state.head == 1U ||
         settings::test_legacy_state.head == 2U);
  const std::uint8_t next = settings::test_legacy_state.head == 1U ? 2U : 1U;
  const std::uint8_t slot = static_cast<std::uint8_t>(next - 1U);
  settings::test_legacy_state.slot_present[slot] = true;
  settings::test_legacy_state.slot[slot] =
      envelope(legacyPayload(volume, revision));
  settings::test_legacy_state.head = next;
}

std::vector<std::uint8_t> encodeOld(
    std::uint32_t generation, settings::DeviceSettings values,
    std::uint16_t schema = 2U) {
  settings::SettingsSnapshot snapshot;
  snapshot.generation = generation;
  snapshot.values = std::move(values);
  std::vector<std::uint8_t> bytes;
  assert(settings::encodeSettingsRecord(snapshot, bytes).ok());
  bytes[4] = static_cast<std::uint8_t>(schema);
  bytes[5] = 0U;
  bytes[6] &= static_cast<std::uint8_t>(~2U);
  const std::uint32_t checksum = crc32(bytes.data(), bytes.size() - 4U);
  for (std::size_t at = 0U; at < 4U; ++at) {
    bytes[bytes.size() - 4U + at] =
        static_cast<std::uint8_t>(checksum >> (at * 8U));
  }
  settings::SettingsSnapshot decoded;
  assert(settings::decodeSettingsRecord(bytes, decoded).ok());
  assert(decoded.decoded_record_schema == schema);
  return bytes;
}

void installNativeCurrent(std::uint32_t generation,
                          const settings::DeviceSettings& values) {
  assert(generation != 0U);
  settings::SettingsSnapshot snapshot;
  snapshot.generation = generation;
  snapshot.decoded_record_schema = settings::kSettingsRecordSchema;
  snapshot.values = values;
  std::vector<std::uint8_t> encoded;
  assert(settings::encodeSettingsRecord(snapshot, encoded).ok());
  settings::test_settings_state = settings::SettingsJournalState{};
  settings::test_settings_state.namespace_available = true;
  settings::test_settings_state.marker_present = true;
  settings::test_settings_state.marker_valid = true;
  settings::test_settings_state.head_present = true;
  settings::test_settings_state.head_generation = generation;
  const std::size_t selected = static_cast<std::size_t>(generation & 1U);
  settings::test_settings_state.slot_present[selected] = true;
  settings::test_settings_state.slot[selected] = std::move(encoded);
}

void installHistorical(std::uint32_t generation = 1U,
                       unsigned volume = 77U,
                       std::uint16_t schema = 2U) {
  settings::DeviceSettings old = candidate(volume);
  old.local_management_password_override.clear();
  old.led_roles_swapped = false;
  old.default_render_strategy = "experimental-six-color";
  const std::uint8_t slot = static_cast<std::uint8_t>(generation & 1U);
  settings::test_settings_state.marker_present = true;
  settings::test_settings_state.marker_valid = true;
  settings::test_settings_state.head_present = true;
  settings::test_settings_state.head_generation = generation;
  settings::test_settings_state.slot_present[slot] = true;
  settings::test_settings_state.slot[slot] =
      encodeOld(generation, std::move(old), schema);
}

NativeSettingsMigrationPlan audit(
    storage::EspNvsBootMountOwner& owner,
    NativeSettingsMigrationGate& gate) {
  owner.setAccess(storage::NvsBootMountAccess::ReadOnlyAudit);
  NativeSettingsMigrationPlan plan;
  assert(gate.auditReadOnly(plan) == NativeSettingsMigrationGateCode::Ok);
  return plan;
}

NativeSettingsMigrationGateCode auditCode(
    storage::EspNvsBootMountOwner& owner,
    NativeSettingsMigrationGate& gate,
    NativeSettingsMigrationPlan& plan) {
  owner.setAccess(storage::NvsBootMountAccess::ReadOnlyAudit);
  return gate.auditReadOnly(plan);
}

NativeSettingsMigrationGateCode execute(
    storage::EspNvsBootMountOwner& owner,
    NativeSettingsMigrationGate& gate,
    const NativeSettingsMigrationPlan& plan,
    NativeSettingsMigrationAuthorization& authorization) {
  owner.setAccess(storage::NvsBootMountAccess::ReadWriteProduct);
  return gate.execute(plan, authorization);
}

int main() {
  const BoardDescriptor board{};

  // A genuinely fresh device with no legacy source is authorized with
  // in-memory defaults only. Selection and execution create no journal or
  // marker; the first explicit user change owns the first settings write.
  resetStores();
  storage::EspNvsBootMountOwner owner;
  NativeSettingsMigrationGate fresh(board, owner);
  NativeSettingsMigrationPlan plan = audit(owner, fresh);
  assert(plan.kind == NativeSettingsMigrationPlanKind::FreshNoMigration);
  NativeSettingsMigrationAuthorization authorization;
  assert(execute(owner, fresh, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.kind == NativeSettingsAuthorityKind::FreshDefaults);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  // A fresh native journal with a verified legacy snapshot starts at target
  // generation one and keeps the untouched legacy snapshot as rollback.
  resetStores();
  installLegacy();
  NativeSettingsMigrationGate first_import(board, owner);
  plan = audit(owner, first_import);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 0U && plan.target_generation == 1U);
  assert(plan.rollback_source ==
         storage::MigrationRollbackSource::LegacySnapshot);
  authorization = NativeSettingsMigrationAuthorization{};
  assert(execute(owner, first_import, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 1U);

  // Old firmware may factory-reset the legacy namespace after Complete. A
  // genuinely absent source leaves the valid native target authoritative and
  // performs no writes; corrupt legacy bytes remain a hard source failure.
  const settings::SettingsJournalState completed_settings =
      settings::test_settings_state;
  const settings::LegacyPortalJournalState completed_legacy =
      settings::test_legacy_state;
  const storage::RawMigrationMarkerJournal completed_marker =
      storage::test_marker_state;
  const unsigned completed_settings_writes =
      settings::test_settings_slot_writes;
  const unsigned completed_marker_writes = storage::test_marker_slot_writes;
  settings::test_legacy_state = settings::LegacyPortalJournalState{};
  settings::test_legacy_state.namespace_available = true;
  NativeSettingsMigrationGate legacy_factory_reset(board, owner);
  plan = audit(owner, legacy_factory_reset);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, legacy_factory_reset, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 1U);
  assert(settings::test_settings_slot_writes == completed_settings_writes &&
         storage::test_marker_slot_writes == completed_marker_writes);

  settings::test_settings_state = completed_settings;
  settings::test_legacy_state = completed_legacy;
  storage::test_marker_state = completed_marker;
  settings::EspNvsSettingsJournalStore absent_newer_store;
  settings::SettingsStoreCore absent_newer_core(
      absent_newer_store, settings::makeGenericDeviceDefaults());
  settings::SettingsSnapshot absent_newer_snapshot;
  assert(absent_newer_core.save(
             candidate(88U), 1U, absent_newer_snapshot).ok());
  assert(absent_newer_snapshot.generation == 2U);
  settings::test_legacy_state = settings::LegacyPortalJournalState{};
  settings::test_legacy_state.namespace_available = true;
  const unsigned absent_newer_writes = settings::test_settings_slot_writes;
  NativeSettingsMigrationGate absent_with_newer_native(board, owner);
  plan = audit(owner, absent_with_newer_native);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, absent_with_newer_native, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 2U);
  assert(settings::test_settings_slot_writes == absent_newer_writes &&
         storage::test_marker_slot_writes == completed_marker_writes);

  settings::test_settings_state = completed_settings;
  settings::test_legacy_state = completed_legacy;
  storage::test_marker_state = completed_marker;
  settings::test_legacy_state.slot[0] = "corrupt";
  NativeSettingsMigrationGate corrupt_after_complete(board, owner);
  assert(auditCode(owner, corrupt_after_complete, plan) ==
         NativeSettingsMigrationGateCode::SourceCorrupt);

  settings::test_settings_state = completed_settings;
  settings::test_legacy_state = settings::LegacyPortalJournalState{};
  storage::test_marker_state = completed_marker;
  NativeSettingsMigrationGate unavailable_after_complete(board, owner);
  assert(auditCode(owner, unavailable_after_complete, plan) ==
         NativeSettingsMigrationGateCode::SourceCorrupt);
  settings::test_settings_state = completed_settings;
  settings::test_legacy_state = completed_legacy;
  storage::test_marker_state = completed_marker;

  // After beta30 completed the original gen1 import, an OTA rollback to the
  // Arduino firmware commits revision+1 into the alternate ink-portal slot.
  // The old slot remains the exact predecessor named by the Complete marker.
  // This exact lineage is sufficient for a post-complete rollover.
  installLegacySuccessor(66U, 20U);
  const settings::SettingsJournalState rollover_settings =
      settings::test_settings_state;
  const settings::LegacyPortalJournalState rollover_legacy =
      settings::test_legacy_state;
  const storage::RawMigrationMarkerJournal rollover_marker =
      storage::test_marker_state;
  auto restoreRollover = [&]() {
    settings::test_settings_state = rollover_settings;
    settings::test_legacy_state = rollover_legacy;
    storage::test_marker_state = rollover_marker;
    settings::test_settings_slot_writes = 0U;
    settings::test_settings_head_writes = 0U;
    storage::test_marker_slot_writes = 0U;
    storage::test_marker_head_writes = 0U;
    settings::test_fail_settings_head_once = false;
    storage::test_fail_marker_slot_call = 0U;
    storage::test_fail_marker_slot_after_commit_call = 0U;
    storage::test_fail_marker_head_once = false;
    storage::test_fail_marker_head_after_commit_once = false;
  };

  NativeSettingsMigrationGate rollback_forward(board, owner);
  plan = audit(owner, rollback_forward);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 1U && plan.target_generation == 2U);
  assert(plan.marker_sequence != 0U &&
         plan.marker_phase == storage::MigrationPhase::Complete);
  assert(plan.rollback_source == storage::MigrationRollbackSource::NativeSlotB);

  // Power loss after the new Prepared slot but before its head switch leaves
  // the old Complete marker authoritative. The same two-slot lineage safely
  // retries Start using the current marker sequence, rather than sequence 0.
  storage::test_fail_marker_head_once = true;
  assert(execute(owner, rollback_forward, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  NativeSettingsMigrationGate prepared_head_restart(board, owner);
  plan = audit(owner, prepared_head_restart);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start &&
         plan.marker_sequence != 0U);
  assert(execute(owner, prepared_head_restart, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 2U);
  settings::SettingsSnapshot rollover_result;
  settings::EspNvsSettingsJournalStore rollover_store;
  settings::SettingsStoreCore rollover_core(
      rollover_store, settings::makeGenericDeviceDefaults());
  assert(rollover_core.load(rollover_result).ok());
  assert(rollover_result.values == candidate(66U));

  // The NVS head may become durable even when its commit call reports an I/O
  // error. Restart observes Prepared plus exact raw seq-1 Complete and resumes
  // instead of retrying Start or entering Recovery.
  restoreRollover();
  NativeSettingsMigrationGate durable_prepared_head(board, owner);
  plan = audit(owner, durable_prepared_head);
  storage::test_fail_marker_head_after_commit_once = true;
  assert(execute(owner, durable_prepared_head, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  storage::MigrationMarkerJournalInspection durable_prepared;
  storage::EspNvsMigrationMarkerJournalStore durable_marker_store;
  assert(storage::MigrationMarkerJournalCore(durable_marker_store).inspect(
             durable_prepared) == storage::MigrationMarkerJournalCode::Ok);
  assert(durable_prepared.marker.phase == storage::MigrationPhase::Prepared);
  NativeSettingsMigrationGate durable_prepared_restart(board, owner);
  plan = audit(owner, durable_prepared_restart);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  assert(execute(owner, durable_prepared_restart, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 2U);

  // A Prepared slot can likewise be durable while its write reports failure.
  // Since the old Complete head remains authoritative, restart safely retries
  // Start with the same source and expected sequence.
  restoreRollover();
  NativeSettingsMigrationGate durable_prepared_slot(board, owner);
  plan = audit(owner, durable_prepared_slot);
  storage::test_fail_marker_slot_after_commit_call = 1U;
  assert(execute(owner, durable_prepared_slot, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  NativeSettingsMigrationGate durable_slot_restart(board, owner);
  plan = audit(owner, durable_slot_restart);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(execute(owner, durable_slot_restart, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 2U);

  // Power loss after Prepared became authoritative but before the target head
  // uses raw journal seq-1 Complete evidence plus the legacy predecessor.
  restoreRollover();
  NativeSettingsMigrationGate target_head_fault(board, owner);
  plan = audit(owner, target_head_fault);
  settings::test_fail_settings_head_once = true;
  assert(execute(owner, target_head_fault, plan, authorization) ==
         NativeSettingsMigrationGateCode::TargetWriteFailed);
  assert(settings::test_settings_state.head_generation == 1U);
  const settings::SettingsJournalState prepared_settings =
      settings::test_settings_state;
  const storage::RawMigrationMarkerJournal prepared_marker =
      storage::test_marker_state;

  // A sequence>1 Prepared cannot reuse the marker-missing historical bootstrap
  // rule. Even a historical-looking schema-2 gen1 target requires the exact
  // raw seq-1 Complete rollover proof and a current native base.
  installHistorical(1U, 66U, 2U);
  settings::SettingsSnapshot forged_historical_target;
  assert(rollover_core.load(forged_historical_target).ok());
  settings::LegacySettingsImport forged_historical_legacy;
  settings::EspNvsReadOnlyLegacyPortalSource forged_legacy_source;
  settings::EspPsaLegacySha256Verifier forged_legacy_sha;
  assert(settings::inspectLegacyPortalSettings(
             forged_legacy_source, forged_legacy_sha,
             settings::makeGenericDeviceDefaults(),
             forged_historical_legacy).ok());
  assert(settings::matchesHistoricalIncompleteImport(
      forged_historical_target, forged_historical_legacy));
  NativeSettingsMigrationGate forged_historical_prepared(board, owner);
  assert(auditCode(owner, forged_historical_prepared, plan) ==
         NativeSettingsMigrationGateCode::TargetCorrupt);
  settings::test_settings_state = prepared_settings;
  storage::test_marker_state = prepared_marker;

  // The inactive raw marker must itself be a coherent exact seq-1 Complete.
  // A checksum-valid previous marker with target-slot parity changed is not
  // acceptable provenance for writing the target.
  corruptPreviousCompleteTargetParity();
  NativeSettingsMigrationGate wrong_previous_parity(board, owner);
  assert(auditCode(owner, wrong_previous_parity, plan) ==
         NativeSettingsMigrationGateCode::TargetCorrupt);
  storage::test_marker_state = prepared_marker;

  NativeSettingsMigrationGate target_head_resume(board, owner);
  plan = audit(owner, target_head_resume);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  assert(execute(owner, target_head_resume, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 2U);

  // Power loss after native gen2 is durable but before TargetWritten is also
  // idempotent; resume must not rewrite the target journal.
  restoreRollover();
  NativeSettingsMigrationGate target_written_fault(board, owner);
  plan = audit(owner, target_written_fault);
  storage::test_fail_marker_slot_call = 2U;
  assert(execute(owner, target_written_fault, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  const unsigned rollover_target_writes = settings::test_settings_slot_writes;
  assert(rollover_target_writes == 1U &&
         settings::test_settings_state.head_generation == 2U);
  storage::test_fail_marker_slot_call = 0U;
  // Once the target is the exact current-source result, it is sufficient
  // resume evidence even if an attempted next-phase slot write overwrote the
  // inactive previous Complete record before its head commit.
  corruptPreviousCompleteTargetParity();
  NativeSettingsMigrationGate target_written_resume(board, owner);
  plan = audit(owner, target_written_resume);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  assert(execute(owner, target_written_resume, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(settings::test_settings_slot_writes == rollover_target_writes);

  // A later native schema-3 save wins over a changed rollback source. It is
  // authorized without rewriting either journal, even though the old Complete
  // marker still names the predecessor fingerprint.
  restoreRollover();
  settings::SettingsSnapshot native_edited;
  settings::DeviceSettings native_values = candidate(88U);
  assert(rollover_core.save(native_values, 1U, native_edited).ok());
  const unsigned native_edit_writes = settings::test_settings_slot_writes;
  const unsigned native_marker_writes = storage::test_marker_slot_writes;
  NativeSettingsMigrationGate native_newer(board, owner);
  plan = audit(owner, native_newer);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, native_newer, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 2U);
  assert(settings::test_settings_slot_writes == native_edit_writes &&
         storage::test_marker_slot_writes == native_marker_writes);

  // The inactive legacy slot is not a security boundary: a missing stale slot
  // or skipped revisions still migrate when the valid preferred source
  // changed and native has not advanced beyond Complete.
  restoreRollover();
  settings::test_legacy_state.slot_present[0] = false;
  NativeSettingsMigrationGate missing_predecessor(board, owner);
  plan = audit(owner, missing_predecessor);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start &&
         plan.target_generation == 2U);

  restoreRollover();
  settings::test_legacy_state.slot[1] =
      envelope(legacyPayload(66U, 22U));
  NativeSettingsMigrationGate skipped_revision(board, owner);
  plan = audit(owner, skipped_revision);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start &&
         plan.target_generation == 2U);

  // Two normal Arduino saves after rollback have already overwritten the
  // Complete marker's legacy predecessor. Native generation is unchanged, so
  // the final valid committed legacy value must still migrate forward.
  restoreRollover();
  installLegacySuccessor(67U, 21U);
  NativeSettingsMigrationGate two_legacy_saves(board, owner);
  plan = audit(owner, two_legacy_saves);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start &&
         plan.target_generation == 2U);
  assert(execute(owner, two_legacy_saves, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 2U);
  assert(rollover_core.load(rollover_result).ok());
  assert(rollover_result.values == candidate(67U));

  restoreRollover();
  NativeSettingsMigrationGate complete_rollover(board, owner);
  plan = audit(owner, complete_rollover);
  assert(execute(owner, complete_rollover, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  installLegacySuccessor(67U, 21U);
  NativeSettingsMigrationGate second_rollover(board, owner);
  plan = audit(owner, second_rollover);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 2U && plan.target_generation == 3U);
  assert(plan.rollback_source == storage::MigrationRollbackSource::NativeSlotA);
  assert(execute(owner, second_rollover, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 3U);
  assert(rollover_core.load(rollover_result).ok());
  assert(rollover_result.values == candidate(67U));

  // Repeated OTA rollback cycles use the same three-way proof at every
  // completed generation; no special trust is granted merely because gen2+
  // exists.
  installLegacySuccessor(68U, 22U);
  NativeSettingsMigrationGate third_rollover(board, owner);
  plan = audit(owner, third_rollover);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 3U && plan.target_generation == 4U);
  assert(plan.rollback_source == storage::MigrationRollbackSource::NativeSlotB);
  assert(execute(owner, third_rollover, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 4U);
  assert(rollover_core.load(rollover_result).ok());
  assert(rollover_result.values == candidate(68U));

  // The old auto-importer could only create generation one. Combined with a
  // schema-1/2 record, exact historical projection, and verified live legacy
  // fingerprint, that is sufficient persistent proof for automatic repair.
  resetStores();
  installLegacy();
  installHistorical();
  NativeSettingsMigrationGate first(board, owner);
  plan = NativeSettingsMigrationPlan{};
  assert(auditCode(owner, first, plan) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 1U && plan.target_generation == 2U);
  assert(plan.rollback_source == storage::MigrationRollbackSource::NativeSlotB);
  assert(settings::test_settings_slot_writes == 0U &&
         settings::test_settings_head_writes == 0U &&
         storage::test_marker_slot_writes == 0U &&
         storage::test_marker_head_writes == 0U);

  // Power loss after Prepared slot but before marker head is recoverable. No
  // target write occurred and the old native rollback slot remains intact.
  const std::vector<std::uint8_t> rollback_slot =
      settings::test_settings_state.slot[1];
  storage::test_fail_marker_head_once = true;
  authorization = NativeSettingsMigrationAuthorization{};
  assert(execute(owner, first, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  assert(settings::test_settings_slot_writes == 0U);
  assert(settings::test_settings_state.slot[1] == rollback_slot);
  storage::MigrationMarkerJournalInspection torn;
  storage::EspNvsMigrationMarkerJournalStore marker_store;
  assert(storage::MigrationMarkerJournalCore(marker_store).inspect(torn) ==
      storage::MigrationMarkerJournalCode::Torn);

  NativeSettingsMigrationGate after_torn(board, owner);
  plan = audit(owner, after_torn);
  assert(plan.kind == NativeSettingsMigrationPlanKind::RecoverPreparedHead);
  assert(execute(owner, after_torn, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid() && authorization.observed_generation == 2U);
  settings::SettingsSnapshot migrated;
  settings::EspNvsSettingsJournalStore settings_store;
  settings::SettingsStoreCore settings_core(
      settings_store, settings::makeGenericDeviceDefaults());
  assert(settings_core.load(migrated).ok());
  assert(migrated.decoded_record_schema == settings::kSettingsRecordSchema);
  assert(migrated.values == candidate());
  assert(settings::test_settings_state.slot[1] == rollback_slot);

  // The beta27 native-gen1 repair completes as historical gen2. If Arduino
  // then commits a direct legacy successor during an OTA rollback, forward
  // migration starts gen3. A reset after Prepared became authoritative but
  // before the gen3 target head must resume from raw seq-1 Complete proof.
  installLegacySuccessor(66U, 20U);
  NativeSettingsMigrationGate historical_rollover_fault(board, owner);
  plan = audit(owner, historical_rollover_fault);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(plan.observed_generation == 2U && plan.target_generation == 3U);
  settings::test_fail_settings_head_once = true;
  assert(execute(owner, historical_rollover_fault, plan, authorization) ==
         NativeSettingsMigrationGateCode::TargetWriteFailed);
  assert(settings::test_settings_state.head_generation == 2U);
  NativeSettingsMigrationGate historical_rollover_resume(board, owner);
  plan = audit(owner, historical_rollover_resume);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  assert(execute(owner, historical_rollover_resume, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 3U);
  assert(settings_core.load(migrated).ok());
  assert(migrated.values == candidate(66U));

  // Power loss after the target became authoritative but before the
  // TargetWritten marker advances resumes without rewriting the target.
  resetStores();
  installLegacy();
  installHistorical();
  NativeSettingsMigrationGate phase_fault(board, owner);
  plan = audit(owner, phase_fault);
  storage::test_fail_marker_slot_call = 2U;
  assert(execute(owner, phase_fault, plan, authorization) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  const unsigned target_writes = settings::test_settings_slot_writes;
  assert(target_writes == 1U);
  NativeSettingsMigrationGate phase_restart(board, owner);
  plan = audit(owner, phase_restart);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  storage::test_fail_marker_slot_call = 0U;
  assert(execute(owner, phase_restart, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(settings::test_settings_slot_writes == target_writes);

  // A failed target-head commit leaves the old head authoritative. Restart
  // retries from Prepared and preserves the rollback slot.
  resetStores();
  installLegacy();
  installHistorical();
  NativeSettingsMigrationGate target_fault(board, owner);
  plan = audit(owner, target_fault);
  settings::test_fail_settings_head_once = true;
  assert(execute(owner, target_fault, plan, authorization) ==
         NativeSettingsMigrationGateCode::TargetWriteFailed);
  assert(settings::test_settings_state.head_generation == 1U);
  NativeSettingsMigrationGate target_restart(board, owner);
  plan = audit(owner, target_restart);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Resume);
  assert(execute(owner, target_restart, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);

  // A user edit is never overwritten, even if every omitted field still has
  // the historical default. The native journal is authorized without marker
  // or settings commits.
  resetStores();
  installLegacy();
  installHistorical(1U, 78U);
  NativeSettingsMigrationGate edited(board, owner);
  plan = audit(owner, edited);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, edited, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.valid());
  assert(storage::test_marker_slot_writes == 0U &&
         settings::test_settings_slot_writes == 0U);

  // Schema 3 is already native authority and cannot be mistaken for the old
  // importer merely because its values collide with the historical projection.
  resetStores();
  installLegacy();
  installHistorical(1U, 77U, settings::kSettingsRecordSchema);
  NativeSettingsMigrationGate schema3(board, owner);
  plan = audit(owner, schema3);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);

  // Same-valued generation >=2 can be an intentional user save. It remains
  // native authority, starts Product normally, and is never overwritten.
  resetStores();
  installLegacy();
  installHistorical(2U);
  NativeSettingsMigrationGate generation2(board, owner);
  plan = audit(owner, generation2);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, generation2, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  // Journal-sequence exhaustion is checked during read-only composition, not
  // after a target or phase write. Complete with an unchanged source remains a
  // safe no-op, while every phase that would need another commit fails with
  // zero writes.
  resetStores();
  installLegacy();
  NativeSettingsMigrationGate exhaustion_seed(board, owner);
  plan = audit(owner, exhaustion_seed);
  assert(execute(owner, exhaustion_seed, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  const settings::SettingsJournalState exhaustion_settings =
      settings::test_settings_state;
  const settings::LegacyPortalJournalState exhaustion_legacy =
      settings::test_legacy_state;
  const storage::RawMigrationMarkerJournal exhaustion_marker =
      storage::test_marker_state;
  const storage::MigrationMarker exhaustion_identity = currentMarker();
  constexpr std::uint64_t kMaxSequence =
      std::numeric_limits<std::uint64_t>::max();

  installCurrentMarker(exhaustion_identity, kMaxSequence);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate exhausted_complete(board, owner);
  plan = audit(owner, exhausted_complete);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Complete);
  assert(execute(owner, exhausted_complete, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  settings::test_settings_state = exhaustion_settings;
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  installCurrentMarker(exhaustion_identity, kMaxSequence);
  installLegacySuccessor(66U, 20U);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate exhausted_rollover(board, owner);
  assert(auditCode(owner, exhausted_rollover, plan) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  // Start consumes five marker sequences. MAX-4 is already one short and is
  // rejected before the target write; MAX-5 has exact capacity and reaches a
  // durable Complete at MAX.
  settings::test_settings_state = exhaustion_settings;
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  installCurrentMarker(exhaustion_identity, kMaxSequence - 4U);
  installLegacySuccessor(66U, 20U);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate one_sequence_short(board, owner);
  assert(auditCode(owner, one_sequence_short, plan) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  settings::test_settings_state = exhaustion_settings;
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  installCurrentMarker(exhaustion_identity, kMaxSequence - 5U);
  installLegacySuccessor(66U, 20U);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate exact_sequence_capacity(board, owner);
  plan = audit(owner, exact_sequence_capacity);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start);
  assert(execute(owner, exact_sequence_capacity, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == 2U &&
         storage::test_marker_state.head_sequence == kMaxSequence);
  settings::EspNvsSettingsJournalStore exhaustion_result_store;
  settings::SettingsStoreCore exhaustion_result_core(
      exhaustion_result_store, settings::makeGenericDeviceDefaults());
  settings::SettingsSnapshot exhaustion_result;
  assert(exhaustion_result_core.load(exhaustion_result).ok());
  assert(exhaustion_result.values == candidate(66U));

  const std::array<storage::MigrationPhase, 4U> exhausted_phases{{
      storage::MigrationPhase::Prepared,
      storage::MigrationPhase::TargetWritten,
      storage::MigrationPhase::TargetVerified,
      storage::MigrationPhase::CommitRecorded,
  }};
  for (storage::MigrationPhase phase : exhausted_phases) {
    settings::test_settings_state = exhaustion_settings;
    settings::test_legacy_state = exhaustion_legacy;
    storage::test_marker_state = exhaustion_marker;
    storage::MigrationMarker phase_marker = exhaustion_identity;
    phase_marker.phase = phase;
    phase_marker.checksum = storage::migrationMarkerChecksum(phase_marker);
    installCurrentMarker(phase_marker, kMaxSequence);
    settings::test_settings_slot_writes = 0U;
    storage::test_marker_slot_writes = 0U;
    NativeSettingsMigrationGate exhausted_phase(board, owner);
    assert(auditCode(owner, exhausted_phase, plan) ==
           NativeSettingsMigrationGateCode::MarkerWriteFailed);
    assert(settings::test_settings_slot_writes == 0U &&
           storage::test_marker_slot_writes == 0U);
  }

  // Prepared exhaustion also fails before writing when the target still names
  // the previous generation.
  settings::test_settings_state = prepared_settings;
  settings::test_legacy_state = rollover_legacy;
  storage::test_marker_state = prepared_marker;
  storage::MigrationMarker exhausted_prepared_identity = currentMarker();
  assert(exhausted_prepared_identity.phase ==
         storage::MigrationPhase::Prepared);
  installCurrentMarker(exhausted_prepared_identity, kMaxSequence);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate exhausted_prepared_target(board, owner);
  assert(auditCode(owner, exhausted_prepared_target, plan) ==
         NativeSettingsMigrationGateCode::MarkerWriteFailed);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  // UINT32_MAX is a valid completed native generation but cannot roll to an
  // unrepresentable next settings generation. Both unchanged and changed
  // legacy sources preserve native authority without journal writes.
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  storage::MigrationMarker max_generation_marker = exhaustion_identity;
  constexpr std::uint32_t kMaxGeneration =
      std::numeric_limits<std::uint32_t>::max();
  max_generation_marker.generation = kMaxGeneration;
  max_generation_marker.target_slot = storage::MigrationSlot::SlotB;
  max_generation_marker.rollback_source =
      storage::MigrationRollbackSource::NativeSlotA;
  max_generation_marker.checksum =
      storage::migrationMarkerChecksum(max_generation_marker);
  installCurrentMarker(
      max_generation_marker, exhaustion_marker.head_sequence);
  installNativeCurrent(kMaxGeneration, candidate());
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate max_generation_complete(board, owner);
  plan = audit(owner, max_generation_complete);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Complete);
  assert(execute(owner, max_generation_complete, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == kMaxGeneration);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);
  installLegacySuccessor(66U, 20U);
  NativeSettingsMigrationGate max_generation_rollover(board, owner);
  plan = audit(owner, max_generation_rollover);
  assert(plan.kind == NativeSettingsMigrationPlanKind::NativeNoMigration);
  assert(execute(owner, max_generation_rollover, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == kMaxGeneration);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);

  // The positive generation boundary remains usable: Complete MAX-1 plus a
  // changed legacy source migrates exactly once to MAX.
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  storage::MigrationMarker penultimate_marker = exhaustion_identity;
  penultimate_marker.generation = kMaxGeneration - 1U;
  penultimate_marker.target_slot = storage::MigrationSlot::SlotA;
  penultimate_marker.rollback_source =
      storage::MigrationRollbackSource::NativeSlotB;
  penultimate_marker.checksum =
      storage::migrationMarkerChecksum(penultimate_marker);
  installCurrentMarker(penultimate_marker, exhaustion_marker.head_sequence);
  installNativeCurrent(kMaxGeneration - 1U, candidate());
  installLegacySuccessor(66U, 20U);
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate penultimate_rollover(board, owner);
  plan = audit(owner, penultimate_rollover);
  assert(plan.kind == NativeSettingsMigrationPlanKind::Start &&
         plan.target_generation == kMaxGeneration);
  assert(execute(owner, penultimate_rollover, plan, authorization) ==
         NativeSettingsMigrationGateCode::Ok);
  assert(authorization.observed_generation == kMaxGeneration);
  assert(exhaustion_result_core.load(exhaustion_result).ok());
  assert(exhaustion_result.values == candidate(66U));

  // Marker generations outside the native uint32 range are codec-valid but
  // unsupported by this gate and fail before any write.
  settings::test_legacy_state = exhaustion_legacy;
  storage::test_marker_state = exhaustion_marker;
  storage::MigrationMarker oversized_generation_marker = exhaustion_identity;
  oversized_generation_marker.generation =
      static_cast<std::uint64_t>(kMaxGeneration) + 1U;
  oversized_generation_marker.target_slot = storage::MigrationSlot::SlotA;
  oversized_generation_marker.rollback_source =
      storage::MigrationRollbackSource::NativeSlotB;
  oversized_generation_marker.checksum =
      storage::migrationMarkerChecksum(oversized_generation_marker);
  installCurrentMarker(
      oversized_generation_marker, exhaustion_marker.head_sequence);
  installNativeCurrent(kMaxGeneration, candidate());
  settings::test_settings_slot_writes = 0U;
  storage::test_marker_slot_writes = 0U;
  NativeSettingsMigrationGate oversized_generation(board, owner);
  assert(auditCode(owner, oversized_generation, plan) ==
         NativeSettingsMigrationGateCode::MarkerMismatch);
  assert(settings::test_settings_slot_writes == 0U &&
         storage::test_marker_slot_writes == 0U);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-migration-gate-"));
  try {
    const stub = join(scratch, "include");
    for (const path of [
      "inkloop/settings", "inkloop/storage", "inkloop",
    ]) mkdirSync(join(stub, path), { recursive: true });
    writeFileSync(join(stub, "inkloop/board.hpp"), board);
    writeFileSync(join(stub, "inkloop/board_prompt_policy.hpp"), prompts);
    writeFileSync(join(stub,
      "inkloop/storage/esp_upgrade_boot_audit.hpp"), nvsBoot);
    writeFileSync(join(stub,
      "inkloop/settings/esp_nvs_settings_store.hpp"), settingsAdapters);
    writeFileSync(join(stub,
      "inkloop/storage/esp_nvs_upgrade_marker_journal.hpp"), markerAdapter);
    const source = join(scratch, "harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", stub,
      "-I", join(idf, "inkloop_product/include"),
      "-I", join(idf, "inkloop_settings/include"),
      "-I", join(idf, "inkloop_storage/include"),
      source,
      join(idf, "inkloop_product/native_settings_migration_gate.cpp"),
      join(idf, "inkloop_settings/device_settings.cpp"),
      join(idf, "inkloop_settings/settings_journal.cpp"),
      join(idf, "inkloop_settings/legacy_portal_import.cpp"),
      join(idf, "inkloop_storage/upgrade_recovery_planner.cpp"),
      join(idf, "inkloop_storage/upgrade_marker_journal.cpp"),
      "-o", binary,
    ];
    if (sanitized) args.splice(1, 0,
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("settings migration gate resumes every historical power-loss boundary", () => {
  buildAndRun(false);
});

test("settings migration gate historical recovery is sanitizer-clean", () => {
  buildAndRun(true);
});
