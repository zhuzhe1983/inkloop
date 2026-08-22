import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const firmware = join(repo, "firmware/inkloop-idf");
const board = join(firmware, "boards/m5_papercolor_c151");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include "inkloop/papercolor_ed2208_protocol.hpp"

using namespace inkloop;

int main() {
  static_assert(kPaperColorEd2208Width == 400);
  static_assert(kPaperColorEd2208Height == 600);
  static_assert(kPaperColorEd2208FrameBytes == 120000);

  constexpr std::array<uint8_t, 12> commands{{
      0xAA, 0x01, 0x00, 0x05, 0x08, 0x06,
      0x03, 0x60, 0x30, 0x50, 0xE3, 0x84}};
  assert(ed2208InitCommandCount() == commands.size());
  for (size_t i = 0; i < commands.size(); ++i) {
    const auto step = ed2208InitCommand(i);
    assert(step.command == commands[i]);
    assert(step.data != nullptr && step.length > 0);
  }
  assert(ed2208InitCommand(99).data == nullptr);

  for (uint8_t index = 0; index < 16; ++index) {
    const bool expected = index <= 3 || index == 5 || index == 6;
    assert(ed2208PaletteIndexValid(index) == expected);
  }
  std::array<uint8_t, kPaperColorEd2208FrameBytes> frame{};
  frame.fill(0x11);
  assert(ed2208FrameValid(frame.data(), frame.size()));
  frame[0] = 0x65;
  assert(ed2208FrameValid(frame.data(), frame.size()));
  frame[59999] = 0x41;
  assert(!ed2208FrameValid(frame.data(), frame.size()));
  frame[59999] = 0x17;
  assert(!ed2208FrameValid(frame.data(), frame.size()));
  assert(!ed2208FrameValid(nullptr, frame.size()));
  assert(!ed2208FrameValid(frame.data(), frame.size() - 1));
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ed2208-"));
  try {
    const source = join(scratch, "protocol.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(board, "include"), source,
      join(board, "papercolor_ed2208_protocol.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("ED2208 native palette and official init sequence pass strict C++17", () => {
  buildAndRun(false);
});

test("ED2208 frame validation is memory-safe under ASan/UBSan", () => {
  buildAndRun(true);
});

test("PaperColor adapter owns exact PM1, shared SPI2, buttons and RGB hardware", () => {
  const source = readFileSync(join(board, "board.cpp"), "utf8");
  const display = readFileSync(join(board, "papercolor_ed2208.cpp"), "utf8");
  const renderer = readFileSync(join(board, "papercolor_renderer.cpp"), "utf8");
  const displayHeader = readFileSync(join(
    board, "include/inkloop/papercolor_ed2208.hpp"), "utf8");
  const common = readFileSync(join(
    firmware, "components/inkloop_board/include/inkloop/board.hpp"), "utf8");
  const manifest = readFileSync(join(board, "idf_component.yml"), "utf8");
  const lock = readFileSync(join(firmware, "dependencies.lock"), "utf8");

  for (const contract of [
    /GPIO_NUM_3/, /GPIO_NUM_2/, /GPIO_NUM_13/, /GPIO_NUM_14/,
    /GPIO_NUM_15/, /GPIO_NUM_10/, /GPIO_NUM_9/, /GPIO_NUM_1/,
    /GPIO_NUM_21/, /SPI2_HOST/,
  ]) assert.match(source, contract);
  assert.match(source, /kExpectedPm1DeviceId = 0x50/);
  assert.match(source, /kExpectedPm1DeviceModel = 0x20/);
  assert.match(source, /setLdoEnable\(true\)/);
  assert.match(source, /initializeI2cAndPm1\(\);[\s\S]*initializeRgb\(\)/);
  assert.match(source, /trans_queue_depth = 0/);
  assert.doesNotMatch(source, /trans_queue_depth = [1-9]/);
  assert.match(source, /count != kRgbCount/);
  assert.match(source, /LED_STRIP_COLOR_COMPONENT_FMT_GRB/);
  assert.match(source, /gpio_get_level\(pin\) == 0/);
  assert.match(source, /boardButtonMask\(BoardButton::Previous\)[\s\S]*boardButtonMask\(BoardButton::Next\)[\s\S]*boardButtonMask\(BoardButton::Voice\)/);
  assert.match(source, /M5PM1_GPIO_NUM_3[\s\S]*M5PM1_GPIO_NUM_4[\s\S]*M5PM1_GPIO_NUM_1/);
  assert.match(common, /virtual IBoardDisplay\* display\(\)/);
  assert.match(common, /virtual IBoardRenderer\* renderer\(\)/);
  assert.match(common, /supportsRenderStrategy\(std::string_view strategy\)/);
  assert.match(common, /kMaximumBoardRenderStrategies = 4U/);
  assert.match(common, /virtual BoardRenderStrategyCatalog renderStrategyCatalog\(\) const = 0/);
  assert.match(common, /virtual IAudioCodecControl\* audioCodec\(\)/);
  assert.match(renderer, /PaperColorRenderer::supportsRenderStrategy/);
  assert.match(renderer, /constexpr BoardRenderStrategyCatalog kRenderStrategyCatalog/);
  assert.match(renderer, /static_assert\(kRenderStrategyCatalog\.valid\(\),/);
  for (const strategy of [
    "official-quality", "classic-six-color", "reflectance-photo", "solid-clean",
  ]) assert.match(renderer, new RegExp(`"${strategy}"`));
  assert.match(renderer, /PaperColorRenderer::renderStrategyCatalog/);
  assert.match(renderer, /parseStrategy\(strategy, selected\)/);
  assert.match(renderer, /streamRenderPixels/);

  assert.match(display, /GPIO_NUM_12/);
  assert.match(display, /GPIO_NUM_11/);
  assert.match(display, /GPIO_NUM_43/);
  assert.match(display, /GPIO_NUM_44/);
  assert.match(display, /kSpiClockHz = 4000000/);
  assert.match(display, /transmitByte\(false, 0x10\)/);
  assert.match(display, /transmitByte\(false, 0x04\)/);
  assert.match(display, /transmitByte\(false, 0x12\)/);
  assert.match(display, /transmitByte\(false, 0x02\)/);
  assert.match(displayHeader, /timeout_ms = 20000/);
  assert.match(display, /controller ready without visible refresh/);
  assert.doesNotMatch(source, /writeFullFrame\(/);

  assert.match(manifest, /m5stack\/m5pm1:[\s\S]*==1\.0\.7/);
  assert.match(manifest, /espressif\/led_strip:[\s\S]*==3\.0\.3/);
  assert.match(lock, /m5stack\/m5pm1:[\s\S]*version: 1\.0\.7/);
  assert.match(lock, /espressif\/led_strip:[\s\S]*version: 3\.0\.3/);
  assert.doesNotMatch(source + display + renderer, /Arduino\.h|M5Unified/);
});
