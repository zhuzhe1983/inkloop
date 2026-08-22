import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
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
const serverHeader = readFileSync(
  join(component, "include/inkloop/recovery/esp_recovery_server.hpp"),
  "utf8",
);
const core = readFileSync(join(component, "recovery_portal.cpp"), "utf8");
const server = readFileSync(join(component, "esp_recovery_server.cpp"), "utf8");
const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");

function between(source, begin, end) {
  const first = source.indexOf(begin);
  const last = source.indexOf(end, first + begin.length);
  assert.ok(first >= 0 && last > first, `${begin} section missing`);
  return source.slice(first, last);
}

test("diagnostic contract is fixed-size, typed and cannot carry raw content", () => {
  const snapshot = between(
    header,
    "enum class RecoveryReason",
    "enum class RecoveryReadResult",
  );
  assert.match(snapshot, /enum class RecoveryReason/);
  assert.match(snapshot, /enum class RecoveryPhase/);
  assert.match(snapshot, /enum class RecoveryOutcome/);
  assert.match(snapshot, /std::array<char, kMaximumRecoveryIdentifierBytes \+ 1U>/);
  assert.match(snapshot, /uint32_t nvs_namespaces/);
  assert.match(snapshot, /bool normal_startup_refused/);
  assert.doesNotMatch(snapshot, /std::string|std::vector|char\s*\*|void\s*\*/);
  assert.doesNotMatch(
    snapshot,
    /namespace_name|file_path|wifi|ssid|password|token|prompt|album_text|chat|url/i,
  );
});

test("portable core limits endpoints and fails closed before cache reads", () => {
  assert.match(core, /request\.path == "\/"/);
  assert.match(core, /request\.path == "\/api\/session"/);
  assert.match(core, /request\.path == "\/api\/diagnostics"/);
  assert.match(core, /route_not_found/);
  assert.match(core, /method_not_allowed/);
  assert.match(core, /session_issued_/);
  assert.match(core, /request\.now_seconds >= session_expires_at_seconds_/);
  assert.match(core, /constantTimeEqual/);
  assert.match(core, /csrf_forbidden/);
  assert.match(core, /normal_startup_refused/);
  assert.match(core, /validSnapshot/);
  const handler = between(
    core,
    "RecoveryResponse RecoveryPortalCore::handle(",
    "const char* RecoveryPortalCore::dashboardHtml()",
  );
  const auth = handler.indexOf("sessionAuthorized(request)");
  const read = handler.indexOf("renderDiagnostic()");
  assert.ok(auth >= 0 && read > auth);
});

test("native seam is synchronous, caller-owned and contains no recovery mutation", () => {
  assert.match(serverHeader, /esp_err_t start\(\)/);
  assert.match(serverHeader, /esp_err_t stop\(\)/);
  assert.match(serverHeader, /bool running\(\) const/);
  assert.match(server, /httpd_start/);
  assert.match(server, /httpd_stop/);
  assert.match(server, /core\.handle\(request\)/);
  assert.doesNotMatch(
    server,
    /xTaskCreate|xTimerCreate|esp_timer_create|setInterval|poll|while\s*\(true\)/,
  );
  assert.doesNotMatch(
    `${header}\n${serverHeader}\n${core}\n${server}`,
    /nvs_(?:set|erase|commit)|fopen|fwrite|unlink|remove\s*\(|format|mkfs|migrate\s*\(|esp_http_client|esp_websocket_client|AlbumStore|TaskStore|LocalChatLog|MyAiClient|ProductRuntime|NativeDisplay|NativeVoice/,
  );
  assert.doesNotMatch(server, /ESP_LOG|printf|puts\s*\(/);
  assert.match(cmake, /esp_http_server/);
  assert.match(cmake, /esp_timer/);
  assert.match(cmake, /lwip/);
  assert.doesNotMatch(cmake, /inkloop_product|inkloop_storage|inkloop_ota|inkloop_portal/);
});
