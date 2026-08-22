import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_recovery",
);
const source = readFileSync(join(component, "recovery_portal.cpp"), "utf8");

const harness = String.raw`
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/recovery/recovery_portal.hpp"

using namespace inkloop::recovery;

template <size_t Size>
void fixed(const std::string& value, std::array<char, Size>& output) {
  assert(value.size() < output.size());
  output.fill('\0');
  std::copy(value.begin(), value.end(), output.begin());
}

struct Cache final : IRecoveryDiagnosticCache {
  mutable RecoveryReadResult result = RecoveryReadResult::Ok;
  mutable bool allow_startup = false;
  mutable bool invalid_enum = false;
  mutable bool unterminated_id = false;
  mutable unsigned reads = 0;

  RecoveryReadResult readRecoveryDiagnostic(
      RecoveryDiagnosticSnapshot& output) const override {
    ++reads;
    if (result != RecoveryReadResult::Ok) return result;
    output.reason = invalid_enum ? static_cast<RecoveryReason>(255)
                                 : RecoveryReason::MigrationRefused;
    output.phase = RecoveryPhase::Migration;
    output.outcome = RecoveryOutcome::RequiresOperator;
    fixed("idf-1.0<script>&\"", output.firmware_id);
    fixed("m5-board\"alpha", output.board_id);
    if (unterminated_id) output.board_id.fill('A');
    output.records.nvs_namespaces = 3;
    output.records.files = 17;
    output.records.settings_records = 4;
    output.records.task_records = 8;
    output.records.album_assets = 12;
    output.records.ota_slots = 2;
    output.normal_startup_refused = !allow_startup;
    return RecoveryReadResult::Ok;
  }
};

RecoveryAccessConfig access() {
  RecoveryAccessConfig value;
  value.access_code = "safe code 42";
  value.session_id = "recovery_session_1234567890";
  value.csrf_token = "recovery_csrf_123456789012";
  value.allowed_hosts[0] = "inkloop.local";
  value.allowed_hosts[1] = "192.168.4.1";
  value.allowed_host_count = 2;
  value.allowed_origins[0] = "http://inkloop.local";
  value.allowed_origins[1] = "http://192.168.4.1";
  value.allowed_origin_count = 2;
  value.session_lifetime_seconds = 300;
  return value;
}

RecoveryRequest request(std::string method, std::string path,
                        uint64_t now = 100) {
  RecoveryRequest value;
  value.method = std::move(method);
  value.path = std::move(path);
  value.host = "inkloop.local";
  value.origin = "http://inkloop.local";
  value.peer_is_local = true;
  value.now_seconds = now;
  return value;
}

int main() {
  Cache cache;
  RecoveryAccessConfig invalid_access = access();
  invalid_access.access_code = "x";
  RecoveryPortalCore invalid(invalid_access, cache);
  assert(!invalid.ready());
  assert(invalid.handle(request("GET", "/")).status == 503);

  RecoveryAccessConfig tail = access();
  tail.allowed_hosts[3] = "unexpected.local";
  RecoveryPortalCore invalid_tail(tail, cache);
  assert(!invalid_tail.ready());

  RecoveryPortalCore portal(access(), cache);
  assert(portal.ready());

  RecoveryRequest root = request("GET", "/");
  RecoveryResponse page = portal.handle(root);
  assert(page.status == 200);
  assert(page.content_type == "text/html; charset=utf-8");
  assert(page.body.find("数据没有被删除、格式化或覆盖") != std::string::npos);
  assert(page.body.find("普通写入服务已停止") != std::string::npos);
  assert(page.body.find("http://inkloop.local/") != std::string::npos);
  assert(page.body.find("串口") != std::string::npos);
  assert(page.body.size() <= kMaximumRecoveryResponseBytes);
  assert(cache.reads == 0);

  RecoveryRequest remote = root;
  remote.peer_is_local = false;
  assert(portal.handle(remote).status == 403);
  RecoveryRequest bad_host = root;
  bad_host.host = "inkloop.local.attacker.test";
  assert(portal.handle(bad_host).status == 400);
  RecoveryRequest bad_origin = root;
  bad_origin.origin = "http://attacker.test";
  assert(portal.handle(bad_origin).status == 403);
  assert(portal.handle(request("GET", "/api/migrate")).status == 404);
  assert(portal.handle(request("POST", "/")).status == 405);
  assert(portal.handle(request("PUT", "/api/diagnostics")).status == 405);
  assert(portal.handle(request("GET", "/api/diagnostics")).status == 401);
  assert(cache.reads == 0);

  RecoveryRequest wrong = request("POST", "/api/session");
  wrong.content_type = "application/x-www-form-urlencoded";
  wrong.body = "code=incorrect";
  wrong.content_length = wrong.body.size();
  RecoveryResponse denied = portal.handle(wrong);
  assert(denied.status == 401);
  assert(denied.body.find("incorrect") == std::string::npos);
  wrong.origin.clear();
  assert(portal.handle(wrong).status == 403);
  wrong.origin = "http://inkloop.local";
  wrong.content_length += 1;
  assert(portal.handle(wrong).status == 413);

  RecoveryRequest login = request("POST", "/api/session");
  login.content_type = "application/x-www-form-urlencoded";
  login.body = "code=safe+code+42";
  login.content_length = login.body.size();
  RecoveryResponse authenticated = portal.handle(login);
  assert(authenticated.status == 200);
  assert(authenticated.body.find("recovery_csrf_123456789012") !=
         std::string::npos);
  assert(authenticated.body.find("safe code 42") == std::string::npos);
  assert(authenticated.set_cookie.find("inkloop_recovery_session=") == 0);
  assert(authenticated.set_cookie.find("HttpOnly") != std::string::npos);
  assert(authenticated.set_cookie.find("SameSite=Strict") != std::string::npos);
  const size_t cookie_end = authenticated.set_cookie.find(';');
  assert(cookie_end != std::string::npos);
  const std::string cookie = authenticated.set_cookie.substr(0, cookie_end);
  assert(portal.handle(login).status == 200);

  RecoveryRequest diagnostic = request("GET", "/api/diagnostics");
  diagnostic.cookie = cookie;
  assert(portal.handle(diagnostic).status == 403);
  diagnostic.csrf_token = access().csrf_token;
  RecoveryResponse state = portal.handle(diagnostic);
  assert(state.status == 200);
  assert(cache.reads == 1);
  assert(state.body.find("\"reason\":\"migration_refused\"") !=
         std::string::npos);
  assert(state.body.find("\"phase\":\"migration\"") != std::string::npos);
  assert(state.body.find("\"outcome\":\"requires_operator\"") !=
         std::string::npos);
  assert(state.body.find("\"normalStartupRefused\":true") !=
         std::string::npos);
  assert(state.body.find("\"dataErased\":false") != std::string::npos);
  assert(state.body.find("\"normalWritersStarted\":false") !=
         std::string::npos);
  assert(state.body.find("\"albumAssets\":12") != std::string::npos);
  assert(state.body.find("\\u003cscript\\u003e\\u0026\\\"") !=
         std::string::npos);
  assert(state.body.find("<script>") == std::string::npos);
  assert(state.body.find("device_token") == std::string::npos);
  assert(state.body.find("pairing_token") == std::string::npos);
  assert(state.body.find("providerUrl") == std::string::npos);
  assert(state.body.find("prompt") == std::string::npos);

  cache.allow_startup = true;
  assert(portal.handle(diagnostic).status == 422);
  cache.allow_startup = false;
  cache.invalid_enum = true;
  assert(portal.handle(diagnostic).status == 422);
  cache.invalid_enum = false;
  cache.unterminated_id = true;
  assert(portal.handle(diagnostic).status == 422);
  cache.unterminated_id = false;
  cache.result = RecoveryReadResult::Busy;
  assert(portal.handle(diagnostic).status == 503);
  cache.result = RecoveryReadResult::Ok;

  RecoveryRequest duplicate_cookie = diagnostic;
  duplicate_cookie.cookie += "; " + cookie;
  assert(portal.handle(duplicate_cookie).status == 401);
  RecoveryRequest expired = diagnostic;
  expired.now_seconds = 400;
  assert(portal.handle(expired).status == 401);
  login.now_seconds = 401;
  assert(portal.handle(login).status == 200);

  RecoveryRequest oversized = root;
  oversized.path.assign(129, 'x');
  assert(portal.handle(oversized).status == 400);
  return 0;
}
`;

