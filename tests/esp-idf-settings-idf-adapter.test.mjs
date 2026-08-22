import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const settings = join(repo, "firmware/inkloop-idf/components/inkloop_settings");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_settings_idf");

test("legacy NVS adapter has a mechanically read-only surface", () => {
  const source = readFileSync(join(native, "esp_nvs_settings_store.cpp"), "utf8");
  assert.match(source, /kSettingsNamespace\[\] = "ink-settings-v1"/);
  assert.match(source, /kLegacyNamespace\[\] = "ink-portal"/);
  assert.match(source, /kLegacySlotAKey\[\] = "snap-a"/);
  assert.match(source, /kLegacySlotBKey\[\] = "snap-b"/);
  const start = source.indexOf(
    "SettingsStatus EspNvsReadOnlyLegacyPortalSource::inspect");
  const end = source.indexOf(
    "bool EspPsaLegacySha256Verifier::matches", start);
  assert.ok(start >= 0 && end > start);
  const legacyBody = source.slice(start, end);
  assert.match(legacyBody, /nvs_open\(kLegacyNamespace, NVS_READONLY/);
  assert.doesNotMatch(legacyBody, /NVS_READWRITE/);
  assert.doesNotMatch(legacyBody, /nvs_set_|nvs_erase|nvs_commit/);
  assert.match(source, /writeSlotAndCommit[\s\S]*nvs_set_blob[\s\S]*nvs_commit/);
  assert.match(source,
    /writeHeadAndMarkerAndCommit[\s\S]*nvs_set_u32[\s\S]*nvs_set_u8[\s\S]*nvs_commit/);
});

test("native NVS and PSA adapters compile and honor their ownership contract", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-settings-idf-"));
  try {
    const include = join(scratch, "include");
    mkdirSync(join(include, "psa"), { recursive: true });
    writeFileSync(join(include, "nvs.h"), String.raw`
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READONLY 2
#define NVS_READWRITE 3
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
#ifdef __cplusplus
}
#endif
`);
    writeFileSync(join(include, "psa/crypto.h"), String.raw`
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009u
#ifdef __cplusplus
extern "C" {
#endif
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(psa_algorithm_t, const uint8_t*, size_t,
                              uint8_t*, size_t, size_t*);
#ifdef __cplusplus
}
#endif
`);
    const harness = String.raw`
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "inkloop/settings/esp_nvs_settings_store.hpp"
#include "nvs.h"
#include "psa/crypto.h"

using namespace inkloop::settings;

static bool missing = true;
static std::string opened_namespace;
static int opened_mode = 0;
static unsigned blob_writes = 0;
static unsigned head_writes = 0;
static unsigned marker_writes = 0;
static unsigned commits = 0;

extern "C" esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
  opened_namespace = name ? name : "";
  opened_mode = mode;
  if (handle) *handle = 1;
  return missing ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t) {}
extern "C" esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}
extern "C" esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}
extern "C" esp_err_t nvs_get_blob(
    nvs_handle_t, const char*, void*, size_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}
extern "C" esp_err_t nvs_get_str(
    nvs_handle_t, const char*, char*, size_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}
extern "C" esp_err_t nvs_set_blob(
    nvs_handle_t, const char*, const void*, size_t size) {
  assert(size > 0); ++blob_writes; return ESP_OK;
}
extern "C" esp_err_t nvs_set_u32(
    nvs_handle_t, const char*, uint32_t value) {
  assert(value > 0); ++head_writes; return ESP_OK;
}
extern "C" esp_err_t nvs_set_u8(
    nvs_handle_t, const char*, uint8_t value) {
  assert(value == kSettingsInitializedMarker); ++marker_writes; return ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t) {
  ++commits; return ESP_OK;
}
extern "C" psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
extern "C" psa_status_t psa_hash_compute(
    psa_algorithm_t algorithm, const uint8_t* input, size_t length,
    uint8_t* output, size_t capacity, size_t* written) {
  assert(algorithm == PSA_ALG_SHA_256);
  assert(input && length == 7 && std::memcmp(input, "payload", 7) == 0);
  assert(output && capacity >= 32 && written);
  for (size_t at = 0; at < 32; ++at) output[at] = static_cast<uint8_t>(at);
  *written = 32;
  return PSA_SUCCESS;
}

int main() {
  EspNvsReadOnlyLegacyPortalSource legacy;
  LegacyPortalJournalState legacy_state;
  assert(legacy.inspect(legacy_state).ok());
  assert(legacy_state.namespace_available);
  assert(opened_namespace == "ink-portal" && opened_mode == NVS_READONLY);
  assert(blob_writes == 0 && head_writes == 0 && marker_writes == 0 &&
         commits == 0);

  EspNvsSettingsJournalStore journal;
  SettingsJournalState state;
  assert(journal.inspect(state).ok());
  assert(state.namespace_available);
  assert(opened_namespace == "ink-settings-v1" &&
         opened_mode == NVS_READONLY);

  missing = false;
  std::vector<std::uint8_t> record{1, 2, 3};
  assert(journal.writeSlotAndCommit(0, record).ok());
  assert(opened_mode == NVS_READWRITE && blob_writes == 1 && commits == 1);
  assert(journal.writeHeadAndMarkerAndCommit(9).ok());
  assert(head_writes == 1 && marker_writes == 1 && commits == 2);
  assert(journal.writeSlotAndCommit(2, record).code ==
         SettingsError::InvalidArgument);
  assert(journal.writeHeadAndMarkerAndCommit(0).code ==
         SettingsError::InvalidArgument);

  EspPsaLegacySha256Verifier verifier;
  const std::string expected =
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f";
  assert(verifier.matches("payload", expected));
  assert(!verifier.matches("payload", std::string(64, '0')));
  assert(!verifier.matches("payload", "BAD"));
  return 0;
}
`;
    const source = join(scratch, "adapter.cpp");
    const binary = join(scratch, "adapter");
    writeFileSync(source, harness);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", include,
      "-I", join(settings, "include"),
      "-I", join(native, "include"),
      source,
      join(settings, "device_settings.cpp"),
      join(settings, "settings_journal.cpp"),
      join(settings, "legacy_portal_import.cpp"),
      join(native, "esp_nvs_settings_store.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], {
      env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" },
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

