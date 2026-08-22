#!/usr/bin/env node

import { readFile, writeFile } from "node:fs/promises";
import { performance } from "node:perf_hooks";

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
  const key = process.argv[index];
  const value = process.argv[index + 1];
  if (!key?.startsWith("--") || value === undefined) {
    throw new Error("usage: --nvs-json PATH [--output PATH] [--prompt TEXT]");
  }
  args.set(key.slice(2), value);
}

const baseUrl = args.get("base-url") || "https://myai.mess.host";
const nvsPath = args.get("nvs-json");
const outputPath = args.get("output") || "/tmp/inkloop-myai-aigc-smoke.png";
const prompt = args.get("prompt") ||
  "A clean cheerful red lighthouse beside a blue sea, bold geometric shapes, " +
  "bright six-color e-paper palette, strong contrast, no text, portrait poster";
// MyAI's existing C151 record uses the public/display byte order. Keep this
// aligned with the firmware's ClientConfig, not the local eFuse fingerprint.
const wireMac = args.get("mac") || "28:84:85:43:DA:0C";

if (!nvsPath) throw new Error("--nvs-json is required");
const parsedBase = new URL(baseUrl);
if (parsedBase.protocol !== "https:" || parsedBase.username || parsedBase.password)
  throw new Error("public HTTPS MyAI base URL required");
if (prompt.length < 1 || prompt.length > 1024) throw new Error("invalid prompt length");

function safeError(body) {
  const value = typeof body?.error === "string" ? body.error
    : typeof body?.code === "string" ? body.code : "request_failed";
  return /^[A-Za-z0-9_. -]{1,128}$/.test(value) ? value : "request_failed";
}

async function boundedJson(response, maximum = 8 * 1024 * 1024) {
  const text = await response.text();
  if (Buffer.byteLength(text) > maximum) throw new Error("response_too_large");
  try {
    return JSON.parse(text);
  } catch {
    throw new Error("invalid_json_response");
  }
}

async function request(method, url, headers = {}, body, maximum) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 60_000);
  try {
    const response = await fetch(url, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      redirect: "error",
      signal: controller.signal,
    });
    const json = await boundedJson(response, maximum);
    if (!response.ok)
      throw new Error(`http_${response.status}:${safeError(json)}`);
    return { status: response.status, json };
  } finally {
    clearTimeout(timeout);
  }
}

const rows = JSON.parse(await readFile(nvsPath, "utf8"));
const headRow = rows.find((row) =>
  row.namespace === "ink-myai-v1" && row.key === "head");
if (!headRow || !Number.isSafeInteger(headRow.data) || headRow.data < 1)
  throw new Error("current MyAI credential slot is unavailable");
const slotIndex = headRow.data & 1;
const slotRow = rows.find((row) =>
  row.namespace === "ink-myai-v1" && row.key === `slot${slotIndex}`);
if (!slotRow || typeof slotRow.data !== "string")
  throw new Error("current MyAI credential snapshot is unavailable");
const credential = JSON.parse(slotRow.data);
const deviceId = credential.device_id;
const deviceToken = credential.device_token;
if (credential.generation !== headRow.data || !/^\d{6}$/.test(deviceId) ||
    typeof deviceToken !== "string" ||
    deviceToken.length < 16 || credential.active !== true) {
  throw new Error("bound active device credential required");
}

const deviceHeaders = {
  Authorization: `Bearer ${deviceToken}`,
  "Content-Type": "application/json",
  "X-Device-ID": deviceId,
  "X-Device-MAC": wireMac,
};

