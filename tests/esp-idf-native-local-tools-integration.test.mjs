import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const tools = join(repo, "firmware/inkloop-idf/components/inkloop_local_tools");
const voice = readFileSync(join(product, "native_voice_service.cpp"), "utf8");
const voiceHeader = readFileSync(
  join(product, "include/inkloop/native_voice_service.hpp"),
  "utf8",
);
const opcodes = readFileSync(
  join(product, "include/inkloop/product_opcodes.hpp"),
  "utf8",
);
const productCmake = readFileSync(join(product, "CMakeLists.txt"), "utf8");
const localTools = readFileSync(join(tools, "local_tools.cpp"), "utf8");
const cloud = readFileSync(join(product, "native_inkloop_service.cpp"), "utf8");

function section(source, start, end) {
  const from = source.indexOf(start);
  assert.notEqual(from, -1, `missing section ${start}`);
  const to = source.indexOf(end, from + start.length);
  assert.notEqual(to, -1, `missing section terminator ${end}`);
  return source.slice(from, to);
}

test("final ASR interception only parses and queues bounded Portal work", () => {
  const inspect = section(
    voice,
    "myai::LocalTranscriptDecision NativeVoiceService::inspect",
    "void NativeVoiceService::onActivationState",
  );
  assert.match(inspect, /local_tool_parser_\.parseFinalAsr/);
  assert.match(inspect, /queueLocalTool\(transcript, parsed\.command\.kind\)/);
  assert.doesNotMatch(
    inspect,
    /handleFinalAsr|queryStorage|deleteImage|clearAlbum|formatTfCard|queryCapacity|open\(/,
  );

  const queued = section(
    voice,
    "bool NativeVoiceService::queueLocalTool",
    "std::string NativeVoiceService::describeLocalToolOutcome",
  );
  assert.match(queued, /text_pool_\.put/);
  assert.match(queued, /WorkClass::Portal/);
  assert.match(queued, /ProductOpcode::PortalRunLocalTool/);
  assert.match(queued, /supervisor_\.post/);
  assert.match(queued, /deadline_ms = 0/);

  const execution = section(
    voice,
    "WorkDisposition NativeVoiceService::handleLocalToolCommand",
    "myai::LocalTranscriptDecision NativeVoiceService::inspect",
  );
  assert.match(execution, /local_tools_session_\.handleFinalAsr/);
  assert.match(execution, /local_tools_session_\.confirm/);
  assert.match(execution, /publishLocalToolOutcome/);
  assert.match(productCmake, /inkloop_local_tools/);
  assert.match(voiceHeader, /attachLocalTools\(local_tools::ILocalToolsAdapter&/);
});

test("local chat persists final text and real tool outcomes only", () => {
  const transcript = section(
    voice,
    "void NativeVoiceService::onTranscript",
    "void NativeVoiceService::onAssistantText",
  );
  assert.match(transcript, /if \(!final\) return;/);
  assert.match(transcript, /isBlankAudioArtifact/);
  assert.match(transcript, /ProductTextKind::AsrFinal/);

  const recognized = section(
    voice,
    "void NativeVoiceService::onLocalCommand",
    "void NativeVoiceService::onVoiceAction",
  );
  assert.doesNotMatch(recognized, /queueChat/);
  assert.match(voice, /ProductTextKind::ToolState, describeLocalToolOutcome/);
  assert.doesNotMatch(voice, /fetch(?:Remote|MyAi)Chat|downloadChatHistory/);
});

test("Portal reads local chat through a bounded Storage-owner snapshot", () => {
  const request = section(
    voice,
    "AdmissionResult NativeVoiceService::requestLocalChatSnapshot",
    "bool NativeVoiceService::tryConsumeLocalChatSnapshot",
  );
  assert.match(request, /ProductOpcode::StorageReadLocalChat/);
  assert.match(request, /WorkClass::Storage/);
  assert.match(request, /chat_snapshot_request_pending_/);

  const read = section(
    voice,
    "WorkDisposition NativeVoiceService::readLocalChatSnapshot",
    "bool NativeVoiceService::queueChat",
  );
  assert.match(read, /chat_log_->readPage/);
  assert.match(read, /boundedUtf8Prefix/);
  assert.match(read, /kNativeLocalChatItemBytes/);
  assert.match(read, /kNativeLocalChatPageTextBytes/);
  assert.match(read, /chat_snapshot_mailbox_/);
  assert.match(read, /clearChatSnapshotMailbox\(\)/);
  assert.doesNotMatch(read, /client_->|http_|wss_|MyAi/);

  const clear = section(
    voice,
    "void NativeVoiceService::clearChatSnapshotMailbox",
    "bool NativeVoiceService::queueChat",
  );
  assert.match(clear, /items\.fill\(NativeLocalChatItem\{\}\)/);
  assert.match(clear, /text\.fill\('\\0'\)/);
  assert.match(productCmake, /-Wframe-larger-than=4096/);

  const consume = section(
    voice,
    "bool NativeVoiceService::tryConsumeLocalChatSnapshot",
    "AdmissionResult NativeVoiceService::postVoiceState",
  );
  assert.match(consume, /xSemaphoreTake\(chat_snapshot_mutex_, 0\)/);
  assert.match(consume, /consumer\.accept\(chat_snapshot_mailbox_\)/);
  assert.match(voiceHeader, /kNativeLocalChatPageItems = 24U/);
  assert.match(voiceHeader, /kNativeLocalChatItemBytes = 2048U/);
  assert.match(voiceHeader, /kNativeLocalChatPageTextBytes = 16U \* 1024U/);
});

test("all destructive album and TF operations require physical confirmation", () => {
  const destructive = section(
    localTools,
    "bool LocalCommandParser::destructive",
    "ToolOutcome LocalToolsSession::handleFinalAsr",
  );
  for (const command of [
    "DeleteImageOrdinal",
    "DeleteImageId",
    "ClearAlbum",
    "FormatTfCard",
  ]) {
    assert.match(destructive, new RegExp(`CommandKind::${command}`));
  }
  assert.match(opcodes, /PortalConfirmLocalTool/);
  const button = section(
    voice,
    "AdmissionResult NativeVoiceService::enqueueTopButton",
    "AdmissionResult NativeVoiceService::enqueueAlbumOrdinal",
  );
  assert.match(button, /local_confirmation_pending_/);
  assert.match(button, /WorkClass::Portal, ProductOpcode::PortalConfirmLocalTool/);

  const queued = section(
    voice,
    "bool NativeVoiceService::queueLocalTool",
    "std::string NativeVoiceService::describeLocalToolOutcome",
  );
  assert.doesNotMatch(queued, /confirmation_token/);
  const description = section(
    voice,
    "std::string NativeVoiceService::describeLocalToolOutcome",
    "void NativeVoiceService::publishLocalToolOutcome",
  );
  assert.doesNotMatch(description, /confirmation_token/);
});

test("saved settings cross only their owning lanes and feed real MyAI prompts", () => {
  const outcome = section(
    voice,
    "void NativeVoiceService::publishLocalToolOutcome",
    "WorkDisposition NativeVoiceService::handleLocalToolCommand",
  );
  assert.match(outcome, /WorkClass::Voice, ProductOpcode::VoiceApplyVolume/);
  assert.match(outcome, /postLedMaximumBrightness/);
  assert.match(outcome, /stageSystemPrompt/);
  assert.match(outcome, /WorkClass::MyAiNetwork/);
  assert.match(outcome, /ProductOpcode::NetworkApplySystemPrompt/);

  const network = section(
    voice,
    "bool NativeVoiceService::applyPendingSystemPrompt",
    "void NativeVoiceService::networkTick",
  );
  assert.match(network, /client_->disconnectVoice\("system_prompt_changed"\)/);
  assert.match(network, /client_->setSystemPrompt/);
  assert.match(opcodes, /VoiceApplyVolume/);
  assert.match(opcodes, /SetLedMaximumBrightness/);

  const aigc = section(
    voice,
    "void NativeVoiceService::serviceAigc",
    "void NativeVoiceService::finishAigc",
  );
  assert.match(aigc, /settings->queryAigcPrompt/);
  assert.match(
    aigc,
    /composeImagePrompt\(\s*board_\.descriptor\(\), aigc_prompt_, configured_template\)/,
  );
  assert.match(aigc, /aigcImageSize\(board_\.descriptor\(\)\)/);
  const compose = section(voice, "std::string composeImagePrompt", "}  // namespace");
  assert.match(compose, /\{\{prompt\}\}/);
  assert.match(compose, /\{subject\}/);
  assert.match(voiceHeader, /kMaximumStoredPromptBytes \+ 1U/);
  assert.match(voice, /validStoredPrompt\(saved_system_prompt\)/);
});

test("local tool outcomes restore the Arduino offline spoken feedback", () => {
  const outcome = section(
    voice,
    "void NativeVoiceService::publishLocalToolOutcome",
    "WorkDisposition NativeVoiceService::handleLocalToolCommand",
  );
  for (const prompt of [
    "ConfirmationRequired",
    "ConfirmationExpired",
    "StorageQueried",
    "StorageFormatted",
    "ImageDeleted",
    "AlbumCleared",
    "SettingsSaved",
    "Error",
  ]) {
    assert.match(outcome, new RegExp(`LocalPrompt::${prompt}`));
  }
  assert.match(outcome, /enqueueLocalPrompt\(feedback\)/);
  assert.match(opcodes, /VoicePromptToolStatus/);
  const playback = section(
    voice,
    "WorkDisposition NativeVoiceService::handleLocalPrompt",
    "WorkDisposition NativeVoiceService::handleVolumePreview",
  );
  assert.match(playback, /safety_confirmation/);
  assert.match(playback, /LocalPrompt::ConfirmationRequired/);
  assert.match(playback, /!assistance_enabled\s*&&\s*!safety_confirmation/);
  for (const asset of [
    "confirmation_press_top_button.wav",
    "confirmation_expired.wav",
    "storage_free_space.wav",
    "storage_formatted.wav",
    "images_deleted.wav",
    "images_cleared.wav",
    "settings_saved.wav",
    "voice_error.wav",
  ]) {
    assert.match(productCmake, new RegExp(asset.replace(".", "\\.")));
  }
});

test("MyAI heartbeat runs on the low-priority Portal lane while Network keeps audio moving", () => {
  const network = section(
    voice,
    "void NativeVoiceService::networkTick",
    "void NativeVoiceService::startPairingIfNeeded",
  );
  assert.match(network, /due\(now, last_heartbeat_ms_ \+ kHeartbeatMs\)/);
  assert.match(network, /scheduleVoiceHeartbeat\(\)/);
  const completionAt = network.indexOf("consumeVoiceHeartbeatCompletion(now)");
  assert.ok(completionAt >= 0);
  assert.match(
    network.slice(Math.max(0, completionAt - 300), completionAt + 80),
    /NativeNetworkDiagnosticOperation::Heartbeat[\s\S]*consumeVoiceHeartbeatCompletion\(now\)/,
  );
  assert.doesNotMatch(network, /client_->heartbeatVoice\(\)/);
  assert.doesNotMatch(
    network.slice(network.indexOf("scheduleVoiceHeartbeat()")),
    /captureBusy\(\) \|\| audio_bridge_->playbackBusy\(\)/,
  );
  const worker = section(
    voice,
    "WorkDisposition NativeVoiceService::performVoiceHeartbeat",
    "bool NativeVoiceService::consumeVoiceHeartbeatCompletion",
  );
  assert.match(worker, /http_\.perform\(work\.request, response\)/);
  assert.match(worker, /work\.clearRequestSensitive\(\)/);
  assert.match(worker, /heartbeat_generation_ != envelope\.generation/);
  assert.match(worker, /heartbeat_correlation_ != envelope\.request_id/);
  assert.match(
    worker,
    /heartbeat_generation_ != generation[\s\S]*heartbeat_correlation_ != correlation/,
  );
  const completion = section(
    voice,
    "bool NativeVoiceService::consumeVoiceHeartbeatCompletion",
    "void NativeVoiceService::stageSystemPrompt",
  );
  assert.match(
    completion,
    /VoiceHeartbeatPhase::Queued[\s\S]*VoiceHeartbeatPhase::Running[\s\S]*heartbeat_phase_deadline_ms_[\s\S]*due\(now_ms, heartbeat_phase_deadline_ms_\)/,
  );
  assert.match(completion, /heartbeat_generation_ = 0U/);
  assert.match(completion, /heartbeat_correlation_ = 0U/);
  assert.match(voice, /kHeartbeatQueuedWatchdogMs = 5000U/);
  assert.match(voice, /kHeartbeatRunningWatchdogMs = 10000U/);
  assert.match(voiceHeader, /heartbeat_phase_deadline_ms_/);
  assert.match(
    voice,
    /envelope\.work_class = WorkClass::Portal[\s\S]*PortalRunVoiceHeartbeat[\s\S]*supervisor_\.post\(envelope\)/,
  );
  assert.match(voice, /completeVoiceHeartbeat/);
  assert.match(voiceHeader, /heartbeat_background_submissions/);
  assert.doesNotMatch(voiceHeader, /heartbeat_audio_deferred_/);
});

test("production cloud and MyAI requests use the running image version", () => {
  const config = section(
    voice,
    "myai::ClientConfig NativeVoiceService::makeConfig",
    "esp_err_t NativeVoiceService::initialize",
  );
  assert.match(config, /esp_app_get_description\(\)/);
  assert.match(config, /config\.clientVersion\s*=\s*app\s*\?\s*app->version/);
  assert.match(cloud, /esp_app_get_description\(\)/);
  assert.match(cloud, /config\.firmware_version\s*=\s*app->version/);
});
