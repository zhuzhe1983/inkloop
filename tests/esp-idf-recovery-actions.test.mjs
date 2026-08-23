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
const header = readFileSync(
  join(component, "include/inkloop/recovery/recovery_portal.hpp"),
  "utf8",
);
const source = readFileSync(join(component, "recovery_portal.cpp"), "utf8");
const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");

const harness = String.raw`
#include <algorithm>
#include <array>
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

template <size_t Size>
void bytes(uint8_t value, std::array<uint8_t, Size>& output) {
  output.fill(value);
}

std::string hex(uint8_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output(kRecoveryActionDigestBytes * 2U, '0');
  for (size_t index = 0; index < kRecoveryActionDigestBytes; ++index) {
    output[index * 2U] = digits[value >> 4U];
    output[index * 2U + 1U] = digits[value & 0x0fU];
  }
  return output;
}

struct Cache final : IRecoveryDiagnosticCache {
  RecoveryReadResult readRecoveryDiagnostic(
      RecoveryDiagnosticSnapshot& output) const override {
    output.reason = RecoveryReason::StorageIntegrityRefused;
    output.phase = RecoveryPhase::StorageAudit;
    output.outcome = RecoveryOutcome::RequiresOperator;
    fixed("idf-actions", output.firmware_id);
    fixed("m5stack-c151", output.board_id);
    output.normal_startup_refused = true;
    return RecoveryReadResult::Ok;
  }
};

void candidate(RecoveryActionCandidate& output, uint8_t digest,
               uint64_t byte_count, bool file_candidate = false) {
  output.state = RecoveryActionCandidateState::Valid;
  output.byte_count = byte_count;
  bytes(digest, output.digest);
  output.digest_present = true;
  if (file_candidate) {
    output.item_count = digest;
    output.item_count_present = true;
    output.modified_unix_seconds = 1700000000U + digest;
    output.modified_time_present = true;
  }
}

struct Owner final : IRecoveryActionOwner {
  RecoveryActionReadResult read_result = RecoveryActionReadResult::Ok;
  RecoveryActionResolveResult configured_result =
      RecoveryActionResolveResult::Ok;
  unsigned inspect_calls = 0U;
  unsigned resolve_calls = 0U;
  unsigned invalid_mode = 0U;
  RecoveryActionRequest last{};

  RecoveryActionReadResult inspectRecoveryActions(
      RecoveryActionInventory& output) override {
    ++inspect_calls;
    if (read_result != RecoveryActionReadResult::Ok) return read_result;
    output = {};
    output.count = 4U;

    auto& display = output.snapshots[0];
    display.domain = RecoveryActionDomain::Display;
    display.backend = RecoveryActionBackend::None;
    display.state = RecoveryActionState::ChoiceRequired;
    candidate(display.candidates[0], 0x11U, 4096U);
    candidate(display.candidates[2], 0x12U, 0U);
    bytes(0xa1U, display.inspection_id);
    display.valid_candidates = 2U;

    auto& tasks = output.snapshots[1];
    tasks.domain = RecoveryActionDomain::Tasks;
    tasks.backend = RecoveryActionBackend::None;
    tasks.state = RecoveryActionState::ChoiceRequired;
    candidate(tasks.candidates[0], 0x21U, 201U, true);
    candidate(tasks.candidates[1], 0x22U, 202U, true);
    candidate(tasks.candidates[2], 0x23U, 203U, true);
    bytes(0xa2U, tasks.inspection_id);
    tasks.valid_candidates = 3U;

    auto& internal = output.snapshots[2];
    internal.domain = RecoveryActionDomain::Album;
    internal.backend = RecoveryActionBackend::Internal;
    internal.state = RecoveryActionState::Recoverable;
    candidate(internal.candidates[1], 0x31U, 301U, true);
    bytes(0xa3U, internal.inspection_id);
    internal.valid_candidates = 1U;

    auto& removable = output.snapshots[3];
    removable.domain = RecoveryActionDomain::Album;
    removable.backend = RecoveryActionBackend::Removable;
    removable.state = RecoveryActionState::Disabled;

    if (invalid_mode == 1U) output.count = 5U;
    if (invalid_mode == 2U) {
      output.snapshots[1].domain = RecoveryActionDomain::Display;
      output.snapshots[1].backend = RecoveryActionBackend::None;
    }
    if (invalid_mode == 3U)
      output.snapshots[0].candidates[1].state =
          RecoveryActionCandidateState::Valid;
    if (invalid_mode == 4U)
      output.snapshots[0].candidates[0].digest_present = false;
    if (invalid_mode == 5U)
      output.snapshots[0].state = static_cast<RecoveryActionState>(255U);
    return RecoveryActionReadResult::Ok;
  }

  RecoveryActionResolveResult resolveRecoveryAction(
      const RecoveryActionRequest& request) override {
    ++resolve_calls;
    last = request;
    uint8_t expected = 0U;
    if (request.domain == RecoveryActionDomain::Display &&
        request.backend == RecoveryActionBackend::None) {
      expected = 0xa1U;
      if (request.choice == RecoveryActionChoice::Next)
        return RecoveryActionResolveResult::InvalidRequest;
    } else if (request.domain == RecoveryActionDomain::Tasks &&
               request.backend == RecoveryActionBackend::None) {
      expected = 0xa2U;
    } else if (request.domain == RecoveryActionDomain::Album &&
               request.backend == RecoveryActionBackend::Internal) {
      expected = 0xa3U;
    } else {
      return RecoveryActionResolveResult::InvalidRequest;
    }
    for (uint8_t byte : request.inspection_id) {
      if (byte != expected) return RecoveryActionResolveResult::SourceChanged;
    }
    return configured_result;
  }
};

RecoveryAccessConfig access() {
  RecoveryAccessConfig value;
  value.access_code = "recovery password";
  value.session_id = "session_actions_0123456789";
  value.csrf_token = "csrf_actions_012345678901";
  value.allowed_hosts[0] = "inkloop.local:8080";
  value.allowed_host_count = 1U;
  value.allowed_origins[0] = "http://inkloop.local:8080";
  value.allowed_origin_count = 1U;
  value.session_lifetime_seconds = 300U;
  return value;
}

RecoveryRequest request(std::string method, std::string path) {
  RecoveryRequest value;
  value.method = std::move(method);
  value.path = std::move(path);
  value.host = "inkloop.local:8080";
  value.origin = "http://inkloop.local:8080";
  value.peer_is_local = true;
  value.now_seconds = 100U;
  return value;
}

std::string authenticate(RecoveryPortalCore& portal) {
  RecoveryRequest login = request("POST", "/api/session");
  login.content_type = "application/x-www-form-urlencoded";
  login.body = "code=recovery+password";
  login.content_length = login.body.size();
  const RecoveryResponse response = portal.handle(login);
  assert(response.status == 200);
  return response.set_cookie.substr(0U, response.set_cookie.find(';'));
}

RecoveryRequest authorized(std::string method, std::string path,
                           const std::string& cookie) {
  RecoveryRequest value = request(std::move(method), std::move(path));
  value.cookie = cookie;
  value.csrf_token = access().csrf_token;
  return value;
}

std::string body(const std::string& domain, const std::string& backend,
                 const std::string& choice, const std::string& snapshot,
                 const std::string& confirm = "resolve") {
  return "domain=" + domain + "&backend=" + backend + "&choice=" + choice +
         "&snapshot=" + snapshot + "&confirm=" + confirm;
}

RecoveryResponse post(RecoveryPortalCore& portal, const std::string& cookie,
                      const std::string& value) {
  RecoveryRequest action = authorized(
      "POST", "/api/recovery/actions/resolve", cookie);
  action.content_type = "application/x-www-form-urlencoded";
  action.body = value;
  action.content_length = action.body.size();
  return portal.handle(action);
}

int main() {
  Cache cache;
  Owner owner;
  RecoveryPortalCore portal(access(), cache, &owner);
  assert(portal.ready());

  RecoveryRequest options = request("GET", "/api/recovery/actions");
  assert(portal.handle(options).status == 401);
  assert(owner.inspect_calls == 0U);
  RecoveryRequest remote = options;
  remote.peer_is_local = false;
  assert(portal.handle(remote).status == 403);
  RecoveryRequest bad_host = options;
  bad_host.host = "inkloop.local:8080.attacker";
  assert(portal.handle(bad_host).status == 400);
  RecoveryRequest bad_origin = options;
  bad_origin.origin = "http://attacker.invalid";
  assert(portal.handle(bad_origin).status == 403);
  assert(owner.inspect_calls == 0U);

  const std::string cookie = authenticate(portal);
  options.cookie = cookie;
  assert(portal.handle(options).status == 403);
  options.csrf_token = access().csrf_token;
  options.body = "x";
  options.content_length = 1U;
  assert(portal.handle(options).status == 400);
  options.body.clear();
  options.content_length = 0U;
  RecoveryResponse inventory = portal.handle(options);
  assert(inventory.status == 200);
  assert(owner.inspect_calls == 1U);
  assert(inventory.body.find("\"domain\":\"display\"") !=
         std::string::npos);
  assert(inventory.body.find("\"choice\":\"next\",\"state\":\"missing\"") !=
         std::string::npos);
  assert(inventory.body.find(hex(0x11U)) != std::string::npos);
  assert(inventory.body.find(hex(0xa3U)) != std::string::npos);
  assert(inventory.body.find("\"items\":33") != std::string::npos);
  assert(inventory.body.find("\"modifiedAt\":1700000033") !=
         std::string::npos);
  assert(inventory.body.find("/tasks.json") == std::string::npos);
  assert(inventory.body.find("password") == std::string::npos);

  owner.read_result = RecoveryActionReadResult::Busy;
  assert(portal.handle(options).body.find("recovery_actions_busy") !=
         std::string::npos);
  owner.read_result = RecoveryActionReadResult::Unavailable;
  assert(portal.handle(options).body.find("recovery_actions_unavailable") !=
         std::string::npos);
  owner.read_result = RecoveryActionReadResult::InvalidData;
  assert(portal.handle(options).body.find("recovery_actions_invalid") !=
         std::string::npos);
  owner.read_result = RecoveryActionReadResult::Ok;
  for (unsigned mode = 1U; mode <= 5U; ++mode) {
    owner.invalid_mode = mode;
    assert(portal.handle(options).status == 422);
  }
  owner.invalid_mode = 0U;

  RecoveryPortalCore unauthenticated(access(), cache, &owner);
  const unsigned resolves_before = owner.resolve_calls;
  assert(post(unauthenticated, "inkloop_recovery_session=wrong",
              body("display", "none", "current", hex(0xa1U))).status == 401);
  assert(owner.resolve_calls == resolves_before);

  const std::string exact =
      body("display", "none", "current", hex(0xa1U));
  RecoveryRequest mutation = authorized(
      "POST", "/api/recovery/actions/resolve", cookie);
  mutation.content_type = "application/x-www-form-urlencoded";
  mutation.body = exact;
  mutation.content_length = exact.size();
  mutation.peer_is_local = false;
  assert(portal.handle(mutation).status == 403);
  mutation.peer_is_local = true;
  mutation.host = "inkloop.local:8080.attacker";
  assert(portal.handle(mutation).status == 400);
  mutation.host = "inkloop.local:8080";
  mutation.origin.clear();
  assert(portal.handle(mutation).status == 403);
  mutation.origin = "http://attacker.invalid";
  assert(portal.handle(mutation).status == 403);
  mutation.origin = "http://inkloop.local:8080";
  mutation.csrf_token = "csrf_actions_wrong_012345";
  assert(portal.handle(mutation).status == 403);
  mutation.csrf_token = access().csrf_token;
  mutation.content_type = "application/x-www-form-urlencoded; charset=UTF-8";
  assert(portal.handle(mutation).status == 415);
  mutation.content_type = "application/x-www-form-urlencoded";
  mutation.content_length += 1U;
  assert(portal.handle(mutation).status == 413);
  assert(owner.resolve_calls == resolves_before);

  const std::array<std::string, 10> invalid_bodies{{
      body("display", "none", "next", hex(0xa1U)),
      body("display", "internal", "current", hex(0xa1U)),
      body("tasks", "removable", "current", hex(0xa2U)),
      body("album", "none", "current", hex(0xa3U)),
      body("display", "none", "current", hex(0xa1U), "yes"),
      "choice=current&domain=display&backend=none&snapshot=" + hex(0xa1U) +
          "&confirm=resolve",
      exact + "&extra=x",
      body("display", "none", "current", std::string(64U, 'A')),
      body("delete", "none", "current", hex(0xa1U)),
      body("display", "none", "target", hex(0xa1U)),
  }};
  for (const std::string& invalid : invalid_bodies) {
    assert(post(portal, cookie, invalid).status == 422);
  }
  assert(owner.resolve_calls == resolves_before);

  RecoveryResponse stale = post(
      portal, cookie, body("display", "none", "current", hex(0xb1U)));
  assert(stale.status == 409);
  assert(stale.body.find("recovery_action_snapshot_stale") !=
         std::string::npos);
  assert(owner.resolve_calls == resolves_before + 1U);

  struct ResultCase {
    RecoveryActionResolveResult result;
    int status;
    const char* error;
  };
  const std::array<ResultCase, 7> results{{
      {RecoveryActionResolveResult::Busy, 503, "recovery_action_busy"},
      {RecoveryActionResolveResult::InvalidRequest, 422,
       "recovery_action_request_invalid"},
      {RecoveryActionResolveResult::SourceChanged, 409,
       "recovery_action_snapshot_stale"},
      {RecoveryActionResolveResult::SourceUnavailable, 409,
       "recovery_action_source_unavailable"},
      {RecoveryActionResolveResult::SelectedUnavailable, 409,
       "recovery_action_selected_unavailable"},
      {RecoveryActionResolveResult::IoError, 503,
       "recovery_action_io_error"},
      {RecoveryActionResolveResult::VerificationFailed, 500,
       "recovery_action_verification_failed"},
  }};
  for (const ResultCase& item : results) {
    owner.configured_result = item.result;
    const RecoveryResponse result = post(portal, cookie, exact);
    assert(result.status == item.status);
    assert(result.body.find(item.error) != std::string::npos);
    assert(result.body.find("\"result\":\"resolved\"") ==
           std::string::npos);
  }

  owner.configured_result = RecoveryActionResolveResult::Ok;
  RecoveryResponse success = post(portal, cookie, exact);
  assert(success.status == 200);
  assert(success.body == "{\"ok\":true,\"result\":\"resolved\"}");
  assert(owner.last.domain == RecoveryActionDomain::Display);
  assert(owner.last.backend == RecoveryActionBackend::None);
  assert(owner.last.choice == RecoveryActionChoice::Current);
  for (uint8_t value : owner.last.inspection_id) assert(value == 0xa1U);

  success = post(
      portal, cookie, body("tasks", "none", "next", hex(0xa2U)));
  assert(success.status == 200);
  assert(owner.last.domain == RecoveryActionDomain::Tasks);
  assert(owner.last.choice == RecoveryActionChoice::Next);
  success = post(portal, cookie,
                 body("album", "internal", "previous", hex(0xa3U)));
  assert(success.status == 200);
  assert(owner.last.domain == RecoveryActionDomain::Album);
  assert(owner.last.backend == RecoveryActionBackend::Internal);
  assert(owner.last.choice == RecoveryActionChoice::Previous);

  // Repeat login remains valid after reads and mutations.
  assert(!authenticate(portal).empty());
  return 0;
}
`;

