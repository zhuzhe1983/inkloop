import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const settings = join(repo, "firmware/inkloop-idf/components/inkloop_settings");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "inkloop/settings/device_settings.hpp"
#include "inkloop/settings/settings_journal.hpp"

using namespace inkloop::settings;

struct Journal final : ISettingsJournalStore {
  SettingsJournalState state;
  bool fail_inspect = false;
  bool fail_slot_before_write = false;
  bool fail_slot_after_write = false;
  bool fail_head_before_write = false;
  bool fail_head_after_write = false;
  unsigned slot_writes = 0;
  unsigned head_writes = 0;

  Journal() { state.namespace_available = true; }

  SettingsStatus inspect(SettingsJournalState& output) override {
    if (fail_inspect) return {SettingsError::Storage, "inspect"};
    output = state;
    return SettingsStatus::success();
  }
  SettingsStatus writeSlotAndCommit(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) override {
    ++slot_writes;
    if (fail_slot_before_write) return {SettingsError::Storage, "slot"};
    state.slot_present[slot] = true;
    state.slot[slot] = encoded;
    return fail_slot_after_write
        ? SettingsStatus{SettingsError::Storage, "uncertain slot"}
        : SettingsStatus::success();
  }
  SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) override {
    ++head_writes;
    if (fail_head_before_write) return {SettingsError::Storage, "head"};
    state.head_present = true;
    state.head_generation = generation;
    state.marker_present = true;
    state.marker_valid = true;
    return fail_head_after_write
        ? SettingsStatus{SettingsError::Storage, "uncertain head"}
        : SettingsStatus::success();
  }
};

