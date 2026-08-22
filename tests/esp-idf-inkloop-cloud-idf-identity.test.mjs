import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const adapter = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_cloud_idf",
);
const cloud = join(repo, "firmware/inkloop-idf/components/inkloop_cloud");
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/cloud/arduino_compatible_identity_store.hpp"

using namespace inkloop::cloud;

struct Nvs final : IArduinoInkloopIdentityNvs {
  ArduinoInkloopIdentityRecord durable;
  bool fail_read = false;
  bool fail_fresh = false;
  bool fail_device = false;
  bool fail_revision = false;
  unsigned fresh_commits = 0;
  unsigned device_commits = 0;
  unsigned revision_commits = 0;

  InkloopCloudStatus read(ArduinoInkloopIdentityRecord& output) override {
    if (fail_read) return {InkloopCloudCode::Storage, 0, 0, "read"};
    output = durable;
    return InkloopCloudStatus::success();
  }
  InkloopCloudStatus commitFreshIdentity(const std::string& secret) override {
    ++fresh_commits;
    if (fail_fresh) return {InkloopCloudCode::Storage, 0, 0, "fresh"};
    durable.device_id_present = true;
    durable.device_id.clear();
    durable.secret_present = true;
    durable.secret = secret;
    durable.revision_present = true;
    durable.revision = 0;
    return InkloopCloudStatus::success();
  }
  InkloopCloudStatus commitDeviceId(const std::string& value) override {
    ++device_commits;
    if (fail_device) return {InkloopCloudCode::Storage, 0, 0, "device"};
    durable.device_id_present = true;
    durable.device_id = value;
    return InkloopCloudStatus::success();
  }
  InkloopCloudStatus commitRevision(uint32_t value) override {
    ++revision_commits;
    if (fail_revision) return {InkloopCloudCode::Storage, 0, 0, "revision"};
    durable.revision_present = true;
    durable.revision = value;
    return InkloopCloudStatus::success();
  }
};

struct Platform final : IArduinoInkloopIdentityPlatform {
  uint64_t mac = 0x0cda43858428ULL;
  bool mac_ok = true;
  bool random_ok = true;
  unsigned random_calls = 0;

  bool readLegacyEfuseMac(uint64_t& output) override {
    output = mac;
    return mac_ok;
  }
  bool fillRandom(uint8_t* bytes, size_t length) override {
    ++random_calls;
    if (!random_ok) return false;
    for (size_t index = 0; index < length; ++index)
      bytes[index] = static_cast<uint8_t>(index);
    return true;
  }
};

