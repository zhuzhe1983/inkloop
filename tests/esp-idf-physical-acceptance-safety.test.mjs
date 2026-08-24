import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const acceptance = readFileSync(
  new URL("../firmware/inkloop-idf/docs/C151_PHYSICAL_ACCEPTANCE.md", import.meta.url),
  "utf8",
);

function section(number, nextNumber = null) {
  const start = acceptance.indexOf(`## ${number}.`);
  const end = nextNumber === null
    ? acceptance.length
    : acceptance.indexOf(`## ${nextNumber}.`, start + 1);
  assert.notEqual(start, -1, `section ${number} must exist`);
  if (nextNumber !== null) {
    assert.notEqual(end, -1, `section ${nextNumber} must exist`);
  }
  return acceptance.slice(start, end);
}

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
  assert.match(
    acceptance,
    /No phase authorizes bootloader, partition-table, NVS, app1, LittleFS, coredump\s+or TF writes/u,
  );
});

test("installed C151 runbook revokes beta30 and leaves beta31 fail-closed", () => {
  assert.match(acceptance, /beta30\s+tuple is \*\*revoked\*\*/u);
  assert.match(
    acceptance,
    /beta30 was never flashed to this installed unit, never\s+booted on it, and never published or released/u,
  );
  for (const revokedBinding of [
    "f2b2f133af0c7a079e39b9a05de7f6370360bc69",
    "0.4.0-beta.30",
    "2846480",
    "6ff7ca999f59567aa21a027272bd027c14ad30fe37aa596a4f6e0da08316afc0",
    "20260824-095436-beta30-f2-exact-commit-fresh",
  ]) {
    assert.equal(acceptance.includes(revokedBinding), false);
  }
  const beta31Lines = acceptance
    .split("\n")
    .filter((line) => /beta31/i.test(line))
    .join("\n");
  assert.doesNotMatch(beta31Lines, /\b[0-9a-f]{40}\b|\b[0-9a-f]{64}\b/u);
  assert.doesNotMatch(
    acceptance,
    /BETA31_(?:COMMIT|SHA256)\s*=\s*["']?[0-9a-f]{40,64}\b/u,
  );
  assert.match(
    acceptance,
    /From one exact clean beta31 source commit, an\s+independently reviewed staging receipt stored outside the repository must bind\s+one explicit commit\/version\/application-SHA-256\/application-size tuple/u,
  );
  assert.match(acceptance, /by itself\s+authorizes no candidate write/u);
  assert.match(
    section(12),
    /Digital final gate and clean builds \| \*\*BLOCKED\*\* \(beta30 revoked; beta31 unbound\)/u,
  );
});

test("inactive app0 requires the external beta31 receipt and exact tuple first", () => {
  const receiptAt = acceptance.indexOf(
    "export INKLOOP_C151_STAGING_RECEIPT=/ABSOLUTE/OUTSIDE/REPOSITORY/",
  );
  const receiptAuthorizationAt = acceptance.indexOf(
    ".authorized_for_inactive_app0 == true",
  );
  const gateAt = acceptance.indexOf(
    "c151_inactive_app0_gate.py gate-app",
  );
  assert.ok(receiptAt >= 0);
  assert.ok(receiptAuthorizationAt > receiptAt);
  assert.ok(gateAt > receiptAuthorizationAt);
  assert.match(acceptance, /\.source\.version == "0\.4\.0-beta\.31"/u);
  assert.match(acceptance, /git rev-parse "\$BETA31_COMMIT\^\{commit\}"/u);
  assert.match(acceptance, /stat -f %z "\$INKLOOP_C151_CANDIDATE"/u);
  assert.match(acceptance, /shasum -a 256 "\$INKLOOP_C151_CANDIDATE"/u);
  assert.match(
    section(2, 3),
    /there is no accepted firmware tuple and no inactive-app0\s+write is authorized/u,
  );
});

test("installed C151 first boot preserves TF custody and post-boot rollback state", () => {
  const staging = section(2, 3);
  assert.match(
    staging,
    /first\s+receipt-bound beta31 candidate boot with no TF card inserted/u,
  );
  assert.match(staging, /complete\s+its local 30-second boot-health soak/u);
  assert.match(staging, /power the device fully\s+off/u);
  assert.match(staging, /reinsert the \*\*same card/u);
  assert.match(staging, /pre-first-boot cancellation path only/u);
  assert.match(staging, /bootloader's automatic rollback/u);
  assert.match(staging, /separately reviewed,\s+state-aware gate/u);
});

test("signed manifests are an OTA gate, not a local inactive-app staging gate", () => {
  const staging = section(2, 3);
  const ota = section(11, 12);
  assert.match(
    staging,
    /Local inactive-app0 staging is bound by\s+this exact receipt and tuple; it does not require or consume an OTA signed\s+manifest/u,
  );
  assert.match(ota, /signed-manifest requirement\s+starts in this OTA section/u);
  assert.match(ota, /OTA signed manifest binds the exact C151 application bytes/u);
});
