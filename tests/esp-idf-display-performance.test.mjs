import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const root = new URL("../", import.meta.url).pathname;
const product = join(root, "firmware/inkloop-idf/components/inkloop_product");
const source = readFileSync(join(product, "native_display_service.cpp"), "utf8");
const header = readFileSync(
  join(product, "include/inkloop/native_display_service.hpp"),
  "utf8",
);

function body(start, end) {
  const from = source.indexOf(start);
  assert.notEqual(from, -1, `missing ${start}`);
  const to = source.indexOf(end, from + start.length);
  assert.notEqual(to, -1, `missing ${end}`);
  return source.slice(from, to);
}

test("album diagnostics separate load/decode, conversion and physical panel time", () => {
  for (const field of [
    "last_load_decode_ms",
    "maximum_load_decode_ms",
    "last_conversion_ms",
    "maximum_conversion_ms",
    "last_panel_refresh_ms",
    "maximum_panel_refresh_ms",
    "last_album_total_ms",
    "maximum_album_total_ms",
    "completed_album_refreshes",
    "panel_writes",
  ]) assert.match(header, new RegExp(field));

  const render = body(
    "bool NativeDisplayService::renderOrdinalAdmitted",
    "NativeDisplayDiagnostics NativeDisplayService::diagnostics",
  );
  const decodeAt = render.indexOf("decodePngFile(");
  const conversionAt = render.indexOf("renderer->renderRgbFullFrame(");
  const panelAt = render.indexOf("writePanelFrame(");
  assert.ok(decodeAt >= 0 && conversionAt > decodeAt && panelAt > conversionAt);
  assert.match(render, /last_load_decode_ms\s*=\s*decoded_ms/);
  assert.match(render, /last_conversion_ms\s*=\s*converted_ms/);
  assert.match(render, /last_album_total_ms\s*=\s*total_ms/);
  assert.match(
    render,
    /album refresh timing load_decode_ms=%lu conversion_ms=%lu [\s\S]*panel_ms=%lu total_ms=%lu/,
  );

  const panel = body(
    "bool NativeDisplayService::writePanelFrame",
    "bool NativeDisplayService::renderOnboardingPage",
  );
  assert.match(panel, /const uint32_t started = nowMs\(\)/);
  assert.match(panel, /display->writeFullFrame\(view\)/);
  assert.match(panel, /last_panel_refresh_ms\s*=\s*elapsed/);
});

test("timing arithmetic is unsigned wrap-safe and telemetry contains no secrets", () => {
  assert.match(source, /const uint32_t elapsed = nowMs\(\) - started/);
  assert.match(source, /const uint32_t total_ms = nowMs\(\) - total_started/);
  const diagnostics = header.slice(
    header.indexOf("struct NativeDisplayDiagnostics"),
    header.indexOf("struct NativeProvisioningPageRequest"),
  );
  assert.doesNotMatch(
    diagnostics,
    /device_token|pairing_token|binding_url|access_value|password/i,
  );
});
