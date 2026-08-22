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
#include <string>

#include "inkloop/settings/device_settings.hpp"
#include "inkloop/settings/legacy_portal_import.hpp"

using namespace inkloop::settings;

std::string testChecksum(const std::string& payload) {
  std::uint32_t value = 2166136261U;
  for (unsigned char ch : payload) value = (value ^ ch) * 16777619U;
  static const char hex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (std::size_t at = 0; at < result.size(); ++at) {
    result[at] = hex[(value >> ((at & 7U) * 4U)) & 15U];
    value = value * 1103515245U + 12345U;
  }
  return result;
}

std::string jsonString(const std::string& value) {
  static const char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (unsigned char ch : value) {
    if (ch == '\"') output += "\\\"";
    else if (ch == '\\') output += "\\\\";
    else if (ch == '\n') output += "\\n";
    else if (ch == '\r') output += "\\r";
    else if (ch == '\t') output += "\\t";
    else if (ch < 0x20U) {
      output += "\\u00";
      output += hex[ch >> 4U];
      output += hex[ch & 15U];
    } else output.push_back(static_cast<char>(ch));
  }
  output += "\"";
  return output;
}

std::string envelope(const std::string& payload, bool valid = true) {
  std::string checksum = testChecksum(payload);
  if (!valid) checksum[0] = checksum[0] == '0' ? '1' : '0';
  return "{\"payload\":" + jsonString(payload) +
      ",\"sha256\":\"" + checksum + "\"}";
}

std::string payload(
    unsigned schema = 2, unsigned storage = 2, unsigned volume = 77,
    const std::string& prompt = "你好 Inkloop", bool include_image = true,
    unsigned refresh = 3, unsigned brightness = 0,
    const std::string& negative = "水印") {
  std::string result = "{\"schema\":" + std::to_string(schema) +
      ",\"fields\":1048575,\"revision\":18446744073709551615,";
  result += "\"onboarding\":{\"unknown\":[1,true,null,{\"x\":\"y\"}]},";
  result += "\"settings\":{\"storage\":" + std::to_string(storage) +
      ",\"volume\":" + std::to_string(volume) +
      ",\"voice_assistance\":false,\"prompt\":" + jsonString(prompt);
  if (include_image)
    result += ",\"image_prompt\":" + jsonString("鲜艳 {prompt}");
  result += ",\"local_password\":\"not-imported\",\"width\":400,"
      "\"height\":600,\"steps\":20,\"negative\":" +
      jsonString(negative) +
      ",\"led_swap\":true,\"led_brightness\":" +
      std::to_string(brightness) + ",\"refresh\":" +
      std::to_string(refresh) +
      ",\"power\":1,\"idle\":120}}";
  return result;
}

struct Verifier final : ILegacySha256Verifier {
  mutable unsigned calls = 0;
  bool matches(const std::string& payload,
               const std::string& expected) const override {
    ++calls;
    return testChecksum(payload) == expected;
  }
};

struct Source final : IReadOnlyLegacyPortalSource {
  mutable unsigned calls = 0;
  SettingsStatus result = SettingsStatus::success();
  LegacyPortalJournalState state;

  Source() { state.namespace_available = true; }
  SettingsStatus inspect(LegacyPortalJournalState& output) const override {
    ++calls;
    if (!result.ok()) return result;
    output = state;
    return SettingsStatus::success();
  }
};

Source populated(const std::string& record) {
  Source source;
  source.state.marker_present = true;
  source.state.marker_valid = true;
  source.state.head_present = true;
  source.state.head = 1;
  source.state.slot_present[0] = true;
  source.state.slot[0] = record;
  return source;
}

