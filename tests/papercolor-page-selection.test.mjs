import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const root = new URL("../firmware/m5-papercolor/", import.meta.url);

test("settled page selection skips a full-circle return to the current image", async (t) => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-page-selection-"));
  t.after(() => rm(temporaryDirectory, { recursive: true, force: true }));
  const source = join(temporaryDirectory, "page_selection.cpp");
  const executable = join(temporaryDirectory, "page_selection");
  await writeFile(source, `
#include <assert.h>
#include "PageSelectionPrimitives.h"
using namespace inkloop;
int main() {
  assert(settledPageDecision(8, 3, 3) == SettledPageDecision::AlreadyCurrent);
  assert(settledPageDecision(8, 3, 2) == SettledPageDecision::Refresh);
  assert(settledPageDecision(8, 3, 4) == SettledPageDecision::Refresh);
  assert(settledPageDecision(0, 0, 0) == SettledPageDecision::Invalid);
  assert(settledPageDecision(8, 8, 0) == SettledPageDecision::Invalid);
  assert(settledPageDecision(8, 0, 8) == SettledPageDecision::Invalid);
  return 0;
}`);
  const compiler = process.env.CXX || "c++";
  for (const sanitizer of [false, true]) {
    const output = sanitizer ? `${executable}-sanitized` : executable;
    const flags = sanitizer
      ? ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    const compile = spawnSync(compiler, [
      "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...flags, "-I", new URL("src/", root).pathname, source, "-o", output,
    ], { encoding: "utf8" });
    assert.equal(compile.status, 0, `${compile.stdout}\n${compile.stderr}`);
    const run = spawnSync(output, [], { encoding: "utf8" });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
  }
});

test("refresh-start speech occurs only after debounce and same-image rejection", async () => {
  const main = await readFile(new URL("src/main.cpp", root), "utf8");
  const processStart = main.indexOf("bool processPendingPage()");
  const processEnd = main.indexOf("void printDiagnosticStatus()", processStart);
  const flow = main.slice(processStart, processEnd);
  const settle = flow.indexOf("kPageSelectionSettleMs");
  const sameImage = flow.indexOf("SettledPageDecision::AlreadyCurrent");
  const announce = flow.indexOf("notifyPageRefreshStarting(target + 1)");
  const write = flow.indexOf("refreshFrame(", announce);
  assert.ok(settle >= 0);
  assert.ok(sameImage > settle);
  assert.ok(announce > sameImage);
  assert.ok(write > announce);
  assert.match(flow, /PAGE_SKIPPED[^\n]*ALREADY_CURRENT/);
});
