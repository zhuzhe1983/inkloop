import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const firmwareSource = new URL("../firmware/m5-papercolor/src/", import.meta.url);

function compileAndRun(source, output, sanitizer = false) {
  const args = [
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-I",
    new URL("../firmware/m5-papercolor/src/", import.meta.url).pathname,
    source,
    "-o",
    output,
  ];
  if (sanitizer) {
    args.unshift("-fno-omit-frame-pointer", "-fsanitize=address,undefined");
  }
  const compiled = spawnSync("c++", args, { encoding: "utf8" });
  assert.equal(compiled.status, 0, compiled.stderr || compiled.stdout);
  const executed = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitizer
      ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS: "halt_on_error=1" }
      : process.env,
  });
  assert.equal(executed.status, 0, executed.stderr || executed.stdout);
}

test("PaperColor integration blockers fail closed under revision faults and preserve legacy mode", async () => {
  const directory = await mkdtemp(join(tmpdir(), "inkloop-integration-blockers-"));
  try {
    const source = join(directory, "blockers.cpp");
    const executable = join(directory, "blockers");
    await writeFile(source, String.raw`
#include <cassert>
#include <cstdint>
#include <vector>

#include "AlbumMutationPrimitives.h"
#include "CompatibilityPrimitives.h"

using namespace inkloop;

int main() {
  assert(!useLegacyDirectDisplay(true, true));
  assert(useLegacyDirectDisplay(false, true));
  assert(useLegacyDirectDisplay(true, false));
  assert(useLegacyDirectDisplay(false, false));

  assert(shouldRecoverClosedVoiceAfterCancel(true, true, true));
  assert(!shouldRecoverClosedVoiceAfterCancel(false, true, true));
  assert(!shouldRecoverClosedVoiceAfterCancel(true, false, true));
  assert(!shouldRecoverClosedVoiceAfterCancel(true, true, false));

  uint64_t promoted = 77;
  int mutationCalls = 0;
  std::vector<int> order;
  AlbumMutationResult failedPersistence = runRevisionGatedAlbumMutation(
      77,
      [&](uint64_t next) { order.push_back(1); assert(next == 78); return false; },
      [&](uint64_t revision, bool healthy) {
        order.push_back(2); assert(revision == 0); assert(!healthy);
      },
      [&]() { ++mutationCalls; return true; },
      &promoted);
  assert(failedPersistence == AlbumMutationResult::RevisionPersistenceFailed);
  assert(promoted == 77 && mutationCalls == 0);
  assert((order == std::vector<int>{1, 2}));

  order.clear();
  AlbumMutationResult failedMutation = runRevisionGatedAlbumMutation(
      77,
      [&](uint64_t next) { order.push_back(1); return next == 78; },
      [&](uint64_t revision, bool healthy) {
        order.push_back(2); assert(revision == 78); assert(healthy);
      },
      [&]() { order.push_back(3); ++mutationCalls; return false; },
      &promoted);
  assert(failedMutation == AlbumMutationResult::MutationFailed);
  assert(promoted == 78 && mutationCalls == 1);
  assert((order == std::vector<int>{1, 2, 3}));

  bool overflowMutation = false;
  AlbumMutationResult overflow = runRevisionGatedAlbumMutation(
      UINT64_MAX,
      [&](uint64_t) { assert(false); return true; },
      [&](uint64_t revision, bool healthy) {
        assert(revision == 0); assert(!healthy);
      },
      [&]() { overflowMutation = true; return true; });
  assert(overflow == AlbumMutationResult::RevisionUnavailable);
  assert(!overflowMutation);
  return 0;
}
`, "utf8");
    compileAndRun(source, executable, false);
    compileAndRun(source, `${executable}-san`, true);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("PaperColor concrete adapters wire cancel recovery and revision gates before every mutation", async () => {
  const [application, main, conversation, conversationHeader] = await Promise.all([
    readFile(new URL("PaperColorApplicationRuntime.cpp", firmwareSource), "utf8"),
    readFile(new URL("main.cpp", firmwareSource), "utf8"),
    readFile(new URL("PaperColorVoiceAdapters.cpp", firmwareSource), "utf8"),
    readFile(new URL("PaperColorVoiceAdapters.h", firmwareSource), "utf8"),
  ]);
  assert.doesNotMatch(conversation, /M5\.Mic\.isEnabled\(\)/);
  assert.match(
    conversation,
    /M5\.Mic\.isRecording\(\) \|\| M5\.Mic\.isRunning\(\)/,
  );

  assert.match(conversation, /cancelTurn\(\)[\s\S]*disconnectVoice\("turn_cancelled"\)/);
  assert.match(conversationHeader, /cancelTurnClosesSession\(\) const override \{ return true; \}/);
  assert.match(application, /shouldRecoverClosedVoiceAfterCancel\([\s\S]*voiceWasReady_ = false;[\s\S]*voiceReconnectAt_ = millis\(\)/);
  assert.match(application, /myAiAuthorized_ && !voiceWasReady_[\s\S]*connectVoiceIfAuthorized\(\)/);

  const voiceRuntime = await readFile(
    new URL("../firmware/m5-papercolor/lib/InkloopVoice/src/VoiceRuntime.cpp", import.meta.url),
    "utf8",
  );
  const cancel = voiceRuntime.slice(
    voiceRuntime.indexOf("Status VoiceRuntime::cancelCurrentTurn()"),
    voiceRuntime.indexOf("Status VoiceRuntime::resetError()"),
  );
  assert.ok(cancel.indexOf("cancelTurnClosesSession()") < cancel.indexOf("cleanupAudio()"));
  assert.ok(cancel.indexOf("sessionReady_ = false") < cancel.indexOf("cleanupAudio()"));

  const portal = application.slice(
    application.indexOf("executeConfirmedOperation("),
    application.indexOf("bool PaperColorApplicationRuntime::mutationBusy"),
  );
  assert.match(portal, /runAlbumMutation\([\s\S]*deleteUserAsset[\s\S]*clearUserAssets[\s\S]*formatFat/);
  for (const method of ["deleteImageById", "clearAllUserImages", "formatStorage"]) {
    const start = application.indexOf(`PaperColorApplicationRuntime::${method}`);
    const next = application.indexOf("\nvoice::Status PaperColorApplicationRuntime::", start + 1);
    const body = application.slice(start, next < 0 ? application.length : next);
    assert.match(body, /runAlbumMutation\(/, method);
  }
  const download = application.slice(
    application.indexOf("aigcPhase_ == AigcPhase::Download"),
    application.indexOf("aigcPhase_ == AigcPhase::Display"),
  );
  assert.ok(download.indexOf("runAlbumMutation(") < download.indexOf("myAi_.downloadImage("));
  assert.match(application, /publishAlbumRevision\([\s\S]*onAlbumRevisionChanged\(albumId_, 0\)/);
  assert.match(application, /currentAlbumRevision\([\s\S]*!albumRevisionHealthy_/);

  assert.match(main, /if \(useLegacyDirectDisplay\([\s\S]*runAlbumDisabledDirectPath\(/);
  assert.match(main, /albumReady = !useLegacyDirectDisplay\([\s\S]*&& album\.begin\(\)/);
  assert.match(main, /if \(!albumReady \|\| useLegacyDirectDisplay\(/);
  assert.ok(
    main.indexOf("reserveAlbumRevisionForScheduledCache()") <
      main.indexOf("album.cacheFrame("),
  );
});
