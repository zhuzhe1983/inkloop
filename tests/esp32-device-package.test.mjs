import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import test from "node:test";

const publicRoot = new URL("../public/", import.meta.url);
const manifestUrl = new URL("firmware/m5-papercolor/manifest.json", publicRoot);

test("M5 PaperColor 刷机清单中的四个镜像均存在且哈希匹配", async () => {
  const manifest = JSON.parse(await readFile(manifestUrl, "utf8"));
  assert.equal(manifest.chipFamily, "ESP32-S3");
  assert.equal(manifest.version, "0.2.0");
  assert.equal(manifest.files.length, 4);
  for (const file of manifest.files) {
    const bytes = await readFile(new URL(file.path.replace(/^\//, ""), publicRoot));
    assert.equal(createHash("sha256").update(bytes).digest("hex"), file.sha256, file.path);
  }
});

test("应用镜像只包含一个可安全覆盖的服务器地址槽位", async () => {
  const manifest = JSON.parse(await readFile(manifestUrl, "utf8"));
  const firmware = await readFile(new URL("firmware/m5-papercolor/firmware.bin", publicRoot));
  const marker = Buffer.from(manifest.serverSlot.marker);
  const first = firmware.indexOf(marker);
  assert.ok(first >= 0, "missing API URL slot");
  assert.equal(firmware.indexOf(marker, first + 1), -1, "API URL slot must be unique");
  const padding = firmware.subarray(first + marker.length, first + manifest.serverSlot.length);
  assert.ok(padding.every((byte) => byte === 0), "API URL slot must have zero-filled padding");
});

test("网页刷机必须在任何异步下载前请求串口授权", async () => {
  const source = await readFile(new URL("../app/lib/esp32-device.ts", import.meta.url), "utf8");
  const flashFunction = source.slice(source.indexOf("export async function flashM5PaperColor"));
  const requestPortIndex = flashFunction.indexOf("serial.requestPort(");
  const firstFetchIndex = flashFunction.indexOf("await fetch(");
  const dynamicImportIndex = flashFunction.indexOf('await import("esptool-js")');

  assert.ok(requestPortIndex >= 0, "missing Web Serial chooser");
  assert.ok(firstFetchIndex >= 0, "missing firmware download");
  assert.ok(dynamicImportIndex >= 0, "missing esptool loader");
  assert.ok(requestPortIndex < firstFetchIndex, "requestPort must preserve the click user gesture");
  assert.ok(requestPortIndex < dynamicImportIndex, "requestPort must run before dynamic imports");
});

test("PaperColor 固件保留结构化串口诊断和硬件自检命令", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const sourceFiles = (await readdir(sourceRoot)).filter((name) => /\.(?:cpp|h)$/.test(name));
  const source = (await Promise.all(sourceFiles.map((name) => readFile(new URL(name, sourceRoot), "utf8")))).join("\n");
  for (const command of ["status", "pair-code", "led-test", "sound-test", "screen-test", "reboot"]) {
    assert.match(source, new RegExp(`command == \\"${command}\\"`), command);
  }
  assert.match(source, /Diagnostics::event\("PAIR_CODE"/);
  assert.match(source, /Diagnostics::event\("PM1"/);
  assert.match(source, /printDiagnosticStatus\(\)/);

  const main = await readFile(new URL("../firmware/m5-papercolor/src/main.cpp", import.meta.url), "utf8");
  const reset = main.indexOf('Diagnostics::event("RESET_REASON"');
  const boot = main.indexOf('Diagnostics::event("BOOT"');
  const board = source.indexOf('Diagnostics::event("BOARD"');
  const pm1 = source.indexOf('Diagnostics::event("PM1"');
  const ready = source.indexOf('"HARDWARE_READY"');
  assert.ok(reset >= 0 && reset < boot, "reset reason must precede boot event");
  assert.ok(board >= 0 && board < pm1 && pm1 < ready, "board/PM1/ready order must be stable");
  assert.match(
    source,
    /Diagnostics::event\(\s*"HARDWARE_READY",\s*ready \? "READY" : "ERROR_BOARD_PM1_OR_RGB_POWER"\s*\)/,
    "hardware-ready must fail closed when board, PM1, or RGB power is unavailable",
  );
  assert.match(source, /Diagnostics::event\("PORTAL_ACCESS", "READY"\)/);
  assert.doesNotMatch(source, /Diagnostics::event\("PORTAL_ACCESS",\s*access_\.(?:bootNonce|sessionId|csrfToken)/);
  const commandEcho = main.indexOf('Diagnostics::event("COMMAND", command)');
  const statusDispatch = main.indexOf("printDiagnosticStatus();", commandEcho);
  assert.ok(commandEcho >= 0 && commandEcho < statusDispatch, "command echo must precede STATUS");
});

test("PaperColor beta 保持 0.2 协议并建立单写屏边界", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const sourceFiles = (await readdir(sourceRoot)).filter((name) => /\.(?:cpp|h)$/.test(name));
  const entries = await Promise.all(sourceFiles.map(async (name) => [name, await readFile(new URL(name, sourceRoot), "utf8")]));
  const allSource = entries.map(([, source]) => source).join("\n");
  const pushOwners = entries.filter(([, source]) => source.includes("pushSprite("));
  const legacyLedOwners = entries.filter(([, source]) => source.includes("M5.Led."));
  const neoPixelOwners = entries.filter(([, source]) => source.includes("Adafruit_NeoPixel"));

  assert.deepEqual(pushOwners.map(([name]) => name), ["DisplayController.cpp"]);
  assert.deepEqual(legacyLedOwners.map(([name]) => name), []);
  assert.deepEqual(neoPixelOwners.map(([name]) => name).sort(), ["LedStatusController.h"]);
  assert.match(allSource, /pixels_\(2, 21, NEO_GRB \+ NEO_KHZ800\)/);
  assert.match(allSource, /kProtocolFirmwareVersion\[\] = "0\.2\.0"/);
  assert.match(allSource, /kBuildVersion\[\] = "0\.3\.0-beta\.1"/);
  assert.match(allSource, /preferences_\.begin\("inkloop", false\)/);
  assert.match(allSource, /kTasksPath\[\] = "\/tasks\.json"/);
  assert.match(allSource, /request\["action"\] = "register"/);
  assert.match(allSource, /request\["hardwareId"\] = hardwareId_/);
  assert.match(allSource, /request\["secret"\] = deviceSecret_/);
  assert.match(allSource, /request\["skuId"\] = kSkuId/);
  assert.match(allSource, /request\["action"\] = "sync"/);
  assert.match(allSource, /request\["appliedRevision"\] = appliedRevision_/);
  assert.match(allSource, /payload\["changed"\]/);
  assert.match(allSource, /payload\["tasks"\]/);
  assert.match(allSource, /Authorization", "InkloopDevice " \+ deviceId_ \+ ":" \+ deviceSecret_/);
  assert.match(allSource, /width == 600 && height == 400/);
  assert.match(allSource, /width != 400 \|\| height != 600/);
  assert.equal(allSource.match(/xSemaphoreCreateMutex\(\)/g)?.length, 5, "display/LED adapters plus the bounded I/O dispatcher own mutexes");
  assert.match(allSource, /dispatch_ = xSemaphoreCreateMutex\(\)/);
  assert.match(allSource, /queue_ = xQueueCreate\(1, sizeof\(WorkItem\)\)/);
  assert.doesNotMatch(allSource, /Diagnostics::event\([^;]*deviceSecret_/s);
  assert.doesNotMatch(allSource, /status\["(?:secret|deviceSecret|token)"\]/);
});

test("PaperColor Slice 1 只启用事务相册且硬件抽象可逆", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const files = await readdir(sourceRoot);
  const source = (await Promise.all(
    files.filter((name) => /\.(?:cpp|h)$/.test(name)).map((name) => readFile(new URL(name, sourceRoot), "utf8")),
  )).join("\n");

  assert.match(source, /bool myAiEnabled = false/);
  assert.match(source, /bool albumEnabled = true/);
  assert.match(source, /bool experimentalRenderEnabled = false/);
  assert.match(source, /bool deepSleepEnabled = false/);
  assert.match(source, /preferences_\.begin\(kNamespace, false\)/);
  assert.match(source, /GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_1/);
  assert.match(source, /ButtonEvent::PreviousPage, ButtonEvent::NextPage, ButtonEvent::Voice/);
  assert.match(source, /xTaskCreatePinnedToCore\([\s\S]*"inkloop-input"[\s\S]*this, 4, &task_, 1\)/);
  assert.doesNotMatch(source, /M5\.Btn[ABC]\.wasPressed\(\)/);
  assert.match(source, /attachOptionalSdBackend/);
  assert.match(source, /beginDataSafeMode/);
  assert.match(source, /led-map swap/);
  assert.match(source, /PERSISTENCE_UNAVAILABLE/);
  assert.match(source, /WAITING_NVS_RECOVERY/);
  assert.match(source, /NVS_RECOVERED_REBOOTING/);
  assert.match(source, /PersistenceReadiness readiness\(settingsReady, identityReady\)/);
  assert.match(source, /if \(!readiness\.safeToStartNetwork\(\)\) recoverPersistentState/);
  const myAiTypes = await readFile(
    new URL("../firmware/m5-papercolor/lib/InkloopMyAi/src/MyAiTypes.h", import.meta.url),
    "utf8",
  );
  assert.match(myAiTypes, /kAppId\[\] = "inkloop"/);
  assert.match(myAiTypes, /kCenterBaseUrl\[\] = "https:\/\/myai\.mess\.host"/);
  assert.match(source, /class Esp32MyAiWebSocket/);
  assert.match(source, /registerDevice\(onboardingCode\.c_str\(\)\)/);
  assert.match(source, /request\["pairingCode"\] = \*requestedPairingCode/);
  assert.match(source, /if \(settings\.current\(\)\.features\.myAiEnabled\)/);
  assert.match(source, /LEGACY_0_2_FEATURES_DISABLED/);
  assert.match(source, /pendingPairing\(resumed\)/);
  assert.match(source, /requestMyAiPairing\(&error\)/);
});

test("PaperColor Slice 1 的相册写入顺序、SD 策略和分页拒绝边界可审计", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const files = await readdir(sourceRoot);
  const entries = await Promise.all(
    files.filter((name) => /\.(?:cpp|h)$/.test(name)).map(async (name) => [name, await readFile(new URL(name, sourceRoot), "utf8")]),
  );
  const source = entries.map(([, value]) => value).join("\n");
  const album = entries.find(([name]) => name === "AlbumStore.cpp")?.[1] || "";
  const transaction = entries.find(([name]) => name === "TransactionalIo.h")?.[1] || "";
  const assetPromote = album.indexOf("transaction.promoteBlob(");
  const indexCommit = album.indexOf("commitIndex(backend, predictedIndex)", assetPromote);

  assert.ok(assetPromote >= 0 && assetPromote < indexCommit);
  assert.match(transaction, /writeAll\(temporaryPath, bytes, length\)[\s\S]*contentEquals\(temporaryPath, bytes, length\)[\s\S]*rename\(temporaryPath, finalPath\)/);
  assert.match(transaction, /writeAll\(nextPath, bytes, length\)[\s\S]*contentEquals\(nextPath, bytes, length\)[\s\S]*rename\(currentPath, previousPath\)[\s\S]*rename\(nextPath, currentPath\)/);
  assert.match(album, /kIndexPreviousPath/);
  assert.match(album, /measureJson\(predictedIndex\)/);
  assert.match(source, /acknowledgementPayloadSize/);
  assert.match(source, /estimateJournalRecordBytes/);
  assert.match(source, /storageCanPreserveReserve/);
  assert.match(source, /kLittleFsFinalReserveBytes = 320 \* 1024/);
  assert.doesNotMatch(source, /kMetadataTransactionBudget|kMaxTaskCount/);
  assert.match(album, /ALBUM_INDEX_RECOVERED/);
  assert.match(album, /ALBUM_DEDUP/);
  assert.match(album, /ALBUM_CORRUPT_ASSET/);
  assert.match(source, /SD\.begin\(47, SPI, 25000000, "\/sd", 8, false\)/);
  assert.match(source, /baseline_\.begin\(false\)/);
  assert.doesNotMatch(source, /SD\.format|LittleFS\.format|LittleFS\.begin\(true\)|SD\.begin\([^;]*true\)/);
  assert.match(source, /f_mkfs\(volume, FM_ANY/);
  assert.match(source, /selectAdjacentPage\(base, state\.count, direction, false\)/);
  assert.match(source, /StorageBackendRef backend/);
  assert.match(source, /loadPage\(\s*const StorageBackendRef& pinnedBackend/s);
  assert.match(source, /markCurrent\(const StorageBackendRef& pinnedBackend/);
  assert.match(source, /pendingPageBackend/);
  assert.match(source, /audioPrompt\.requestDisplayBusy\(\)/);
  assert.match(source, /buttons\.suppressUntilRelease\(\)/);
  assert.match(source, /xQueueCreate\(16, sizeof\(uint8_t\)\)/);
  assert.match(source, /xTaskCreatePinnedToCore\([\s\S]*"inkloop-input"[\s\S]*this, 4, &task_, 1\)/);
  assert.match(source, /GPIO_NUM_10[\s\S]*GPIO_NUM_9[\s\S]*GPIO_NUM_1/);
  assert.match(source, /xQueueSend\(queue_, &event, 0\)/);
  assert.match(source, /buttons\.suppressUntilRelease\(\)/);
  assert.match(source, /LedState::Downloading/);
  assert.match(source, /LedState::Caching/);
  assert.match(source, /LedState::Writing/);
  assert.match(source, /LedState::Complete/);
});

test("PaperColor Slice 1 remediation 持久化写屏意图并禁止元数据失败时重刷", async () => {
  const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
  const files = await readdir(sourceRoot);
  const entries = await Promise.all(
    files.filter((name) => /\.(?:cpp|h)$/.test(name)).map(async (name) => [name, await readFile(new URL(name, sourceRoot), "utf8")]),
  );
  const allSource = entries.map(([, value]) => value).join("\n");
  const main = entries.find(([name]) => name === "main.cpp")?.[1] || "";
  const journal = entries.find(([name]) => name === "DisplayTransaction.cpp")?.[1] || "";
  const taskStore = entries.find(([name]) => name === "TaskStore.cpp")?.[1] || "";

  assert.match(allSource, /kJournalPath = "\/display-txn\.json"/);
  assert.match(journal, /journal\["backend"\] = backend/);
  assert.match(journal, /journal\["assetId"\] = assetId/);
  assert.match(journal, /journal\["previousCurrent"\] = previousCurrent/);
  assert.match(journal, /stage_ = DisplayJournalStage::Prepared;[\s\S]*persist\(\)/);
  assert.match(journal, /stage_ = DisplayJournalStage::Displayed;[\s\S]*persist\(\)/);
  assert.match(journal, /album_\.markCurrent\(transaction_\.backend_, transaction_\.assetId_\)/);
  assert.match(journal, /tasks_\.isRunAcknowledged\(transaction_\.task_, transaction_\.runAt_\)/);
  assert.match(journal, /tasks_\.markRunWithDay\(/);
  assert.match(journal, /DISPLAY_TXN_AMBIGUOUS/);
  assert.match(journal, /DISPLAY_TXN_CORRUPT/);
  assert.match(taskStore, /task\["lastRun"\][\s\S]*>= static_cast<uint32_t>\(now\)\) return true/);
  assert.match(taskStore, /const RecordRecovery recovery = persistence\.recover\(/);
  assert.match(taskStore, /TaskPersistenceCore persistence\(io\)[\s\S]*persistence\.commitValidatedDetailed\(/);
  assert.match(taskStore, /TASK_INDEX_COMMIT_FAILED/);
  assert.match(taskStore, /task\.is<JsonObjectConst>\(\)/);
  assert.doesNotMatch(taskStore, /task\.is<JsonObject>\(\)/);
  const albumStore = entries.find(([name]) => name === "AlbumStore.cpp")?.[1] || "";
  assert.match(albumStore, /value\.is<JsonObjectConst>\(\)/);
  assert.doesNotMatch(albumStore, /value\.is<JsonObject>\(\)/);
  assert.doesNotMatch(taskStore, /storage_\.remove\(kTasksPath\)/);
  assert.match(allSource, /currentPath\(\) \{ return "\/tasks\.json"; \}/);
  assert.match(allSource, /previousPath\(\) \{ return "\/tasks\.prev"; \}/);

  const taskDisplay = main.indexOf('displayTransaction.begin(asset, "task"');
  const taskRefresh = main.indexOf("display.showPng(", taskDisplay);
  const taskDisplayed = main.indexOf("displayTransaction.confirmDisplayed()", taskRefresh);
  assert.ok(taskDisplay >= 0 && taskDisplay < taskRefresh && taskRefresh < taskDisplayed);
  assert.doesNotMatch(main.slice(taskDisplayed, main.indexOf("bool processPendingPage")), /display\.showPng\(/);
  assert.match(main, /if \(displayTransaction\.active\(\)\)[\s\S]*displayTransaction\.retryFinalize\(\)[\s\S]*return;/);
  assert.match(main, /if \(processPendingPage\(\)\) \{[\s\S]*return;[\s\S]*now - lastFullRefreshAt >= kFullRefreshCooldownMs/);
  assert.match(allSource, /display-recover \[target\|previous\]/);
  assert.match(main, /if \(useLegacyDirectDisplay\([\s\S]*runAlbumDisabledDirectPath\(/);
});
