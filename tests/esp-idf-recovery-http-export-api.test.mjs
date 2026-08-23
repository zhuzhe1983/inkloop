import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const root = process.cwd();
const component = join(root,
  "firmware/inkloop-idf/components/inkloop_recovery");
const ownerSource = readFileSync(join(root,
  "firmware/inkloop-idf/main/recovery_action_owner.cpp"), "utf8");
const serverSource = readFileSync(join(component,
  "esp_recovery_server.cpp"), "utf8");

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/recovery/recovery_portal.hpp"

using namespace inkloop::recovery;

template <size_t Size>
void fill(uint8_t value, std::array<uint8_t, Size>& output) {
  output.fill(value);
}

std::string digest(uint8_t value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output(64U, '0');
  for (size_t at = 0U; at < 32U; ++at) {
    output[at * 2U] = hex[value >> 4U];
    output[at * 2U + 1U] = hex[value & 0x0fU];
  }
  return output;
}

struct Cache final : IRecoveryDiagnosticCache {
  RecoveryReadResult readRecoveryDiagnostic(
      RecoveryDiagnosticSnapshot&) const override {
    return RecoveryReadResult::Unavailable;
  }
};

struct Export final : IRecoveryExportOwner {
  std::array<uint8_t, kRecoveryExportSessionBytes> session{};
  RecoveryExportExpectedIndexes expected{};
  uint64_t remaining = 0U;
  unsigned prepares = 0U;
  unsigned aborts = 0U;
  unsigned closes = 0U;

  Export() { session.fill(0x33U); }

  RecoveryExportResult prepareRecoveryExport(
      const RecoveryExportExpectedIndexes& value,
      RecoveryExportSnapshot& output) override {
    ++prepares;
    expected = value;
    output = {};
    output.session_id = session;
    for (size_t at = 0U; at < 3U; ++at) {
      output.candidates[at].byte_count = 100U + at;
      output.candidates[at].digest = value.digests[at];
      output.candidates[at].asset_entries = 1U;
    }
    output.asset_count = 1U;
    output.inventory_pages = 1U;
    output.total_bytes = 353U;
    return RecoveryExportResult::Ok;
  }

  RecoveryExportResult readRecoveryExportInventory(
      const std::array<uint8_t, kRecoveryExportSessionBytes>& id,
      uint32_t page, RecoveryExportInventoryPage& output) override {
    if (id != session || page != 0U) return RecoveryExportResult::InvalidRequest;
    output = {};
    output.session_id = session;
    output.page = 0U;
    output.asset_offset = 0U;
    output.count = 1U;
    output.assets[0].byte_count = 50U;
    fill(0x44U, output.assets[0].digest);
    output.assets[0].candidate_mask = 7U;
    return RecoveryExportResult::Ok;
  }

  RecoveryExportResult openRecoveryExport(
      const RecoveryExportOpenRequest& request,
      RecoveryExportStream& output) override {
    if (request.session_id != session || request.item > 3U)
      return RecoveryExportResult::InvalidRequest;
    output = {};
    output.handle = 71U;
    output.item = request.item;
    if (request.item < 3U) {
      output.byte_count = 100U + request.item;
      output.digest = expected.digests[request.item];
    } else {
      output.byte_count = 50U;
      fill(0x44U, output.digest);
    }
    remaining = output.byte_count;
    return RecoveryExportResult::Ok;
  }

  RecoveryExportResult readRecoveryExport(
      uint32_t handle, uint8_t* output, size_t capacity,
      size_t& bytes_read) override {
    bytes_read = 0U;
    if (handle != 71U || !output || capacity == 0U)
      return RecoveryExportResult::InvalidRequest;
    if (remaining == 0U) return RecoveryExportResult::Complete;
    bytes_read = static_cast<size_t>(std::min<uint64_t>(remaining, capacity));
    std::fill(output, output + bytes_read, 0x5aU);
    remaining -= bytes_read;
    return RecoveryExportResult::Ok;
  }

  void closeRecoveryExport(uint32_t handle) override {
    assert(handle == 71U);
    ++closes;
  }

  RecoveryExportResult finishRecoveryExport(
      const std::array<uint8_t, kRecoveryExportSessionBytes>& id) override {
    return id == session ? RecoveryExportResult::Complete
                         : RecoveryExportResult::SessionStale;
  }

  void abortRecoveryExport(
      const std::array<uint8_t, kRecoveryExportSessionBytes>& id) override {
    if (id == session) ++aborts;
  }
};

RecoveryRequest request(const std::string& method, const std::string& path,
                        const std::string& body = {}) {
  RecoveryRequest output;
  output.method = method;
  output.path = path;
  output.host = "inkloop.local:8080";
  output.origin = "http://inkloop.local:8080";
  output.peer_is_local = true;
  output.now_seconds = 2U;
  output.content_length = body.size();
  output.body = body;
  if (!body.empty())
    output.content_type = "application/x-www-form-urlencoded";
  return output;
}

