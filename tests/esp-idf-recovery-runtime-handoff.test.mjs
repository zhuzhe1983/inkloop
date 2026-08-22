import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const source = readFileSync(
  join(repo, "firmware/inkloop-idf/main/app_main.cpp"),
  "utf8",
);

function body(signature, nextSignature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = source.indexOf(nextSignature, start + signature.length);
  assert.notEqual(end, -1, `missing ${nextSignature}`);
  return source.slice(start, end);
}

test("runtime failure enters Recovery only after an ESP_OK quiesce handoff", () => {
  const handoff = body(
    "[[noreturn]] void runRecoveryAfterProductFailure",
    "}\n\nextern \"C\" void app_main",
  );
  assert.match(handoff, /runtime\.shutdownForRecovery\(\)/);
  assert.match(
    handoff,
    /if \(quiesced == ESP_OK\)[\s\S]*runRecoveryNetwork\(/,
  );
  assert.ok(
    handoff.indexOf("quiesced == ESP_OK") <
      handoff.indexOf("runRecoveryNetwork("),
  );
  assert.match(handoff, /kMaximumQuiesceAttempts = 8U/);
  assert.match(handoff, /esp_restart\(\)/);
  assert.doesNotMatch(
    handoff.slice(handoff.indexOf("could not quiesce")),
    /runRecoveryNetwork\(/,
  );
});

test("both post-composition failure branches use the guarded handoff", () => {
  const app = source.slice(source.indexOf('extern "C" void app_main'));
  assert.equal(
    (app.match(/runRecoveryAfterProductFailure\(/g) || []).length,
    2,
  );
  const directRecovery = app.match(/runRecoveryNetwork\(/g) || [];
  assert.ok(directRecovery.length >= 1);
  const afterRuntime = app.slice(app.indexOf("static inkloop::EspProductRuntime"));
  assert.doesNotMatch(afterRuntime, /runRecoveryNetwork\(/);
});
