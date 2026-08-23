import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const idf = path.join(repo, "firmware", "inkloop-idf");
const include = path.join(idf, "components", "inkloop_runtime", "include");
const contracts = path.join(idf, "components", "inkloop_contracts", "include");
const admission = path.join(
  idf, "components", "inkloop_runtime", "runtime_admission.cpp");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/runtime_admission.hpp"

using namespace inkloop;

WorkEnvelope command(WorkClass work_class, uint64_t generation,
                     uint64_t request_id, uint32_t deadline = 0) {
  WorkEnvelope value{};
  value.generation = generation;
  value.request_id = request_id;
  value.work_class = work_class;
  value.kind = EnvelopeKind::Command;
  value.disposition = WorkDisposition::Accepted;
  value.deadline_ms = deadline;
  return value;
}

int main() {
  std::array<size_t, kTaskLaneCount> capacities = kTaskQueueDepths;
  capacities[taskLaneIndex(TaskLane::Input)] = 2;
  capacities[taskLaneIndex(TaskLane::Voice)] = 1;
  RuntimeAdmission policy(capacities);
  assert(std::string(admissionResultName(AdmissionResult::NotReady)) ==
         "NOT_READY");

  assert(RuntimeAdmission::routeFor(command(WorkClass::Button, 1, 1)) ==
         TaskLane::Input);
  assert(RuntimeAdmission::routeFor(command(WorkClass::Voice, 1, 1)) ==
         TaskLane::Voice);
  assert(RuntimeAdmission::routeFor(command(WorkClass::InkloopNetwork, 1, 1)) ==
         TaskLane::Network);
  assert(RuntimeAdmission::routeFor(command(WorkClass::MyAiNetwork, 1, 1)) ==
         TaskLane::Network);

  auto first = command(WorkClass::Button, 1, 1);
  auto second = command(WorkClass::Button, 1, 2);
  auto third = command(WorkClass::Button, 1, 3);
  assert(policy.admit(TaskLane::Input, first, 10) == AdmissionResult::Admitted);
  assert(policy.admit(TaskLane::Input, second, 10) == AdmissionResult::Admitted);
  assert(policy.admit(TaskLane::Input, third, 10) == AdmissionResult::QueueFull);
  assert(policy.used(TaskLane::Input) == 2);
  assert(policy.release(TaskLane::Input) == AdmissionResult::Admitted);
  assert(policy.admit(TaskLane::Input, third, 10) == AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Input) == AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Input) == AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Input) == AdmissionResult::Underflow);

  assert(policy.admit(TaskLane::Voice, first, 10) == AdmissionResult::WrongLane);
  auto invalid = first;
  invalid.generation = 0;
  assert(policy.admit(TaskLane::Input, invalid, 10) ==
         AdmissionResult::InvalidEnvelope);
  invalid = first;
  invalid.request_id = 0;
  assert(policy.admit(TaskLane::Input, invalid, 10) ==
         AdmissionResult::InvalidEnvelope);
  invalid = first;
  invalid.payload_bytes = kMaxReferencedPayloadBytes + 1;
  assert(policy.admit(TaskLane::Input, invalid, 10) ==
         AdmissionResult::InvalidEnvelope);
  invalid = first;
  invalid.disposition = WorkDisposition::Complete;
  assert(policy.admit(TaskLane::Input, invalid, 10) ==
         AdmissionResult::InvalidEnvelope);

  assert(policy.cancelBefore(WorkClass::Button, 5) == AdmissionResult::Admitted);
  assert(policy.cancelBefore(WorkClass::Button, 3) == AdmissionResult::Admitted);
  assert(policy.generationFloor(WorkClass::Button) == 5);
  assert(policy.admit(TaskLane::Input,
                      command(WorkClass::Button, 4, 4), 10) ==
         AdmissionResult::StaleGeneration);
  assert(policy.admit(TaskLane::Input,
                      command(WorkClass::Button, 5, 5), 10) ==
         AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Input) == AdmissionResult::Admitted);

  auto deadline = command(WorkClass::Voice, 1, 6, 100);
  assert(policy.admit(TaskLane::Voice, deadline, 99) ==
         AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Voice) == AdmissionResult::Admitted);
  assert(policy.admit(TaskLane::Voice, deadline, 100) ==
         AdmissionResult::Expired);
  deadline.deadline_ms = 0x10U;
  assert(policy.admit(TaskLane::Voice, deadline, 0xfffffff0U) ==
         AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Voice) == AdmissionResult::Admitted);
  assert(policy.admit(TaskLane::Voice, deadline, 0x11U) ==
         AdmissionResult::Expired);

  // Durable accepted work remains executable even when its owner has spent
  // more than the two-minute idle window in a synchronous gateway call.
  auto durable = command(WorkClass::MyAiNetwork, 1, 8, 0);
  assert(policy.admit(TaskLane::Network, durable, 100) ==
         AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Network) == AdmissionResult::Admitted);
  assert(policy.shouldExecute(TaskLane::Network, durable, 120101) ==
         AdmissionResult::Admitted);

  auto result = command(WorkClass::Voice, 9, 7);
  result.kind = EnvelopeKind::Result;
  result.disposition = WorkDisposition::Complete;
  assert(RuntimeAdmission::routeFor(result) == TaskLane::Control);
  assert(policy.admit(TaskLane::Control, result, 10) ==
         AdmissionResult::Admitted);
  assert(policy.release(TaskLane::Control) == AdmissionResult::Admitted);
  result.disposition = WorkDisposition::Accepted;
  assert(policy.admit(TaskLane::Control, result, 10) ==
         AdmissionResult::InvalidEnvelope);

  for (uint64_t generation = 10; generation < 100000; ++generation) {
    auto value = command(WorkClass::Storage, generation, generation);
    assert(policy.admit(TaskLane::Storage, value,
                        static_cast<uint32_t>(generation)) ==
           AdmissionResult::Admitted);
    assert(policy.release(TaskLane::Storage) == AdmissionResult::Admitted);
  }
  const auto snapshot = policy.snapshot();
  for (size_t used : snapshot.used) assert(used == 0);
  return 0;
}
`;

function compileAndRun(source, output, sanitizers = false) {
  const args = [
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    `-I${include}`,
    `-I${contracts}`,
    source,
    admission,
    "-o",
    output,
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

test("runtime admission is bounded, generation-safe and wrap-safe", async () => {
  const temp = await mkdtemp(path.join(tmpdir(), "inkloop-idf-supervisor-"));
  try {
    const source = path.join(temp, "runtime_admission_harness.cpp");
    await writeFile(source, harness);
    compileAndRun(source, path.join(temp, "strict"));
    compileAndRun(source, path.join(temp, "sanitized"), true);
  } finally {
    await rm(temp, { recursive: true, force: true });
  }
});

test("native supervisor preserves ISR, queue and startup contracts", async () => {
  const [header, source, topology, contractsSource, admissionHeader] = await Promise.all([
    readFile(path.join(include, "inkloop", "runtime_supervisor.hpp"), "utf8"),
    readFile(path.join(idf, "components", "inkloop_runtime", "runtime_supervisor.cpp"), "utf8"),
    readFile(path.join(include, "inkloop", "task_topology.hpp"), "utf8"),
    readFile(path.join(contracts, "inkloop", "work_contracts.hpp"), "utf8"),
    readFile(path.join(include, "inkloop", "runtime_admission.hpp"), "utf8"),
  ]);
  const combined = `${header}\n${source}\n${topology}\n${contractsSource}\n${admissionHeader}`;
  assert.match(source, /xQueueCreateStatic/);
  assert.match(source, /xTaskCreatePinnedToCore/);
  assert.match(source, /xQueueSendFromISR/);
  assert.match(source, /ulTaskNotifyTake\(pdTRUE, portMAX_DELAY\)/);
  assert.match(header, /registerTickHandler/);
  assert.match(header, /freezeAdmissionForSleep/);
  assert.match(header, /sleepAdmissionStillSafe/);
  assert.match(header, /thawAdmissionAfterSleepAbort/);
  assert.match(source, /slot\.tick_handler\(slot\.tick_context\)/);
  assert.match(source, /now - last_tick/);
  assert.match(source, /xQueueSend\([^;]+, 0\)/s);
  assert.match(source, /input_isr_overflow/);
  assert.match(source, /admission_\.shouldExecute/);
  assert.match(source, /created != slots_\.size\(\)/);
  assert.match(header, /enum class Lifecycle[\s\S]+Initializing[\s\S]+Starting[\s\S]+Stopping/);
  assert.match(source, /lifecycle_ = Lifecycle::Initializing/);
  assert.match(source, /lifecycle_ = Lifecycle::Starting/);
  assert.match(source, /lifecycle_ = Lifecycle::Stopping/);
  assert.match(source, /lifecycle_ != Lifecycle::Running[\s\S]+AdmissionResult::NotReady/);
  assert.match(
    source,
    /freezeAdmissionForSleep\(\)[\s\S]{0,900}allAdmissionIdleLocked\(\)[\s\S]{0,200}allOtherCallbacksIdleLocked\(caller\)/,
  );
  assert.match(
    source,
    /sleepAdmissionStillSafe\(\) const[\s\S]{0,700}!sleep_button_event_pending_/,
  );
  assert.match(
    source,
    /postButtonFromIsr[\s\S]{0,900}sleep_admission_frozen_[\s\S]{0,120}sleep_button_event_pending_ = true/,
  );
  assert.match(
    source,
    /slot\.tick_handler && !ticks_frozen[\s\S]{0,500}!sleep_admission_frozen_[\s\S]{0,160}callback_active_\[index\] = true/,
  );
  assert.match(source, /portENTER_CRITICAL\(&mux_\);[\s\S]+xQueueSend\([^;]+, 0\)[\s\S]+portEXIT_CRITICAL\(&mux_\);/);
  assert.match(source, /portENTER_CRITICAL_ISR\(&mux_\);[\s\S]+xQueueSendFromISR[\s\S]+portEXIT_CRITICAL_ISR\(&mux_\);/);
  assert.match(topology, /"ink-input", 1, 22/);
  assert.match(topology, /"ink-voice", 1, 20/);
  assert.match(topology, /"ink-portal", 0, 3/);
  assert.match(topology, /"ink-portal", 0, 3, 16384, 4/);
  assert.doesNotMatch(combined, /Arduino\.h|M5Unified|WiFiManager|WebServer|HTTPClient/);
});