int main() {
  RecoveryAccessConfig access;
  access.access_code = "local-pass";
  access.session_id = "SESSION_TOKEN_1234567890";
  access.csrf_token = "CSRF_TOKEN_123456789012";
  access.allowed_hosts[0] = "inkloop.local:8080";
  access.allowed_host_count = 1U;
  access.allowed_origins[0] = "http://inkloop.local:8080";
  access.allowed_origin_count = 1U;
  Cache cache;
  Export owner;
  RecoveryPortalCore core(access, cache, nullptr, &owner);
  assert(core.ready());

  const std::string body = "current=" + digest(0x11U) +
      "&next=" + digest(0x22U) + "&previous=" + digest(0x33U) +
      "&confirm=readonly_export";
  assert(core.handle(request("POST", "/api/recovery/export/prepare", body)).status == 401);

  auto login = request("POST", "/api/session", "code=local-pass");
  login.now_seconds = 1U;
  const RecoveryResponse logged = core.handle(login);
  assert(logged.status == 200);
  assert(logged.set_cookie.find("inkloop_recovery_session=") == 0U);

  auto authorized = [&](const std::string& method, const std::string& path,
                        const std::string& value = std::string()) {
    RecoveryRequest output = request(method, path, value);
    output.cookie = "inkloop_recovery_session=SESSION_TOKEN_1234567890";
    output.csrf_token = "CSRF_TOKEN_123456789012";
    return output;
  };

  const RecoveryResponse prepared = core.handle(authorized(
      "POST", "/api/recovery/export/prepare", body));
  assert(prepared.status == 200);
  assert(prepared.body.find("\"assetCount\":1") != std::string::npos);
  assert(owner.prepares == 1U);

  const std::string session(32U, '0');
  (void)session;
  const std::string session_hex = "33333333333333333333333333333333";
  const RecoveryResponse inventory = core.handle(authorized(
      "GET", "/api/recovery/export/inventory/" + session_hex + "/0"));
  assert(inventory.status == 200);
  assert(inventory.body.find("\"candidateMask\":7") != std::string::npos);

  RecoveryExportStream stream;
  const RecoveryResponse opened = core.openRecoveryExportFile(authorized(
      "GET", "/api/recovery/export/file/" + session_hex + "/0"), stream);
  assert(opened.status == 200 && stream.handle == 71U && stream.item == 0U);
  std::array<uint8_t, 32U> chunk{};
  size_t count = 0U;
  uint64_t total = 0U;
  for (;;) {
    const RecoveryExportResult result = core.readRecoveryExportFile(
        stream.handle, chunk.data(), chunk.size(), count);
    if (result == RecoveryExportResult::Complete) break;
    assert(result == RecoveryExportResult::Ok && count > 0U);
    total += count;
  }
  assert(total == 100U);
  core.closeRecoveryExportFile(stream.handle);
  assert(owner.closes == 1U);

  const RecoveryResponse finished = core.handle(authorized(
      "POST", "/api/recovery/export/finish",
      "session=" + session_hex + "&confirm=verify_export"));
  assert(finished.status == 200 &&
         finished.body.find("verified") != std::string::npos);

  const RecoveryResponse duplicate = core.handle(authorized(
      "POST", "/api/recovery/export/prepare",
      "current=" + digest(0x11U) + "&next=" + digest(0x11U) +
      "&previous=" + digest(0x33U) + "&confirm=readonly_export"));
  assert(duplicate.status == 422);
  return 0;
}
`;

test("Recovery HTTP export API enforces auth, expected hashes and bounded streams", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-recovery-http-api-"));
  try {
    const source = join(scratch, "recovery_http_api.cpp");
    const binary = join(scratch, "recovery_http_api");
    writeFileSync(source, harness);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "recovery_portal.cpp"), "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally { rmSync(scratch, { recursive: true, force: true }); }
});

test("Recovery export owner surface is path-free and source operations are read-only", () => {
  const header = readFileSync(join(component,
    "include/inkloop/recovery/recovery_portal.hpp"), "utf8");
  const contract = header.slice(header.indexOf("struct RecoveryExportExpectedIndexes"),
    header.indexOf("struct RecoveryAccessConfig"));
  assert.match(contract, /IRecoveryExportOwner/);
  assert.doesNotMatch(contract, /std::string|std::vector|filesystem/);
  const implementation = ownerSource.slice(
    ownerSource.indexOf("EspRecoveryActionOwner::prepareRecoveryExport"),
    ownerSource.indexOf("bool EspRecoveryActionOwner::postActionAuditClean"));
  assert.match(implementation, /fopen\(path\.c_str\(\), "rb"\)/);
  assert.match(implementation, /stream_hash\.finish/);
  assert.match(
    ownerSource,
    /kExportSessionLifetimeMs\s*=\s*30U\s*\*\s*60U\s*\*\s*1000U/,
  );
  assert.doesNotMatch(implementation,
    /"[aw]b"|rename\s*\(|unlink\s*\(|remove\s*\(|mkdir|format/);
  assert.match(serverSource, /kMaximumRecoveryExportChunkBytes/);
  assert.match(serverSource, /vTaskDelay\(kRecoveryExportChunkDelay\)/);
});
