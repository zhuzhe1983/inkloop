import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const portal = join(repo, "firmware/inkloop-idf/components/inkloop_portal");
const owner = readFileSync(join(product, "native_portal_owner.cpp"), "utf8");
const ownerHeader = readFileSync(
  join(product, "include/inkloop/native_portal_owner.hpp"), "utf8");
const core = readFileSync(join(portal, "portal_core.cpp"), "utf8");
const coreHeader = readFileSync(
  join(portal, "include/inkloop/portal/portal_core.hpp"), "utf8");

function between(source, begin, end) {
  const start = source.indexOf(begin);
  const finish = source.indexOf(end, start + begin.length);
  assert.ok(start >= 0 && finish > start, `${begin} section missing`);
  return source.slice(start, finish);
}

test("Portal lane caches one coherent numeric RuntimeSupervisor snapshot", () => {
  const refresh = between(
    owner,
    "void NativePortalOwner::refreshState()",
    "void NativePortalOwner::refreshAlbum()",
  );
  assert.equal((refresh.match(/supervisor_\.telemetry\(\)/g) ?? []).length, 1);
  assert.match(refresh, /static_assert\(kTaskLaneCount == portal::kPortalRuntimeLaneCount/);
  assert.match(refresh, /next\.runtime\.lanes\[index\]/);
  assert.match(refresh, /state_cache_ = std::move\(next\)/);
  assert.match(refresh, /initialized_[\s\S]*next_runtime_summary_ms_[\s\S]*ESP_LOGI/);
  assert.match(owner, /kRuntimeSummaryMs = 60000U/);
  assert.match(ownerHeader, /next_runtime_summary_ms_/);

  const readState = between(
    owner,
    "portal::PortalResult NativePortalOwner::readState",
    "portal::PortalResult NativePortalOwner::readAlbumPage",
  );
  assert.doesNotMatch(readState, /telemetry\(|uxTask|heap_caps|esp_timer/);
  assert.match(owner, /state_cache_\.display_width = board_descriptor\.width/);
  assert.match(owner, /state_cache_\.display_height = board_descriptor\.height/);
});

test("runtime state contract is fixed-size and validates every lane before JSON", () => {
  assert.match(coreHeader, /std::array<PortalRuntimeLaneTelemetry, kPortalRuntimeLaneCount>/);
  assert.match(coreHeader, /uint8_t lane_count = 0/);
  assert.doesNotMatch(
    between(
      coreHeader,
      "struct PortalRuntimeLaneTelemetry",
      "struct PortalRenderStrategyCapability",
    ),
    /std::string|std::vector|char\s*\*|void\s*\*/,
  );
  const validation = between(
    core,
    "bool validRuntimeTelemetry",
    "bool validState",
  );
  assert.match(validation, /runtime\.lane_count/);
  assert.match(validation, /lane\.queue_depth > lane\.queue_high_water/);
  assert.match(validation, /lane\.queue_high_water > lane\.queue_capacity/);
  assert.match(validation, /lane\.configured_core < 0/);
  assert.match(validation, /lane\.observed_core < -1/);
  assert.match(validation, /!lane\.stack_sampled/);
  assert.match(core, /validRuntimeTelemetry\(state\.runtime\)/);
  assert.ok(core.includes("runtimeTelemetry"));
  assert.ok(core.includes("lanes"));
  assert.doesNotMatch(core, /\"taskName\"|\"taskHandle\"|\"address\"/);
});

test("shared Portal defaults are SKU-neutral and mDNS uses the board id", () => {
  assert.match(coreHeader, /uint16_t display_width = 0U/);
  assert.match(coreHeader, /uint16_t display_height = 0U/);
  assert.match(core, /state\.display_width > 0U/);
  assert.match(core, /state\.display_height > 0U/);
  assert.match(owner, /state_cache_\.display_width = board_descriptor\.width/);
  assert.match(owner, /state_cache_\.display_height = board_descriptor\.height/);
  assert.match(owner, /"Inkloop %\.55s"/);
  assert.match(owner, /mdns_instance_name_set\(mdns_instance_name_\.data\(\)\)/);
  assert.doesNotMatch(owner, /Inkloop PaperColor/);
  assert.match(ownerHeader, /std::array<char, 64> mdns_instance_name_/);
  assert.match(owner, /mdns_hostname_set\("inkloop"\)/);
});

test("WebUI reuses visible authenticated state polling and preserves dynamic canvas", () => {
  const html = core.split('R"INKLOOP(')[1]?.split(')INKLOOP"')[0] ?? "";
  assert.match(html, /runtime-diagnostics/);
  assert.match(html, /runtimeTelemetry/);
  assert.match(html, /response\.clone\(\)\.json\(\)/);
  assert.match(html, /visibilitychange/);
  assert.match(html, /portal\.hidden/);
  assert.match(html, /cadence=5000/);
  assert.doesNotMatch(html, /setInterval\s*\(/);
  assert.match(html, /state\.displayWidth/);
  assert.match(html, /state\.displayHeight/);
  assert.match(html, /canvas\.style\.aspectRatio=width\+'\/'\+height/);
});
