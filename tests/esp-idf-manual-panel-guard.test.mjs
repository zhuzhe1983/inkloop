import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const include = join(product, "include");

const display = readFileSync(join(product, "native_display_service.cpp"), "utf8");
const power = readFileSync(join(product, "native_power_owner.cpp"), "utf8");
const portal = readFileSync(join(product, "native_portal_owner.cpp"), "utf8");
const voice = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
const inkloop = readFileSync(join(product, "native_inkloop_service.cpp"), "utf8");

function section(text, startText, endText) {
  const start = text.indexOf(startText);
  assert.notEqual(start, -1, `missing ${startText}`);
  const end = text.indexOf(endText, start + startText.length);
  assert.ok(end > start, `missing ${endText}`);
  return text.slice(start, end);
}

test("manual panel guard is strict C++17 and wrap-safe for exactly 30 seconds", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-panel-guard-"));
  const harness = join(scratch, "manual_panel_guard.cpp");
  const binary = join(scratch, "manual_panel_guard");
  writeFileSync(harness, String.raw`
#include <cassert>
#include <cstdint>
#include "inkloop/manual_panel_guard.hpp"

int main() {
  inkloop::ManualPanelGuard guard;
  assert(!guard.active(0));
  guard.noteCompletedUserRefresh(1000U);
  assert(guard.active(1000U));
  assert(guard.active(30999U));
  assert(!guard.active(31000U));

  guard.noteCompletedUserRefresh(50000U);
  assert(guard.active(50001U));
  guard.reset();
  assert(!guard.active(50001U));

  guard.noteCompletedUserRefresh(UINT32_MAX - 10000U);
  assert(guard.active(UINT32_MAX - 1U));
  assert(guard.active(19998U));
  assert(!guard.active(19999U));

  inkloop::ManualPanelGuard invalid_zero(0U);
  invalid_zero.noteCompletedUserRefresh(1U);
  assert(!invalid_zero.active(1U));
  inkloop::ManualPanelGuard invalid_half(0x80000000U);
  invalid_half.noteCompletedUserRefresh(1U);
  assert(!invalid_half.active(1U));

  // Model a background Display command that was admitted immediately before
  // the user refresh completed. Consumption must not write during the hold;
  // the same scheduled work may be retried exactly at the deadline.
  inkloop::ManualPanelGuard admitted_race;
  unsigned panel_writes = 0U;
  const auto consume_scheduled = [&](std::uint32_t now) {
    if (admitted_race.active(now)) return false;
    ++panel_writes;
    return true;
  };
  admitted_race.noteCompletedUserRefresh(9000U);
  assert(!consume_scheduled(9000U));
  assert(!consume_scheduled(38999U));
  assert(panel_writes == 0U);
  assert(consume_scheduled(39000U));
  assert(panel_writes == 1U);
  return 0;
}
`);
  try {
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", include,
      harness,
      join(product, "manual_panel_guard.cpp"),
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

test("only a completed real user refresh arms the panel hold", () => {
  const render = section(
    display,
    "bool NativeDisplayService::renderOrdinalAdmitted(size_t ordinal,",
    "NativeDisplayDiagnostics NativeDisplayService::diagnostics() const",
  );
  const unchanged = render.indexOf("++diagnostics_.unchanged_skips");
  const write = render.indexOf("writePanelFrame(frame.get(), frame.size())");
  const persist = render.indexOf("album_store_->markCurrent(asset.id)");
  const arm = render.indexOf("manual_panel_guard_.noteCompletedUserRefresh(nowMs())");
  const finish = render.lastIndexOf("navigation_.finish(index.assets.size(), ordinal)");
  assert.ok(unchanged >= 0 && unchanged < write);
  assert.ok(write >= 0 && persist > write && arm > persist && finish > arm);
  assert.match(render, /if \(user_initiated\) manual_panel_guard_/);

  const selection = section(
    display,
    "void NativeDisplayService::service()",
    "bool NativeDisplayService::writePanelFrame",
  );
  assert.match(selection, /renderOrdinal\(ordinal, true\)/);

  const handler = section(
    display,
    "WorkDisposition NativeDisplayService::handle",
    "bool NativeDisplayService::renderOrdinal(size_t ordinal,",
  );
  assert.match(handler, /DisplayInteractiveAlbumOrdinal/);
  assert.match(
    handler,
    /scheduled_background && manualPanelHoldActive\(nowMs\(\)\)[\s\S]*WorkDisposition::Busy/,
  );
  assert.ok(
    handler.indexOf("manualPanelHoldActive(nowMs())") <
      handler.lastIndexOf("renderOrdinal("),
  );
  assert.match(
    handler,
    /renderOrdinal\([\s\S]{0,80}ordinal, user_initiated, constrained_asset\)/,
  );
});

test("background writers defer while interactive paths remain explicit", () => {
  const defer = section(
    power,
    "bool NativePowerOwner::deferBackgroundPanel",
    "void NativePowerOwner::refreshBlockers",
  );
  assert.match(defer, /wake_hold \|\| display_\.manualPanelHoldActive\(now_ms\)/);

  const portalDisplay = section(
    portal,
    "AdmissionResult NativePortalOwner::requestDisplay",
    "void NativePortalOwner::serviceCommand",
  );
  assert.match(portalDisplay, /DisplayInteractiveAlbumOrdinal/);

  const aigc = section(
    voice,
    "void NativeVoiceService::serviceAigc",
    "void NativeVoiceService::finishAigc",
  );
  assert.match(aigc, /DisplayInteractiveAlbumOrdinal/);
  assert.match(aigc, /DisplayDiagnosticAigcOrdinal/);

  assert.match(inkloop, /ProductOpcode::DisplayAlbumOrdinal/);
  assert.doesNotMatch(inkloop, /DisplayInteractiveAlbumOrdinal/);
});