int main() {
  const std::string device =
      "esp32-12345678-1234-1234-1234-123456789abc";

  // Existing Arduino Preferences values are read exactly and never rewritten.
  Nvs existing;
  existing.durable.device_id_present = true;
  existing.durable.device_id = device;
  existing.durable.secret_present = true;
  existing.durable.secret = std::string(64, 'a');
  existing.durable.revision_present = true;
  existing.durable.revision = 91;
  Platform platform;
  ArduinoCompatibleInkloopIdentityStore store(existing, platform);
  InkloopIdentitySnapshot snapshot;
  assert(store.loadOrCreate(snapshot).ok());
  assert(snapshot.hardware_id == "M5PC-0CDA43858428");
  assert(snapshot.device_id == device);
  assert(snapshot.secret == std::string(64, 'a'));
  assert(snapshot.applied_revision == 91);
  assert(platform.random_calls == 0 && existing.fresh_commits == 0);

  // A genuinely missing secret is generated and committed once, in lowercase
  // hex, while binding/revision reset in the same transaction.
  Nvs fresh;
  fresh.durable.device_id_present = true;
  fresh.durable.device_id = device;
  fresh.durable.revision_present = true;
  fresh.durable.revision = 17;
  Platform entropy;
  ArduinoCompatibleInkloopIdentityStore fresh_store(fresh, entropy);
  assert(fresh_store.loadOrCreate(snapshot).ok());
  assert(snapshot.device_id.empty() && snapshot.applied_revision == 0);
  assert(snapshot.secret ==
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f");
  assert(entropy.random_calls == 1 && fresh.fresh_commits == 1);
  const std::string first_secret = snapshot.secret;
  assert(fresh_store.loadOrCreate(snapshot).ok());
  assert(snapshot.secret == first_secret);
  assert(entropy.random_calls == 1 && fresh.fresh_commits == 1);

  // Failed initialization commit does not publish a new snapshot or mutate
  // any old Arduino value.
  Nvs failed_fresh;
  failed_fresh.durable.device_id_present = true;
  failed_fresh.durable.device_id = device;
  failed_fresh.durable.revision_present = true;
  failed_fresh.durable.revision = 23;
  failed_fresh.fail_fresh = true;
  Platform failed_entropy;
  ArduinoCompatibleInkloopIdentityStore failed_store(
      failed_fresh, failed_entropy);
  assert(failed_store.loadOrCreate(snapshot).code ==
         InkloopCloudCode::Storage);
  assert(snapshot.hardware_id.empty() && snapshot.secret.empty());
  assert(failed_fresh.durable.device_id == device);
  assert(failed_fresh.durable.revision == 23);
  assert(!failed_fresh.durable.secret_present);

  // Malformed non-empty secrets fail closed instead of rotating credentials.
  Nvs malformed;
  malformed.durable.secret_present = true;
  malformed.durable.secret = std::string(64, 'A');
  Platform malformed_platform;
  ArduinoCompatibleInkloopIdentityStore malformed_store(
      malformed, malformed_platform);
  assert(malformed_store.loadOrCreate(snapshot).code ==
         InkloopCloudCode::Storage);
  assert(malformed.fresh_commits == 0 && malformed_platform.random_calls == 0);

  // Every later value is submitted as one transaction; failure leaves the old
  // readable key in place.
  existing.fail_device = true;
  assert(store.saveDeviceId(std::string(45, 'x')).code ==
         InkloopCloudCode::Storage);
  assert(existing.durable.device_id == device);
  existing.fail_device = false;
  const std::string replacement =
      "esp32-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  assert(store.saveDeviceId(replacement).ok());
  assert(existing.durable.device_id == replacement);
  existing.fail_revision = true;
  assert(store.saveAppliedRevision(92).code == InkloopCloudCode::Storage);
  assert(existing.durable.revision == 91);
  existing.fail_revision = false;
  assert(store.saveAppliedRevision(92).ok());
  assert(existing.durable.revision == 92);
  assert(store.saveDeviceId("bad").code == InkloopCloudCode::InvalidArgument);

  platform.mac_ok = false;
  assert(store.loadOrCreate(snapshot).code == InkloopCloudCode::Storage);
  assert(snapshot.hardware_id.empty());
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-cloud-idf-identity-"));
  try {
    const source = join(scratch, "identity.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer,
      "-I", join(adapter, "include"),
      "-I", join(cloud, "include"),
      "-I", join(storage, "include"),
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      source,
      join(adapter, "arduino_compatible_identity_store.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
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

test("ESP-IDF identity core preserves Arduino keys and commits atomically", () => {
  buildAndRun(false);
});

test("ESP-IDF identity core survives transaction faults under ASan/UBSan", () => {
  buildAndRun(true);
});

test("native identity adapter uses exact legacy namespace without erase/logging", () => {
  const source = readFileSync(join(adapter, "esp_nvs_identity_store.cpp"), "utf8");
  assert.match(source, /kArduinoNamespace\[\]\s*=\s*"inkloop"/);
  assert.match(source, /kDeviceIdKey\[\]\s*=\s*"device-id"/);
  assert.match(source, /kSecretKey\[\]\s*=\s*"secret"/);
  assert.match(source, /kRevisionKey\[\]\s*=\s*"revision"/);
  assert.match(source, /esp_efuse_mac_get_default/);
  assert.match(source, /esp_fill_random/);
  assert.match(source, /nvs_set_str[\s\S]+nvs_set_u32[\s\S]+nvs_set_str[\s\S]+nvs_commit/);
  assert.doesNotMatch(source, /nvs_flash_erase|nvs_erase_all|nvs_erase_key/);
  assert.doesNotMatch(source, /ESP_LOG|printf\s*\(|puts\s*\(/);
});
