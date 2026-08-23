import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const scriptUrl = new URL("../scripts/myai-papercolor-smoke.mjs", import.meta.url);

test("PaperColor MyAI smoke probes each candidate with its short-lived probe token", async () => {
  const source = await readFile(scriptUrl, "utf8");
  const probeStart = source.indexOf("const probeToken =");
  const probeEnd = source.indexOf("const passing =", probeStart);
  assert.ok(probeStart >= 0 && probeEnd > probeStart, "missing bounded probe flow");

  const probe = source.slice(probeStart, probeEnd);
  assert.match(probe, /created\.json\.probe_token/);
  assert.match(probe, /Buffer\.byteLength\(probeToken\) > 2048/);
  assert.match(probe, /Authorization: `Bearer \$\{probeToken\}`/);
  assert.match(probe, /"X-Device-ID": deviceId/);
  assert.match(probe, /"X-Device-MAC": wireMac/);
  assert.match(probe, /"X-Gateway-ID": candidateId/);
  assert.doesNotMatch(probe, /headers:\s*deviceHeaders/);
});

test("PaperColor MyAI smoke uses the selected gateway lease for every gateway call", async () => {
  const source = await readFile(scriptUrl, "utf8");
  const headersStart = source.indexOf("const gatewayHeaders =");
  const disconnectStart = source.indexOf("} finally {", headersStart);
  assert.ok(headersStart >= 0 && disconnectStart > headersStart, "missing gateway lease flow");

  const gateway = source.slice(headersStart, disconnectStart);
  assert.match(gateway, /Authorization: `Bearer \$\{gatewayToken\}`/);
  assert.match(gateway, /"X-Gateway-Session-Token": gatewayToken/);
  assert.match(gateway, /"X-Gateway-Session-ID": sessionId/);
  assert.match(gateway, /"X-Gateway-ID": gatewayId/);

  for (const route of [
    "/gateway/v1/gateway/sessions/start",
    "/gateway/v1/aigc/generate",
    "/gateway/v1/aigc/status",
    "/gateway/v1/aigc/output",
  ]) {
    const routeIndex = gateway.indexOf(route);
    assert.ok(routeIndex >= 0, `missing ${route}`);
    const callTail = gateway.slice(routeIndex, routeIndex + 180);
    assert.match(callTail, /gatewayHeaders/, `${route} must use the selected gateway lease`);
    assert.doesNotMatch(callTail, /deviceHeaders/, `${route} must not use the device token`);
  }

  assert.match(gateway, /setTimeout\(resolve, 5_000\)/);
  assert.doesNotMatch(gateway, /setTimeout\(resolve, 3_000\)/);
});
