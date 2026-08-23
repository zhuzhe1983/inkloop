import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const firmware = join(repo, "firmware/inkloop-idf");
const common = join(firmware, "components/inkloop_board");
const product = join(firmware, "components/inkloop_product");
const mock = join(firmware, "boards/mock_minimal");
const appMain = join(firmware, "main/app_main.cpp");
const mockBuildTool = join(firmware, "tools/build_mock_minimal.sh");
const projectCmake = join(firmware, "CMakeLists.txt");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "inkloop/board.hpp"
#include "inkloop/board_prompt_policy.hpp"
#include "inkloop/mock_board.hpp"

using namespace inkloop;

constexpr std::array<BoardButton, 3> kKnownButtons{{
    BoardButton::Previous, BoardButton::Next, BoardButton::Voice}};

void exercisePortableBoardConsumer(IBoardAdapter& board) {
  const BoardDescriptor& capabilities = board.descriptor();
  assert(capabilities.id != nullptr);
  assert(capabilities.width > 0U && capabilities.height > 0U);
  assert(capabilities.palette_colors > 0U);
  assert(board.initialize() == ESP_OK);

  IBoardDisplay* display = board.display();
  assert(display != nullptr);
  IBoardRenderer* renderer = board.renderer();
  assert(renderer != nullptr);
  const BoardRenderStrategyCatalog catalog = renderer->renderStrategyCatalog();
  assert(catalog.valid());
  assert(catalog.count == 3U);
  assert(catalog.entries[0].id == "official-quality");
  assert(catalog.entries[0].display_name == "官方高质量");
  assert(catalog.entries[1].id == "classic-six-color");
  assert(catalog.entries[1].display_name == "经典六色抖动");
  assert(catalog.entries[2].id == "solid-clean");
  assert(catalog.entries[2].display_name == "纯色 / 文字");
  assert(catalog.entries[3].id.empty());
  assert(catalog.entries[3].display_name.empty());
  for (size_t index = 0; index < catalog.count; ++index) {
    assert(renderer->supportsRenderStrategy(catalog.entries[index].id));
  }
  assert(!catalog.contains("reflectance-photo"));
  std::vector<uint8_t> rgb(
      static_cast<size_t>(capabilities.width) * capabilities.height * 3U,
      0xffU);
  std::vector<uint8_t> frame(capabilities.packed4BppFrameBytes(), 0x10U);
  const BoardRgbFrameView rgb_view{
      rgb.data(), rgb.size(), capabilities.width, capabilities.height,
      static_cast<size_t>(capabilities.width) * 3U};
  assert(renderer->renderRgbFullFrame(
             rgb_view, "official-quality", frame.data(), frame.size()) ==
         ESP_OK);
  assert(renderer->supportsRenderStrategy("official-quality"));
  assert(renderer->supportsRenderStrategy("classic-six-color"));
  assert(renderer->supportsRenderStrategy("solid-clean"));
  assert(!renderer->supportsRenderStrategy("reflectance-photo"));
  assert(!renderer->supportsOnboardingFrames());
  const BoardFrameView view{frame.data(), frame.size(), capabilities.width,
                            capabilities.height,
                            BoardFrameFormat::Palette4Bpp};
  assert(display->writeFullFrame(view) == ESP_OK);

  for (const BoardButton button : kKnownButtons) {
    if (!capabilities.supportsButton(button)) continue;
    assert(board.buttonGpio(button) != GPIO_NUM_NC);
    (void)board.buttonPressed(button);
  }

  if (capabilities.rgb_pixels > 0U) {
    std::vector<BoardRgbPixel> pixels(capabilities.rgb_pixels);
    assert(board.setRgb(pixels.data(), pixels.size()) == ESP_OK);
  }
  if (capabilities.has_sd) assert(board.prepareSdCard() == ESP_OK);
  if (capabilities.has_microphone || capabilities.has_speaker) {
    assert(board.audioCodec() != nullptr);
    (void)board.audioConfig();
  }
}

