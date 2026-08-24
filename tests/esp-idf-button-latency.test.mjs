import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(
  repo, "firmware/inkloop-idf/components/inkloop_product");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <type_traits>

#include "inkloop/button_latency_core.hpp"

using namespace inkloop;

int main() {
  static_assert(std::is_trivially_copyable<ButtonLatencyObservation>::value);
  static_assert(ButtonLatencyCore::elapsedUs(0x10U, 0xfffffff0U) == 0x20U);

  ButtonLatencyCore core;
  assert(!core.recordCapture(0, ButtonLatencyButton::Previous, 1));
  assert(core.recordCapture(1, ButtonLatencyButton::Previous, 0xfffffff0U));
  assert(core.recordControlAdmission(1, 5U));
  assert(core.complete(1, ButtonLatencyOutcome::Navigation, 10U));

  ButtonLatencyObservation observation;
  assert(core.peek(observation));
  assert(observation.event_id == 1U);
  assert(observation.button == ButtonLatencyButton::Previous);
  assert(observation.outcome == ButtonLatencyOutcome::Navigation);
  assert(observation.control_admitted);
  assert(ButtonLatencyCore::elapsedUs(observation.control_admitted_us,
                                      observation.captured_us) == 21U);
  assert(ButtonLatencyCore::elapsedUs(observation.terminal_us,
                                      observation.captured_us) == 26U);
  assert(!core.pop(2U));
  assert(core.pop(1U));
  assert(!core.peek(observation));

  // Successful feedback may not be manufactured before Control admission.
  assert(core.recordCapture(2, ButtonLatencyButton::Top, 100U));
  assert(!core.complete(2, ButtonLatencyOutcome::Led, 101U));
  assert(core.recordControlAdmission(2, 102U));
  assert(core.complete(2, ButtonLatencyOutcome::Led, 103U));
  assert(core.peek(observation));
  assert(observation.outcome == ButtonLatencyOutcome::Led);
  assert(core.pop(2U));

  // Raw bounce is still a terminal, machine-visible GPIO event.
  assert(core.recordCapture(3, ButtonLatencyButton::Next, 200U));
  assert(core.recordControlAdmission(3, 205U));
  assert(core.complete(3, ButtonLatencyOutcome::Debounced, 206U));
  assert(core.peek(observation));
  assert(observation.outcome == ButtonLatencyOutcome::Debounced);
  assert(core.pop(3U));

  // Missing queue/feedback paths expire without blocking a responsive lane.
  assert(core.recordCapture(4, ButtonLatencyButton::Top, 1000U));
  core.expire(1499U, 500U);
  assert(!core.peek(observation));
  core.expire(1500U, 500U);
  assert(core.peek(observation));
  assert(observation.event_id == 4U);
  assert(observation.outcome == ButtonLatencyOutcome::NotReady);
  assert(!observation.control_admitted);
  assert(core.pop(4U));

  // The active table is bounded and collision replacement is diagnosed.
  assert(core.recordCapture(5, ButtonLatencyButton::Previous, 3000U));
  assert(core.recordCapture(
      5U + ButtonLatencyCore::kActiveCapacity,
      ButtonLatencyButton::Next, 3001U));
  auto snapshot = core.snapshot();
  assert(snapshot.active_overwrites == 1U);
  assert(snapshot.active == 1U);

  // Completion storage is bounded. An overrun is counted, never allocated.
  ButtonLatencyCore bounded;
  for (uint64_t id = 1U;
       id <= ButtonLatencyCore::kCompletedCapacity + 1U; ++id) {
    assert(bounded.recordCapture(id, ButtonLatencyButton::Previous,
                                 static_cast<uint32_t>(id)));
    assert(bounded.recordControlAdmission(id, static_cast<uint32_t>(id + 1U)));
    const bool completed = bounded.complete(
        id, ButtonLatencyOutcome::Navigation,
        static_cast<uint32_t>(id + 2U));
    assert(completed == (id <= ButtonLatencyCore::kCompletedCapacity));
  }
  snapshot = bounded.snapshot();
  assert(snapshot.pending == ButtonLatencyCore::kCompletedCapacity);
  assert(snapshot.completion_drops == 1U);
  assert(snapshot.active == 0U);
  return 0;
}
`;

function compileAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-button-latency-"));
  try {
    const source = join(scratch, "button_latency_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(product, "include"), source,
      join(product, "button_latency_core.cpp"), "-o", binary,
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

test("button latency core is bounded, wrap-safe, and sanitizer-clean", () => {
  compileAndRun(false);
  compileAndRun(true);
});

test("button latency high-priority paths are numeric-only", () => {
  const core = readFileSync(join(product, "button_latency_core.cpp"), "utf8");
  const telemetry = readFileSync(
    join(product, "button_latency_telemetry.cpp"), "utf8");
  const capture = telemetry.match(
    /void ButtonLatencyTelemetry::recordCapture[\s\S]*?\n\}/,
  )?.[0] ?? "";
  assert.ok(capture.length > 0);
  assert.match(capture, /portENTER_CRITICAL\(&mux_\)/);
  assert.doesNotMatch(capture, /CRITICAL_ISR/);
  assert.match(capture, /core_\.recordCapture/);
  assert.doesNotMatch(
    capture,
    /ESP_LOG|printf|snprintf|postSerialDiagnosticEvent|new\s|malloc|std::string/,
  );
  assert.doesNotMatch(
    core,
    /ESP_LOG|printf|snprintf|new\s|malloc|std::string|std::vector/,
  );
  assert.match(
    telemetry,
    /void ButtonLatencyTelemetry::service[\s\S]*postSerialDiagnosticEvent/,
  );
  assert.equal(
    telemetry.match(/postSerialDiagnosticEvent/g)?.length,
    1,
    "one completed GPIO edge has only one non-blocking serial attempt",
  );
  assert.match(
    telemetry,
    /const bool posted = sink->postSerialDiagnosticEvent\(event\);[\s\S]*?if \(!posted[\s\S]*?sink_drops_[\s\S]*?core_\.pop/,
    "serial backpressure drops locally instead of retrying every Portal tick",
  );
});
