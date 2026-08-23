import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include "inkloop/album_navigation_core.hpp"

using namespace inkloop;

int main() {
  AlbumNavigationCore navigation(1000);
  size_t ordinal = 123;
  bool changed = true;
  assert(navigation.step(1, 10, ordinal) == AlbumStepResult::NotReady);
  assert(navigation.synchronize(0, AlbumNavigationCore::kNoOrdinal));
  assert(navigation.step(1, 10, ordinal) == AlbumStepResult::Empty);

  assert(navigation.synchronize(5, 2));
  assert(navigation.step(1, 100, ordinal) == AlbumStepResult::Selected);
  assert(ordinal == 3);
  assert(!navigation.takeSettled(1099, ordinal, changed));
  assert(navigation.step(1, 1099, ordinal) == AlbumStepResult::Selected);
  assert(ordinal == 4);
  assert(!navigation.takeSettled(2098, ordinal, changed));
  assert(navigation.takeSettled(2099, ordinal, changed));
  assert(ordinal == 4 && changed && navigation.refreshing());
  assert(navigation.step(-1, 2100, ordinal) == AlbumStepResult::Busy);
  navigation.finish(5, 4);

  // A complete lap returns to the persisted current page and emits no refresh.
  for (unsigned press = 0; press < 5; ++press) {
    assert(navigation.step(1, 3000 + press, ordinal) ==
           AlbumStepResult::Selected);
  }
  assert(ordinal == 4);
  assert(navigation.takeSettled(4004, ordinal, changed));
  assert(!changed && !navigation.refreshing());

  // No-current behavior is deterministic in both directions.
  assert(navigation.synchronize(3, AlbumNavigationCore::kNoOrdinal));
  assert(navigation.step(1, 5000, ordinal) == AlbumStepResult::Selected);
  assert(ordinal == 0);
  navigation.synchronize(3, AlbumNavigationCore::kNoOrdinal);
  assert(navigation.step(-1, 5001, ordinal) == AlbumStepResult::Selected);
  assert(ordinal == 2);

  // Signed deadline comparison remains correct over uint32 wrap.
  AlbumNavigationCore wrapping(20);
  assert(wrapping.synchronize(2, 0));
  assert(wrapping.step(1, UINT32_MAX - 9U, ordinal) ==
         AlbumStepResult::Selected);
  assert(!wrapping.takeSettled(9, ordinal, changed));
  assert(wrapping.takeSettled(10, ordinal, changed));
  assert(changed && ordinal == 1);

  wrapping.finish(2, 1);
  assert(wrapping.beginImmediate(0));
  assert(wrapping.refreshing());
  assert(!wrapping.synchronize(2, 1));
  wrapping.invalidate();
  assert(wrapping.step(1, 100, ordinal) == AlbumStepResult::NotReady);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-album-navigation-"));
  try {
    const source = join(scratch, "navigation.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(product, "include"), source,
      join(product, "album_navigation_core.cpp"), "-o", binary,
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

test("album selection settles once, wraps safely, and skips unchanged pages", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("button control stays non-blocking while display owns all slow work", () => {
  const runtime = readFileSync(join(product, "product_runtime.cpp"), "utf8");
  const display = readFileSync(join(product, "native_display_service.cpp"), "utf8");
  assert.match(runtime, /RawButtonPrevious[\s\S]*selectRelative/);
  assert.doesNotMatch(runtime, /RawButtonPrevious[\s\S]{0,1200}(open\(|readCatalog|writeFullFrame)/);
  assert.match(display, /registerTickHandler\([\s\S]*TaskLane::Display[\s\S]*kDisplayTickMs/);
  assert.match(display, /takeSettled[\s\S]*postRefreshStarting[\s\S]*renderOrdinal/);
  assert.match(display, /index\.current == asset\.id[\s\S]*unchanged_skips/);
});

test("local ordinal prompts are composable beyond the first three", () => {
  const prompt = readFileSync(join(product, "local_prompt_player.cpp"), "utf8");
  const cmake = readFileSync(join(product, "CMakeLists.txt"), "utf8");
  assert.match(prompt, /ordinal > 99U/);
  assert.match(prompt, /tens > 1U[\s\S]*kTen[\s\S]*ones != 0U/);
  assert.match(prompt, /kPlaybackChunkBytes = 320U/);
  assert.match(prompt, /std::min<size_t>\(remaining, kPlaybackChunkBytes\)/);
  assert.match(prompt, /playbackDrained/);
  assert.match(cmake, /EMBED_FILES[\s\S]*ordinal_digit_nine\.wav[\s\S]*display_refresh_start\.wav/);
});
