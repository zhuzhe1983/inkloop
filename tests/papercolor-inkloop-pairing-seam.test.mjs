import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

test("PaperColor Inkloop registration seam accepts only exact ASCII six digits", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-pairing-seam-"));
  try {
    const harness = join(temporary, "pairing.cpp");
    const executable = join(temporary, "pairing");
    await writeFile(harness, String.raw`
#include <cassert>
#include "InkloopPairingPrimitives.h"
int main() {
  using inkloop::exactSixDigitPairingCode;
  assert(exactSixDigitPairingCode("000000", 6));
  assert(exactSixDigitPairingCode("364728", 6));
  assert(!exactSixDigitPairingCode("12345", 5));
  assert(!exactSixDigitPairingCode("1234567", 7));
  assert(!exactSixDigitPairingCode(" 123456", 7));
  const char unicodeDigits[] = "１２３４５６";
  assert(!exactSixDigitPairingCode(unicodeDigits, sizeof(unicodeDigits) - 1));
}
`);
    const built = spawnSync("c++", [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-Ifirmware/m5-papercolor/src",
      harness,
      "-o",
      executable,
    ], { cwd: new URL("../", import.meta.url), encoding: "utf8" });
    assert.equal(built.status, 0, `${built.stdout}\n${built.stderr}`);
    const ran = spawnSync(executable, [], { encoding: "utf8" });
    assert.equal(ran.status, 0, `${ran.stdout}\n${ran.stderr}`);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("InkloopClient keeps legacy registration and adds an exact optional pairingCode field", async () => {
  const header = await readFile(
    new URL("../firmware/m5-papercolor/src/InkloopClient.h", import.meta.url),
    "utf8",
  );
  const source = await readFile(
    new URL("../firmware/m5-papercolor/src/InkloopClient.cpp", import.meta.url),
    "utf8",
  );
  assert.match(header, /RegistrationResult registerDevice\(\);/);
  assert.match(header, /RegistrationResult registerDevice\(const String& requestedPairingCode\);/);
  assert.match(source, /registerDeviceImpl\(nullptr\)/);
  assert.match(source, /exactSixDigitPairingCode\([\s\S]*requestedPairingCode->c_str\(\), requestedPairingCode->length\(\)\)/);
  assert.match(source, /request\["pairingCode"\] = \*requestedPairingCode/);
  assert.match(source, /nextPairingCode != \*requestedPairingCode/);
  assert.match(source, /ISRG[\s\S]{0,32}Root X1/);
  assert.match(source, /Google Trust Services/);
  assert.equal((source.match(/-----BEGIN CERTIFICATE-----/g) || []).length, 2);
  assert.match(source, /client\.setCACert\(kRootCa\)/);
});
