import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

test("PaperColor production refresh selects each requested full-screen renderer", async () => {
  const runtime = await readFile(
    new URL(
      "../firmware/m5-papercolor/src/PaperColorApplicationRuntime.cpp",
      import.meta.url,
    ),
    "utf8",
  );
  assert.match(runtime, /config\.experimentalPrequantizationEnabled = true/);
  assert.match(runtime, /parseRenderStrategyId\(requested, &selected\)/);
  assert.match(runtime, /request\.strategy = selected/);
  assert.doesNotMatch(runtime, /EXPERIMENTAL_BYPASSED_PHYSICAL_GATE/);
});
