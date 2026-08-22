import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const project = new URL("../firmware/inkloop-idf/", import.meta.url);
const root = project.pathname;

test("PCM backpressure and service cadence are bounded and generation-safe", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-idf-runtime-"));
  const harness = join(scratch, "runtime_foundations.cpp");
  const binary = join(scratch, "runtime_foundations");
  writeFileSync(harness, String.raw`
#include <array>
#include <cassert>
#include <cstdint>

#include "inkloop/cadence_policy.hpp"
#include "inkloop/pcm_backpressure.hpp"

using inkloop::CadencePolicy;
using inkloop::PcmBackpressureRing;
using inkloop::PcmFlowSignal;

int main() {
  std::array<uint8_t, 16> storage{};
  PcmBackpressureRing ring(storage.data(), storage.size(), 4, 10, 8);
  assert(ring.valid());
  const uint32_t first = ring.beginTurn();
  assert(first != 0);

  const std::array<uint8_t, 6> a{{0, 1, 2, 3, 4, 5}};
  const std::array<uint8_t, 4> b{{6, 7, 8, 9}};
  const std::array<uint8_t, 8> c{{10, 11, 12, 13, 14, 15, 16, 17}};
  assert(ring.push(first, a.data(), a.size()).signal == PcmFlowSignal::Continue);
  assert(ring.push(first, b.data(), b.size()).signal == PcmFlowSignal::PauseIngress);
  assert(ring.ingressPaused());

  std::array<uint8_t, 16> output{};
  auto transfer = ring.pop(output.data(), 6);
  assert(transfer.signal == PcmFlowSignal::ResumeIngress);
  for (size_t i = 0; i < a.size(); ++i) assert(output[i] == a[i]);

  assert(ring.push(first, c.data(), c.size()).signal == PcmFlowSignal::PauseIngress);
  const size_t before_full = ring.buffered();
  assert(ring.push(first, a.data(), a.size()).signal == PcmFlowSignal::Full);
  assert(ring.buffered() == before_full);
  assert(ring.finishIngress(first).signal == PcmFlowSignal::Continue);
  transfer = ring.pop(output.data(), output.size());
  assert(transfer.signal == PcmFlowSignal::Closed);
  assert(transfer.bytes == b.size() + c.size());
  for (size_t i = 0; i < b.size(); ++i) assert(output[i] == b[i]);
  for (size_t i = 0; i < c.size(); ++i) assert(output[b.size() + i] == c[i]);
  assert(ring.complete());

  const uint32_t second = ring.beginTurn();
  assert(second != first);
  assert(ring.push(first, a.data(), a.size()).signal ==
         PcmFlowSignal::StaleGeneration);
  assert(ring.push(second, a.data(), 3).signal == PcmFlowSignal::InvalidFrame);
  assert(ring.push(second, a.data(), a.size()).bytes == a.size());
  assert(ring.cancel(first).signal == PcmFlowSignal::StaleGeneration);
  assert(ring.cancel(second).signal == PcmFlowSignal::Closed);
  assert(!ring.active() && ring.buffered() == 0);

  CadencePolicy cadence;
  assert(!cadence.inkloopSyncDue(0));
  cadence.setNetworkReady(true);
  assert(cadence.inkloopSyncDue(0));
  cadence.acknowledgeInkloopSync(100, true);
  assert(!cadence.inkloopSyncDue(30099));
  assert(cadence.inkloopSyncDue(30100));
  cadence.acknowledgeInkloopSync(30100, true);
  cadence.markInkloopDirty();
  assert(cadence.inkloopSyncDue(30101));

  assert(!cadence.aigcStatusDue(50000));
  cadence.beginAigcStatusPolling(50000);
  assert(!cadence.aigcStatusDue(54999));
  assert(cadence.aigcStatusDue(55000));
  cadence.acknowledgeAigcStatus(55000, false);
  assert(!cadence.aigcStatusDue(59999));
  assert(cadence.aigcStatusDue(60000));
  cadence.acknowledgeAigcStatus(60000, true);
  assert(!cadence.aigcStatusDue(999999));

  cadence.openVoiceLease(1000);
  assert(!cadence.myaiHeartbeatDue(30999));
  assert(cadence.myaiHeartbeatDue(31000));
  cadence.acknowledgeMyAiHeartbeat(31000, false);
  assert(cadence.myaiHeartbeatDue(31001));
  cadence.acknowledgeMyAiHeartbeat(31001, true);
  assert(!cadence.myaiHeartbeatDue(61000));
  assert(cadence.myaiHeartbeatDue(61001));
  cadence.closeVoiceLease();
  assert(!cadence.myaiHeartbeatDue(999999));

  cadence.setNetworkReady(false);
  assert(!cadence.inkloopSyncDue(999999));
  return 0;
}
`);

  try {
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", join(root, "components/inkloop_audio/include"),
      "-I", join(root, "components/inkloop_runtime/include"),
      harness,
      join(root, "components/inkloop_audio/pcm_backpressure.cpp"),
      join(root, "components/inkloop_runtime/cadence_policy.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], {
      env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" },
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

test("runtime foundations do not reintroduce Arduino polling or unbounded PCM", () => {
  const audioHeader = readFileSync(
    join(root, "components/inkloop_audio/include/inkloop/pcm_backpressure.hpp"),
    "utf8",
  );
  const cadence = readFileSync(
    join(root, "components/inkloop_runtime/cadence_policy.cpp"), "utf8");
  const combined = `${audioHeader}\n${cadence}`;
  assert.doesNotMatch(combined, /Arduino|M5Unified|HTTPClient|WebServer|delay\s*\(/);
  assert.match(audioHeader, /PauseIngress/);
  assert.match(audioHeader, /StaleGeneration/);
  assert.match(cadence, /aigc_active_/);
  assert.doesNotMatch(cadence, /portal/i);
});