int main() {
  DeviceSettings generic = makeGenericDeviceDefaults();
  assert(validDeviceSettings(generic));
  assert(!generic.assistant_prompt.empty() &&
         !generic.aigc_prompt_template.empty());

  DeviceSettings paper;
  assert(makePaperColorDefaults(
      400, 600, "Spectra 6 六色电子纸", paper).ok());
  assert(validDeviceSettings(paper));
  assert(paper.assistant_prompt.find("400x600") != std::string::npos);
  assert(!makePaperColorDefaults(0, 600, "panel", paper).ok());
  assert(!makePaperColorDefaults(400, 600, "", paper).ok());

  DeviceSettings bounded = generic;
  bounded.assistant_prompt.assign(kMaximumAssistantPromptBytes, 'a');
  bounded.aigc_prompt_template.assign(kMaximumAigcPromptTemplateBytes, 'b');
  bounded.negative_prompt.assign(kMaximumNegativePromptBytes, 'c');
  bounded.local_management_password_override.assign(
      kMaximumLocalManagementPasswordBytes, 'p');
  assert(validDeviceSettings(bounded));
  bounded.assistant_prompt.push_back('x');
  assert(!validDeviceSettings(bounded));
  bounded = generic;
  bounded.aigc_prompt_template.assign(kMaximumAigcPromptTemplateBytes + 1,
                                      'x');
  assert(!validDeviceSettings(bounded));
  bounded = generic;
  bounded.negative_prompt.assign(kMaximumNegativePromptBytes + 1, 'x');
  assert(!validDeviceSettings(bounded));
  bounded = generic;
  bounded.aigc_prompt_template.clear();
  assert(!validDeviceSettings(bounded));
  bounded = generic;
  bounded.assistant_prompt.clear();
  assert(!validDeviceSettings(bounded));
  bounded = generic;
  bounded.negative_prompt.clear();
  assert(validDeviceSettings(bounded));
  bounded.local_management_password_override = "short";
  assert(!validDeviceSettings(bounded));
  bounded.local_management_password_override = "valid passphrase 123";
  assert(validDeviceSettings(bounded));
  bounded.local_management_password_override.assign(64, 'p');
  assert(!validDeviceSettings(bounded));

  assert(validUtf8Text("中文\ntext", 64, false));
  assert(!validUtf8Text(std::string("\xc0\x80", 2), 64, false));
  assert(!validUtf8Text(std::string("\xed\xa0\x80", 3), 64, false));
  assert(!validUtf8Text(std::string("\xf4\x90\x80\x80", 4), 64, false));
  assert(!validUtf8Text(std::string("a\0b", 3), 64, false));
  assert(!validUtf8Text(std::string("a\x01", 2), 64, false));
  assert(validRenderStrategyId("vendor.photo-v2"));
  assert(!validRenderStrategyId("Official Quality"));
  assert(!validRenderStrategyId("../official"));

  DeviceSettings invalid = generic;
  invalid.volume_percent = 101;
  assert(!validDeviceSettings(invalid));
  invalid = generic;
  invalid.led_maximum_brightness_percent = 101;
  assert(!validDeviceSettings(invalid));
  invalid = generic;
  invalid.asset_storage_preference =
      static_cast<AssetStoragePreference>(255);
  assert(!validDeviceSettings(invalid));
  invalid = generic;
  invalid.volume_percent = 0;
  invalid.led_maximum_brightness_percent = 0;
  assert(validDeviceSettings(invalid));

  SettingsSnapshot encoded_input;
  encoded_input.generation = 77;
  encoded_input.values = paper;
  encoded_input.values.volume_percent = 0;
  encoded_input.values.led_maximum_brightness_percent = 100;
  encoded_input.values.voice_assistance_enabled = false;
  encoded_input.values.asset_storage_preference =
      AssetStoragePreference::Removable;
  encoded_input.values.default_render_strategy = "solid-clean";
  encoded_input.values.local_management_password_override = "portal pass 42";
  std::vector<std::uint8_t> encoded;
  assert(encodeSettingsRecord(encoded_input, encoded).ok());
  assert(encoded.size() <= kMaximumSettingsRecordBytes);
  SettingsSnapshot decoded;
  assert(decodeSettingsRecord(encoded, decoded).ok());
  assert(decoded.generation == encoded_input.generation);
  assert(decoded.values == encoded_input.values);

  // Every single-bit mutation is caught by header/value validation or CRC32.
  for (std::size_t at = 0; at < encoded.size(); ++at) {
    std::vector<std::uint8_t> tampered = encoded;
    tampered[at] ^= 1U;
    assert(!decodeSettingsRecord(tampered, decoded).ok());
  }
  std::vector<std::uint8_t> truncated(encoded.begin(), encoded.end() - 1);
  assert(!decodeSettingsRecord(truncated, decoded).ok());
  std::vector<std::uint8_t> oversized(kMaximumSettingsRecordBytes + 1, 0);
  assert(!decodeSettingsRecord(oversized, decoded).ok());

  Journal journal;
  SettingsStoreCore store(journal, generic);
  SettingsSnapshot snapshot;
  assert(store.load(snapshot).ok());
  assert(snapshot.generation == 0 && snapshot.values == generic);

  DeviceSettings first = generic;
  first.volume_percent = 0;
  first.led_maximum_brightness_percent = 100;
  assert(store.save(first, 0, snapshot).ok());
  assert(snapshot.generation == 1 && snapshot.values == first);
  assert(journal.state.head_generation == 1 &&
         journal.state.slot_present[1]);
  assert(store.load(snapshot).ok() && snapshot.values == first);
  assert(store.save(first, 0, snapshot).code == SettingsError::Conflict);

  DeviceSettings second = first;
  second.volume_percent = 100;
  second.voice_assistance_enabled = false;
  assert(store.save(second, 1, snapshot).ok());
  assert(snapshot.generation == 2 && journal.state.slot_present[0]);

  // Slot failure before write does not alter the committed generation.
  journal.fail_slot_before_write = true;
  assert(store.save(first, 2, snapshot).code == SettingsError::Storage);
  journal.fail_slot_before_write = false;
  assert(store.load(snapshot).ok() && snapshot.generation == 2 &&
         snapshot.values == second);

  // A slot written with an error is an orphan and is never promoted.
  journal.fail_slot_after_write = true;
  assert(store.save(first, 2, snapshot).code == SettingsError::Storage);
  journal.fail_slot_after_write = false;
  assert(store.load(snapshot).ok() && snapshot.generation == 2 &&
         snapshot.values == second);

  // Head failure preserves generation 2; retry can replace the orphan slot.
  journal.fail_head_before_write = true;
  assert(store.save(first, 2, snapshot).code == SettingsError::Storage);
  journal.fail_head_before_write = false;
  assert(store.load(snapshot).ok() && snapshot.generation == 2);

  // An adapter can report an uncertain commit even if NVS made it durable.
  // Post-write authoritative inspection converts that into success.
  journal.fail_head_after_write = true;
  assert(store.save(first, 2, snapshot).ok());
  journal.fail_head_after_write = false;
  assert(snapshot.generation == 3 && snapshot.values == first);

  // A first-save orphan has no committed head and loads defaults, never the
  // orphan. A retry overwrites it and publishes only after head commit.
  Journal first_boot;
  SettingsStoreCore first_boot_store(first_boot, generic);
  first_boot.fail_slot_after_write = true;
  assert(first_boot_store.save(first, 0, snapshot).code ==
         SettingsError::Storage);
  assert(first_boot_store.load(snapshot).ok() && snapshot.generation == 0 &&
         snapshot.values == generic);
  first_boot.fail_slot_after_write = false;
  assert(first_boot_store.save(first, 0, snapshot).ok());

  // Corrupt committed state fails closed. An unselected orphan may be corrupt
  // without taking down a valid committed head.
  std::vector<std::uint8_t> good = journal.state.slot[1];
  journal.state.slot[1][0] ^= 1U;
  assert(store.load(snapshot).code == SettingsError::Corrupt);
  journal.state.slot[1] = good;
  journal.state.slot[0][0] ^= 1U;
  assert(store.load(snapshot).ok() && snapshot.generation == 3);
  // A valid head can outlive the advisory marker if power fails between the
  // two metadata writes. The selected record remains fully verified.
  journal.state.marker_present = false;
  journal.state.marker_valid = false;
  assert(store.load(snapshot).ok() && snapshot.generation == 3);
  journal.state.marker_present = true;
  journal.state.marker_valid = false;
  assert(store.load(snapshot).code == SettingsError::Corrupt);
  journal.state.marker_valid = true;
  journal.state.head_present = false;
  assert(store.load(snapshot).code == SettingsError::Corrupt);
  journal.state.head_present = true;
  journal.state.namespace_available = false;
  assert(store.load(snapshot).code == SettingsError::Storage);
  journal.state.namespace_available = true;

  Journal invalid_defaults_journal;
  DeviceSettings bad_defaults = generic;
  bad_defaults.assistant_prompt.clear();
  SettingsStoreCore invalid_defaults(invalid_defaults_journal, bad_defaults);
  assert(invalid_defaults.load(snapshot).code == SettingsError::InvalidState);

  // A maximum committed generation remains readable but cannot wrap.
  Journal exhausted;
  SettingsSnapshot maximum;
  maximum.generation = std::numeric_limits<std::uint32_t>::max();
  maximum.values = generic;
  assert(encodeSettingsRecord(maximum, exhausted.state.slot[1]).ok());
  exhausted.state.slot_present[1] = true;
  exhausted.state.marker_present = true;
  exhausted.state.marker_valid = true;
  exhausted.state.head_present = true;
  exhausted.state.head_generation = maximum.generation;
  SettingsStoreCore exhausted_store(exhausted, generic);
  assert(exhausted_store.load(snapshot).ok());
  assert(exhausted_store.save(generic, maximum.generation, snapshot).code ==
         SettingsError::Exhausted);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-settings-journal-"));
  try {
    const source = join(scratch, "harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(settings, "include"),
      source,
      join(settings, "device_settings.cpp"),
      join(settings, "settings_journal.cpp"),
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

test("settings journal and bounds pass strict C++17", () => {
  buildAndRun(false);
});

test("settings journal survives adversarial transitions under ASan/UBSan", () => {
  buildAndRun(true);
});
