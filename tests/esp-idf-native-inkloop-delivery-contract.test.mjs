import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf/components");
const product = join(idf, "inkloop_product");
const service = readFileSync(join(product, "native_inkloop_service.cpp"), "utf8");
const serviceHeader = readFileSync(
  join(product, "include/inkloop/native_inkloop_service.hpp"),
  "utf8",
);
const display = readFileSync(join(product, "native_display_service.cpp"), "utf8");
const runtime = readFileSync(join(product, "product_runtime.cpp"), "utf8");
const papercolorRenderer = readFileSync(
  join(repo, "firmware/inkloop-idf/boards/m5_papercolor_c151/papercolor_renderer.cpp"),
  "utf8",
);
const downloader = readFileSync(
  join(idf, "inkloop_cloud_idf/esp_frame_downloader.cpp"),
  "utf8",
);
const frameAlbumSink = readFileSync(
  join(idf, "inkloop_cloud_idf/inkloop_frame_album_sink.cpp"),
  "utf8",
);

function body(source, signature, nextSignature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = nextSignature ? source.indexOf(nextSignature, start + signature.length) : -1;
  return source.slice(start, end < 0 ? undefined : end);
}

test("production Inkloop identity derives its SKU from the selected board", () => {
  assert.match(
    serviceHeader,
    /NativeInkloopService\(RuntimeSupervisor& supervisor,\s*const BoardDescriptor& board/,
  );
  assert.match(serviceHeader, /const BoardDescriptor& board_/);
  assert.match(service, /!board_\.valid\(\)/);
  assert.match(service, /config\.sku_id\s*=\s*board_\.id/);
  assert.match(
    runtime,
    /inkloop_\(supervisor_,\s*board\.descriptor\(\),\s*storage\.taskRoot\(\)/,
  );
  assert.doesNotMatch(service, /config\.sku_id\s*=\s*"m5-papercolor-c151"/);
});

test("30-second scheduling is wrap-safe and reuses the exact MyAI six-digit code", () => {
  assert.match(service, /kSyncIntervalMs\s*=\s*30000U/);
  assert.match(service, /kTaskRetryMs\s*=\s*30000U/);
  assert.match(service, /static_cast<int32_t>\(now_ms - deadline_ms\)\s*>=\s*0/);
  assert.match(service, /next_sync_ms_\s*=\s*now_ms \+ boundedRetry\(requested_delay_ms\)/);
  assert.match(service, /next_task_attempt_ms_\s*=\s*now_ms \+ kTaskRetryMs/);
  assert.match(service, /next_ack_attempt_ms_\s*=\s*now_ms \+ kTaskRetryMs/);

  const registration = body(
    service,
    "void NativeInkloopService::synchronize",
    "bool NativeInkloopService::reconcileTaskAssets",
  );
  assert.match(registration, /sixDigits\(onboarding\.device_code\.data\(\)\)/);
  assert.match(
    registration,
    /registerDevice\(\s*std::string\(onboarding\.device_code\.data\(\)\),\s*registration\)/,
  );
  assert.match(registration, /registration_required_\s*=\s*!registration\.paired/);
  assert.match(registration, /tasks_synchronized_\s*=\s*false/);
  assert.match(registration, /registration\.paired\)[\s\S]{0,80}next_sync_ms_\s*=\s*now_ms/);

  const due = (now, deadline) => ((now - deadline) | 0) >= 0;
  assert.equal(due(0xfffffff0, 0x00000020), false);
  assert.equal(due(0x00000020, 0x00000020), true);
  assert.equal(due(0x00000021, 0x00000020), true);
  assert.equal(due(0x00000020, 0xfffffff0), true);
  const scheduledAcrossWrap = (0xfffffff0 + 30000) >>> 0;
  assert.equal(due(0xfffffff0, scheduledAcrossWrap), false);
  assert.equal(due(scheduledAcrossWrap, scheduledAcrossWrap), true);
});

test("cached local tasks run offline, while first online use is gated by sync", () => {
  const initialize = body(
    service,
    "esp_err_t NativeInkloopService::initialize",
    "void NativeInkloopService::scheduleSync",
  );
  assert.match(initialize, /registration_required_\s*=\s*client_->identity\(\)\.device_id\.empty\(\)/);
  assert.match(initialize, /tasks_synchronized_\s*=\s*!registration_required_/);
  assert.match(initialize, /next_sync_ms_\s*=\s*nowMs\(\)/);

  const tick = body(
    service,
    "void NativeInkloopService::portalTick",
    "bool NativeInkloopService::busy",
  );
  const synchronizeAt = tick.indexOf("synchronize(onboarding, now)");
  const runAt = tick.indexOf("runDueTask(now, wifi_online)");
  assert.ok(synchronizeAt >= 0 && runAt > synchronizeAt);
  assert.match(tick, /wifi_online\s*&&\s*due\(now, next_sync_ms_\)/);
  assert.match(
    tick,
    /scheduled_display_allowed\s*&&\s*!registration_required_\s*&&[\s\S]{0,100}tasks_synchronized_[\s\S]{0,100}runDueTask/,
  );
  assert.match(tick, /!registration_required_\s*&&\s*tasks_synchronized_/);

  const run = body(
    service,
    "void NativeInkloopService::runDueTask",
    "bool NativeInkloopService::handleControlResult",
  );
  assert.match(
    run,
    /if \(task\.id\.empty\(\)\) \{[\s\S]{0,160}scheduleTaskRetry\(now_ms\)[\s\S]{0,80}return;/,
  );
  const missingAt = run.indexOf("ordinal == storage::kMaximumAlbumEntries");
  const offlineReturnAt = run.indexOf("if (!download_allowed)", missingAt);
  const downloadAt = run.indexOf("downloader_.download", missingAt);
  assert.ok(missingAt >= 0 && offlineReturnAt > missingAt && downloadAt > offlineReturnAt);
  assert.match(
    run.slice(missingAt, downloadAt),
    /if \(!download_allowed\) \{[\s\S]{0,160}scheduleTaskRetry\(now_ms\)[\s\S]{0,80}return;/,
  );
});

test("task replacement propagation and streaming commit happen before display admission", () => {
  const synchronize = body(
    service,
    "void NativeInkloopService::synchronize",
    "bool NativeInkloopService::reconcileTaskAssets",
  );
  // syncTasks can commit the replacement and then fail to persist its applied
  // revision. `result.changed` distinguishes that partial commit from a pure
  // transport failure, which must not revoke valid offline-cache authority.
  assert.match(
    synchronize,
    /if \(!status\.ok\(\)\) \{[\s\S]{0,900}result\.changed[\s\S]{0,220}tasks_synchronized_\s*=\s*false/,
  );
  const reconcileGateAt = synchronize.lastIndexOf("reconcileTaskAssets()");
  const synchronizedAt = synchronize.indexOf(
    "tasks_synchronized_ = true",
    reconcileGateAt,
  );
  assert.ok(reconcileGateAt >= 0 && synchronizedAt > reconcileGateAt);
  const failedReconcilePath = synchronize.slice(reconcileGateAt, synchronizedAt);
  assert.match(failedReconcilePath, /tasks_synchronized_\s*=\s*false/);
  assert.match(failedReconcilePath, /return;/);

  const reconcile = body(
    service,
    "bool NativeInkloopService::reconcileTaskAssets",
    "bool NativeInkloopService::queueDisplay",
  );
  assert.match(
    reconcile,
    /AlbumTaskBinding\{task\.id,\s*task\.frame_hash,\s*task\.render_strategy\}/,
  );
  const pruneAt = reconcile.indexOf("pruneTaskAssets");
  const reloadAt = reconcile.indexOf("display_.reloadCatalog");
  assert.ok(pruneAt >= 0 && reloadAt > pruneAt);

  const run = body(
    service,
    "void NativeInkloopService::runDueTask",
    "bool NativeInkloopService::handleControlResult",
  );
  const downloadAt = run.indexOf("downloader_.download");
  const committedAt = run.indexOf("ordinal = committed.ordinal", downloadAt);
  const reloadAfterDownloadAt = run.indexOf("display_.reloadCatalog", committedAt);
  const displayAt = run.indexOf("queueDisplay", reloadAfterDownloadAt);
  assert.ok(
    downloadAt >= 0 &&
      committedAt > downloadAt &&
      reloadAfterDownloadAt > committedAt &&
      displayAt > reloadAfterDownloadAt,
  );
  assert.match(
    run,
    /asset\.content_sha256\s*==\s*task\.frame_hash\s*&&\s*asset\.task_id\s*==\s*task\.id/,
  );

  assert.match(downloader, /std::array<uint8_t,\s*kReadBufferBytes>\s+buffer/);
  assert.match(downloader, /while \(remaining > 0U\)[\s\S]*esp_http_client_read/);
  assert.match(downloader, /stream\.append\(buffer\.data\(\),\s*static_cast<size_t>\(count\)\)/);
  assert.match(downloader, /stream\.finish\(metadata\)/);
  assert.doesNotMatch(downloader, /std::vector\s*</);
  assert.match(
    frameAlbumSink,
    /committed\.content_sha256\s*!=\s*metadata\.sha256/,
  );
  assert.doesNotMatch(
    frameAlbumSink,
    /committed\.asset_id\s*!=\s*metadata\.sha256/,
  );

  // A strategy-only replacement of the currently visible content is not
  // unchanged: it must be converted and physically refreshed before Complete.
  assert.doesNotMatch(
    display,
    /if \(index\.current == asset\.id\) \{[\s\S]{0,240}\+\+diagnostics_\.unchanged_skips/,
  );
  assert.match(
    display,
    /index\.current_render_strategy\s*==\s*asset\.render_strategy/,
  );
  const selectedAssetAt = display.indexOf(
    "const storage::AlbumIndexAsset asset = index.assets[ordinal]",
  );
  const supportAt = display.indexOf(
    "renderer->supportsRenderStrategy(asset.render_strategy)",
    selectedAssetAt,
  );
  const convertAt = display.indexOf("renderer->renderRgbFullFrame(", supportAt);
  const selectedStrategyAt = display.indexOf(
    "rgb_view, asset.render_strategy",
    convertAt,
  );
  const panelAt = display.indexOf("writePanelFrame(frame.get(), frame.size())", convertAt);
  const persistAt = display.indexOf("album_store_->markCurrent(asset.id)", panelAt);
  assert.ok(
    selectedAssetAt >= 0 &&
      supportAt > selectedAssetAt &&
      convertAt > supportAt &&
      selectedStrategyAt > convertAt &&
      panelAt > selectedStrategyAt &&
      persistAt > panelAt,
  );

  // The SKU renderer now owns string-id parsing. Product passes the exact
  // persisted per-asset ID across that abstraction rather than parsing it into
  // a PaperColor-only enum itself.
  const boardRender = body(
    papercolorRenderer,
    "esp_err_t PaperColorRenderer::renderRgbFullFrame",
    "esp_err_t PaperColorRenderer::renderProvisioningFrame",
  );
  const boardSupportAt = boardRender.indexOf("supportsRenderStrategy(strategy)");
  const boardParseAt = boardRender.indexOf("parseStrategy(strategy, selected)");
  const streamAt = boardRender.indexOf("streamRenderPixels(", boardParseAt);
  assert.ok(boardSupportAt >= 0 && boardParseAt > boardSupportAt && streamAt > boardParseAt);
});

test("only the matching Display Complete reaches Portal-owned markRun", () => {
  const queued = body(
    service,
    "bool NativeInkloopService::queueDisplay",
    "void NativeInkloopService::runDueTask",
  );
  assert.match(queued, /pending\.phase\s*=\s*DisplayMailboxPhase::AwaitingResult/);
  assert.match(queued, /pending\.request_id\s*=\s*nextRequestId\(\)/);
  assert.match(queued, /work_class\s*=\s*WorkClass::Display/);
  assert.match(queued, /opcode\s*=\s*productOpcode\(ProductOpcode::DisplayAlbumOrdinal\)/);
  assert.match(queued, /supervisor_\.post\(command\)[\s\S]*AdmissionResult::Admitted/);
  assert.match(queued, /display_mailbox_\s*=\s*DisplayMailbox\{\}[\s\S]*scheduleTaskRetry\(now_ms\)/);

  const control = body(
    service,
    "bool NativeInkloopService::handleControlResult",
    "void NativeInkloopService::drainAcknowledgement",
  );
  assert.match(control, /envelope\.kind\s*!=\s*EnvelopeKind::Result/);
  assert.match(control, /envelope\.work_class\s*!=\s*WorkClass::Display/);
  assert.match(control, /DisplayAlbumOrdinal/);
  assert.match(control, /display_mailbox_\.request_id\s*==\s*envelope\.request_id/);
  assert.match(control, /envelope\.disposition\s*==\s*WorkDisposition::Complete/);
  assert.match(control, /DisplayMailboxPhase::AcknowledgementReady/);
  assert.match(control, /else \{[\s\S]*display_failures[\s\S]*kTaskRetryMs/);
  assert.doesNotMatch(control, /markRun|task_store_|album_store_|downloader_|syncTasks|registerDevice/);

  const acknowledgement = body(
    service,
    "void NativeInkloopService::drainAcknowledgement",
    "void NativeInkloopService::portalTick",
  );
  assert.match(acknowledgement, /DisplayMailboxPhase::AcknowledgementReady/);
  assert.match(acknowledgement, /task_store_->markRun\(/);
  assert.match(acknowledgement, /next_ack_attempt_ms_\s*=\s*now_ms \+ kTaskRetryMs/);

  const productControl = body(
    runtime,
    "WorkDisposition EspProductRuntime::handleControl",
    "WorkDisposition EspProductRuntime::handleNetwork",
  );
  assert.match(productControl, /inkloop_\.handleControlResult\(envelope\)/);
  assert.doesNotMatch(productControl, /markRun|task_store_|album_store_|download\(|syncTasks|registerDevice/);
});

test("a dropped Display result cannot wedge the delivery mailbox forever", () => {
  assert.match(serviceHeader, /uint32_t result_deadline_ms\s*=\s*0/);
  const queued = body(
    service,
    "bool NativeInkloopService::queueDisplay",
    "void NativeInkloopService::runDueTask",
  );
  assert.match(
    queued,
    /pending\.result_deadline_ms\s*=\s*now_ms\s*\+\s*kDisplayResultTimeoutMs/,
  );
  assert.match(
    queued,
    /command\.deadline_ms\s*=\s*pending\.result_deadline_ms/,
  );
  assert.match(service, /kDisplayResultTimeoutMs\s*=\s*[1-9][0-9]*U/);

  // RuntimeSupervisor's result post is deliberately non-blocking, so the
  // Portal owner must expire AwaitingResult itself and schedule a retry.
  const expiry = body(
    service,
    "void NativeInkloopService::expireDisplayResult",
    "void NativeInkloopService::portalTick",
  );
  assert.match(expiry, /DisplayMailboxPhase::AwaitingResult/);
  assert.match(expiry, /due\(now_ms,\s*display_mailbox_\.result_deadline_ms\)/);
  assert.match(expiry, /display_mailbox_\s*=\s*DisplayMailbox\{\}/);
  assert.match(expiry, /\+\+diagnostics_\.display_result_timeouts/);
  assert.match(expiry, /next_task_attempt_ms_\s*=\s*now_ms\s*\+\s*kTaskRetryMs/);
  const tick = body(
    service,
    "void NativeInkloopService::portalTick",
    "bool NativeInkloopService::busy",
  );
  assert.ok(
    tick.indexOf("expireDisplayResult(now)") >= 0 &&
      tick.indexOf("expireDisplayResult(now)") <
        tick.indexOf("if (!slow_io_allowed"),
  );
});

test("scheduled display request ids remain isolated through sequence wrap", () => {
  assert.match(service, /kRequestNamespace\s*=\s*0x494e4b0000000000ULL/);
  assert.match(service, /sequence_\s*=\s*\(sequence_ \+ 1U\)\s*&\s*0x000000ffffffffffULL/);
  assert.match(service, /while \(sequence_ == 0\)/);
  assert.match(service, /kRequestNamespace \| sequence_/);
  assert.match(serviceHeader, /uint64_t request_id\s*=\s*0/);

  const namespace = 0x494e4b0000000000n;
  const mask = 0x000000ffffffffffn;
  const next = (value) => {
    let sequence = (value + 1n) & mask;
    if (sequence === 0n) sequence = 1n;
    return namespace | sequence;
  };
  assert.equal(next(0n), 0x494e4b0000000001n);
  assert.equal(next(mask - 1n), 0x494e4bffffffffffn);
  assert.equal(next(mask), 0x494e4b0000000001n);
  assert.notEqual(next(0n), 1n); // voice/display-local sequences start here.
});
