import { execFileSync } from "node:child_process";
import {
  mkdirSync, mkdtempSync, rmSync, writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");

const nvsStub = String.raw`
#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr std::size_t NVS_KEY_NAME_MAX_SIZE = 16;
constexpr std::size_t NVS_NS_NAME_MAX_SIZE = 16;
using nvs_handle_t = std::uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
enum nvs_type_t {
  NVS_TYPE_U8 = 0x01, NVS_TYPE_I8 = 0x11,
  NVS_TYPE_U16 = 0x02, NVS_TYPE_I16 = 0x12,
  NVS_TYPE_U32 = 0x04, NVS_TYPE_I32 = 0x14,
  NVS_TYPE_U64 = 0x08, NVS_TYPE_I64 = 0x18,
  NVS_TYPE_STR = 0x21, NVS_TYPE_BLOB = 0x42, NVS_TYPE_ANY = 0xff,
};
struct nvs_entry_info_t {
  char namespace_name[NVS_NS_NAME_MAX_SIZE];
  char key[NVS_KEY_NAME_MAX_SIZE];
  nvs_type_t type;
};
struct nvs_opaque_iterator_t { std::size_t at; };
using nvs_iterator_t = nvs_opaque_iterator_t*;

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_entry_find_in_handle(nvs_handle_t, nvs_type_t, nvs_iterator_t*);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*);
esp_err_t nvs_entry_next(nvs_iterator_t*);
void nvs_release_iterator(nvs_iterator_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, std::uint8_t*);
esp_err_t nvs_get_i8(nvs_handle_t, const char*, std::int8_t*);
esp_err_t nvs_get_u16(nvs_handle_t, const char*, std::uint16_t*);
esp_err_t nvs_get_i16(nvs_handle_t, const char*, std::int16_t*);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, std::uint32_t*);
esp_err_t nvs_get_i32(nvs_handle_t, const char*, std::int32_t*);
esp_err_t nvs_get_u64(nvs_handle_t, const char*, std::uint64_t*);
esp_err_t nvs_get_i64(nvs_handle_t, const char*, std::int64_t*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, std::size_t*);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*);
`;

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "inkloop/storage/esp_upgrade_snapshot_source.hpp"
#include "nvs.h"

using namespace inkloop::storage;

namespace {
struct FakeEntry { const char* key; nvs_type_t type; };
constexpr std::array<FakeEntry, 3> kEntries{{
    {"z", NVS_TYPE_U8}, {"a", NVS_TYPE_STR}, {"m", NVS_TYPE_I16}}};
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                   nvs_handle_t* handle) {
  assert(mode == NVS_READONLY);
  if (std::strcmp(name, "inkloop-v2") != 0) return ESP_ERR_NVS_NOT_FOUND;
  *handle = 1U;
  return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1U); }
esp_err_t nvs_entry_find_in_handle(nvs_handle_t handle, nvs_type_t type,
                                   nvs_iterator_t* output) {
  assert(handle == 1U && type == NVS_TYPE_ANY);
  *output = new nvs_opaque_iterator_t{0U};
  return ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t* info) {
  if (!iterator || iterator->at >= kEntries.size()) return ESP_FAIL;
  std::memset(info, 0, sizeof(*info));
  std::strcpy(info->namespace_name, "inkloop-v2");
  std::strcpy(info->key, kEntries[iterator->at].key);
  info->type = kEntries[iterator->at].type;
  return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* iterator) {
  if (!iterator || !*iterator) return ESP_ERR_NVS_NOT_FOUND;
  if (++(*iterator)->at < kEntries.size()) return ESP_OK;
  delete *iterator;
  *iterator = nullptr;
  return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_release_iterator(nvs_iterator_t iterator) { delete iterator; }

esp_err_t nvs_get_u8(nvs_handle_t, const char* key, std::uint8_t* output) {
  if (std::strcmp(key, "z") != 0) return ESP_ERR_NVS_NOT_FOUND;
  *output = 7U;
  return ESP_OK;
}
esp_err_t nvs_get_i16(nvs_handle_t, const char* key, std::int16_t* output) {
  if (std::strcmp(key, "m") != 0) return ESP_ERR_NVS_NOT_FOUND;
  *output = -2;
  return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* output,
                      std::size_t* length) {
  if (std::strcmp(key, "a") != 0) return ESP_ERR_NVS_NOT_FOUND;
  if (!output) { *length = 3U; return ESP_OK; }
  if (*length < 3U) return ESP_FAIL;
  std::memcpy(output, "hi", 3U);
  *length = 3U;
  return ESP_OK;
}
#define MISSING_GET(name, type) \
  esp_err_t name(nvs_handle_t, const char*, type*) { \
    return ESP_ERR_NVS_NOT_FOUND; \
  }
MISSING_GET(nvs_get_i8, std::int8_t)
MISSING_GET(nvs_get_u16, std::uint16_t)
MISSING_GET(nvs_get_u32, std::uint32_t)
MISSING_GET(nvs_get_i32, std::int32_t)
MISSING_GET(nvs_get_u64, std::uint64_t)
MISSING_GET(nvs_get_i64, std::int64_t)
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}

