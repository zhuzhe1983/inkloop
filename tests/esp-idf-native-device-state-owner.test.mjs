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
const appMain = readFileSync(join(
  repo, "firmware/inkloop-idf/main/app_main.cpp"), "utf8");
const voiceSource = readFileSync(join(
  product, "native_voice_service.cpp"), "utf8");

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

test("native device state consumes only a completed migration authorization", () => {
  const nvs = readFileSync(
    join(settingsIdf, "esp_nvs_settings_store.cpp"), "utf8");
  assert.match(
    header,
    /public local_tools::ILocalToolsAdapter,[\s\S]*public IPortalSettingsOwner,[\s\S]*public IPortalAlbumMutationOwner/,
  );
  assert.match(
    body("initialize"),
    /loadRollbackCompatibleSettings\([\s\S]*store_, extension_store_, snapshot_\)/,
  );
  assert.match(
    body("initialize"),
    /authorization\.kind == NativeSettingsAuthorityKind::FreshDefaults[\s\S]*snapshot_\.generation == 0U/,
  );
  assert.match(
    body("initialize"),
    /authorization\.kind == NativeSettingsAuthorityKind::NativeJournal[\s\S]*snapshot_\.generation == authorization\.observed_generation[\s\S]*authorization\.migration_generation/,
  );
  assert.doesNotMatch(
    body("initialize"),
    /inspectLegacyPortalSettings|legacy_|saveRollbackCompatibleSettings/,
  );
  assert.match(
    body("commitLocked"),
    /saveRollbackCompatibleSettings\([\s\S]*store_, extension_store_,[\s\S]*next, snapshot_\.generation, committed\)/,
  );
  assert.match(header, /EspNvsSettingsExtensionJournalStore extension_journal_/);
  assert.match(header, /SettingsExtensionStoreCore extension_store_/);
  assert.match(
    body("commitLocked"),
    /snapshot_ = std::move\(committed\)[\s\S]*return status/,
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
    /patch\.has_led_roles_swapped[\s\S]*next\.led_roles_swapped/,
    /patch\.has_voice_assistance_enabled[\s\S]*next\.voice_assistance_enabled/,
    /patch\.has_assistant_prompt[\s\S]*next\.assistant_prompt/,
    /patch\.has_image_prompt_template[\s\S]*next\.aigc_prompt_template/,
    /applyNativeDeviceAigcSettingsPatch\(patch, next\)/,
    /patch\.has_negative_prompt[\s\S]*next\.negative_prompt/,
    /patch\.has_asset_storage_preference[\s\S]*next\.asset_storage_preference/,
    /patch\.has_default_render_strategy[\s\S]*next\.default_render_strategy/,
    /patch\.has_local_management_password_override[\s\S]*next\.local_management_password_override/,
  ]) assert.match(portalApply, mapping);
  for (const method of [
    "setVolume", "setAssistantPrompt", "setAigcPrompt", "setAigcSteps",
    "setAigcNegativePrompt", "setDefaultRenderStrategy",
    "setLedMaximumBrightness",
  ]) {
    assert.match(body(method), /commitLocked\(next\)/);
  }
  assert.match(body("portalSnapshot"), /local_management_password_overridden/);
  assert.match(body("portalSnapshot"), /led_roles_swapped/);
  assert.doesNotMatch(body("portalSnapshot"), /local_management_password_override\s*=/);
  assert.match(body("effectiveAssetPreference"), /boot_effective_preference_/);
  assert.doesNotMatch(body("effectiveAssetPreference"), /snapshot_/);
  assert.match(body("queryAigcNegativePrompt"), /negative_prompt/);
  assert.match(
    body("queryAigcSteps"),
    /nativeDeviceAigcSteps\(snapshot_\.values\)/,
  );
  assert.match(body("setAigcSteps"), /kMinimumAigcSteps/);
  assert.match(body("setAigcSteps"), /kMaximumAigcSteps/);
  assert.match(body("setAigcSteps"), /next\.aigc_steps = steps/);
  assert.match(body("queryDefaultRenderStrategy"), /default_render_strategy/);
  assert.match(body("setAigcNegativePrompt"), /validNegativePrompt/);
  assert.match(body("setAigcNegativePrompt"), /next\.negative_prompt = prompt/);
  assert.match(body("setDefaultRenderStrategy"), /validRenderStrategyId/);
  assert.match(body("setDefaultRenderStrategy"), /renderStrategyCatalog\(\)/);
  assert.match(body("setDefaultRenderStrategy"), /catalog\.contains\(strategy\)/);
  assert.match(body("setDefaultRenderStrategy"), /supportsRenderStrategy\(strategy\)/);
  assert.match(header, /NativeDeviceStateOwner\([^;]*IBoardRenderer& renderer\)/s);
  assert.match(appMain, /IBoardRenderer\* const renderer = board_adapter\.renderer\(\)/);
  assert.match(appMain, /if \(!renderer\)[\s\S]*failPendingBoot\("board_renderer"\)/);
  assert.match(
    appMain,
    /NativeDeviceStateOwner device_state\(\s*board, storage, \*renderer\)/,
  );
});

test("MyAI image requests consume the atomically persisted image steps", () => {
  const start = voiceSource.indexOf("void NativeVoiceService::serviceAigc");
  const end = voiceSource.indexOf("void NativeVoiceService::finishAigc", start);
  assert.ok(start >= 0 && end > start);
  const aigc = voiceSource.slice(start, end);
  assert.match(aigc, /queryAigcSteps\(configured_steps\)/);
  assert.match(aigc, /configured_steps < local_tools::kMinimumAigcSteps/);
  assert.match(aigc, /configured_steps > local_tools::kMaximumAigcSteps/);
  assert.match(aigc, /aigc_request_\.steps = configured_steps/);
  assert.doesNotMatch(aigc, /aigc_request_\.steps\s*=\s*20/);
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

test("local storage query uses the selected filesystem capacity adapter", () => {
  assert.doesNotMatch(source, /statvfs/);
  assert.match(source, /selectedAlbum\(\)/);
  assert.match(source, /queryCapacity\(output\.total_bytes, output\.remaining_bytes\)/);
});

test("album list and ordinal selection expose bounded metadata without selecting or leaking paths", () => {
  const summary = body("queryAlbumSummary");
  const selection = body("resolveImageByOrdinal");
  assert.match(summary, /readCatalog\(index\)/);
  assert.match(summary, /output\.count/);
  assert.match(summary, /output\.current_ordinal/);
  assert.match(selection, /readCatalog\(index\)/);
  assert.match(selection, /validAlbumAssetId\(selected\.id\)/);
  assert.match(selection, /output\.asset_id = selected\.id/);
  assert.match(selection, /output\.zero_based_index = one_based_ordinal - 1U/);
  assert.match(selection, /output\.ordinal = one_based_ordinal/);
  assert.match(selection, /output\.total/);
  for (const bounded of [summary, selection]) {
    assert.doesNotMatch(bounded, /absoluteAssetPath|\.path|selectedRoot|markCurrent/);
  }
  assert.doesNotMatch(selection, /writeFullFrame|renderRgbFullFrame/);
});
