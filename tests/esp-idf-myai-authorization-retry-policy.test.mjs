import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");

test("transient MyAI Offline state remains recoverable without retry storms", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-myai-retry-"));
  try {
    const source = join(scratch, "retry.cpp");
    const binary = join(scratch, "retry");
    writeFileSync(source, String.raw`
#include <cassert>
#include "inkloop/myai_authorization_retry_policy.hpp"

using inkloop::MyAiAuthorizationRetryPolicy;
using inkloop::myai::ActivationState;

int main() {
  assert(MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::Bound));
  assert(MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::Offline));
  assert(!MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::Unconfigured));
  assert(!MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::Pairing));
  assert(!MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::PaymentRequired));
  assert(!MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::RecoveryRequired));
  assert(!MyAiAuthorizationRetryPolicy::mayCheck(ActivationState::Error));

  assert(MyAiAuthorizationRetryPolicy::shouldCheck(
      ActivationState::Offline, true));
  assert(!MyAiAuthorizationRetryPolicy::shouldCheck(
      ActivationState::Offline, false));
  assert(MyAiAuthorizationRetryPolicy::shouldCheck(
      ActivationState::Bound, true));
  assert(!MyAiAuthorizationRetryPolicy::shouldCheck(
      ActivationState::Bound, false));

  assert(MyAiAuthorizationRetryPolicy::nextDelay(false, 0) == 5000U);
  assert(MyAiAuthorizationRetryPolicy::nextDelay(false, 1200U) == 5000U);
  assert(MyAiAuthorizationRetryPolicy::nextDelay(false, 9000U) == 9000U);
  assert(MyAiAuthorizationRetryPolicy::nextDelay(true, 0) == 600000U);
  // A request that started at zero but completed after a 30-second transport
  // timeout must still wait five seconds after completion before retrying.
  assert(MyAiAuthorizationRetryPolicy::nextDeadline(
      30000U, false, 0U) == 35000U);
  assert(MyAiAuthorizationRetryPolicy::nextDeadline(
      30000U, false, 9000U) == 39000U);
  assert(MyAiAuthorizationRetryPolicy::nextDeadline(
      0xfffffff0U, false, 5000U) == 0x00001378U);
  return 0;
}
`);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(product, "include"),
      "-I", join(myai, "include"),
      source, "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
  assert.ok(true);
});
