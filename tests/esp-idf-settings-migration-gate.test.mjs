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
inline bool test_fail_marker_head_once = false;

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
    return MigrationJournalStoreCode::Ok;
  }
};
}
`;

const harness = String.raw`
#include <cassert>
#include <cstdint>
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

std::string quote(const std::string& value) {
  std::string output = "\"";
  for (char ch : value) {
    if (ch == '\"' || ch == '\\') output.push_back('\\');
    output.push_back(ch);
  }
  output.push_back('\"');
  return output;
}

std::string legacyPayload(unsigned volume = 77U) {
  return "{\"schema\":2,\"revision\":19,\"settings\":{"
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
  storage::test_fail_marker_head_once = false;
}

void installLegacy(unsigned volume = 77U) {
  const std::string payload = legacyPayload(volume);
  settings::test_legacy_state.marker_present = true;
  settings::test_legacy_state.marker_valid = true;
  settings::test_legacy_state.head_present = true;
  settings::test_legacy_state.head = 1U;
  settings::test_legacy_state.slot_present[0] = true;
  settings::test_legacy_state.slot[0] = envelope(payload);
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