namespace inkloop::storage {
std::array<RecordProbe, kProtectedNvsNamespaces.size()>
EspNvsUpgradeInventory::inspect() const {
  std::array<RecordProbe, kProtectedNvsNamespaces.size()> result{};
  result.fill(RecordProbe::Missing);
  result[0] = RecordProbe::Valid;
  return result;
}
PosixUpgradeInventory::PosixUpgradeInventory(std::string root)
    : internal_root_(std::move(root)), paths_valid_(true) {}
std::array<RecordProbe, kProtectedFilePaths.size()>
PosixUpgradeInventory::inspectFiles() const {
  std::array<RecordProbe, kProtectedFilePaths.size()> result{};
  result.fill(RecordProbe::Missing);
  return result;
}
UpgradeAuditInput PosixUpgradeInventory::inspect(
    const std::array<RecordProbe, kProtectedNvsNamespaces.size()>&) const {
  return {};
}
bool upgradeRecordIdValid(UpgradeRecordId record) {
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? record.index < kProtectedNvsNamespaces.size()
      : record.index < kProtectedFilePaths.size();
}
std::uint64_t upgradeRecordMaximumBytes(UpgradeRecordId record) {
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? kMaximumUpgradeNvsNamespaceBytes : 512U * 1024U;
}
const char* upgradeRecordName(UpgradeRecordId record) {
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? kProtectedNvsNamespaces[record.index] : kProtectedFilePaths[record.index];
}
}

class Metadata final : public IUpgradeSnapshotMetadataProvider {
 public:
  bool inspectUpgradeSnapshotMetadata(
      UpgradeSnapshotMetadata& output) const override {
    output.internal_mounted = true;
    output.source_layout = UpgradeSourceLayout::Legacy;
    output.source_layout_schema_version = 1U;
    output.legacy_source_durable = true;
    return true;
  }
};

class Sink final : public IUpgradeByteSink {
 public:
  bool write(const std::uint8_t* bytes, std::size_t length) override {
    if (!bytes && length != 0U) return false;
    value.insert(value.end(), bytes, bytes + length);
    return true;
  }
  std::vector<std::uint8_t> value;
};

int main() {
  Metadata metadata;
  EspUpgradeSnapshotSource source("/littlefs", metadata);
  UpgradeSnapshotMetadata snapshot;
  assert(source.ready() && source.inspectMetadata(snapshot));
  Sink sink;
  assert(source.streamRecord(
      {UpgradeRecordDomain::NvsNamespace, 0U},
      kMaximumUpgradeNvsNamespaceBytes, sink) ==
      UpgradeRecordStreamCode::Valid);
  const std::vector<std::uint8_t> expected{
      'I','N','K','N','V','S','1',0,
      10,'i','n','k','l','o','o','p','-','v','2',3,0,
      1,'a',0x21,2,0,0,0,'h','i',
      1,'m',0x12,2,0,0,0,0xfe,0xff,
      1,'z',0x01,1,0,0,0,7};
  assert(sink.value == expected);

  assert(source.inspectMetadata(snapshot));
  Sink too_small;
  assert(source.streamRecord(
      {UpgradeRecordDomain::NvsNamespace, 0U}, 8U, too_small) ==
      UpgradeRecordStreamCode::TooLarge);
  Sink missing;
  assert(source.streamRecord(
      {UpgradeRecordDomain::NvsNamespace, 1U},
      kMaximumUpgradeNvsNamespaceBytes, missing) ==
      UpgradeRecordStreamCode::Missing);
  assert(missing.value.empty());
  return 0;
}
`;

function write(root, relative, data) {
  const path = join(root, relative);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, data);
}

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-source-"));
  try {
    write(scratch, "stubs/nvs.h", nvsStub);
    write(scratch, "source.cpp", harness);
    const binary = join(scratch, sanitized ? "asan" : "strict");
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(scratch, "stubs"), "-I", join(storage, "include"),
      join(scratch, "source.cpp"),
      join(storage, "esp_upgrade_snapshot_source.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
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

test("native NVS snapshot stream is deterministic under strict C++17", () => {
  run(false);
});

test("native NVS snapshot stream is memory-safe under ASan/UBSan", () => {
  run(true);
});
