import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const settings = join(repo, "firmware/inkloop-idf/components/inkloop_settings");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

#include "inkloop/settings/settings_extension_journal.hpp"

using namespace inkloop::settings;

struct MainJournal final : ISettingsJournalStore {
  SettingsJournalState state;
  bool fail_slot_before = false;
  bool fail_slot_after = false;
  bool fail_head_before = false;
  bool fail_head_after_head = false;
  bool fail_head_after_marker = false;
  unsigned slot_writes = 0U;
  unsigned head_writes = 0U;

  MainJournal() { state.namespace_available = true; }

  SettingsStatus inspect(SettingsJournalState& output) override {
    output = state;
    return SettingsStatus::success();
  }
  SettingsStatus writeSlotAndCommit(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) override {
    ++slot_writes;
    if (fail_slot_before) return {SettingsError::Storage, "main slot before"};
    state.slot_present[slot] = true;
    state.slot[slot] = encoded;
    return fail_slot_after
        ? SettingsStatus{SettingsError::Storage, "main slot after"}
        : SettingsStatus::success();
  }
  SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) override {
    ++head_writes;
    if (fail_head_before) return {SettingsError::Storage, "main head before"};
    state.head_present = true;
    state.head_generation = generation;
    if (fail_head_after_head)
      return {SettingsError::Storage, "main after head set"};
    state.marker_present = true;
    state.marker_valid = true;
    return fail_head_after_marker
        ? SettingsStatus{SettingsError::Storage, "main after marker set"}
        : SettingsStatus::success();
  }
};

struct ExtensionJournal final : ISettingsExtensionJournalStore {
  SettingsExtensionJournalState state;
  bool fail_inspect = false;
  bool fail_slot_before = false;
  bool fail_slot_after = false;
  bool fail_head_before = false;
  bool fail_head_after = false;
  unsigned slot_writes = 0U;
  unsigned head_writes = 0U;

  ExtensionJournal() { state.namespace_available = true; }

  SettingsStatus inspect(SettingsExtensionJournalState& output) override {
    if (fail_inspect) return {SettingsError::Storage, "ext inspect"};
    output = state;
    return SettingsStatus::success();
  }
  SettingsStatus writeSlot(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) override {
    ++slot_writes;
    if (fail_slot_before) return {SettingsError::Storage, "ext slot before"};
    state.slot_present[slot] = true;
    state.slot[slot] = encoded;
    return fail_slot_after
        ? SettingsStatus{SettingsError::Storage, "ext slot after"}
        : SettingsStatus::success();
  }
  SettingsStatus writeHead(std::uint32_t sequence) override {
    ++head_writes;
    if (fail_head_before) return {SettingsError::Storage, "ext head before"};
    state.head_present = true;
    state.head_sequence = sequence;
    return fail_head_after
        ? SettingsStatus{SettingsError::Storage, "ext head after"}
        : SettingsStatus::success();
  }
};

struct Rig {
  MainJournal main_journal;
  ExtensionJournal extension_journal;
  DeviceSettings defaults = makeGenericDeviceDefaults();
  SettingsStoreCore main{main_journal, defaults};
  SettingsExtensionStoreCore extension{extension_journal};

  SettingsSnapshot load() {
    SettingsSnapshot output;
    assert(loadRollbackCompatibleSettings(main, extension, output).ok());
    return output;
  }

  SettingsStatus save(
      const DeviceSettings& values, std::uint32_t expected,
      SettingsSnapshot& output, bool force_main = false) {
    return saveRollbackCompatibleSettings(
        main, extension, values, expected, output, force_main);
  }
};

DeviceSettings beta27MainValues(const DeviceSettings& input) {
  DeviceSettings output = input;
  output.led_roles_swapped = false;
  output.aigc_steps = kDefaultAigcSteps;
  return output;
}