function buildAndRun(sanitize) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-recovery-portal-"));
  try {
    const harnessPath = join(scratch, "recovery.cpp");
    const binary = join(scratch, sanitize ? "sanitized" : "strict");
    writeFileSync(harnessPath, harness);
    const flags = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(component, "include"),
      harnessPath,
      join(component, "recovery_portal.cpp"),
      "-o",
      binary,
    ];
    if (sanitize) flags.splice(5, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    execFileSync("c++", flags, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitize
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("recovery portal supports bounded repeat authentication and fixed diagnostics", () => {
  buildAndRun(false);
});

test("recovery portal survives adversarial requests under ASan/UBSan", () => {
  buildAndRun(true);
});

test("recovery browser is static, escaped, accessible and never polls", () => {
  const html = source.split('R"INKLOOP_RECOVERY(')[1]
    ?.split(')INKLOOP_RECOVERY"')[0];
  assert.ok(html);
  assert.match(html, /width=device-width,initial-scale=1/);
  assert.match(html, /role="status" aria-live="polite"/);
  assert.match(html, /autocomplete="current-password"/);
  assert.match(html, /默认与已保存的家庭 Wi-Fi 密码相同/);
  assert.doesNotMatch(html, /访问码无效、已使用/);
  assert.match(html, /普通写入服务已停止/);
  assert.match(html, /数据没有被删除、格式化或覆盖/);
  assert.match(html, /http:\/\/inkloop\.local\//);
  assert.match(html, /串口/);
  assert.match(html, /sessionStorage/);
  assert.match(html, /X-Inkloop-CSRF/);
  assert.match(html, /\/api\/diagnostics/);
  assert.match(html, /textContent/);
  assert.doesNotMatch(html, /innerHTML|document\.write|eval\s*\(/);
  assert.doesNotMatch(html, /setInterval|setTimeout|WebSocket|EventSource/);
  assert.doesNotMatch(html, /https?:\/\/(?!inkloop\.local)/);
  const scripts = html.split("<script>").slice(1).map(
    (part) => part.split("</script>")[0],
  );
  assert.equal(scripts.length, 1);
  assert.doesNotThrow(() => new Function(scripts[0]));
  assert.ok(Buffer.byteLength(html) <= 16384);
});