int main() {
  mock_board_reset();
  IBoardAdapter& board = board_adapter();
  const BoardDescriptor& capabilities = board.descriptor();

  assert(capabilities.width == 128U);
  assert(capabilities.height == 296U);
  assert(capabilities.palette_colors == 2U);
  assert(!capabilities.has_psram);
  assert(!capabilities.has_sd);
  assert(!capabilities.has_microphone);
  assert(!capabilities.has_speaker);
  assert(capabilities.rgb_pixels == 0U);
  assert(!capabilities.supportsButton(BoardButton::Previous));
  assert(capabilities.supportsButton(BoardButton::Next));
  assert(!capabilities.supportsButton(BoardButton::Voice));
  assert(boardResolution(capabilities) == "128×296");
  assert(aigcImageSize(capabilities) == "128x296");
  assert(myAiDeviceLabel(capabilities) == "Inkloop mock-minimal");
  assert(myAiInstallationFingerprintPrefix(capabilities) == "mock-minimal");
  assert(defaultAssistantPrompt(capabilities).find("128×296") !=
         std::string::npos);
  assert(defaultImagePromptTemplate(capabilities).find("2 色电子纸") !=
         std::string::npos);
  bool landscape = true;
  assert(classifyBoardPngGeometry(capabilities, 128U, 296U, landscape));
  assert(!landscape);
  assert(classifyBoardPngGeometry(capabilities, 296U, 128U, landscape));
  assert(landscape);
  assert(!classifyBoardPngGeometry(capabilities, 400U, 600U, landscape));

  exercisePortableBoardConsumer(board);
  MockBoardObservations observations = mock_board_observations();
  assert(observations.initializations == 1U);
  assert(observations.display_accesses == 1U);
  assert(observations.renderer_accesses == 1U);
  assert(observations.rgb_frame_renders == 1U);
  assert(observations.frame_writes == 1U);
  assert(observations.button_gpio_reads == 1U);
  assert(observations.button_state_reads == 1U);
  assert(observations.rgb_writes == 0U);
  assert(observations.sd_preparations == 0U);
  assert(observations.audio_codec_accesses == 0U);

  mock_board_set_next_pressed(true);
  assert(board.buttonPressed(BoardButton::Next));
  assert(board.buttonGpio(BoardButton::Previous) == GPIO_NUM_NC);

  IBoardDisplay* display = board.display();
  std::vector<uint8_t> frame(capabilities.packed4BppFrameBytes(), 0x10U);
  BoardFrameView invalid{frame.data(), frame.size(), capabilities.height,
                         capabilities.width,
                         BoardFrameFormat::Palette4Bpp};
  assert(display != nullptr);
  assert(display->writeFullFrame(invalid) == ESP_ERR_INVALID_ARG);
  frame[0] = 0x20U;
  const BoardFrameView invalid_palette{
      frame.data(), frame.size(), capabilities.width, capabilities.height,
      BoardFrameFormat::Palette4Bpp};
  assert(display->writeFullFrame(invalid_palette) == ESP_ERR_INVALID_ARG);
  assert(board.setRgb(nullptr, 0U) == ESP_ERR_NOT_SUPPORTED);
  assert(board.prepareSdCard() == ESP_ERR_NOT_SUPPORTED);
  assert(board.audioCodec() == nullptr);
  board.shutdown();
  assert(board.display() == nullptr);
  return 0;
}
`;

function writeStub(root, relative, source) {
  const target = join(root, relative);
  mkdirSync(dirname(target), { recursive: true });
  writeFileSync(target, source);
}

function readSourceTree(root) {
  return readdirSync(root, { withFileTypes: true })
    .sort((left, right) => left.name.localeCompare(right.name))
    .map((entry) => {
      const path = join(root, entry.name);
      return entry.isDirectory()
        ? readSourceTree(path)
        : readFileSync(path, "utf8");
    })
    .join("\n");
}

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-cross-sku-board-"));
  try {
    const stubs = join(scratch, "stubs");
    writeStub(stubs, "driver/gpio.h", String.raw`
#pragma once
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_NC = -1;
`);
    writeStub(stubs, "driver/i2c_master.h", String.raw`
#pragma once
using i2c_master_bus_handle_t = void*;
`);
    writeStub(stubs, "driver/spi_common.h", String.raw`
#pragma once
using spi_host_device_t = int;
`);
    writeStub(stubs, "esp_err.h", String.raw`
#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;
`);
    writeStub(stubs, "inkloop/esp_i2s_audio.hpp", String.raw`