void assertMainRawIsBeta27Compatible(
    const MainJournal& journal, std::uint32_t generation) {
  const std::uint8_t slot = static_cast<std::uint8_t>(generation & 1U);
  assert(journal.state.slot_present[slot]);
  const std::vector<std::uint8_t>& raw = journal.state.slot[slot];
  assert(raw.size() >= 30U);
  assert(raw[4] == 2U && raw[5] == 0U);
  assert((raw[6] & static_cast<std::uint8_t>(~1U)) == 0U);
  assert(raw[15] == 0U);
  SettingsSnapshot decoded;
  assert(decodeSettingsRecord(raw, decoded).ok());
  assert(decoded.decoded_record_schema == 2U);
  assert(!decoded.values.led_roles_swapped);
  assert(decoded.values.aigc_steps == kDefaultAigcSteps);
}

std::uint32_t testCrc32(const std::uint8_t* bytes, std::size_t length) {
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

void rewriteCrc(std::vector<std::uint8_t>& bytes) {
  const std::uint32_t checksum = testCrc32(bytes.data(), bytes.size() - 4U);
  for (std::size_t at = 0U; at < 4U; ++at) {
    bytes[bytes.size() - 4U + at] =
        static_cast<std::uint8_t>(checksum >> (at * 8U));
  }
}

int main() {
  Rig rig;
  SettingsSnapshot current = rig.load();
  assert(current.generation == 0U && !current.values.led_roles_swapped &&
         current.values.aigc_steps == kDefaultAigcSteps);

  // Extension-only save advances only ext-head.
  DeviceSettings extension_only = current.values;
  extension_only.led_roles_swapped = true;
  extension_only.aigc_steps = 37U;
  assert(rig.save(extension_only, 0U, current).ok());
  assert(current.generation == 0U && rig.main_journal.slot_writes == 0U &&
         rig.extension_journal.state.head_sequence == 1U);
  current = rig.load();
  assert(current.values == extension_only);

  // Main-only save advances schema-2 main and preserves ext-head/value.
  DeviceSettings main_only = current.values;
  main_only.volume_percent = 77U;
  assert(rig.save(main_only, 0U, current).ok());
  assert(current.generation == 1U &&
         rig.extension_journal.state.head_sequence == 1U);
  assert(current.values == main_only);
  assertMainRawIsBeta27Compatible(rig.main_journal, 1U);

  // Mixed save prepares extension, commits schema-2 main, then publishes one
  // ext-head key.
  DeviceSettings mixed = current.values;
  mixed.volume_percent = 78U;
  mixed.led_roles_swapped = false;
  mixed.aigc_steps = 41U;
  assert(rig.save(mixed, 1U, current).ok());
  assert(current.generation == 2U && current.values == mixed &&
         rig.extension_journal.state.head_sequence == 2U);
  assertMainRawIsBeta27Compatible(rig.main_journal, 2U);

  // A never-released schema-3 record remains readable. Its next explicit save
  // must seed LED/steps into the sidecar and downgrade the main raw bytes to
  // schema 2 even when the user edits only a main field.
  Rig schema_three;
  SettingsSnapshot schema_three_state = schema_three.load();
  DeviceSettings schema_three_main = schema_three_state.values;
  schema_three_main.volume_percent = 64U;
  assert(schema_three.main.save(beta27MainValues(schema_three_main), 0U,
                                schema_three_state).ok());
  std::vector<std::uint8_t>& schema_three_raw =
      schema_three.main_journal.state.slot[1];
  schema_three_raw[4] = 3U;
  schema_three_raw[6] |= 2U;
  rewriteCrc(schema_three_raw);
  schema_three_state = schema_three.load();
  assert(schema_three_state.decoded_record_schema == 3U &&
         schema_three_state.values.led_roles_swapped &&
         schema_three_state.values.aigc_steps == kDefaultAigcSteps);
  DeviceSettings schema_three_edit = schema_three_state.values;
  schema_three_edit.volume_percent = 65U;
  assert(schema_three.save(schema_three_edit, 1U,
                           schema_three_state).ok());
  assert(schema_three_state.generation == 2U &&
         schema_three_state.decoded_record_schema == 2U &&
         schema_three_state.values.led_roles_swapped &&
         schema_three.extension_journal.state.head_present);
  assertMainRawIsBeta27Compatible(schema_three.main_journal, 2U);

  const DeviceSettings committed = current.values;
  const unsigned main_slot_writes = rig.main_journal.slot_writes;
  const unsigned main_head_writes = rig.main_journal.head_writes;

  DeviceSettings attempted = committed;
  attempted.volume_percent = 79U;
  attempted.led_roles_swapped = true;
  attempted.aigc_steps = 42U;

  // Every pre-head single-key boundary leaves the old committed composite.
  rig.extension_journal.fail_slot_before = true;
  assert(!rig.save(attempted, 2U, current).ok());
  rig.extension_journal.fail_slot_before = false;
  assert(rig.load().values == committed);
  assert(rig.main_journal.slot_writes == main_slot_writes &&
         rig.main_journal.head_writes == main_head_writes);

  rig.extension_journal.fail_slot_after = true;
  assert(!rig.save(attempted, 2U, current).ok());
  rig.extension_journal.fail_slot_after = false;
  assert(rig.load().values == committed);

  rig.main_journal.fail_slot_before = true;
  assert(!rig.save(attempted, 2U, current).ok());
  rig.main_journal.fail_slot_before = false;
  assert(rig.load().generation == 2U && rig.load().values == committed);

  rig.main_journal.fail_slot_after = true;
  assert(!rig.save(attempted, 2U, current).ok());
  rig.main_journal.fail_slot_after = false;
  assert(rig.load().generation == 2U && rig.load().values == committed);

  rig.main_journal.fail_head_before = true;
  assert(!rig.save(attempted, 2U, current).ok());
  rig.main_journal.fail_head_before = false;
  assert(rig.load().generation == 2U && rig.load().values == committed);

  // nvs_set(main head) is immediate. A failure before the advisory marker is
  // resolved by authoritative readback, then ext-head can safely publish.
  rig.main_journal.fail_head_after_head = true;
  assert(rig.save(attempted, 2U, current).ok());
  rig.main_journal.fail_head_after_head = false;
  assert(current.generation == 3U && current.values == attempted);

  // A failure before the sole ext-head write leaves a real new main plus the
  // old extension. The operation fails and returns that authoritative partial
  // state instead of falsely claiming requested steps were saved.
  DeviceSettings head_failure = current.values;
  head_failure.volume_percent = 80U;
  head_failure.led_roles_swapped = false;
  head_failure.aigc_steps = 43U;
  const SettingsExtensionValues old_extension =
      settingsExtensionValues(current.values);
  rig.extension_journal.fail_head_before = true;
  assert(!rig.save(head_failure, 3U, current).ok());
  rig.extension_journal.fail_head_before = false;
  assert(current.generation == 4U && current.values.volume_percent == 80U);
  assert(settingsExtensionValues(current.values) == old_extension);
  assert(current.values.aigc_steps != head_failure.aigc_steps);
  assert(rig.load().values == current.values);
  assertMainRawIsBeta27Compatible(rig.main_journal, 4U);

  // An uncertain ext-head write is success only when readback proves that the
  // one selector key actually advanced to the exact staged record.
  DeviceSettings uncertain = current.values;
  uncertain.aigc_steps = 44U;
  rig.extension_journal.fail_head_after = true;
  assert(rig.save(uncertain, 4U, current).ok());
  rig.extension_journal.fail_head_after = false;
  assert(current.generation == 4U && current.values == uncertain);

  // Exact orphan-collision regression: ext seq 11 targets main generation 11,
  // power dies before ext-head, then beta27 performs its own main-only save to
  // generation 11. The orphan has the same main generation but is never used.
  Rig collision;
  SettingsSnapshot collision_state = collision.load();
  for (std::uint8_t step = 0U; step < 10U; ++step) {
    DeviceSettings next = collision_state.values;
    next.aigc_steps = static_cast<std::uint8_t>(21U + step);
    assert(collision.save(next, collision_state.generation,
                          collision_state).ok());
  }
  assert(collision.extension_journal.state.head_sequence == 10U &&
         collision_state.generation == 0U);
  for (std::uint8_t volume = 1U; volume <= 10U; ++volume) {
    DeviceSettings old_writer = collision_state.values;
    old_writer.volume_percent = volume;
    SettingsSnapshot main_committed;
    assert(collision.main.save(beta27MainValues(old_writer),
                               collision_state.generation,
                               main_committed).ok());
    collision_state = collision.load();
  }
  assert(collision_state.generation == 10U);
  const SettingsExtensionValues committed_ten =
      settingsExtensionValues(collision_state.values);
  SettingsExtensionSnapshot orphan_eleven;
  SettingsExtensionValues orphan_values = committed_ten;
  orphan_values.led_roles_swapped = !orphan_values.led_roles_swapped;
  orphan_values.aigc_steps = 49U;
  assert(collision.extension.prepare(
      11U, orphan_values, orphan_eleven).ok());
  assert(orphan_eleven.sequence == 11U &&
         collision.extension_journal.state.head_sequence == 10U);
  DeviceSettings beta27_write = collision_state.values;
  beta27_write.volume_percent = 11U;
  SettingsSnapshot beta27_committed;
  assert(collision.main.save(beta27MainValues(beta27_write), 10U,
                             beta27_committed).ok());
  collision_state = collision.load();
  assert(collision_state.generation == 11U &&
         collision_state.values.volume_percent == 11U &&
         settingsExtensionValues(collision_state.values) == committed_ten &&
         settingsExtensionValues(collision_state.values) != orphan_values);

  // Orphan corruption is ignored, but a present bad head/selected record is a
  // fail-closed error.
  const std::uint8_t orphan_slot =
      static_cast<std::uint8_t>(orphan_eleven.sequence & 1U);
  collision.extension_journal.state.slot[orphan_slot][0] ^= 1U;
  assert(collision.load().generation == 11U);
  collision.extension_journal.state.head_sequence = orphan_eleven.sequence;
  SettingsSnapshot rejected;
  assert(loadRollbackCompatibleSettings(
      collision.main, collision.extension, rejected).code ==
      SettingsError::Corrupt);

  Rig invalid_head;
  invalid_head.extension_journal.state.head_present = true;
  invalid_head.extension_journal.state.head_sequence = 0U;
  assert(loadRollbackCompatibleSettings(
      invalid_head.main, invalid_head.extension, rejected).code ==
      SettingsError::Corrupt);

  // Codec CRC, flags, bounds, and selected-main ordering are fail closed.
  SettingsExtensionSnapshot encoded_snapshot;
  encoded_snapshot.sequence = 7U;
  encoded_snapshot.settings_generation = 3U;
  encoded_snapshot.values.led_roles_swapped = true;
  encoded_snapshot.values.aigc_steps = 35U;
  std::vector<std::uint8_t> encoded;
  assert(encodeSettingsExtensionRecord(encoded_snapshot, encoded).ok());
  SettingsExtensionSnapshot decoded;
  assert(decodeSettingsExtensionRecord(encoded, decoded).ok() &&
         decoded.sequence == 7U && decoded.values == encoded_snapshot.values);
  for (std::size_t at = 0U; at < encoded.size(); ++at) {
    std::vector<std::uint8_t> changed = encoded;
    changed[at] ^= 1U;
    assert(!decodeSettingsExtensionRecord(changed, decoded).ok());
  }

  Rig future;
  future.extension_journal.state.head_present = true;
  future.extension_journal.state.head_sequence = 7U;
  future.extension_journal.state.slot_present[1] = true;
  future.extension_journal.state.slot[1] = encoded;
  assert(loadRollbackCompatibleSettings(
      future.main, future.extension, rejected).code == SettingsError::Corrupt);

  Rig exhausted;
  SettingsExtensionSnapshot maximum;
  maximum.sequence = std::numeric_limits<std::uint32_t>::max();
  maximum.settings_generation = 0U;
  maximum.values = settingsExtensionValues(exhausted.defaults);
  assert(encodeSettingsExtensionRecord(
      maximum, exhausted.extension_journal.state.slot[1]).ok());
  exhausted.extension_journal.state.slot_present[1] = true;
  exhausted.extension_journal.state.head_present = true;
  exhausted.extension_journal.state.head_sequence = maximum.sequence;
  SettingsExtensionSnapshot no_room;
  assert(exhausted.extension.prepare(
      0U, maximum.values, no_room).code == SettingsError::Exhausted);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-settings-extension-"));
  try {
    const source = join(scratch, "extension.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(settings, "include"),
      source,
      join(settings, "device_settings.cpp"),
      join(settings, "settings_journal.cpp"),
      join(settings, "settings_extension_journal.cpp"),
      "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0,
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    }
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

test("settings extension single-selector power-loss matrix passes strict C++17", () => {
  buildAndRun(false);
});

test("settings extension transaction is clean under ASan and UBSan", () => {
  buildAndRun(true);
});

test("beta31 schema-2 main bytes decode with the exact 219d001 beta27 codec", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-beta27-settings-golden-"));
  try {
    const oldRoot = join(scratch, "beta27");
    const oldFiles = [
      "firmware/inkloop-idf/components/inkloop_settings/device_settings.cpp",
      "firmware/inkloop-idf/components/inkloop_settings/settings_journal.cpp",
      "firmware/inkloop-idf/components/inkloop_settings/include/inkloop/settings/device_settings.hpp",
      "firmware/inkloop-idf/components/inkloop_settings/include/inkloop/settings/settings_journal.hpp",
    ];
    for (const path of oldFiles) {
      const relative = path.replace(
        "firmware/inkloop-idf/components/inkloop_settings/", "");
      const destination = join(oldRoot, relative);
      mkdirSync(dirname(destination), { recursive: true });
      writeFileSync(destination, execFileSync(
        "git", ["show", `219d001:${path}`], { encoding: "utf8" }));
    }

    const raw = join(scratch, "main-slot.bin");
    const generator = join(scratch, "generator.cpp");
    writeFileSync(generator, String.raw`
#include <cassert>
#include <cstdint>
#include <fstream>
#include <vector>
#include "inkloop/settings/settings_journal.hpp"
int main(int argc, char** argv) {
  assert(argc == 2);
  inkloop::settings::SettingsSnapshot snapshot;
  snapshot.generation = 11U;
  snapshot.values = inkloop::settings::makeGenericDeviceDefaults();
  snapshot.values.volume_percent = 73U;
  std::vector<std::uint8_t> encoded;
  assert(inkloop::settings::encodeSettingsRecord(snapshot, encoded).ok());
  std::ofstream output(argv[1], std::ios::binary);
  output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  assert(output.good());
}
`);
    const generatorBinary = join(scratch, "generator");
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(settings, "include"), generator,
      join(settings, "device_settings.cpp"),
      join(settings, "settings_journal.cpp"),
      "-o", generatorBinary,
    ], { stdio: "pipe" });
    execFileSync(generatorBinary, [raw], { stdio: "pipe" });

    const decoder = join(scratch, "decoder.cpp");
    writeFileSync(decoder, String.raw`
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>
#include "inkloop/settings/settings_journal.hpp"
int main(int argc, char** argv) {
  assert(argc == 2);
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<std::uint8_t> encoded(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  inkloop::settings::SettingsSnapshot decoded;
  assert(inkloop::settings::decodeSettingsRecord(encoded, decoded).ok());
  assert(decoded.generation == 11U && decoded.values.volume_percent == 73U);
}
`);
    const decoderBinary = join(scratch, "decoder");
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(oldRoot, "include"), decoder,
      join(oldRoot, "device_settings.cpp"),
      join(oldRoot, "settings_journal.cpp"),
      "-o", decoderBinary,
    ], { stdio: "pipe" });
    execFileSync(decoderBinary, [raw], { stdio: "pipe" });

    const bytes = readFileSync(raw);
    assert.equal(bytes[4], 2);
    assert.equal(bytes[5], 0);
    assert.equal(bytes[15], 0);
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});
