import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
const portalRoot = new URL(
  "../firmware/m5-papercolor/lib/InkloopPortal/", import.meta.url,
);

function compileAndRun(source, output, sanitized) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", portalRoot.pathname, source, "-o", output,
  ];
  if (sanitized) args.unshift(
    "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
  );
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

test("local management password and prompt defaults are bounded under C++11", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-settings-v2-"));
  try {
    const source = join(temporary, "settings.cpp");
    const executable = join(temporary, "settings");
    await writeFile(source, String.raw`
#include <cassert>
#include <string>
#include "PortalContracts.h"
#include "PortalSecurityPrimitives.h"

int main() {
  using namespace inkloop::portal;
  assert(kLegacyPortalSnapshotSchemaVersion == 1);
  assert(kPortalSnapshotSchemaVersion == 2);
  assert((kLegacyPortalSnapshotFields & SnapshotImagePromptTemplate) == 0);
  assert((kLegacyPortalSnapshotFields & SnapshotLocalManagementPassword) == 0);
  assert((kAllPortalSnapshotFields & SnapshotImagePromptTemplate) != 0);
  assert((kAllPortalSnapshotFields & SnapshotLocalManagementPassword) != 0);
  assert(validLocalManagementPassword("A1b2C3d4"));
  assert(validLocalManagementPassword("Inkloop-9Q2K"));
  assert(validLocalManagementPassword("easy wifi password"));
  assert(validLocalManagementPassword("simple;pass"));
  assert(validLocalManagementPassword("back\\slash8"));
  assert(validLocalManagementPassword(std::string(63, 'A')));
  assert(!validLocalManagementPassword("A1b2C3d"));
  assert(!validLocalManagementPassword(std::string(64, 'A')));
  assert(!validLocalManagementPassword(std::string("bad\npass9", 9)));
  assert(!validLocalManagementPassword("密码Inkloop9"));
  PortalSettings defaults;
  assert(defaults.ledMaximumBrightnessPercent == 60);
  assert(defaults.localManagementPassword.empty());
  assert(!defaults.assistantPrompt.empty() && defaults.assistantPrompt.size() <= 512);
  assert(!defaults.imagePromptTemplate.empty() &&
         defaults.imagePromptTemplate.size() <= 512);
  assert(defaults.imagePromptTemplate.find("{prompt}") != std::string::npos);
  assert(!defaults.image.negativePrompt.empty() &&
         defaults.image.negativePrompt.size() <= 384);
  assert(defaults.image.width == 400 && defaults.image.height == 600);
  assert(defaults.assistantPrompt.find("400×600") != std::string::npos);
  assert(defaults.assistantPrompt.find("底边朝下") != std::string::npos);
  assert(defaults.assistantPrompt.find("15–30") != std::string::npos);
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-sanitized`, true);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("v1 migration preserves fields and atomically adds only v2 settings", async () => {
  const [runtime, portal] = await Promise.all([
    readFile(new URL("PaperColorPortalRuntime.cpp", sourceRoot), "utf8"),
    readFile(new URL("InkloopPortal.cpp", portalRoot), "utf8"),
  ]);
  const migration = runtime.slice(
    runtime.indexOf("snapshot.schemaVersion ==\n                 portal::kLegacyPortalSnapshotSchemaVersion"),
    runtime.indexOf("} else if (loaded == PortalSnapshotLoadResult::LoadedLegacy)"),
  );
  assert.match(migration, /snapshot\.presentFields == portal::kLegacyPortalSnapshotFields/);
  assert.match(migration, /snapshot\.schemaVersion = portal::kPortalSnapshotSchemaVersion/);
  assert.match(migration, /snapshot\.presentFields = portal::kAllPortalSnapshotFields/);
  assert.match(runtime, /settings\["led_brightness"\] \| 60U/);
  assert.match(migration, /\+\+snapshot\.revision/);
  assert.match(migration, /snapshot\.settings\.localManagementPassword = initialPassword/);
  assert.match(migration, /storeSnapshot\(snapshot\)/);
  assert.doesNotMatch(migration, /makeFreshPortalSnapshot/);
  assert.match(runtime, /settings\["image_prompt"\] = snapshot\.settings\.imagePromptTemplate/);
  assert.match(runtime, /settings\["local_password"\] = snapshot\.settings\.localManagementPassword/);
  assert.match(runtime, /if \(current &&[\s\S]*settings\["image_prompt"\][\s\S]*settings\["local_password"\]/);
  assert.match(runtime, /access_\.bootNonce = snapshot\.settings\.localManagementPassword/);
  assert.match(runtime, /initialManagementPassword[\s\S]*return wifiPassword[\s\S]*return "inkloop8"/);
  assert.match(runtime, /WiFi\.psk\(\)/);
  assert.doesNotMatch(runtime, /Diagnostics::event\([^;]*localManagementPassword/);
  assert.match(portal, /localManagementPassword[\s\S]*configured[\s\S]*minimumLength/);
  assert.doesNotMatch(portal, /jsonEscape\(settings_\.localManagementPassword\)/);
  assert.doesNotMatch(portal, /htmlEscape\(settings_\.localManagementPassword\)/);
});