function buildAndRun(sanitize) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-recovery-actions-"));
  try {
    const harnessPath = join(scratch, "actions.cpp");
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
    if (sanitize) {
      flags.splice(
        5,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
    }
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

test("typed recovery actions enforce auth, exact requests and stale identity", () => {
  buildAndRun(false);
});

test("typed recovery actions pass the adversarial ASan and UBSan matrix", () => {
  buildAndRun(true);
});

test("recovery action interface is fixed, storage-independent and closed", () => {
  const contract = header.slice(
    header.indexOf("inline constexpr size_t kRecoveryActionCandidateCount"),
    header.indexOf("struct RecoveryAccessConfig"),
  );
  assert.match(contract, /enum class RecoveryActionDomain[\s\S]*Display[\s\S]*Tasks[\s\S]*Album/);
  assert.match(contract, /enum class RecoveryActionChoice[\s\S]*Current[\s\S]*Next[\s\S]*Previous/);
  assert.match(contract, /std::array<RecoveryActionCandidate, kRecoveryActionCandidateCount>/);
  assert.match(contract, /std::array<uint8_t, kRecoveryActionDigestBytes> inspection_id/);
  assert.match(contract, /inspectRecoveryActions/);
  assert.match(contract, /resolveRecoveryAction/);
  assert.doesNotMatch(contract, /std::string|std::vector|char\s*\*|void\s*\*/);
  assert.doesNotMatch(
    `${header}\n${source}\n${cmake}`,
    /inkloop\/storage|inkloop_storage|PosixLegacy|EspLegacyDisplay|TaskStore|AlbumStore|fopen|unlink|rename\s*\(|nvs_(?:set|erase|commit)|mkfs/,
  );
  assert.match(source, /request\.path == "\/api\/recovery\/actions"/);
  assert.match(source, /request\.path == "\/api\/recovery\/actions\/resolve"/);
  assert.match(source, /application\/x-www-form-urlencoded/);
  assert.match(source, /request\.content_length != request\.body\.size\(\)/);
  assert.match(source, /request\.origin\.empty\(\)/);
  assert.match(source, /confirm != "resolve"/);
  assert.match(source, /RecoveryActionDomain::Display[\s\S]*RecoveryActionChoice::Next/);
});

test("browser requires explicit choice and confirmation with display semantics", () => {
  const html = source.split('R"INKLOOP_RECOVERY(')[1]
    ?.split(')INKLOOP_RECOVERY"')[0];
  assert.ok(html);
  assert.match(html, /采用目标画面并完成事务/);
  assert.match(html, /保留上一画面并放弃事务/);
  assert.match(html, /Current、Next、Previous 仅是物理槽位名/);
  assert.match(html, /不保证新旧来源/);
  assert.match(html, /采用 Current 槽位/);
  assert.match(html, /采用 Next 槽位/);
  assert.match(html, /采用 Previous 槽位/);
  assert.doesNotMatch(
    html,
    /Current 是已提交版本|当前已提交|最新待提交|上一回滚/,
  );
  assert.match(html, /条目：/);
  assert.match(html, /更新时间：/);
  assert.match(html, /摘要、条目数和文件时间核对差异/);
  assert.match(html, /必须手动选择一项并勾选明确确认/);
  assert.match(
    html,
    /radio\.disabled=.*recoverable.*choice_required/,
  );
  assert.match(html, /snapshot.*dataset\.snapshot/);
  assert.match(html, /\/api\/recovery\/actions\/resolve/);
  assert.match(html, /value\.result!==['"]resolved['"]/);
  assert.match(html, /owner 已确认恢复操作成功/);
  assert.doesNotMatch(html, /checked\s*=\s*(?:true|['"]checked['"])/);
  assert.doesNotMatch(html, /innerHTML|document\.write|eval\s*\(/);
  const scripts = html.split("<script>").slice(1).map(
    (part) => part.split("</script>")[0],
  );
  assert.equal(scripts.length, 1);
  assert.doesNotThrow(() => new Function(scripts[0]));
  assert.ok(Buffer.byteLength(html) <= 16384);
});
