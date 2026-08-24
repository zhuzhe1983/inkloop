import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import test from "node:test";

const pythonTest = fileURLToPath(
  new URL("./test_c151_deferred_tf_app0_gate.py", import.meta.url),
);

test("C151 deferred-TF gate permits only one fail-closed app0 staging attempt", () => {
  const completed = spawnSync("python3", [pythonTest, "-q"], {
    encoding: "utf8",
    timeout: 120_000,
  });
  assert.equal(
    completed.status,
    0,
    `python gate tests failed\nstdout:\n${completed.stdout}\nstderr:\n${completed.stderr}`,
  );
  assert.match(completed.stderr, /Ran 13 tests/u);
  assert.match(completed.stderr, /OK/u);
});
