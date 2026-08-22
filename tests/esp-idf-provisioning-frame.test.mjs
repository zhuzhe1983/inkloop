import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_onboarding",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include "inkloop/onboarding/provisioning_frame.hpp"

using namespace inkloop::onboarding;

int main() {
  std::array<uint8_t, 120000> first{};
  std::array<uint8_t, 120000> second{};
  const PairingFrameSpec spec{};
  assert(renderProvisioningFrame4Bpp(
      spec, "Inkloop-8428-Settings", "z-Home@P4ss!",
      "inkloop.local", "192.168.4.1", first.data(), first.size()) ==
      ProvisioningFrameResult::Ok);
  // Case-sensitive and punctuation-bearing values affect the actual frame;
  // they are not uppercased, masked, truncated or replaced with a QR.
  assert(renderProvisioningFrame4Bpp(
      spec, "INKLOOP-8428-SETTINGS", "z-home@P4ss!",
      "inkloop.local", "192.168.4.1", second.data(), second.size()) ==
      ProvisioningFrameResult::Ok);
  assert(first != second);
  for (uint8_t packed : first) {
    const uint8_t high = packed >> 4U;
    const uint8_t low = packed & 0x0fU;
    assert((high <= 3U || high == 5U || high == 6U) &&
           (low <= 3U || low == 5U || low == 6U));
  }
  assert(renderProvisioningFrame4Bpp(
      spec, "Inkloop-8428-Settings", "short",
      "inkloop.local", "192.168.4.1", first.data(), first.size()) ==
      ProvisioningFrameResult::InvalidInput);
  assert(renderProvisioningFrame4Bpp(
      spec, "Inkloop-8428-Settings", "z-Home@P4ss!",
      "inkloop.local", "192.168.4.1", first.data(), first.size() - 1U) ==
      ProvisioningFrameResult::InvalidFrame);
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-provisioning-"));
  try {
    const source = join(scratch, "test.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "provisioning_frame.cpp"), "-o", binary,
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

test("Settings AP frame preserves exact printable credentials", () => run(false));
test("Settings AP frame is safe under ASan/UBSan", () => run(true));
