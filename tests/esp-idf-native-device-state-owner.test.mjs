import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf/components");
const product = join(idf, "inkloop_product");
const settingsIdf = join(idf, "inkloop_settings_idf");
const header = readFileSync(join(
  product, "include/inkloop/native_device_state_owner.hpp"), "utf8");
const source = readFileSync(
  join(product, "native_device_state_owner.cpp"), "utf8");

function body(name) {
  const start = source.indexOf(`NativeDeviceStateOwner::${name}`);
  assert.notEqual(start, -1, `${name} is missing`);
  const open = source.indexOf("{", start);
  assert.notEqual(open, -1, `${name} has no body`);
  let depth = 0;
  for (let at = open; at < source.length; at += 1) {
    if (source[at] === "{") depth += 1;
    if (source[at] === "}") depth -= 1;
    if (depth === 0) return source.slice(open + 1, at);
  }
  assert.fail(`${name} body is unterminated`);
}

test("native device state is one CAS settings owner with read-only Arduino import", () => {
  const nvs = readFileSync(
    join(settingsIdf, "esp_nvs_settings_store.cpp"), "utf8");
  assert.match(
    header,
    /public local_tools::ILocalToolsAdapter,[\s\S]*public IPortalSettingsOwner,[\s\S]*public IPortalAlbumMutationOwner/,
  );
  assert.match(body("initialize"), /store_\.load\(snapshot_\)/);
  assert.match(
    body("initialize"),
    /snapshot_\.generation == 0U[\s\S]*inspectLegacyPortalSettings[\s\S]*store_\.save\(candidate\.values, 0U, committed\)/,
  );
  assert.match(
    body("commitLocked"),
    /store_\.save\(next, snapshot_\.generation, committed\)/,
  );
  assert.match(nvs, /nvs_open\(kLegacyNamespace, NVS_READONLY, &handle\)/);
  assert.doesNotMatch(
    body("initialize"),
    /nvs_set_|nvs_erase|Preferences|\/littlefs|\/sd/,
  );
  assert.match(body("defaultsFor"), /defaultAssistantPrompt\(board\)/);
  assert.match(body("defaultsFor"), /defaultImagePromptTemplate\(board\)/);
  assert.match(body("defaultsFor"), /defaultNegativePrompt\(board\)/);
  assert.doesNotMatch(body("defaultsFor"), /PaperColor|\b400\b|\b600\b/);
});

test("Portal and local-tools fields map to the same validated settings snapshot", () => {
  const portalApply = body("applyPortalSettings");
  for (const mapping of [
    /patch\.has_volume[\s\S]*next\.volume_percent = patch\.volume/,
    /patch\.has_led_maximum_brightness[\s\S]*next\.led_maximum_brightness_percent/,
    /patch\.has_voice_assistance_enabled[\s\S]*next\.voice_assistance_enabled/,
    /patch\.has_assistant_prompt[\s\S]*next\.assistant_prompt/,
    /patch\.has_image_prompt_template[\s\S]*next\.aigc_prompt_template/,
    /patch\.has_negative_prompt[\s\S]*next\.negative_prompt/,
    /patch\.has_asset_storage_preference[\s\S]*next\.asset_storage_preference/,
    /patch\.has_default_render_strategy[\s\S]*next\.default_render_strategy/,
    /patch\.has_local_management_password_override[\s\S]*next\.local_management_password_override/,
  ]) assert.match(portalApply, mapping);
  for (const method of [
    "setVolume", "setAssistantPrompt", "setAigcPrompt",
    "setLedMaximumBrightness",
  ]) {
    assert.match(body(method), /commitLocked\(next\)/);
  }
  assert.match(body("portalSnapshot"), /local_management_password_overridden/);
  assert.doesNotMatch(body("portalSnapshot"), /local_management_password_override\s*=/);
  assert.match(body("effectiveAssetPreference"), /boot_effective_preference_/);
  assert.doesNotMatch(body("effectiveAssetPreference"), /snapshot_/);
  assert.match(body("queryAigcNegativePrompt"), /negative_prompt/);
  assert.match(body("queryDefaultRenderStrategy"), /default_render_strategy/);
});

test("storage mutations fail closed before initialization and TF format has no internal target", () => {
  for (const method of [
    "deletePortalAlbumItem", "queryStorage", "deleteImageByOrdinal",
    "deleteImageById", "clearAlbum", "formatTfCard",
  ]) {
    assert.match(body(method), /if \(!initialized_\)/, `${method} lacks ready gate`);
  }
  const format = body("formatTfCard");
  assert.match(header, /attachStorageMaintenanceCoordinator/);
  assert.match(format, /storage_maintenance_->formatTfCardConfirmed\(\)/);
  assert.doesNotMatch(
    format,
    /storage_\.formatSdCardConfirmed|AssetStoragePreference|Internal|LittleFS|littlefs|internal_base_path/,
  );
});
