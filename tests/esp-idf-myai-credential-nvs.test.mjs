import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf");

const coreHarness = String.raw`
#include <cassert>
#include <map>
#include <string>

#include "CredentialPersistence.h"

using namespace inkloop::myai;

struct Codec final : ICredentialRecordCodec {
  mutable unsigned next = 1;
  mutable std::map<std::string, CredentialSnapshot> records;
  Status encode(const CredentialSnapshot& value, std::string& output) const override {
    if (!credentialSnapshotCoherent(value)) return Status(ErrorCode::Protocol);
    output = "record-" + std::to_string(next++);
    records[output] = value;
    return Status::success();
  }
  Status decode(const std::string& input, CredentialSnapshot& value) const override {
    const auto found = records.find(input);
    if (found == records.end()) return Status(ErrorCode::Protocol);
    value = found->second;
    return Status::success();
  }
};

struct Journal final : ICredentialJournalStore {
  CredentialJournalState state;
  bool failInspect = false;
  bool failSlot = false;
  bool failHead = false;
  unsigned slotWrites = 0;
  unsigned headWrites = 0;
  Journal() { state.namespaceAvailable = true; }
  Status inspect(CredentialJournalState& output) override {
    if (failInspect) return Status(ErrorCode::Storage);
    output = state;
    return Status::success();
  }
  Status writeSlotAndCommit(uint8_t slot, const std::string& encoded) override {
    ++slotWrites;
    if (failSlot) return Status(ErrorCode::Storage);
    state.slotPresent[slot] = true;
    state.slot[slot] = encoded;
    return Status::success();
  }
  Status writeHeadAndMarkerAndCommit(uint32_t generation) override {
    ++headWrites;
    if (failHead) return Status(ErrorCode::Storage);
    state.headPresent = true;
    state.head = generation;
    state.markerPresent = true;
    state.markerValid = true;
    return Status::success();
  }
};

PendingPairing pending() {
  PendingPairing value;
  value.deviceId = "692639";
  value.pairingToken = "pairing-secret";
  value.bindingUrl = "https://myai.mess.host/bind/692639";
  value.expiresAt = "2026-08-22T06:00:00Z";
  return value;
}

int main() {
  Codec codec;
  Journal journal;
  CredentialPersistenceCore store(journal, codec);
  CredentialSnapshot snapshot;
  assert(store.load(snapshot).ok() && snapshot.generation == 0);
  assert(store.clearPendingAtomically().code == ErrorCode::InvalidState);

  assert(store.initializeFingerprintAtomically("papercolor-installation").ok());
  assert(journal.slotWrites == 1 && journal.headWrites == 1);
  assert(journal.state.head == 1 && journal.state.slotPresent[1]);
  assert(store.load(snapshot).ok());
  assert(snapshot.generation == 1 &&
         snapshot.installationFingerprint == "papercolor-installation");
  assert(store.initializeFingerprintAtomically("papercolor-installation").ok());
  assert(journal.slotWrites == 1);
  assert(store.initializeFingerprintAtomically("other").code == ErrorCode::Conflict);

  PendingPairing pairing = pending();
  assert(store.savePendingAtomically(pairing).ok());
  assert(journal.state.head == 2 && journal.state.slotPresent[0]);
  assert(store.load(snapshot).ok() && snapshot.pending.valid());
  assert(store.promoteBoundAtomically("wrong-secret", "692639", "device-token", true).code ==
         ErrorCode::Conflict);
  assert(journal.state.head == 2);
  assert(store.promoteBoundAtomically("pairing-secret", "692639", "device-token", false).ok());
  assert(store.load(snapshot).ok());
  assert(snapshot.generation == 3 && snapshot.deviceId == "692639" &&
         snapshot.deviceToken == "device-token" && !snapshot.active &&
         snapshot.pending.empty());

  // A failed head phase leaves generation 3 authoritative. The orphan slot is
  // ignored and a retry can safely replace it with generation 4.
  journal.failHead = true;
  const Status interrupted = store.clearRuntimeCredentialAtomically();
  assert(interrupted.code == ErrorCode::Storage);
  assert(interrupted.detail.find("device-token") == std::string::npos);
  assert(journal.state.head == 3 && journal.state.slotPresent[0]);
  assert(store.load(snapshot).ok() && snapshot.generation == 3 &&
         snapshot.deviceToken == "device-token");
  journal.failHead = false;
  assert(store.clearRuntimeCredentialAtomically().ok());
  assert(store.load(snapshot).ok() && snapshot.generation == 4 &&
         snapshot.deviceId.empty() && snapshot.deviceToken.empty() &&
         !snapshot.active);

  // Marker-less valid data is the existing Arduino legacy form and remains
  // readable. Bad marker, bad active slot, missing head and unavailable NVS
  // all fail closed without rotating credentials.
  journal.state.markerPresent = false;
  journal.state.markerValid = false;
  assert(store.load(snapshot).ok() && snapshot.generation == 4);
  journal.state.markerPresent = true;
  assert(store.load(snapshot).code == ErrorCode::Storage);
  journal.state.markerValid = true;
  const std::string good = journal.state.slot[0];
  journal.state.slot[0] = "corrupt";
  assert(store.load(snapshot).code == ErrorCode::Storage);
  journal.state.slot[0] = good;
  journal.state.headPresent = false;
  assert(store.load(snapshot).code == ErrorCode::Storage);
  journal.state.headPresent = true;
  journal.state.namespaceAvailable = false;
  assert(store.load(snapshot).code == ErrorCode::Storage);
  journal.state.namespaceAvailable = true;

  PendingPairing bad = pairing;
  bad.pairingToken.clear();
  assert(store.savePendingAtomically(bad).code == ErrorCode::InvalidArgument);
  assert(store.promoteBoundAtomically("pairing-secret", "12345x", "token", true).code ==
         ErrorCode::InvalidArgument);
  return 0;
}
`;

