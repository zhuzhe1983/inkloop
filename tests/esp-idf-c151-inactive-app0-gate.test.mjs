import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import test from "node:test";

const pythonTest = fileURLToPath(
  new URL("./test_c151_inactive_app0_gate.py", import.meta.url),
);

test("C151 accepted-candidate inactive-app0 gate is fail-closed under simulated staging", () => {
  const completed = spawnSync("python3", [pythonTest, "-q"], {
    encoding: "utf8",
    timeout: 120_000,
  });
  assert.equal(
    completed.status,
    0,
    `python gate tests failed\nstdout:\n${completed.stdout}\nstderr:\n${completed.stderr}`,
  );
  assert.match(completed.stderr, /Ran 14 tests/u);
  assert.match(completed.stderr, /OK/u);
});
