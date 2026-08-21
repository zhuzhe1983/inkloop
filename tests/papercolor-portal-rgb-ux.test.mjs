import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);

function compileAndRun(source, output, sanitized) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", sourceRoot.pathname, source, "-o", output,
  ];
  if (sanitized) {
    args.unshift(
      "-O1", "-g", "-fno-omit-frame-pointer",
      "-fsanitize=address,undefined",
    );
  }
  const built = spawnSync(process.env.CXX || "c++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, built.stderr || built.stdout);
  const ran = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitized ? {
      ...process.env,
      ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    } : process.env,
  });
  assert.equal(ran.status, 0, ran.stderr || ran.stdout);
}

test("RGB role diagnostic is visible, bounded, non-blocking, and restores role state", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-rgb-role-"));
  try {
    const source = join(temporary, "rgb.cpp");
    const executable = join(temporary, "rgb");
    await writeFile(source, String.raw`
#include <cassert>
#include "LedRoleDiagnosticPrimitives.h"

int main() {
  using namespace inkloop;
  assert(kLedDiagnosticBrightness == 144);
  assert(kLedDiagnosticTotalMilliseconds == 4600);
  const LedRoleDiagnosticFrame power = ledRoleDiagnosticFrame(0);
  assert(power.active && power.illuminated && !power.complete);
  assert(power.role == LedDiagnosticRole::PowerProof);
  assert(power.red == 255 && power.green == 255 && power.blue == 255);
  const LedRoleDiagnosticFrame voiceBlue = ledRoleDiagnosticFrame(800);
  assert(voiceBlue.active && voiceBlue.illuminated && !voiceBlue.complete);
  assert(voiceBlue.role == LedDiagnosticRole::Voice);
  assert(voiceBlue.phase == 1 && voiceBlue.cycle == 0);
  assert(voiceBlue.red == 0 && voiceBlue.green == 110 && voiceBlue.blue == 255);
  const LedRoleDiagnosticFrame voiceOff = ledRoleDiagnosticFrame(1600);
  assert(voiceOff.active && !voiceOff.illuminated && voiceOff.phase == 2);
  const LedRoleDiagnosticFrame voiceCyan = ledRoleDiagnosticFrame(1750);
  assert(voiceCyan.role == LedDiagnosticRole::Voice && voiceCyan.cycle == 1);
  assert(voiceCyan.red == 0 && voiceCyan.green == 255 && voiceCyan.blue == 255);
  const LedRoleDiagnosticFrame imageYellow = ledRoleDiagnosticFrame(2700);
  assert(imageYellow.role == LedDiagnosticRole::Image && imageYellow.cycle == 0);
  assert(imageYellow.red == 255 && imageYellow.green == 210 && imageYellow.blue == 0);
  const LedRoleDiagnosticFrame imageOrange = ledRoleDiagnosticFrame(3650);
  assert(imageOrange.role == LedDiagnosticRole::Image && imageOrange.cycle == 1);
  assert(imageOrange.red == 255 && imageOrange.green == 110 && imageOrange.blue == 0);
  const LedRoleDiagnosticFrame complete = ledRoleDiagnosticFrame(4600);
  assert(!complete.active && complete.complete && !complete.illuminated);
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-sanitized`, true);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("production RGB integration polls asynchronously and fails closed", async () => {
  const [controller, header, application, board] = await Promise.all([
    readFile(new URL("LedStatusController.cpp", sourceRoot), "utf8"),
    readFile(new URL("LedStatusController.h", sourceRoot), "utf8"),
    readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("PaperColorBoardSupport.cpp", sourceRoot), "utf8"),
  ]);
  const start = controller.slice(
    controller.indexOf("bool LedStatusController::runPixelDiagnostic()"),
    controller.indexOf("void LedStatusController::pollPixelDiagnostic"),
  );
  assert.match(start, /diagnosticActive_ \|\| count_ != 2 \|\| !calibrated_/);
  assert.match(start, /LED_ROLE_DIAGNOSTIC", "START"/);
  assert.doesNotMatch(start, /delay\s*\(|while\s*\(|for\s*\(/);
  assert.match(header, /void pollPixelDiagnostic\(uint32_t nowMilliseconds\)/);
  assert.match(controller, /renderRolesLocked\(desiredRoleBrightness_\)/);
  assert.match(controller, /LED_ROLE_DIAGNOSTIC", "COMPLETE"/);
  assert.match(controller, /desiredRoleBrightness_ = brightness;[\s\S]*if \(!diagnosticActive_\)/);
  assert.match(application, /leds_\.pollPixelDiagnostic\(millis\(\)\)/);
  assert.match(application, /if \(leds_\.pixelDiagnosticActive\(\)\)[\s\S]*noteMeaningfulActivity[\s\S]*return;/);
  assert.match(application, /mutationBusy\(\) const[\s\S]*leds_\.pixelDiagnosticActive\(\)/);
  assert.match(application, /leds_\.count\(\) != 2/);
  assert.match(application, /!leds_\.mappingCalibrated\(\)[\s\S]*setMapping\(true, desiredVoiceLedIndex\)/);
  assert.match(header, /Adafruit_NeoPixel pixels_/);
  assert.match(header, /pixels_\(2, 21, NEO_GRB \+ NEO_KHZ800\)/);
  assert.match(controller, /pixels_\.begin\(\)/);
  assert.match(controller, /pixels_\.setBrightness\(scaledBrightness\(kLedDiagnosticBrightness\)\)/);
  assert.match(header, /setMaximumBrightnessPercent\(uint8_t percent\)/);
  assert.match(controller, /maximumBrightnessPercent_\) \* 255U/);
  assert.match(application, /testLedRoles[\s\S]*pendingLedMaximumBrightnessPercent_ = maximumBrightnessPercent[\s\S]*setMaximumBrightnessPercent\([\s\S]*pendingLedMaximumBrightnessPercent_[\s\S]*runPixelDiagnostic/);
  assert.match(application, /LED_ROLE_DIAGNOSTIC", "QUEUED"/);
  assert.match(application, /ledDiagnosticPending_ = true/);
  assert.match(application, /ledDiagnosticPending_ && !leds_\.pixelDiagnosticActive\(\)[\s\S]*startPendingLedDiagnostic/);
  assert.match(application, /mutationBusy\(\) const[\s\S]*ledDiagnosticPending_/);
  assert.match(controller, /PowerProof[\s\S]*setPixelColor[\s\S]*pixels_\.show\(\)/);
  assert.match(controller, /NEOPIXEL_GPIO21_GRB_READY/);
  assert.match(board, /setLdoEnable\(true\)/);
  assert.match(board, /RGB_POWER[\s\S]*LDO_ENABLE_FAILED/);
  assert.match(board, /board_M5PaperColor && pm1Ready_/);
  assert.match(board, /HARDWARE_READY[\s\S]*ready \? "READY" : "ERROR_BOARD_PM1_OR_RGB_POWER"/);
});
