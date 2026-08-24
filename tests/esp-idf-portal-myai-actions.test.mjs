import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.cwd();
const product = join(root, "firmware/inkloop-idf/components/inkloop_product");
const voice = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
const voiceHeader = readFileSync(
  join(product, "include/inkloop/native_voice_service.hpp"), "utf8");
const portal = readFileSync(join(product, "native_portal_owner.cpp"), "utf8");
const portalHeader = readFileSync(
  join(product, "include/inkloop/native_portal_owner.hpp"), "utf8");
const opcodes = readFileSync(
  join(product, "include/inkloop/product_opcodes.hpp"), "utf8");

function body(source, signature, nextSignature) {
  const begin = source.indexOf(signature);
  assert.notEqual(begin, -1, `missing ${signature}`);
  const end = nextSignature ? source.indexOf(nextSignature, begin + signature.length) : source.length;
  assert.notEqual(end, -1, `missing boundary ${nextSignature}`);
  return source.slice(begin, end);
}

test("Portal commands admit bounded typed actions instead of fixed 503", () => {
  const admission = body(portal, "portal::PortalResult NativePortalOwner::tryEnqueue", "bool NativePortalOwner::takeCommand");
  for (const name of [
    "PreviewVolume", "StartMyAiPairing", "RebindMyAi",
    "GenerateImage", "ClearLocalChat",
  ]) {
    assert.match(admission, new RegExp(`PortalCommandType::${name}`));
  }
  const interactiveAdmission = body(
    admission,
    "case portal::PortalCommandType::PreviewVolume:",
    "case portal::PortalCommandType::RequestFirmwareUpdate:",
  );
  assert.doesNotMatch(
    interactiveAdmission,
    /return portal::PortalResult::Unavailable/,
  );
  const dispatch = body(portal, "void NativePortalOwner::serviceCommand", "bool NativePortalOwner::loadSettings");
  for (const call of [
    "enqueueVolumePreview", "enqueueStartMyAiPairing", "enqueueRebindMyAi",
    "enqueueImageGeneration", "enqueueClearLocalChat",
  ]) assert.match(dispatch, new RegExp(`voice_\\.${call}\\(`));
});

test("native Portal state copies the selected board's bounded capabilities", () => {
  const copy = body(portal, "bool copyBoardCapabilities", "bool boardSupportsRenderStrategy");
  assert.match(copy, /board\.descriptor\(\)/);
  assert.match(copy, /renderer->renderStrategyCatalog\(\)/);
  assert.match(copy, /catalog\.valid\(\)/);
  assert.match(copy, /descriptor\.has_microphone/);
  assert.match(copy, /descriptor\.has_speaker/);
  assert.match(copy, /descriptor\.rgb_pixels/);
  assert.match(copy, /descriptor\.has_sd/);
  assert.match(copy, /output\.render_strategy_count = catalog\.count/);
  assert.match(copy, /renderer->supportsRenderStrategy\(catalog\.entries\[index\]\.id\)/);
  const initialize = body(portal, "esp_err_t NativePortalOwner::initialize", "esp_err_t NativePortalOwner::attachSettingsOwner");
  assert.match(initialize, /copyBoardCapabilities\(board_, state_cache_\.capabilities\)/);
  assert.match(initialize, /return ESP_ERR_INVALID_STATE/);
  const admission = body(portal, "portal::PortalResult NativePortalOwner::tryEnqueue", "bool NativePortalOwner::takeCommand");
  assert.match(admission, /has_microphone[\s\S]*has_speaker/);
  assert.match(admission, /rgb_pixels == 0U/);
  assert.match(admission, /has_sd/);
  assert.match(admission, /boardSupportsRenderStrategy/);
});

test("Portal LED role settings stay SKU-neutral and apply through the sole LED owner", () => {
  const initialize = body(
    portal,
    "esp_err_t NativePortalOwner::initialize",
    "esp_err_t NativePortalOwner::attachSettingsOwner",
  );
  assert.match(
    initialize,
    /leds_\.setPresentation\([\s\S]*led_maximum_brightness_percent,[\s\S]*led_roles_swapped, false\)/,
  );
  const admission = body(
    portal,
    "portal::PortalResult NativePortalOwner::tryEnqueue",
    "bool NativePortalOwner::takeCommand",
  );
  assert.match(
    admission,
    /has_led_roles_swapped[\s\S]*board_\.descriptor\(\)\.rgb_pixels < 2U/,
  );
  const apply = body(
    portal,
    "void NativePortalOwner::applySettings",
    "void NativePortalOwner::refreshState",
  );
  assert.match(
    apply,
    /has_led_maximum_brightness \|\| patch\.has_led_roles_swapped[\s\S]*leds_\.setPresentation\([\s\S]*true\)/,
  );
});

