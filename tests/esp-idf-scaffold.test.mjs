import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import test from "node:test";

const root = new URL("../firmware/inkloop-idf/", import.meta.url);
const read = (path) => readFile(new URL(path, root), "utf8");

test("ESP-IDF scaffold keeps responsive priorities above status/background work", async () => {
  const topology = await read("components/inkloop_runtime/include/inkloop/task_topology.hpp");
  const validation = await read("components/inkloop_runtime/task_topology.cpp");
  const expected = [
    ['"ink-input", 1, 22'],
    ['"ink-voice", 1, 20'],
    ['"ink-control", 1, 18'],
    ['"ink-led", 1, 8'],
    ['"ink-storage", 0, 7'],
    ['"ink-display", 0, 6'],
    // WSS/audio ingress must preempt all slow-core filesystem and rendering
    // work; otherwise Portal/image operations can starve realtime speech.
    ['"ink-network", 0, 9'],
    ['"ink-portal", 0, 3'],
  ];
  for (const [source] of expected) assert.match(topology, new RegExp(source));
  assert.match(validation, /input must preempt voice/);
  assert.match(validation, /LED status must never preempt control/);
  assert.match(validation, /Portal must remain the lowest service priority/);
});

test("ESP-IDF scaffold is board-selectable and contains no Arduino adapter", async () => {
  const cmake = await read("CMakeLists.txt");
  const main = await read("main/app_main.cpp");
  const board = await read("boards/m5_papercolor_c151/board.cpp");
  const handoff = await read("docs/ARDUINO_TO_ESP_IDF_HANDOFF.md");
  assert.match(cmake, /INKLOOP_BOARD "m5_papercolor_c151"/);
  assert.match(cmake, /boards\/\$\{INKLOOP_BOARD\}/);
  assert.match(cmake, /INKLOOP_BOARD_COMPONENT/);
  assert.match(main, /board_initialize\(\)/);
  assert.match(main, /if \(board\.has_sd\)[\s\S]*prepareSdCard\(\)/);
  assert.match(board, /class PaperColorBoardAdapter final : public IBoardAdapter/);
  assert.match(board, /IBoardAdapter& board_adapter\(\)/);
  assert.match(handoff, /MyAI role: third-party client only/);
  assert.match(handoff, /button event p99 ≤ 20 ms/);
  assert.match(handoff, /AIGC status: no timer while idle/);

  const roots = [
    "main",
    "components/inkloop_contracts/include/inkloop",
    "components/inkloop_runtime/include/inkloop",
    "boards/m5_papercolor_c151/include/inkloop",
  ];
  const sources = [];
  for (const directory of roots) {
    for (const name of await readdir(new URL(`${directory}/`, root))) {
      if (/\.(?:cpp|hpp|h)$/.test(name)) sources.push(await read(`${directory}/${name}`));
    }
  }
  const combined = sources.join("\n");
  assert.doesNotMatch(combined, /#include\s*[<"]Arduino\.h[>"]/);
  assert.doesNotMatch(combined, /M5Unified|HTTPClient|WiFiManager|Preferences|WebServer/);
});

test("ESP-IDF partition scaffold preserves the current non-destructive 16 MiB layout", async () => {
  const current = await readFile(
    new URL("../m5-papercolor/default_16MB.csv", root), "utf8");
  const next = await read("partitions.csv");
  const sdkconfig = await read("sdkconfig.defaults");
  const normalize = (value) => value
    .split(/\r?\n/)
    .filter((line) => line.trim() && !line.trim().startsWith("#"))
    .map((line) => line.replace(/\s+/g, "").toLowerCase());
  assert.deepEqual(normalize(next), normalize(current));
  assert.match(sdkconfig, /CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y/);
  assert.match(sdkconfig, /CONFIG_ESPTOOLPY_FLASHSIZE="16MB"/);
});

test("ESP-IDF application version is SemVer for signed OTA ordering", async () => {
  const version = (await read("version.txt")).trim();
  const cmake = await read("CMakeLists.txt");
  assert.match(
    version,
    /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/,
  );
  assert.match(cmake, /file\(STRINGS "\$\{CMAKE_CURRENT_LIST_DIR\}\/version\.txt" PROJECT_VER/);
  assert.match(cmake, /version\.txt must contain the Inkloop firmware SemVer/);
});
