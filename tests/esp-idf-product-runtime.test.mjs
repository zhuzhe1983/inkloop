import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf");
const product = join(idf, "components/inkloop_product");

test("native composition starts pinned owners and arms buttons last", () => {
  const source = readFileSync(join(product, "product_runtime.cpp"), "utf8");
  const voice = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
  const main = readFileSync(join(idf, "main/app_main.cpp"), "utf8");
  const begin = source.slice(source.indexOf("esp_err_t EspProductRuntime::begin"));
  const wifiAt = begin.indexOf("wifi_.initialize");
  const supervisorAt = begin.indexOf("supervisor_.start");
  const recoveryAt = begin.indexOf("power_.afterSupervisorStarted");
  assert.ok(wifiAt >= 0 && supervisorAt > wifiAt && recoveryAt > supervisorAt);
  assert.match(source, /registerHandler\([\s\S]*TaskLane::Control/);
  assert.match(voice, /TaskLane::Voice[\s\S]*TaskLane::Storage/);
  assert.match(source, /display_\.configure\(\)/);
  assert.match(source, /registerTickHandler\([\s\S]*TaskLane::Network[\s\S]*10/);
  assert.match(source, /unavailable lane rejected/);
  assert.match(source, /power_\.recovering\(\)/);
  assert.match(source, /power_\.noteButtonActivity\(nowMs\(\)\)/);
  assert.doesNotMatch(source, /unavailableHandler[\s\S]{0,400}WorkDisposition::Complete/);
  assert.match(main, /runReadOnlyUpgradeBootAudit[\s\S]*board_initialize\(\)[\s\S]*runtime\.begin\(\)/);
  assert.match(main, /NativeDeviceStateOwner device_state[\s\S]*device_state\.initialize\(\)[\s\S]*device_state\.effectiveAssetPreference\(\)/);
  assert.match(
    main,
    /attachStorageMaintenanceCoordinator\(runtime\)[\s\S]*attachPortalSettingsOwner\(device_state\)/,
  );
  assert.match(main, /attachPortalSettingsOwner\(device_state\)[\s\S]*attachPortalAlbumMutationOwner\(device_state\)[\s\S]*attachPortalFirmwareUpdateOwner\([\s\S]*attachLocalTools\(device_state\)[\s\S]*runtime\.begin\(\)/);
  assert.match(main, /local_management_password_override[\s\S]*setLocalAccessCodeOverride/);
  assert.match(source, /asset_preference[\s\S]*selectedAlbumStore\([\s\S]*asset_preference/);
  assert.match(source, /wifi\.provisioning_ap[\s\S]*requestProvisioningPage/);
  assert.match(source, /pairing_view_available[\s\S]*requestMyAiPairingPage/);
  assert.match(source, /NativeDisplayPageRequestResult::Unchanged[\s\S]*visible_provisioning_fingerprint_/);
  assert.doesNotMatch(source, /requestProvisioningPage[\s\S]{0,800}WifiStationPhase::Connecting/);
  const power = readFileSync(join(product, "native_power_owner.cpp"), "utf8");
  assert.match(power, /inkloop_\.nextTaskEpoch\(now, next_task\)/);
  assert.doesNotMatch(power, /PosixTaskStore|task_schedule_/);
});

test("voice, AIGC cache and album display are native product owners", () => {
  const source = readFileSync(join(product, "product_runtime.cpp"), "utf8");
  const voice = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
  assert.match(source, /RawButtonVoice[\s\S]*voice_\.enqueueTopButton\(\)/);
  assert.match(source, /handleNetworkCommand\(envelope\)/);
  assert.match(source, /voice_\.networkTick\(wifi_\.online\(\)\)/);
  assert.match(voice, /TaskLane::Voice[\s\S]*voiceTick[\s\S]*5/);
  assert.match(voice, /serviceVoice\(\)[\s\S]*servicePlayback[\s\S]*captureStep/);
  assert.match(voice, /handleNetworkCommand[\s\S]*beginVoiceTurn[\s\S]*disconnectVoice/);
  assert.match(voice, /onTranscript[\s\S]*if \(!final\) return;[\s\S]*ProductTextKind::AsrFinal/);
  assert.match(voice, /PortalRunAigc[\s\S]*serviceAigc/);
  assert.match(voice, /kAigcPollMs = 5000/);
  assert.match(voice, /AigcAlbumSink[\s\S]*takeCommittedAsset/);
  assert.doesNotMatch(voice, /audio_base64|appendAudio|StorageAppendAudio/);
  assert.match(source, /RawButtonPrevious[\s\S]*RawButtonNext[\s\S]*selectRelative/);
  assert.match(source, /AlbumRefreshStarting[\s\S]*enqueueAlbumOrdinal/);
  assert.match(voice, /VoicePromptRefreshOrdinal[\s\S]*local_prompts_\.requestOrdinal/);
});

test("Display results invalidate the Portal album cache without Control-lane I/O", () => {
  const runtime = readFileSync(
    join(product, "product_runtime.cpp"),
    "utf8",
  );
  const portalHeader = readFileSync(
    join(product, "include/inkloop/native_portal_owner.hpp"),
    "utf8",
  );
  const portal = readFileSync(
    join(product, "native_portal_owner.cpp"),
    "utf8",
  );
  const controlAt = runtime.indexOf("WorkDisposition EspProductRuntime::handleControl");
  const networkAt = runtime.indexOf("WorkDisposition EspProductRuntime::handleNetwork", controlAt);
  const control = runtime.slice(controlAt, networkAt);
  assert.match(
    control,
    /EnvelopeKind::Result[\s\S]{0,120}WorkClass::Display[\s\S]{0,120}portal_\.requestAlbumRefresh\(\)/,
  );
  assert.ok(
    control.indexOf("portal_.requestAlbumRefresh()") <
      control.indexOf("voice_.handleControlResult(envelope)"),
  );
  assert.match(portalHeader, /std::atomic<uint32_t>\s+album_refresh_generation_/);
  assert.match(
    portal,
    /void NativePortalOwner::requestAlbumRefresh\(\)[\s\S]{0,420}fetch_add\(1U, std::memory_order_acq_rel\)/,
  );
  assert.match(
    portal,
    /requested_album_generation[\s\S]{0,260}tryRefreshAlbum\(\)[\s\S]{0,160}album_refresh_applied_generation_\s*=\s*requested_album_generation/,
  );
});
