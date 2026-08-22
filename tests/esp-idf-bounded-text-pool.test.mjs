import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(
  repo, "firmware/inkloop-idf/components/inkloop_product");
const fakeFreeRtos = String.raw`
#pragma once
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(value) do { (void)(value); } while (0)
#define portEXIT_CRITICAL(value) do { (void)(value); } while (0)
`;
const harness = String.raw`
#include <cassert>
#include <string>
#include "inkloop/bounded_text_pool.hpp"
using namespace inkloop;
int main() {
  BoundedTextPool pool;
  assert(pool.put(ProductTextKind::AsrFinal, "") == 0);
  assert(pool.put(ProductTextKind::AsrFinal,
                  std::string(BoundedTextPool::kMaximumTextBytes + 1, 'x')) == 0);
  uint64_t tickets[BoundedTextPool::kSlotCount] = {};
  for (size_t i = 0; i < BoundedTextPool::kSlotCount; ++i) {
    tickets[i] = pool.put(ProductTextKind::AsrFinal,
                          "hello-" + std::to_string(i));
    assert(tickets[i] != 0);
  }
  assert(pool.used() == BoundedTextPool::kSlotCount);
  assert(pool.put(ProductTextKind::AssistantFinal, "overflow") == 0);
  ProductTextKind kind = ProductTextKind::ToolState;
  std::string text;
  assert(pool.take(tickets[3], kind, text));
  assert(kind == ProductTextKind::AsrFinal && text == "hello-3");
  assert(!pool.take(tickets[3], kind, text));
  assert(pool.used() == BoundedTextPool::kSlotCount - 1);
  const uint64_t replacement =
      pool.put(ProductTextKind::AssistantFinal, "reply");
  assert(replacement != 0 && replacement != tickets[3]);
  assert(pool.release(replacement));
  assert(!pool.release(replacement));
  for (size_t i = 0; i < BoundedTextPool::kSlotCount; ++i) {
    if (i != 3) assert(pool.release(tickets[i]));
  }
  assert(pool.used() == 0);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-text-pool-"));
  try {
    mkdirSync(join(scratch, "freertos"));
    writeFileSync(join(scratch, "freertos/FreeRTOS.h"), fakeFreeRtos);
    const source = join(scratch, "pool_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", scratch, "-I", join(product, "include"), source,
      join(product, "bounded_text_pool.cpp"), "-o", binary,
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

test("cross-task text pool is bounded, ticketed and zero-released", () => {
  buildAndRun(false);
  buildAndRun(true);
});
