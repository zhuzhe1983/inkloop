import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const main = path.join(repo, "firmware", "inkloop-idf", "main");

test("app observes pending OTA before initialization and confirms only from live runtime evidence", async () => {
  const [source, cmake] = await Promise.all([
    readFile(path.join(main, "app_main.cpp"), "utf8"),
    readFile(path.join(main, "CMakeLists.txt"), "utf8"),
  ]);
  const earlyTick = source.indexOf("ota_health.tick(");
  const topology = source.indexOf("validate_task_topology()");
  const storageAudit = source.indexOf("runReadOnlyUpgradeBootAudit(storage)");
  const board = source.indexOf("board_initialize()");
  const runtimeBegin = source.indexOf("runtime.begin()");
  const monitorTelemetry = source.lastIndexOf("runtime.telemetry()");
  assert.ok(earlyTick >= 0 && earlyTick < topology);
  assert.ok(topology < storageAudit && storageAudit < board);
  assert.ok(board < runtimeBegin && runtimeBegin < monitorTelemetry);
  assert.match(source, /productionOtaBootHealthConfig\(\)/);
  assert.match(source, /systemEspOtaFunctions\(\)/);
  assert.match(source, /priorResetWasFatal\(\)/);
  assert.match(source, /diagnostics\.startup_failed != 0U/);
  assert.match(source, /vTaskDelay\(pdMS_TO_TICKS\(100U\)\)/);
  assert.match(cmake, /\binkloop_ota\b/);
});

test("every pre-runtime refusal marks a pending image fatal without direct OTA mutation", async () => {
  const source = await readFile(path.join(main, "app_main.cpp"), "utf8");
  for (const stage of [
    "task_topology", "upgrade_inventory", "persistence_contract",
    "board_initialize", "upgrade_gate", "native_settings", "asset_backend",
    "removable_upgrade_gate", "product_composition", "product_runtime",
  ]) {
    assert.match(source, new RegExp(`failPendingBoot\\(\\"${stage}\\"\\)`));
  }
  assert.doesNotMatch(source, /esp_ota_mark_app_|esp_ota_set_boot_partition|esp_ota_begin|esp_ota_write|esp_partition_(?:write|erase)/);
  const healthStart = source.indexOf("inkloop::OtaBootHealthState logged_state");
  const updateStart = source.indexOf(
    "inkloop::OtaUpdateRequest ota_request;", healthStart);
  assert.ok(healthStart >= 0 && updateStart > healthStart);
  const healthComposer = source.slice(healthStart, updateStart);
  assert.match(healthComposer, /if \(ota_pending\)/);
  assert.match(healthComposer, /ota_health\.tick\(/);
  assert.match(healthComposer, /runtime\.telemetry\(\)/);
  assert.doesNotMatch(
    healthComposer, /wifiSnapshot|MyAI|myai|cloud|authorized/);
});