let sessionId = "";
let gatewayId = "";
try {
  const checked = await request(
    "POST", `${baseUrl}/api/v1/devices/check`, deviceHeaders,
    { device_id: deviceId, mac_address: wireMac }, 64 * 1024);
  if (checked.json.authorized !== true || checked.json.device?.active !== true)
    throw new Error("device_not_authorized");
  console.log(`CHECK PASS http=${checked.status} active=true`);

  const created = await request(
    "POST", `${baseUrl}/api/v1/client/sessions`, deviceHeaders,
    {
      device_id: deviceId,
      mac_address: wireMac,
      app_id: "inkloop",
      client_id: "inkloop-papercolor-image-smoke",
      client_version: "0.3.0-beta.1",
      required_scenarios: ["image"],
      required_kinds: ["aigc"],
    }, 128 * 1024);
  sessionId = created.json.session?.id || "";
  const gateways = Array.isArray(created.json.gateways) ? created.json.gateways : [];
  if (!sessionId || gateways.length === 0) throw new Error("no_gateway_candidates");
  console.log(`SESSION PASS http=${created.status} candidates=${gateways.length}`);

  const probes = [];
  for (const candidate of gateways) {
    const ping = new URL(candidate.ping_url);
    if (ping.protocol !== "https:") continue;
    const started = performance.now();
    let ok = false;
    try {
      const response = await fetch(ping, {
        method: "HEAD",
        headers: deviceHeaders,
        redirect: "error",
        signal: AbortSignal.timeout(8_000),
      });
      ok = response.status >= 200 && response.status < 400;
    } catch {}
    probes.push({
      gateway_id: String(candidate.id || ""),
      ok,
      latency_ms: Math.max(1, Math.round(performance.now() - started)),
      checked_at: new Date().toISOString(),
    });
  }
  const passing = probes.filter((probe) => probe.ok && probe.gateway_id)
    .sort((left, right) => left.latency_ms - right.latency_ms);
  if (passing.length === 0) throw new Error("no_reachable_gateway");
  gatewayId = passing[0].gateway_id;
  console.log(`PROBE PASS reachable=${passing.length} best_ms=${passing[0].latency_ms}`);

  const selected = await request(
    "POST", `${baseUrl}/api/v1/client/sessions/select`, deviceHeaders,
    { session_id: sessionId, gateway_id: gatewayId, probe_results: probes },
    128 * 1024);
  const gatewayToken = selected.json.gateway_token;
  const gatewayBase = selected.json.gateway?.base_url;
  if (typeof gatewayToken !== "string" || gatewayToken.length < 16 ||
      typeof gatewayBase !== "string" || !gatewayBase.startsWith("https://"))
    throw new Error("invalid_gateway_selection");
  console.log(`SELECT PASS http=${selected.status}`);

  const started = await request(
    "POST", `${gatewayBase}/api/v1/gateway/sessions/start`,
    { "Content-Type": "application/json", "X-Gateway-Session-Token": gatewayToken },
    { session_id: sessionId, gateway_id: gatewayId }, 64 * 1024);
  console.log(`GATEWAY_START PASS http=${started.status}`);

  const generated = await request(
    "POST", `${gatewayBase}/gateway/v1/aigc/generate`, deviceHeaders,
    {
      device_id: deviceId,
      mac_address: wireMac,
      app_id: "inkloop",
      prompt,
      negative_prompt: "text, watermark, logo, blurry, low contrast, gray haze",
      model: "t2i",
      size: "400x600",
      steps: 24,
    }, 128 * 1024);
  const promptId = generated.json.prompt_id;
  if (typeof promptId !== "string" || !promptId) throw new Error("missing_prompt_id");
  console.log(`GENERATE PASS http=${generated.status} status=${generated.json.status || "unknown"}`);

  let terminal;
  for (let attempt = 1; attempt <= 120; ++attempt) {
    await new Promise((resolve) => setTimeout(resolve, 3_000));
    const status = await request(
      "POST", `${gatewayBase}/gateway/v1/aigc/status`, deviceHeaders,
      { device_id: deviceId, mac_address: wireMac, app_id: "inkloop", prompt_id: promptId },
      256 * 1024);
    const state = String(status.json.status || "").toLowerCase();
    if (attempt === 1 || attempt % 10 === 0)
      console.log(`POLL attempt=${attempt} status=${state || "unknown"}`);
    if (["completed", "complete", "succeeded"].includes(state)) {
      terminal = status.json;
      break;
    }
    if (["failed", "error", "cancelled", "rejected"].includes(state))
      throw new Error(`aigc_${state}`);
  }
  const output = terminal?.outputs?.[0];
  if (!output?.filename) throw new Error("aigc_poll_timeout_or_missing_output");
  console.log(`POLL PASS status=${terminal.status} outputs=${terminal.outputs.length}`);

  const fetched = await request(
    "POST", `${gatewayBase}/gateway/v1/aigc/output`, deviceHeaders,
    {
      device_id: deviceId,
      mac_address: wireMac,
      app_id: "inkloop",
      prompt_id: promptId,
      node_id: String(output.node_id || ""),
      filename: output.filename,
      subfolder: String(output.subfolder || ""),
      type: String(output.type || "output"),
    }, 8 * 1024 * 1024);
  if (!["image/png", "image/x-png"].includes(fetched.json.content_type) ||
      typeof fetched.json.content_base64 !== "string")
    throw new Error("invalid_aigc_output_envelope");
  const image = Buffer.from(fetched.json.content_base64, "base64");
  if (image.length < 33 || !image.subarray(0, 8).equals(
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])))
    throw new Error("output_is_not_png");
  const width = image.readUInt32BE(16);
  const height = image.readUInt32BE(20);
  if (width !== 400 || height !== 600)
    throw new Error(`unexpected_dimensions_${width}x${height}`);
  await writeFile(outputPath, image, { mode: 0o600 });
  console.log(`OUTPUT PASS http=${fetched.status} size=${image.length} dimensions=${width}x${height}`);
  console.log(`ARTIFACT ${outputPath}`);
} finally {
  if (sessionId && gatewayId) {
    try {
      await request(
        "POST", `${baseUrl}/api/v1/client/sessions/disconnect`, deviceHeaders,
        { session_id: sessionId, gateway_id: gatewayId, reason: "smoke_complete" },
        64 * 1024);
      console.log("DISCONNECT PASS");
    } catch {
      console.log("DISCONNECT WARN");
    }
  }
}
