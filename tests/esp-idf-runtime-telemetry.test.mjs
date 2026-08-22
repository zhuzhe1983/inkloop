import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const idf = path.join(repo, "firmware", "inkloop-idf");
const runtime = path.join(idf, "components", "inkloop_runtime");
const include = path.join(runtime, "include");
const contracts = path.join(idf, "components", "inkloop_contracts", "include");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "inkloop/runtime_telemetry.hpp"

using namespace inkloop;

int main() {
  static_assert(std::is_trivially_copyable<RuntimeLaneTelemetry>::value);
  static_assert(std::is_trivially_copyable<RuntimeTelemetrySnapshot>::value);
  static_assert(sizeof(RuntimeTelemetrySnapshot) <= 1024);
  static_assert(RuntimeTelemetryAccumulator::elapsed32(
                    0x10U, 0xfffffff0U) == 0x20U);
  static_assert(RuntimeTelemetryAccumulator::saturatingAdd(
                    std::numeric_limits<uint32_t>::max() - 1U, 9U) ==
                std::numeric_limits<uint32_t>::max());

  RuntimeTelemetryAccumulator metrics;
  metrics.configureLane(TaskLane::Input, 32, 1, 22);
  metrics.configureLane(TaskLane::Network, 8, 0, 9);
  metrics.recordTaskStarted(TaskLane::Input, 1, 22, 0xfffffff0U);
  metrics.recordQueueDepth(TaskLane::Input, 1);
  metrics.recordQueueDepth(TaskLane::Input, 19);
  metrics.recordQueueDepth(TaskLane::Input, 3);
  metrics.recordHandler(TaskLane::Input, 900, 0x10U);
  metrics.recordHandler(TaskLane::Input, 400, 0x11U);

  metrics.recordTaskStarted(TaskLane::Network, 0, 9, 100);
  metrics.recordTick(TaskLane::Network, 80, 35, 10, 1000, 135);
  metrics.recordTick(TaskLane::Network, 110, 10, 10, 1000, 145);
  metrics.recordResourceSample(TaskLane::Input, 1000, true, 5000, true, 7000,
                               1, 22, 150);
  metrics.recordResourceSample(TaskLane::Input, 1200, true, 4000, true, 6000,
                               1, 22, 151);
  metrics.recordResourceSample(TaskLane::Network, 800, false, 0, false, 0,
                               0, 9, 152);

  auto snapshot = metrics.snapshot();
  const auto& input = snapshot.lanes[taskLaneIndex(TaskLane::Input)];
  assert(input.queue_capacity == 32);
  assert(input.queue_depth == 3);
  assert(input.queue_high_water == 19);
  assert(input.handler_count == 2);
  assert(input.handler_max_us == 900);
  assert(input.stack_sampled && input.stack_low_water_bytes == 1000);
  assert(input.configured_core == 1 && input.observed_core == 1);
  assert(input.configured_priority == 22 && input.observed_priority == 22);
  assert(input.task_running);
  assert(RuntimeTelemetryAccumulator::elapsed32(input.last_progress_ms,
                                                 0xfffffff0U) == 167U);

  const auto& network = snapshot.lanes[taskLaneIndex(TaskLane::Network)];
  assert(network.tick_count == 2);
  assert(network.tick_max_us == 110);
  assert(network.tick_late_count == 1);
  assert(network.tick_missed == 2);
  assert(network.tick_late_max_us == 25000);
  assert(network.stack_low_water_bytes == 800);
  assert(snapshot.internal_heap_sampled);
  assert(snapshot.internal_heap_min_free_bytes == 4000);
  assert(snapshot.psram_available && snapshot.psram_min_free_bytes == 6000);
  assert(snapshot.resource_sample_count == 3);
  assert(snapshot.sequence > 0 && snapshot.last_managed_update_ms == 152);

  metrics.recordTaskStopped(TaskLane::Input);
  snapshot = metrics.snapshot();
  assert(!snapshot.lanes[taskLaneIndex(TaskLane::Input)].task_running);
  assert(snapshot.lanes[taskLaneIndex(TaskLane::Input)].queue_depth == 0);
  assert(snapshot.lanes[taskLaneIndex(TaskLane::Input)].queue_high_water == 19);
  return 0;
}
`;

function compileAndRun(source, output, sanitizers = false) {
  const args = [
    "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
    `-I${include}`, `-I${contracts}`, source,
    path.join(runtime, "runtime_telemetry.cpp"), "-o", output,
  ];
  if (sanitizers) {
    args.unshift("-fno-omit-frame-pointer", "-fsanitize=address,undefined");
  }
  const built = spawnSync("clang++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, `${built.stdout}\n${built.stderr}`);
  const ran = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitizers
      ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
      : process.env,
  });
  assert.equal(ran.status, 0, `${ran.stdout}\n${ran.stderr}`);
}

test("runtime telemetry is fixed-size, wrap-safe, monotonic and saturating", async () => {
  const temp = await mkdtemp(path.join(tmpdir(), "inkloop-runtime-telemetry-"));
  try {
    const source = path.join(temp, "telemetry_harness.cpp");
    await writeFile(source, harness);
    compileAndRun(source, path.join(temp, "strict"));
    compileAndRun(source, path.join(temp, "sanitized"), true);
  } finally {
    await rm(temp, { recursive: true, force: true });
  }
});

test("supervisor measures only on managed task context and keeps ISR bounded", async () => {
  const [header, telemetryHeader, source] = await Promise.all([
    readFile(path.join(include, "inkloop", "runtime_supervisor.hpp"), "utf8"),
    readFile(path.join(include, "inkloop", "runtime_telemetry.hpp"), "utf8"),
    readFile(path.join(runtime, "runtime_supervisor.cpp"), "utf8"),
  ]);
  assert.match(header, /RuntimeTelemetrySnapshot telemetry\(\) const/);
  assert.match(source, /uxTaskGetStackHighWaterMark\(nullptr\)/);
  assert.match(source, /heap_caps_get_minimum_free_size\(\s*MALLOC_CAP_INTERNAL\)/);
  assert.match(source, /heap_caps_get_minimum_free_size\([\s\S]*MALLOC_CAP_SPIRAM/);
  assert.match(source, /recordHandlerTiming\(lane, elapsedUs\(started_us\)\)/);
  assert.match(source, /recordTickTiming\(lane, elapsedUs\(started_us\), elapsed/);
  assert.match(source, /static_cast<TickType_t>\(now - last_tick\)/);
  assert.match(source, /pdMS_TO_TICKS\(10000\)/);
  assert.match(source, /slot\.tick_handler\s*\?\s*slot\.tick_interval\s*:\s*resource_interval/);
  assert.doesNotMatch(source, /slot\.tick_handler\s*\?\s*slot\.tick_interval\s*:\s*portMAX_DELAY/);
  assert.match(source, /sample_heap = lane == TaskLane::Portal/);

  const isr = source.match(
    /AdmissionResult RuntimeSupervisor::postButtonFromIsr\([\s\S]*?\n}\n\nAdmissionResult RuntimeSupervisor::cancelBefore/,
  )?.[0] ?? "";
  assert.ok(isr.length > 0);
  assert.doesNotMatch(isr, /uxTaskGetStackHighWaterMark|heap_caps_get|ESP_LOG|new\s|malloc\s*\(/);
  assert.match(isr, /recordQueueDepth/);
  assert.doesNotMatch(telemetryHeader, /std::(?:string|vector|deque|list|map)|char\s*\*/);
});