test("native Portal publishes mDNS readiness and never claims a failed hostname", () => {
  const refresh = body(
    portal,
    "void NativePortalOwner::refreshState",
    "void NativePortalOwner::refreshAlbum",
  );
  assert.match(refresh, /next\.mdns_ready = mdns_started_/);
  const start = body(
    portal,
    "esp_err_t NativePortalOwner::startServer",
    "esp_err_t NativePortalOwner::stopServer",
  );
  assert.match(start, /state_cache_\.mdns_ready = mdns_started_/);
  assert.match(start, /if \(mdns_started_\)[\s\S]*http:\/\/inkloop\.local/);
  assert.match(start, /else \{[\s\S]*mDNS unavailable:[\s\S]*http:\/\/%s\//);
  assert.match(start, /acquirePortalMdnsServiceLease\(this\)/);
  assert.match(start, /initialized == ESP_OK \|\| initialized == ESP_ERR_INVALID_STATE/);
  assert.match(portalHeader, /bool mdns_service_lease_held_ = false/);
  assert.match(portal, /g_portal_mdns_service_owner\.compare_exchange_strong/);
  assert.doesNotMatch(portal, /mdns_free\s*\(/);
  const stop = body(
    portal,
    "esp_err_t NativePortalOwner::stopServer",
    "void NativePortalOwner::tick",
  );
  assert.match(stop, /mdns_service_remove[\s\S]*releasePortalMdnsServiceLease\(this\)/);
  assert.match(stop, /state_cache_\.mdns_ready = false/);
});

test("volume preview is explicit Voice-lane audio and restores persisted volume", () => {
  assert.match(opcodes, /VoicePreviewVolume = 14/);
  const preview = body(voice, "WorkDisposition NativeVoiceService::handleVolumePreview", "void NativeVoiceService::restoreVolumeAfterPreview");
  assert.match(preview, /setVolumePercent\(envelope\.flags\)/);
  assert.match(preview, /local_prompts_\.requestVolumePreview/);
  const restore = body(voice, "void NativeVoiceService::restoreVolumeAfterPreview", "WorkDisposition NativeVoiceService::handleTopButton");
  assert.match(restore, /setVolumePercent\(saved\)/);
  assert.match(restore, /hardware_volume_percent_ = saved/);
  assert.match(voice, /if \(!assistance_enabled && !safety_confirmation\)/);
  assert.match(voice, /explicit volume-preview action and destructive-operation confirmation/);
});

test("MyAI pairing and rebind remain Network-owned and never synthesize a public code", () => {
  assert.match(opcodes, /NetworkStartMyAiPairing = 203/);
  assert.match(opcodes, /NetworkRebindMyAi = 204/);
  const actions = body(voice, "void NativeVoiceService::serviceRequestedPairingActions", "void NativeVoiceService::publishOnboarding");
  assert.match(actions, /client_->resetCredentialForRebind\(\)/);
  assert.match(actions, /client_->pendingPairing\(pending\)/);
  assert.match(actions, /activation_state_ != myai::ActivationState::Unconfigured/);
  assert.match(actions, /publishOnboarding\(&pending\)/);
  const publish = body(voice, "void NativeVoiceService::publishOnboarding", "void NativeVoiceService::handoffAigcIfReady");
  assert.match(publish, /sixDigits\(pairing->onboardingCode\)/);
  assert.match(publish, /copyBounded\(pairing->bindingUrl/);
  assert.doesNotMatch(publish, /esp_random|pairingToken|deviceToken/);
});

test("Portal distinguishes a token from live MyAI authorization and reports real SemVer", () => {
  assert.match(voiceHeader, /bool authorization_verified = false/);
  const network = body(
    voice,
    "void NativeVoiceService::networkTick",
    "myai::Status NativeVoiceService::startPairingNow",
  );
  assert.match(network, /checkAuthorization\(authorized\)/);
  assert.match(network, /authorization_verified_ = checked\.ok\(\) && authorized/);
  assert.match(network, /if \(!authorization_verified_\) return;[\s\S]*handoffAigcIfReady/);
  assert.match(network, /publishOnboarding\(nullptr\)/);
  const publish = body(
    voice,
    "void NativeVoiceService::publishOnboarding",
    "void NativeVoiceService::handoffAigcIfReady",
  );
  assert.match(publish, /next\.authorization_verified = authorization_verified_/);
  assert.match(portal, /authorization_verified \? portal::MyAiPortalState::Active/);
  assert.match(portal, /ActivationState::PaymentRequired[\s\S]*MyAiPortalState::PaymentRequired/);
  assert.match(portal, /ActivationState::RecoveryRequired[\s\S]*MyAiPortalState::RecoveryRequired/);
  assert.match(portal, /voice_\.myAiErrorSnapshot\(\)/);
  const error = body(
    voice,
    "void NativeVoiceService::onError",
    "NativeVoiceDiagnostics NativeVoiceService::diagnostics",
  );
  assert.match(error, /network_diagnostic_operation_/);
  assert.match(error, /myai_error_\.source = source/);
  assert.match(error, /myai_error_\.http_status = http_status/);
  assert.match(error, /sanitizeDiagnosticDetail\(status\.detail\)/);
  assert.match(error, /myai_error_\.detail\.fill/);
  assert.match(error, /queueChat\(ProductTextKind::ToolState, message\)/);
  assert.doesNotMatch(error, /status\.detail\.c_str\(\)/);
  assert.match(portal, /next\.myai_error\.detail = myai_error\.detail\.data\(\)/);
  assert.match(portal, /esp_app_get_description\(\)/);
  assert.doesNotMatch(portal, /idf-native/);
});

test("manual image generation uses the real AIGC album/display path and saved policies", () => {
  assert.match(opcodes, /NetworkQueueAigc = 205/);
  const enqueue = body(voice, "AdmissionResult NativeVoiceService::enqueueImageGeneration", "AdmissionResult NativeVoiceService::enqueueClearLocalChat");
  assert.match(enqueue, /text_pool_\.put\(ProductTextKind::AigcState, prompt\)/);
  assert.match(enqueue, /NetworkQueueAigc/);
  const aigc = body(voice, "void NativeVoiceService::serviceAigc", "void NativeVoiceService::finishAigc");
  assert.match(aigc, /queryAigcNegativePrompt\(configured_negative\)/);
  assert.match(aigc, /queryDefaultRenderStrategy/);
  assert.match(aigc, /renderer->renderStrategyCatalog\(\)/);
  assert.match(aigc, /catalog\.valid\(\)/);
  assert.match(aigc, /catalog\.contains\(configured_render_strategy\)/);
  assert.match(aigc, /renderer->supportsRenderStrategy\(configured_render_strategy\)/);
  assert.match(aigc, /aigc\.render_strategy_fallback selected=official-quality/);
  assert.doesNotMatch(aigc, /storage::validRenderStrategy/);
  assert.match(aigc, /AigcAlbumSink sink\(\*album_store_[\s\S]*aigc_render_strategy_/);
  assert.match(aigc, /DisplayInteractiveAlbumOrdinal/);
  assert.doesNotMatch(aigc, /negativePrompt\s*=\s*"细小文字/);
  assert.doesNotMatch(aigc, /AigcAlbumSink sink\([\s\S]*"reflectance-photo"/);
  const upload = body(portal, "void NativePortalOwner::finishActiveUpload", "void NativePortalOwner::serviceUpload");
  assert.match(upload, /state_cache_\.settings\.default_render_strategy/);
  assert.match(upload, /state_cache_\.capabilities\.supportsRenderStrategy/);
  assert.match(upload, /boardSupportsRenderStrategy/);
});

test("local chat clear is a sole Storage-lane operation and publishes an empty snapshot", () => {
  assert.match(opcodes, /StorageClearLocalChat = 302/);
  const enqueue = body(voice, "AdmissionResult NativeVoiceService::enqueueClearLocalChat", "esp_err_t NativeVoiceService::seedPersistedVoiceSettings");
  assert.match(enqueue, /beginTrackedStorageWork\(\)/);
  assert.match(enqueue, /WorkClass::Storage/);
  const storage = body(voice, "WorkDisposition NativeVoiceService::handleStorage", "WorkDisposition NativeVoiceService::readLocalChatSnapshot");
  assert.match(storage, /chat_log_->clear\(\)/);
  assert.match(storage, /clearChatSnapshotMailbox\(\)/);
  assert.match(storage, /chat_snapshot_ready_ = true/);
  assert.doesNotMatch(voice, /chat_snapshot_mailbox_\s*=\s*NativeLocalChatSnapshot\{\}/);
});

test("Portal storage maintenance is fail-closed, drained, and cache-safe", () => {
  assert.match(portalHeader, /bool beginStorageMaintenance\(\)/);
  assert.match(portalHeader, /bool finishStorageMaintenance\(bool storage_changed,[\s\S]*bool storage_available\)/);
  const begin = body(portal, "bool NativePortalOwner::beginStorageMaintenance", "bool NativePortalOwner::finishStorageMaintenance");
  assert.match(begin, /storage_maintenance_active_ = true/);
  assert.match(begin, /storage_http_operations_ != 0U/);
  assert.match(begin, /preview_busy \|\| mutationBusy\(\)/);
  assert.match(begin, /stopServer\(\)/);
  assert.match(begin, /endStorageMaintenance\(\);[\s\S]*return false/);
  const finish = body(portal, "bool NativePortalOwner::finishStorageMaintenance", "void NativePortalOwner::endStorageMaintenance");
  assert.match(finish, /album_cache_ready_ = false/);
  assert.match(finish, /chat_cache_ready_ = false/);
  assert.match(finish, /storage_available_ = storage_available/);
  const tick = body(portal, "void NativePortalOwner::tick", "bool NativePortalOwner::mutationBusy");
  assert.match(tick, /storageMaintenanceActive\(\)[\s\S]*stopServer\(\)/);
  assert.match(tick, /restart_refresh_required_[\s\S]*refreshState\(\)[\s\S]*refreshAlbum\(\)[\s\S]*startServer\(\)/);
});

test("Voice maintenance drains owners and format confirmation cannot self-deadlock", () => {
  assert.match(voiceHeader, /bool beginStorageMaintenance\(\)/);
  assert.match(voiceHeader, /bool finishStorageMaintenance\(bool storage_changed,[\s\S]*bool storage_available\)/);
  const begin = body(voice, "bool NativeVoiceService::beginStorageMaintenance", "bool NativeVoiceService::finishStorageMaintenance");
  assert.match(begin, /storage_maintenance_active_ = true/);
  assert.match(begin, /tracked_storage_work_ != 0U/);
  assert.match(begin, /voice_turn_active_ \|\| local_audio_active_/);
  assert.match(begin, /aigc_phase_ != AigcPhase::Idle \|\| aigc_exclusive_/);
  assert.match(begin, /storage_maintenance_active_ = false[\s\S]*return false/);
  const top = body(voice, "AdmissionResult NativeVoiceService::enqueueTopButton", "AdmissionResult NativeVoiceService::enqueueAlbumOrdinal");
  assert.match(top, /confirm_format \? 0U : 1U/);
  const portalHandler = body(voice, "WorkDisposition NativeVoiceService::handlePortalCommand", "void NativeVoiceService::portalTick");
  assert.match(portalHandler, /PortalConfirmLocalTool[\s\S]*envelope\.flags != 0U[\s\S]*finishTrackedStorageWork/);
  const recover = body(voice, "WorkDisposition NativeVoiceService::handleStorage", "WorkDisposition NativeVoiceService::readLocalChatSnapshot");
  assert.match(recover, /StorageRecoverLocalChatAfterFormat/);
  assert.match(recover, /chat_log_->clear\(\)/);
  assert.match(recover, /chat_log_->recover\(chat_recovery\)/);
  assert.match(recover, /storage_available_ = recovered/);
});

test("storage availability and secrets fail closed", () => {
  const state = body(portal, "void NativePortalOwner::refreshState", "void NativePortalOwner::refreshAlbum");
  assert.match(state, /next\.storage_ready = false/);
  assert.match(state, /album_store_->queryCapacity[\s\S]*next\.storage_ready = true/);
  assert.doesNotMatch(state, /statvfs/);
  assert.match(state, /else \{[\s\S]*next\.storage_ready = false/);
  const unavailable = body(voice, "bool NativeVoiceService::finishStorageMaintenance", "void NativeVoiceService::endStorageMaintenance");
  assert.match(unavailable, /if \(!storage_available\)/);
  assert.match(unavailable, /storage_available_ = false/);
  assert.doesNotMatch(voice + portal, /ESP_LOG\w*\([^\n]*(pairing_token|device_token|local_management_password|local_password)/i);
});
