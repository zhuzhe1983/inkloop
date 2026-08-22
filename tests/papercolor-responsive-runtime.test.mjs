import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '..');
const firmware = path.join(root, 'firmware', 'm5-papercolor');
const read = relative => fs.readFileSync(path.join(firmware, relative), 'utf8');

test('PaperColor splits blocking I/O from the responsive control owner', () => {
  const executor = read('src/ResponsiveWorkExecutor.cpp');
  const main = read('src/main.cpp');
  const buttons = read('src/ButtonRouter.cpp');
  const runtime = read('src/PaperColorApplicationRuntime.cpp');
  const myai = read('src/PaperColorMyAiAdapters.cpp');
  const inkloop = read('src/InkloopClient.cpp');

  assert.match(executor, /kResponsiveWorkerCore = 0/);
  assert.match(executor, /kResponsiveWorkerPriority = 1/);
  assert.match(executor, /xQueueCreate\(1, sizeof\(WorkItem\)\)/);
  assert.match(buttons, /"inkloop-input", 2048, this, 4, &task_, 1/);
  assert.match(main, /buttons\.poll\(\)[\s\S]*pollSerialConsole\(\)[\s\S]*audioPrompt\.poll\(\)/);
  assert.match(runtime, /void PaperColorApplicationRuntime::pumpResponsiveUi/);
  assert.match(runtime, /portal_\.loop\(\)/);
  assert.match(runtime, /ResponsiveWorkKind::DisplayHardware/);
  assert.match(runtime, /ResponsiveWorkKind::StorageHardware/);
  assert.match(myai, /ResponsiveWorkKind::MyAiNetwork/);
  assert.match(myai, /ResponsiveWorkKind::MyAiImageStream/);
  assert.match(myai, /ResponsiveWorkKind::WebSocketHandshake/);
  assert.match(inkloop, /ResponsiveWorkKind::InkloopNetwork/);
  assert.match(runtime, /receiveBackpressured\(\)[\s\S]*websocket_\.loop\(\)/);
  assert.match(main, /voiceRealtimeActive\(\)[\s\S]*\? 5 : 50/);
  assert.match(main, /activeKind\(\)[\s\S]*ResponsiveWorkKind::StorageHardware/);
  assert.doesNotMatch(main, /PAGE_REJECTED", "SLOW_CORE_BUSY/);

  const voiceAt = runtime.indexOf('streamingAudio_.poll();', runtime.indexOf('void PaperColorApplicationRuntime::loop()'));
  const ledAt = runtime.indexOf('leds_.pollPixelDiagnostic', voiceAt);
  const portalAt = runtime.indexOf('portal_.loop();', ledAt);
  assert.ok(voiceAt >= 0 && voiceAt < ledAt && ledAt < portalAt,
    'voice must run before LED state and adaptive Portal work');
  assert.doesNotMatch(runtime.slice(ledAt, portalAt), /pixelDiagnosticActive\(\)[\s\S]*return;/,
    'LED diagnostics must never own or stop the control loop');

  const commitAt = myai.indexOf('work.status = sink.commit(metadata)');
  const dispatchAt = myai.indexOf('ResponsiveWorkKind::MyAiImageStream');
  assert.ok(commitAt > dispatchAt, 'album commit must return to the state-owner task');
});

test('PaperColor runtime applies bounded service cadence and adaptive Portal polling', () => {
  const config = read('src/AppConfig.h');
  const runtime = read('src/PaperColorApplicationRuntime.cpp');
  const portal = read('src/PaperColorPortalRuntime.cpp');

  assert.match(config, /kSyncIntervalMs = 30000/);
  assert.equal((runtime.match(/nextAigcPollAt_ = millis\(\) \+ 5000U/g) || []).length, 2);
  assert.match(runtime, /!voice_\.turnActive\(\)[\s\S]*!voice_\.captureActive\(\)[\s\S]*!streamingAudio_\.active\(\)[\s\S]*lastHeartbeatAt_ >= 30000U/);
  assert.match(portal, /recentlyUsed \? 2U : 250U/);
  assert.match(portal, /now - lastPortalRequestAt_ < 15000U/);
  assert.match(portal, /serverStarted_ \|\| requestActive_/);
});
