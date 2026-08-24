import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_product",
);
const localTools = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_local_tools",
);
const contracts = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_contracts",
);

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>

#include "inkloop/local_tool_display_correlation.hpp"

using namespace inkloop;

void completeOnce(WorkDisposition disposition, uint64_t request_id) {
  LocalToolDisplayCorrelation state;
  assert(state.arm(request_id, 3U, 7U, "asset_03.png", 100U, 120000U) ==
         LocalToolDisplayArmResult::Armed);
  assert(state.matches(request_id, 2U));
  assert(!state.matches(request_id, 1U));
  assert(!state.resolve(request_id + 1U, disposition));
  assert(!state.resolve(request_id, WorkDisposition::Accepted));
  assert(state.resolve(request_id, disposition));
  assert(state.phase() == LocalToolDisplayPhase::Terminal);
  assert(!state.resolve(request_id, WorkDisposition::Complete));
  LocalToolDisplayTerminal terminal;
  assert(state.takeTerminal(terminal));
  assert(terminal.request_id == request_id);
  assert(terminal.ordinal == 3U && terminal.total == 7U);
  assert(std::string(terminal.expected_asset_id.data()) == "asset_03.png");
  assert(terminal.disposition == disposition);
  assert(!state.takeTerminal(terminal));
  assert(!state.active() && state.requestId() == 0U);
}

int main() {
  static_assert(std::is_trivially_copyable<LocalToolDisplayTerminal>::value,
                "correlation terminal must remain POD");
  static_assert(kLocalToolDisplaySelectionFlag == 0x80U,
                "the album's bounded 96 entries leave one correlation bit");

  for (const WorkDisposition disposition : {
           WorkDisposition::Complete,
           WorkDisposition::Failed,
           WorkDisposition::Busy,
           WorkDisposition::Cancelled,
           WorkDisposition::TimedOut,
       }) {
    completeOnce(disposition,
                 0x1234000000000000ULL +
                     static_cast<uint64_t>(disposition));
  }

  LocalToolDisplayCorrelation concurrent;
  assert(concurrent.arm(11U, 1U, 2U, "first.png", 10U, 100U) ==
         LocalToolDisplayArmResult::Armed);
  assert(concurrent.arm(12U, 2U, 2U, "second.png", 11U, 100U) ==
         LocalToolDisplayArmResult::Busy);
  assert(concurrent.owns(11U) && !concurrent.owns(12U));
  assert(!concurrent.expire(109U));
  assert(concurrent.expire(110U));
  LocalToolDisplayTerminal timed_out;
  assert(concurrent.takeTerminal(timed_out));
  assert(timed_out.request_id == 11U);
  assert(timed_out.disposition == WorkDisposition::TimedOut);

  // Deadline arithmetic must stay correct across uint32_t wrap.
  LocalToolDisplayCorrelation wrapped;
  assert(wrapped.arm(20U, 2U, 2U, "wrap.png", 0xfffffff0U, 64U) ==
         LocalToolDisplayArmResult::Armed);
  assert(!wrapped.expire(0x0000002fU));
  assert(wrapped.expire(0x00000030U));
  assert(wrapped.takeTerminal(timed_out));

  // A deadline that wraps to exactly zero is still armed.  Treating zero as
  // an "unset" sentinel would permanently strand the single-flight slot.
  LocalToolDisplayCorrelation zero_deadline;
  assert(zero_deadline.arm(21U, 1U, 1U, "zero.png", 0xffffffc0U, 64U) ==
         LocalToolDisplayArmResult::Armed);
  assert(!zero_deadline.expire(0xffffffffU));
  assert(zero_deadline.expire(0x00000000U));
  assert(zero_deadline.takeTerminal(timed_out));
  assert(timed_out.request_id == 21U);
  assert(timed_out.disposition == WorkDisposition::TimedOut);

  for (const std::string& invalid : {
           std::string(), std::string("../escape"), std::string("a/b"),
           std::string("bad id"), std::string(65U, 'a'),
       }) {
    LocalToolDisplayCorrelation rejected;
    assert(rejected.arm(1U, 1U, 1U, invalid, 0U, 1U) ==
           LocalToolDisplayArmResult::Invalid);
  }
  LocalToolDisplayCorrelation rejected;
  assert(rejected.arm(0U, 1U, 1U, "a.png", 0U, 1U) ==
         LocalToolDisplayArmResult::Invalid);
  assert(rejected.arm(1U, 0U, 1U, "a.png", 0U, 1U) ==
         LocalToolDisplayArmResult::Invalid);
  assert(rejected.arm(1U, 2U, 1U, "a.png", 0U, 1U) ==
         LocalToolDisplayArmResult::Invalid);
  assert(rejected.arm(1U, 1U, 1U, "a.png", 0U, 0U) ==
         LocalToolDisplayArmResult::Invalid);

  // Bounded adversarial transition exercise for sanitizer builds.
  for (uint64_t iteration = 1U; iteration <= 20000U; ++iteration) {
    LocalToolDisplayCorrelation state;
    const uint32_t ordinal = static_cast<uint32_t>((iteration % 96U) + 1U);
    assert(state.arm(iteration, ordinal, 96U, "bounded_asset-1.png",
                     static_cast<uint32_t>(iteration), 1000U) ==
           LocalToolDisplayArmResult::Armed);
    if ((iteration & 1U) == 0U) {
      assert(state.resolve(iteration, WorkDisposition::Complete));
    } else {
      assert(state.expire(static_cast<uint32_t>(iteration + 1000U)));
    }
    LocalToolDisplayTerminal terminal;
    assert(state.takeTerminal(terminal));
    assert(terminal.request_id == iteration);
  }
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-local-display-"));
  try {
    const source = join(scratch, "harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(product, "include"),
      "-I",
      join(localTools, "include"),
      "-I",
      join(contracts, "include"),
      source,
      join(product, "local_tool_display_correlation.cpp"),
      "-o",
      binary,
    ];
    if (sanitized) {
      args.splice(
        1,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
    }
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

test("local-tool Display correlation passes strict C++17 transitions", () => {
  buildAndRun(false);
});

test("local-tool Display correlation is ASan/UBSan clean", () => {
  buildAndRun(true);
});