int main() {
  DeviceSettings defaults = makeGenericDeviceDefaults();
  Verifier verifier;
  LegacySettingsImport imported;

  Source absent;
  assert(inspectLegacyPortalSettings(
      absent, verifier, defaults, imported).ok());
  assert(absent.calls == 1 && verifier.calls == 0);
  assert(imported.state == LegacyImportState::Absent);
  assert(imported.values == defaults);

  const std::string valid_payload = payload();
  Source source = populated(envelope(valid_payload));
  assert(inspectLegacyPortalSettings(
      source, verifier, defaults, imported).ok());
  assert(source.calls == 1 && imported.state == LegacyImportState::Candidate);
  assert(imported.source_schema == 2);
  assert(imported.source_revision == UINT64_MAX);
  assert(!imported.used_fallback_slot);
  assert(imported.values.volume_percent == 77);
  assert(imported.values.led_maximum_brightness_percent == 0);
  assert(!imported.values.voice_assistance_enabled);
  assert(imported.values.assistant_prompt == "你好 Inkloop");
  assert(imported.values.aigc_prompt_template == "鲜艳 {prompt}");
  assert(imported.values.negative_prompt == "水印");
  assert(imported.values.asset_storage_preference ==
         AssetStoragePreference::Removable);
  assert(imported.values.default_render_strategy == "solid-clean");

  // Released schema 1 lacked an image prompt. Candidate decoding preserves
  // the caller's reviewed default instead of synthesizing a SKU assumption.
  Source schema_one = populated(envelope(payload(1, 0, 0, "agent", false, 0,
                                                 100, "")));
  assert(inspectLegacyPortalSettings(
      schema_one, verifier, defaults, imported).ok());
  assert(imported.source_schema == 1);
  assert(imported.values.aigc_prompt_template ==
         defaults.aigc_prompt_template);
  assert(imported.values.negative_prompt.empty());
  assert(imported.values.volume_percent == 0);
  assert(imported.values.led_maximum_brightness_percent == 100);
  assert(imported.values.asset_storage_preference ==
         AssetStoragePreference::Automatic);
  assert(imported.values.default_render_strategy == "official-quality");

  // The Arduino loader accepts a checksum-valid alternate slot if its selected
  // slot is torn. We surface that fact for explicit migration review.
  Source fallback = populated(envelope(valid_payload, false));
  fallback.state.slot_present[1] = true;
  fallback.state.slot[1] = envelope(payload(2, 1, 100, "fallback"));
  assert(inspectLegacyPortalSettings(
      fallback, verifier, defaults, imported).ok());
  assert(imported.used_fallback_slot);
  assert(imported.values.volume_percent == 100);
  assert(imported.values.asset_storage_preference ==
         AssetStoragePreference::Internal);

  // Marker-less records existed before the marker migration and remain a
  // read-only candidate. A present-but-invalid marker never falls back.
  Source markerless = populated(envelope(valid_payload));
  markerless.state.marker_present = false;
  markerless.state.marker_valid = false;
  assert(inspectLegacyPortalSettings(
      markerless, verifier, defaults, imported).ok());
  Source bad_marker = markerless;
  bad_marker.state.marker_present = true;
  assert(inspectLegacyPortalSettings(
      bad_marker, verifier, defaults, imported).code == SettingsError::Corrupt);

  Source no_head = populated(envelope(valid_payload));
  no_head.state.head_present = false;
  assert(inspectLegacyPortalSettings(
      no_head, verifier, defaults, imported).code == SettingsError::Corrupt);
  Source bad_head = populated(envelope(valid_payload));
  bad_head.state.head = 3;
  assert(inspectLegacyPortalSettings(
      bad_head, verifier, defaults, imported).code == SettingsError::Corrupt);
  Source bad_checksum = populated(envelope(valid_payload, false));
  assert(inspectLegacyPortalSettings(
      bad_checksum, verifier, defaults, imported).code ==
         SettingsError::Corrupt);
  Source too_large = populated(std::string(
      kMaximumLegacyPortalRecordBytes + 1, 'x'));
  assert(inspectLegacyPortalSettings(
      too_large, verifier, defaults, imported).code == SettingsError::Corrupt);

  Source empty_prompt = populated(envelope(payload(2, 0, 1, "")));
  assert(inspectLegacyPortalSettings(
      empty_prompt, verifier, defaults, imported).code ==
         SettingsError::Corrupt);
  Source bad_schema = populated(envelope(payload(3)));
  assert(inspectLegacyPortalSettings(
      bad_schema, verifier, defaults, imported).code == SettingsError::Corrupt);
  Source long_prompt = populated(envelope(payload(
      2, 0, 1, std::string(kMaximumAssistantPromptBytes + 1, 'x'))));
  assert(inspectLegacyPortalSettings(
      long_prompt, verifier, defaults, imported).code ==
         SettingsError::Corrupt);

  // A lone UTF-16 surrogate cannot enter the persisted UTF-8 model.
  const std::string invalid_unicode =
      "{\"schema\":2,\"revision\":1,\"settings\":{"
      "\"storage\":0,\"volume\":60,\"prompt\":\"\\ud800\","
      "\"image_prompt\":\"image\",\"negative\":\"\","
      "\"refresh\":0}}";
  Source unicode = populated(envelope(invalid_unicode));
  assert(inspectLegacyPortalSettings(
      unicode, verifier, defaults, imported).code == SettingsError::Corrupt);

  Source unavailable;
  unavailable.state.namespace_available = false;
  assert(inspectLegacyPortalSettings(
      unavailable, verifier, defaults, imported).code ==
         SettingsError::Storage);
  Source failed;
  failed.result = {SettingsError::Storage, "nvs"};
  assert(inspectLegacyPortalSettings(
      failed, verifier, defaults, imported).code == SettingsError::Storage);

  DeviceSettings invalid_defaults = defaults;
  invalid_defaults.aigc_prompt_template.clear();
  assert(inspectLegacyPortalSettings(
      absent, verifier, invalid_defaults, imported).code ==
         SettingsError::InvalidArgument);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-settings-legacy-"));
  try {
    const source = join(scratch, "harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(settings, "include"),
      source,
      join(settings, "device_settings.cpp"),
      join(settings, "legacy_portal_import.cpp"),
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

test("legacy Arduino settings import is strict and read-only by API", () => {
  buildAndRun(false);
});

test("legacy importer survives corrupt records under ASan/UBSan", () => {
  buildAndRun(true);
});
