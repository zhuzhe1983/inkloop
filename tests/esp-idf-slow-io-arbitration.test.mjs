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
#include "inkloop/slow_io_arbitration.hpp"

using inkloop::SlowIoArbitration;

int main() {
  // Regression: AIGC being active is intentionally not a Portal drain gate.
  // Existing Portal mutations drain first, then AIGC can own the album.
  assert(SlowIoArbitration::portalMayDrain(false, false));
  assert(!SlowIoArbitration::aigcMayMutate(false, true, false));
  assert(SlowIoArbitration::aigcMayMutate(false, false, false));

  // A physical display write or Inkloop transaction still closes the Portal
  // mutation gate, and every owner remains mutually exclusive for album I/O.
  assert(!SlowIoArbitration::portalMayDrain(true, false));
  assert(!SlowIoArbitration::portalMayDrain(false, true));
  assert(!SlowIoArbitration::aigcMayMutate(true, false, false));
  assert(!SlowIoArbitration::aigcMayMutate(false, false, true));
  assert(!SlowIoArbitration::inkloopMayRun(true, false, false));
  assert(!SlowIoArbitration::inkloopMayRun(false, false, true));
  assert(SlowIoArbitration::inkloopMayRun(false, false, false));

  // Regression: once NativeInkloopService owns the Portal lane its public
  // busy view is true, but that ownership flag must not reject the admitted
  // synchronize/runDueTask section itself.
  assert(SlowIoArbitration::inkloopOwnerBusy(false, true, false));
  assert(!SlowIoArbitration::inkloopOwnerAdmittedBusy(false, false));
  assert(SlowIoArbitration::inkloopOwnerAdmittedBusy(true, false));
  assert(SlowIoArbitration::inkloopOwnerAdmittedBusy(false, true));
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-slow-io-"));
  try {
    const source = join(scratch, "slow_io.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(product, "include"), source, "-o", binary,
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

test("slow I/O arbitration drains Portal before AIGC without circular wait", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("product runtime re-samples each slow owner between serialized stages", () => {
  const runtime = readFileSync(join(product, "product_runtime.cpp"), "utf8");
  const portalAt = runtime.indexOf("SlowIoArbitration::portalMayDrain");
  const aigcAt = runtime.indexOf("SlowIoArbitration::aigcMayMutate");
  const inkloopAt = runtime.indexOf("SlowIoArbitration::inkloopMayRun");
  assert.ok(portalAt >= 0 && aigcAt > portalAt && inkloopAt > aigcAt);
  assert.doesNotMatch(
    runtime,
    /const bool slow_io_idle[\s\S]{0,180}!voice_\.portalBusy\(\)[\s\S]{0,180}portal_\.tick/,
  );

  const service = readFileSync(
    join(product, "native_inkloop_service.cpp"),
    "utf8",
  );
  const admittedAt = service.indexOf(
    "void NativeInkloopService::portalTickAdmitted",
  );
  const admittedEnd = service.indexOf(
    "bool NativeInkloopService::admittedSlowIoBusy",
    admittedAt,
  );
  assert.ok(admittedAt >= 0 && admittedEnd > admittedAt);
  const admitted = service.slice(admittedAt, admittedEnd);
  assert.match(admitted, /admittedSlowIoBusy\(\)/);
  assert.doesNotMatch(admitted, /!slow_io_allowed\s*\|\|\s*busy\(\)/);
  assert.match(admitted, /synchronize\(onboarding, now\)/);
  assert.match(admitted, /runDueTask\(now, wifi_online\)/);
});
