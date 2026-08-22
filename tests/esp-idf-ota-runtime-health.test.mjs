import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const idf = path.join(repo, "firmware", "inkloop-idf");
const ota = path.join(idf, "components", "inkloop_ota");
const runtime = path.join(idf, "components", "inkloop_runtime");
const contracts = path.join(idf, "components", "inkloop_contracts");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <limits>

#include "inkloop/ota_runtime_health.hpp"

using namespace inkloop;

static RuntimeTelemetrySnapshot healthyRuntime(std::uint32_t now) {
  RuntimeTelemetrySnapshot value;
  for (std::size_t at = 0; at < value.lanes.size(); ++at) {
    auto& lane = value.lanes[at];
    lane.configured_core = at < 4 ? 1 : 0;
    lane.observed_core = lane.configured_core;
    lane.configured_priority = static_cast<std::uint8_t>(22 - at);
    lane.observed_priority = lane.configured_priority;
    lane.task_running = true;
    lane.stack_sampled = true;
    lane.stack_low_water_bytes = 512;
    lane.handler_count = static_cast<std::uint32_t>(at);
    lane.tick_count = 2;
    lane.last_progress_ms = now;
  }
  return value;
}

static OtaBootStageState healthyStage() {
  OtaBootStageState value;
  value.storage_gate_observed = true;
  value.storage_gate_healthy = true;
  value.board_observed = true;
  value.board_healthy = true;
  value.runtime_observed = true;
  value.runtime_healthy = true;
  value.fatal_status_observed = true;
  value.fatal_status_clear = true;
  return value;
}

int main() {
  const OtaBootHealthConfig config = productionOtaBootHealthConfig();
  assert(config.soak_window_ms == 30000U);
  assert(config.pending_deadline_ms == 120000U);
  assert(config.telemetry_max_age_ms == 15000U);
  assert(config.mandatory_lane_mask == 0x00ffU);
  assert(OtaBootHealthCore(config).configurationValid());

  const std::uint32_t now = 40000U;
  RuntimeTelemetrySnapshot runtime = healthyRuntime(now);
  OtaBootHealthEvidence evidence = composeOtaBootHealthEvidence(
      now, healthyStage(), runtime);
  assert(evidence.storage_upgrade_gate.present &&
         evidence.storage_upgrade_gate.healthy);
  assert(evidence.board_initialized.present &&
         evidence.board_initialized.healthy);
  assert(evidence.product_runtime_started.present &&
         evidence.product_runtime_started.healthy);
  assert(evidence.fatal_status_clear.present &&
         evidence.fatal_status_clear.healthy);
  for (std::size_t at = 0; at < kTaskLaneCount; ++at) {
    assert(evidence.supervisor_lanes[at].present);
    assert(evidence.supervisor_lanes[at].progress_count == at + 3U);
    assert(evidence.supervisor_lanes[at].observed_ms == now);
  }

  runtime.lanes[2].observed_core = 0;
  evidence = composeOtaBootHealthEvidence(now, healthyStage(), runtime);
  assert(!evidence.supervisor_lanes[2].present);
  assert(evidence.supervisor_lanes[2].progress_count == 0U);
  runtime = healthyRuntime(now);
  runtime.lanes[3].stack_sampled = false;
  assert(!composeOtaBootHealthEvidence(now, healthyStage(), runtime)
              .supervisor_lanes[3].present);
  runtime = healthyRuntime(now);
  runtime.lanes[4].task_running = false;
  assert(!composeOtaBootHealthEvidence(now, healthyStage(), runtime)
              .supervisor_lanes[4].present);

  runtime = healthyRuntime(now);
  runtime.lanes[0].handler_count = std::numeric_limits<std::uint32_t>::max();
  assert(composeOtaBootHealthEvidence(now, healthyStage(), runtime)
             .supervisor_lanes[0].progress_count ==
         std::numeric_limits<std::uint32_t>::max());

  OtaBootStageState fatal = healthyStage();
  fatal.explicit_fatal_health_failure = true;
  assert(composeOtaBootHealthEvidence(now, fatal, healthyRuntime(now))
             .explicit_fatal_health_failure);
  return 0;
}
`;

function compileAndRun(source, output, sanitized) {
  const args = [
    "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
    `-I${path.join(ota, "include")}`,
    `-I${path.join(runtime, "include")}`,
    `-I${path.join(contracts, "include")}`,
    source,
    path.join(ota, "ota_runtime_health.cpp"),
    path.join(ota, "ota_boot_health.cpp"),
    "-o", output,
  ];
  if (sanitized) {
    args.unshift("-fno-omit-frame-pointer", "-fsanitize=address,undefined");
  }
  const built = spawnSync("clang++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, `${built.stdout}\n${built.stderr}`);
  const ran = spawnSync(output, [], {
    encoding: "utf8",
    env: sanitized
      ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
      : process.env,
  });
  assert.equal(ran.status, 0, `${ran.stdout}\n${ran.stderr}`);
}

test("production OTA evidence requires every live, sampled and correctly placed lane", async () => {
  const scratch = await mkdtemp(path.join(tmpdir(), "inkloop-ota-runtime-"));
  try {
    const source = path.join(scratch, "runtime-health.cpp");
    await writeFile(source, harness);
    compileAndRun(source, path.join(scratch, "strict"), false);
    compileAndRun(source, path.join(scratch, "sanitized"), true);
  } finally {
    await rm(scratch, { recursive: true, force: true });
  }
});

test("idle supervisor lanes wake only for the slow health sample", async () => {
  const source = await readFile(
    path.join(runtime, "runtime_supervisor.cpp"), "utf8");
  assert.match(source, /const TickType_t resource_interval = pdMS_TO_TICKS\(10000\)/);
  assert.match(source, /slot\.tick_handler\s*\?\s*slot\.tick_interval\s*:\s*resource_interval/);
  assert.doesNotMatch(source, /slot\.tick_handler\s*\?\s*slot\.tick_interval\s*:\s*portMAX_DELAY/);
});