#pragma once
namespace inkloop {
class IAudioCodecControl {
 public:
  virtual ~IAudioCodecControl() = default;
};
struct EspI2sAudioConfig {};
}  // namespace inkloop
`);

    const source = join(scratch, "cross_sku_board.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", stubs,
      "-I", join(common, "include"),
      "-I", join(product, "include"),
      "-I", join(mock, "include"),
      source, join(mock, "board.cpp"),
      join(product, "board_prompt_policy.cpp"), "-o", binary,
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

test("minimal mock SKU passes a strict capability-driven C++17 consumer", () => {
  buildAndRun(false);
});

test("minimal mock SKU capability paths are memory-safe under ASan/UBSan", () => {
  buildAndRun(true);
});

test("generic board contract contains no fixed PaperColor hardware facts", () => {
  const contract = readSourceTree(common);
  assert.match(contract, /button_mask/);
  assert.match(contract, /supportsButton\(BoardButton button\)/);
  assert.match(contract, /packed4BppFrameBytes\(\)/);
  assert.match(contract, /kMaximumBoardRenderStrategies = 4U/);
  assert.match(contract, /BoardRenderStrategyCatalog/);
  assert.match(contract, /renderStrategyCatalog\(\)/);
  assert.doesNotMatch(
    contract,
    /\b(?:400|600)\b|m5|papercolor|c151|ed2208|pm1|GPIO_NUM_\d+|SPI2_HOST|M5Unified|Arduino/i,
  );
});

test("mock board remains an isolated selectable ESP-IDF component", () => {
  const cmake = readFileSync(join(mock, "CMakeLists.txt"), "utf8");
  const source = readFileSync(join(mock, "board.cpp"), "utf8");
  const sdkconfig = readFileSync(join(mock, "sdkconfig.defaults"), "utf8");
  assert.match(cmake, /idf_component_register\(/);
  assert.match(cmake, /REQUIRES inkloop_audio_idf inkloop_board/);
  assert.match(source, /"mock-minimal"/);
  assert.match(source, /constexpr BoardRenderStrategyCatalog kRenderStrategyCatalog/);
  assert.match(source, /static_assert\(kRenderStrategyCatalog\.valid\(\),/);
  assert.doesNotMatch(source, /reflectance-photo/);
  assert.doesNotMatch(source, /m5|papercolor|c151|ed2208|pm1|SPI2_HOST/i);
  assert.match(sdkconfig, /CONFIG_ESP_CONSOLE_SECONDARY_NONE=y/);
  assert.match(
    sdkconfig,
    /# CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is not set/,
  );
});

test("product boot gates every SD probe on the selected board capability", () => {
  const source = readFileSync(appMain, "utf8");
  assert.match(
    source,
    /if \(board\.has_sd\) \{[\s\S]*board_adapter\.prepareSdCard\(\)[\s\S]*board_adapter\.sdCardInserted\(\)[\s\S]*storage\.mountSd\(/,
  );
  assert.match(source, /else \{[\s\S]*SD:UNSUPPORTED board=%s/);
  assert.equal((source.match(/prepareSdCard\(\)/g) ?? []).length, 1);
  assert.equal((source.match(/sdCardInserted\(\)/g) ?? []).length, 1);
});

test("mock full-product build tool uses an isolated board-specific cache", () => {
  const source = readFileSync(mockBuildTool, "utf8");
  const cmake = readFileSync(projectCmake, "utf8");
  assert.match(source, /build-mock-minimal/);
  assert.match(source, /project_dir="\$proof_root\/project"/);
  assert.match(source, /build_dir="\$proof_root\/build-esp32s3"/);
  assert.match(source, /ln -sfn "\$project_root\/\$entry" "\$target"/);
  assert.match(source, /refusing non-symlink proof input/);
  assert.match(
    source,
    /CMakeLists\.txt boards components main partitions\.csv sdkconfig\.defaults version\.txt/,
  );
  assert.match(source, /INKLOOP_BOARD=mock_minimal/);
  assert.match(source, /IDF_TARGET=esp32s3/);
  assert.match(source, /IDF_TARGET:STRING=esp32s3/);
  assert.match(source, /SDKCONFIG="\$build_dir\/sdkconfig"/);
  assert.match(source, /INKLOOP_BOARD:STRING=mock_minimal/);
  assert.doesNotMatch(source, /boards\/m5_papercolor|INKLOOP_BOARD=m5/);
  assert.match(
    cmake,
    /if\(INKLOOP_BOARD STREQUAL "mock_minimal"\)[\s\S]*idf_build_set_property\(DEPENDENCIES_LOCK[\s\S]*CMAKE_BINARY_DIR[\s\S]*endif\(\)/,
  );
});
