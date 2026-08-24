import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf");
const product = join(idf, "components/inkloop_product");
const fakeBoard = String.raw`
#pragma once
#include <cstdint>
namespace inkloop {
struct BoardRgbPixel { uint8_t red=0; uint8_t green=0; uint8_t blue=0; };
}
`;
const harness = String.raw`
#include <cassert>
#include "inkloop/status_led_core.hpp"
using namespace inkloop;
int main() {
  StatusLedCore leds;
  StatusLedFrame zero = leds.render(0, 0);
  assert(zero.count == 0);

  StatusLedFrame frame = leds.render(0, 2);
  assert(frame.count == 2);
  auto pixels = frame.pixels;
  assert(pixels[0].red == 0 && pixels[0].green == 0 && pixels[0].blue == 0);
  assert(pixels[1].red == 0 && pixels[1].green == 0 && pixels[1].blue == 0);

  leds.setVoiceMode(VoiceLedMode::Listening);
  pixels = leds.render(0, 2).pixels;
  assert(pixels[0].green > pixels[0].red && pixels[0].green > pixels[0].blue);
  assert(pixels[1].red == 0 && pixels[1].green == 0 && pixels[1].blue == 0);
  auto swapped = leds.render(0, 2, true).pixels;
  assert(swapped[1].green == pixels[0].green);
  assert(swapped[0].red == 0 && swapped[0].green == 0 &&
         swapped[0].blue == 0);

  leds.setVoiceMode(VoiceLedMode::Blocked);
  pixels = leds.render(0, 2).pixels;
  assert(pixels[0].red > 0 && pixels[0].green == 0 && pixels[0].blue == 0);

  leds.setImageMode(ImageLedMode::Generating);
  pixels = leds.render(800, 2).pixels;
  assert(pixels[1].green > pixels[1].red && pixels[1].green > pixels[1].blue);
  leds.setImageMode(ImageLedMode::Writing);
  frame = leds.render(0, 2);
  pixels = frame.pixels;
  assert(pixels[1].red > 0 && pixels[1].green > 0 && pixels[1].blue == 0);

  // A one-pixel board preserves both logical signals by taking the maximum
  // of each channel; a two-pixel board retains the C151 Voice/Image order.
  const StatusLedFrame combined = leds.render(0, 1);
  assert(combined.count == 1);
  assert(combined.pixels[0].red ==
         (pixels[0].red > pixels[1].red ? pixels[0].red : pixels[1].red));
  assert(combined.pixels[0].green ==
         (pixels[0].green > pixels[1].green ? pixels[0].green : pixels[1].green));
  assert(combined.pixels[0].blue ==
         (pixels[0].blue > pixels[1].blue ? pixels[0].blue : pixels[1].blue));

  // A brightness change runs the same bounded hardware/role flow as the old
  // firmware: both white, left blue/cyan twice, right yellow/orange twice,
  // then the latest logical state resumes without blocking the LED task.
  leds.startHardwareTest(1000);
  pixels = leds.render(1000, 2).pixels;
  assert(pixels[0].red > 0 && pixels[0].green > 0 && pixels[0].blue > 0);
  assert(pixels[1].red > 0 && pixels[1].green > 0 && pixels[1].blue > 0);
  pixels = leds.render(2000, 2).pixels;
  assert(pixels[0].blue > 0 && pixels[1].red == 0);
  pixels = leds.render(4000, 2).pixels;
  assert(pixels[0].red == 0 && pixels[1].red > 0 && pixels[1].green > 0);
  const StatusLedFrame one_pixel_test = leds.render(4000, 1);
  assert(one_pixel_test.count == 1 && one_pixel_test.pixels[0].red > 0 &&
         one_pixel_test.pixels[0].green > 0);
  const StatusLedFrame swapped_test = leds.render(4000, 2, true);
  assert(swapped_test.pixels[0].red > 0 &&
         swapped_test.pixels[0].green > 0 &&
         swapped_test.pixels[1].red == 0);
  assert(leds.hardwareTestActive(5599));
  assert(!leds.hardwareTestActive(5600));
  pixels = leds.render(5600, 2).pixels;
  assert(pixels[0].red > 0 && pixels[1].red > 0);

  leds.setMaximumBrightness(0);
  pixels = leds.render(0, 2).pixels;
  assert(pixels[0].red == 0 && pixels[1].red == 0);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-led-"));
  try {
    const fake = join(scratch, "inkloop");
    execFileSync("mkdir", ["-p", fake]);
    writeFileSync(join(fake, "board.hpp"), fakeBoard);
    const source = join(scratch, "status_led_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", scratch, "-I", join(product, "include"), source,
      join(product, "status_led_core.cpp"), "-o", binary,
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

test("logical left voice and right image LEDs preserve visible state colors", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("native LED owner is non-blocking and the only product RGB writer", () => {
  const owner = readFileSync(join(product, "status_led_owner.cpp"), "utf8");
  const core = readFileSync(join(product, "status_led_core.cpp"), "utf8");
  const productSources = readdirSync(product)
    .filter((name) => name.endsWith(".cpp"));
  const allProduct = productSources
    .map((name) => readFileSync(join(product, name), "utf8"))
    .join("\n");
  const otherProduct = productSources
    .filter((name) => name !== "status_led_owner.cpp")
    .map((name) => readFileSync(join(product, name), "utf8"))
    .join("\n");
  assert.match(owner, /registerTickHandler[\s\S]*20/);
  assert.match(owner, /board_\.descriptor\(\)\.rgb_pixels/);
  assert.match(owner, /core_\.render\(nowMs\(\), physical_pixels_, swap_roles\)/);
  assert.match(owner, /if \(frame\.count > 0U\)[\s\S]*board_\.setRgb\(frame\.pixels\.data\(\), frame\.count\)/);
  assert.match(owner, /void EspStatusLedOwner::shutdown\(\)[\s\S]*BoardRgbPixel[^;]+dark\{\}[\s\S]*board_\.setRgb\(dark\.data\(\), physical_pixels_\)/);
  assert.match(core, /available_pixels == 0U/);
  assert.match(core, /available_pixels == 1U[\s\S]*merge\(logical\[0\], logical\[1\]\)/);
  assert.match(core, /output\.pixels = roles_swapped[\s\S]*output\.pixels\.size\(\)/);
  assert.match(owner, /ProductOpcode::SetLedMaximumBrightness/);
  assert.match(owner, /envelope\.flags\) \* 255U \/ 100U/);
  assert.match(owner, /core_\.startHardwareTest\(nowMs\(\)\)/);
  assert.match(owner, /setPresentation[\s\S]*roles_swapped_ = physical_pixels_ >= 2U/);
  assert.match(core, /roles_swapped[\s\S]*logical\[1\][\s\S]*logical\[0\]/);
  assert.equal((owner.match(/board_\.setRgb\(/g) ?? []).length, 2);
  assert.equal((allProduct.match(/\.setRgb\(/g) ?? []).length, 2);
  assert.equal((otherProduct.match(/\.setRgb\(/g) ?? []).length, 0);
  assert.doesNotMatch(owner, /std::array<BoardRgbPixel,\s*2>|pixels\.size\(\)/);
  assert.doesNotMatch(owner, /vTaskDelay|delay\(|sleep\(/);
});