function buildCore(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-credential-core-"));
  try {
    const source = join(scratch, "core.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, coreHarness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(myai, "include/inkloop/myai"),
      source, join(myai, "CredentialPersistence.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
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

test("credential journal is generation-safe under strict C++17", () => {
  buildCore(false);
});

test("credential journal survives adversarial transitions under ASan and UBSan", () => {
  buildCore(true);
});

test("native codec is byte-compatible with the Arduino JSON/SHA record", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-credential-codec-"));
  try {
    const include = join(scratch, "include");
    mkdirSync(join(include, "psa"), { recursive: true });
    const canonical = '{"schema":1,"generation":7,"fingerprint":"papercolor-c151-0cda43858428","device_id":"692639","pending":{"device_id":"","token":"","binding_url":"","expires_at":""},"device_token":"device-token","active":true}';
    const digest = createHash("sha256").update(canonical).digest();
    const digestBytes = [...digest].join(",");
    const checksum = digest.toString("hex");
    const finalRecord = `${canonical.slice(0, -1)},"checksum":"${checksum}"}`;
    writeFileSync(join(include, "nvs.h"), String.raw`
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 0
#define NVS_READONLY 1
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*);
esp_err_t nvs_get_u32(nvs_handle_t,const char*,uint32_t*);
esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
esp_err_t nvs_set_str(nvs_handle_t,const char*,const char*);
esp_err_t nvs_set_u32(nvs_handle_t,const char*,uint32_t);
esp_err_t nvs_set_u8(nvs_handle_t,const char*,uint8_t);
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
psa_status_t psa_hash_compute(psa_algorithm_t,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
#ifdef __cplusplus
}
#endif
`);
    const harness = String.raw`
#include <cassert>
#include <cstring>
#include <string>
#include "inkloop/myai/esp_nvs_credential_store.hpp"
#include "nvs.h"
#include "psa/crypto.h"
using namespace inkloop::myai;
static const char expectedCanonical[] = ${JSON.stringify(canonical)};
static const unsigned char expectedDigest[32] = {${digestBytes}};
extern "C" psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
extern "C" psa_status_t psa_hash_compute(psa_algorithm_t algorithm,
    const uint8_t* input,size_t length,uint8_t* output,size_t capacity,size_t* written) {
  assert(algorithm == PSA_ALG_SHA_256 && capacity >= 32 && written);
  assert(length == std::strlen(expectedCanonical));
  assert(std::memcmp(input, expectedCanonical, length) == 0);
  std::memcpy(output, expectedDigest, 32); *written = 32; return PSA_SUCCESS;
}
extern "C" esp_err_t nvs_open(const char*,int,nvs_handle_t*) { return 2; }
extern "C" void nvs_close(nvs_handle_t) {}
extern "C" esp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*) { return 2; }
extern "C" esp_err_t nvs_get_u32(nvs_handle_t,const char*,uint32_t*) { return 2; }
extern "C" esp_err_t nvs_get_str(nvs_handle_t,const char*,char*,size_t*) { return 2; }
extern "C" esp_err_t nvs_set_str(nvs_handle_t,const char*,const char*) { return 2; }
extern "C" esp_err_t nvs_set_u32(nvs_handle_t,const char*,uint32_t) { return 2; }
extern "C" esp_err_t nvs_set_u8(nvs_handle_t,const char*,uint8_t) { return 2; }
extern "C" esp_err_t nvs_commit(nvs_handle_t) { return 2; }
int main() {
  JsonSha256CredentialCodec codec;
  CredentialSnapshot value;
  value.generation = 7;
  value.installationFingerprint = "papercolor-c151-0cda43858428";
  value.deviceId = "692639";
  value.deviceToken = "device-token";
  value.active = true;
  std::string encoded;
  assert(codec.encode(value, encoded).ok());
  assert(encoded == ${JSON.stringify(finalRecord)});
  CredentialSnapshot decoded;
  assert(codec.decode(encoded, decoded).ok());
  assert(decoded.generation == 7 && decoded.deviceId == "692639" &&
         decoded.deviceToken == "device-token" && decoded.active);
  encoded.back() = ']';
  assert(!codec.decode(encoded, decoded).ok());
  return 0;
}
`;
    const source = join(scratch, "codec.cpp");
    const binary = join(scratch, "codec");
    writeFileSync(source, harness);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", include,
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      "-I", join(native, "include"),
      source,
      join(myai, "CredentialPersistence.cpp"),
      join(native, "esp_nvs_credential_store.cpp"),
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

test("native NVS adapter preserves namespace and two-phase commit contract", () => {
  const source = readFileSync(join(native, "esp_nvs_credential_store.cpp"), "utf8");
  assert.match(source, /kNamespace\[\] = "ink-myai-v1"/);
  assert.match(source, /kMarkerKey\[\] = "initialized"/);
  assert.match(source, /kSlot0Key\[\] = "slot0"/);
  assert.match(source, /kSlot1Key\[\] = "slot1"/);
  assert.match(source, /inspect[\s\S]*nvs_open\(kNamespace, NVS_READONLY/);
  assert.match(source, /opened == ESP_ERR_NVS_NOT_FOUND[\s\S]*namespaceAvailable = true/);
  assert.match(source, /writeSlotAndCommit[\s\S]*nvs_set_str[\s\S]*nvs_commit/);
  assert.match(source, /writeHeadAndMarkerAndCommit[\s\S]*nvs_set_u32[\s\S]*nvs_set_u8[\s\S]*nvs_commit/);
  assert.match(source, /PSA_ALG_SHA_256/);
  assert.doesNotMatch(source, /ESP_LOG|printf\s*\(|puts\s*\(|nvs_erase|nvs_flash_erase/);
});
