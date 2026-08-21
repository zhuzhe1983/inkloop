import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
const portalRoot = new URL(
  "../firmware/m5-papercolor/lib/InkloopPortal/", import.meta.url,
);

function compileAndRun(source, output, sanitizer) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", sourceRoot.pathname, "-I", portalRoot.pathname, source, "-o", output,
  ];
  if (sanitizer) {
    args.unshift(
      "-O1", "-g", "-fno-omit-frame-pointer",
      "-fsanitize=address,undefined",
    );
  }
  const built = spawnSync("c++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, built.stderr || built.stdout);
  const ran = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitizer
      ? {
          ...process.env,
          ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
          UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
        }
      : process.env,
  });
  assert.equal(ran.status, 0, ran.stderr || ran.stdout);
}

test("portal access and Wi-Fi AP compact layouts preserve exact bounded values", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-compact-status-"));
  try {
    const source = join(temporary, "compact.cpp");
    const executable = join(temporary, "compact");
    await writeFile(source, String.raw`
#include <cassert>
#include <string>
#include "CompactStatusLayoutPrimitives.h"
#include "PortalAccessDisplayPrimitives.h"

int main() {
  using namespace inkloop;
  const std::string portal = std::string(24, '5') + "-B";
  assert(portal.size() == 26);
  const CompactStatusValueLayout portalLayout = layoutPortalAccessCode(portal);
  assert(portalLayout.valid);
  assert(portalLayout.firstLine.size() == 13);
  assert(portalLayout.secondLine.size() == 13);
  assert(portalLayout.firstLine + portalLayout.secondLine == portal);

  const CompactStatusValueLayout wifi = layoutWifiAccessPoint("Inkloop-8428");
  assert(wifi.valid);
  assert(wifi.firstLine == "Inkloop-8428");
  assert(wifi.secondLine.empty());

  const std::string widestWifi(32, 'A');
  const CompactStatusValueLayout wrappedWifi =
      layoutWifiAccessPoint(widestWifi);
  assert(wrappedWifi.valid);
  assert(wrappedWifi.firstLine.size() == 16);
  assert(wrappedWifi.secondLine.size() == 16);
  assert(wrappedWifi.firstLine + wrappedWifi.secondLine == widestWifi);
  assert(!layoutPortalAccessCode(std::string(27, 'A')).valid);
  assert(!layoutWifiAccessPoint(std::string(33, 'A')).valid);
  assert(!layoutWifiAccessPoint("Inkloop 8428").valid);
  assert(!layoutPortalAccessCode(std::string()).valid);

  const std::string password = "INKLOOP7K9Q2";
  const PortalAccessDisplayLayout settings = makePortalAccessDisplayLayout(
      "Inkloop-8428-Settings", "192.168.4.1", password);
  assert(settings.valid);
  assert(settings.passwordLines.size() == 1);
  assert(settings.passwordLines[0] == password);
  assert(settings.ipUrl == "http://192.168.4.1/");
  assert(settings.localUrl == "http://inkloop.local/");
  const PortalAccessDisplayLayout longest = makePortalAccessDisplayLayout(
      "Inkloop-8428-Settings", "192.168.4.1", std::string(63, 'A'));
  assert(longest.valid);
  assert(longest.passwordLines.size() == 3);
  std::string joined;
  for (const std::string& line : longest.passwordLines) joined += line;
  assert(joined == std::string(63, 'A'));
  assert(!makePortalAccessDisplayLayout(
      "Inkloop-8428-Settings", "192.168.4.1", std::string(7, 'A')).valid);
  assert(!makePortalAccessDisplayLayout(
      "Inkloop-8428-Settings", "not-an-ip", password).valid);
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-sanitized`, true);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("only portal and Wi-Fi boot pages use explicit compact rendering without secret logs", async () => {
  const [display, header, main] = await Promise.all([
    readFile(new URL("DisplayController.cpp", sourceRoot), "utf8"),
    readFile(new URL("DisplayController.h", sourceRoot), "utf8"),
    readFile(new URL("main.cpp", sourceRoot), "utf8"),
  ]);
  assert.match(header, /showPortalAccess\(/);
  assert.match(header, /showSettingsPortal\(/);
  assert.match(header, /showWifiSetup\(/);
  assert.match(display, /showPortalAccess[\s\S]*showCompactStatus\([\s\S]*13/);
  assert.match(display, /showSettingsPortal[\s\S]*makePortalAccessDisplayLayout/);
  const settingsScreen = display.slice(
    display.indexOf("bool DisplayController::showSettingsPortal"),
    display.indexOf("bool DisplayController::showWifiSetup"),
  );
  assert.doesNotMatch(settingsScreen, /qrcode|wifiQrPayload|WIFI:T:/);
  assert.match(settingsScreen, /layout\.ipUrl/);
  assert.match(settingsScreen, /layout\.localUrl/);
  assert.match(settingsScreen, /Settings password \(defaults to home Wi-Fi\)/);
  assert.match(settingsScreen, /Not the six-digit MyAI binding code/);
  assert.match(display, /showWifiSetup[\s\S]*showCompactStatus\([\s\S]*16/);
  assert.match(display, /showCompactStatus[\s\S]*setTextFont\(4\)/);
  assert.match(display, /showStatus[\s\S]*setTextFont\(7\)/);

  const wifiBoot = main.slice(
    main.indexOf("void beginWifiProvisioning()"),
    main.indexOf("void pollWifiProvisioning()"),
  );
  assert.doesNotMatch(wifiBoot, /safeShowWifiSetup\(/);

  const wifiPortal = main.slice(
    main.indexOf("void showWifiPortal("),
    main.indexOf("void applyWifiProvisioningActions("),
  );
  assert.match(wifiPortal, /safeShowWifiSetup\(/);
  assert.doesNotMatch(wifiPortal, /safeShowStatus\(/);

  const portalWrapper = main.slice(
    main.indexOf("bool safeShowSettingsPortal("),
    main.indexOf("bool safeShowWifiSetup("),
  );
  assert.match(portalWrapper, /display\.showSettingsPortal/);
  assert.doesNotMatch(portalWrapper, /Diagnostics::event\([^;]*accessCode/);
  const online = main.slice(
    main.indexOf("void completeOnlineInitialization()"),
    main.indexOf("void loop()"),
  );
  assert.match(online, /safeShowSettingsPortal\([\s\S]*portalAccessCode\(\)/);
  assert.match(online, /MyAI service unavailable[\s\S]*No binding QR - contact Inkloop developer/);
  assert.match(online, /AwaitingMyAiPairing[\s\S]*WAITING_FOR_AUTHORITATIVE_MYAI_QR/);
});
