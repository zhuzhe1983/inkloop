import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const acceptance = readFileSync(
  new URL("../firmware/inkloop-idf/docs/C151_PHYSICAL_ACCEPTANCE.md", import.meta.url),
  "utf8",
);

test("installed C151 acceptance never recommends the generic destructive flash target", () => {
  assert.doesNotMatch(
    acceptance,
    /^\s*idf\.py\b[^\n]*\bflash\s*$/mu,
    "a copy-pasteable generic flash command can overwrite app0 and otadata",
  );
  assert.match(acceptance, /Do \*\*not\*\* run the generic `idf\.py flash`/u);
  assert.match(acceptance, /retain the running image as rollback/u);
  assert.match(acceptance, /authorize only the inactive app\s+range/u);
  assert.match(acceptance, /verify a full readback hash/u);
  assert.match(acceptance, /`erase-flash` is also forbidden/u);
});
