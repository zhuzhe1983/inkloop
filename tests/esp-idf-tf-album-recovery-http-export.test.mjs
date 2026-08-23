import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  symlinkSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  canonicalNewOutputPath,
  parseRecoveryAuthInput,
  runRemoteRecoveryExport,
  validateRecoveryBaseUrl,
} from "../firmware/inkloop-idf/tools/export_tf_album_recovery_http.mjs";

const exporterSource = readFileSync(new URL(
  "../firmware/inkloop-idf/tools/export_tf_album_recovery_http.mjs",
  import.meta.url,
), "utf8");

function hash(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function asset(seed) {
  const bytes = Buffer.from(`remote-${seed}-${"x".repeat(64)}`, "utf8");
  return { bytes, sha256: hash(bytes) };
}

function index(entries, current) {
  return Buffer.from(JSON.stringify({
    schema: 1,
    current,
    currentRenderStrategy: "official-quality",
    assets: entries.map((entry, at) => ({
      id: entry.sha256,
      path: `/inkloop-album/${entry.sha256}.png`,
      contentSha256: entry.sha256,
      bytes: entry.bytes.length,
      landscape: false,
      created: at + 1,
      taskId: "",
      renderStrategy: "official-quality",
    })),
  }), "utf8");
}

function fixture(finishChanged = false) {
  const first = asset("first");
  const second = asset("second");
  const third = asset("third");
  const candidateBytes = [
    index([first, second], first.sha256),
    index([second, third], third.sha256),
    index([first, third], first.sha256),
  ];
  const candidates = candidateBytes.map((bytes, item) => ({
    item,
    bytes: bytes.length,
    sha256: hash(bytes),
    assetEntries: 2,
  }));
  const assets = [
    { ...first, candidateMask: 5 },
    { ...second, candidateMask: 3 },
    { ...third, candidateMask: 6 },
  ].map((value, ordinal) => ({ ...value, ordinal, item: ordinal + 3 }));
  const session = "1234567890abcdef1234567890abcdef";
  const cookie = "SESSION_TOKEN_1234567890";
  const csrf = "CSRF_TOKEN_123456789012";
  const calls = [];
  let aborted = false;
  const responseJson = (value, status = 200, headers = {}) => new Response(
    JSON.stringify(value), {
      status,
      headers: { "Content-Type": "application/json", ...headers },
    });

  async function fetchImpl(url, options = {}) {
    const parsed = new URL(url);
    const method = options.method ?? "GET";
    calls.push(`${method} ${parsed.pathname}`);
    assert.equal(options.redirect, "error");
    assert.equal(options.headers.Origin, "http://localhost:8080");
    assert.notEqual(parsed.pathname, "/api/session");
    assert.equal(options.headers.Cookie,
      `inkloop_recovery_session=${cookie}`);
    assert.equal(options.headers["X-Inkloop-CSRF"], csrf);
    if (parsed.pathname === "/api/recovery/export/prepare") {
      assert.equal(options.body.get("confirm"), "readonly_export");
      assert.deepEqual([
        options.body.get("current"), options.body.get("next"),
        options.body.get("previous"),
      ], candidates.map((value) => value.sha256));
      return responseJson({
        ok: true,
        session,
        assetCount: assets.length,
        inventoryPages: 1,
        totalBytes: candidates.reduce((sum, value) => sum + value.bytes, 0) +
          assets.reduce((sum, value) => sum + value.bytes.length, 0),
        candidates,
      });
    }
    if (parsed.pathname === `/api/recovery/export/inventory/${session}/0`) {
      return responseJson({
        ok: true,
        session,
        page: 0,
        assetOffset: 0,
        assets: assets.map((value) => ({
          ordinal: value.ordinal,
          item: value.item,
          bytes: value.bytes.length,
          sha256: value.sha256,
          candidateMask: value.candidateMask,
        })),
      });
    }
    const filePrefix = `/api/recovery/export/file/${session}/`;
    if (parsed.pathname.startsWith(filePrefix)) {
      const item = Number(parsed.pathname.slice(filePrefix.length));
      const bytes = item < 3 ? candidateBytes[item] : assets[item - 3]?.bytes;
      const digest = item < 3 ? candidates[item]?.sha256 : assets[item - 3]?.sha256;
      assert.ok(bytes && digest);
      return new Response(bytes, {
        status: 200,
        headers: {
          "Content-Type": "application/octet-stream",
          "X-Inkloop-SHA256": digest,
          "X-Inkloop-Bytes": String(bytes.length),
        },
      });
    }
    if (parsed.pathname === "/api/recovery/export/finish") {
      assert.equal(options.body.get("session"), session);
      assert.equal(options.body.get("confirm"), "verify_export");
      return finishChanged
        ? responseJson({ ok: false, error: "recovery_export_source_changed" }, 409)
        : responseJson({ ok: true, result: "verified" });
    }
    if (parsed.pathname === "/api/recovery/export/abort") {
      aborted = true;
      return responseJson({ ok: true, result: "aborted" });
    }
    return responseJson({ ok: false, error: "route_not_found" }, 404);
  }
  return {
    fetchImpl,
    candidates,
    candidateBytes,
    assets,
    expected: {
      current: candidates[0].sha256,
      next: candidates[1].sha256,
      previous: candidates[2].sha256,
    },
    auth: { sessionCookie: cookie, csrfToken: csrf },
    calls,
    wasAborted: () => aborted,
  };
}

test("authenticated Recovery HTTP export preserves the bound candidate and asset union", async () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-http-export-"));
  const output = join(scratch, "backup");
  const source = fixture();
  const fetchImpl = async (...args) => {
    assert.equal(existsSync(output), true,
      "the exclusive output directory must exist before network I/O");
    return source.fetchImpl(...args);
  };
  try {
    const manifest = await runRemoteRecoveryExport({
      baseUrl: "http://localhost:8080/",
      outputRoot: output,
      ...source.auth,
      expected: source.expected,
      fetchImpl,
    });
    assert.equal(manifest.complete, true);
    assert.equal(manifest.transport, "authenticated-recovery-http");
    assert.equal(manifest.sourceVerifiedAfterTransfer, true);
    assert.equal(manifest.uniqueReferencedAssets, 3);
    assert.equal(source.wasAborted(), false);
    for (let at = 0; at < 3; ++at) {
      assert.deepEqual(readFileSync(join(output, "candidates",
        ["index.json", "index.next", "index.prev"][at])),
      source.candidateBytes[at]);
    }
    for (const value of source.assets)
      assert.deepEqual(readFileSync(join(output, "assets",
        `${value.sha256}.png`)), value.bytes);
    assert.equal(JSON.parse(readFileSync(join(output, "manifest.json"),
      "utf8")).complete, true);
    assert.ok(source.calls[0]?.endsWith("/api/recovery/export/prepare"));
    assert.equal(source.calls.some((call) => call.endsWith("/api/session")),
      false);
    assert.ok(source.calls.at(-1)?.endsWith("/api/recovery/export/finish"));
  } finally { rmSync(scratch, { recursive: true, force: true }); }
});

test("remote export fails closed when final Recovery TOCTOU verification changes", async () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-http-export-stale-"));
  const output = join(scratch, "backup");
  const source = fixture(true);
  try {
    await assert.rejects(runRemoteRecoveryExport({
      baseUrl: "http://localhost:8080/",
      outputRoot: output,
      ...source.auth,
      expected: source.expected,
      fetchImpl: source.fetchImpl,
    }), /recovery_export_source_changed/);
    assert.equal(source.wasAborted(), true);
    assert.equal(existsSync(join(output, "manifest.json")), false);
    assert.equal(JSON.parse(readFileSync(join(output, "INCOMPLETE.json"),
      "utf8")).complete, false);
  } finally { rmSync(scratch, { recursive: true, force: true }); }
});

test("remote exporter accepts only exact private IPv4 Recovery origins", () => {
  assert.match(
    exporterSource,
    /FINAL_VERIFICATION_TIMEOUT_MS\s*=\s*30\s*\*\s*60_000/,
  );
  assert.equal(validateRecoveryBaseUrl("http://localhost:8080/").origin,
    "http://localhost:8080");
  for (const value of [
    "http://10.0.0.1:8080/", "http://10.255.255.254:8080/",
    "http://172.16.0.1:8080/", "http://172.31.255.254:8080/",
    "http://192.168.4.1:8080/", "http://192.168.199.156:8080/",
  ]) assert.equal(validateRecoveryBaseUrl(value).href, value);
  for (const value of [
    "http://inkloop.local:8080/", "https://192.168.4.1:8080/",
    "http://192.168.4.1/", "http://192.168.4.1:8081/",
    "http://192.168.4.1:8080",
    "http://INKLOOP.local:8080/", "http://inkloop.local:8080/path",
    "http://inkloop.local:8080/?candidate=current",
    "http://inkloop.local:8080/#candidate",
    "http://printer.local:8080/", "http://inkloop.local.attacker:8080/",
    "http://example.com:8080/", "http://203.0.113.8:8080/",
    "http://127.0.0.1:8080/", "http://169.254.1.1:8080/",
    "http://172.15.255.255:8080/", "http://172.32.0.1:8080/",
    "http://192.167.255.255:8080/", "http://192.169.0.1:8080/",
    "http://192.168.004.001:8080/", "http://localhost.local:8080/",
    "http://[::1]:8080/", "http://[fd00::1]:8080/",
    "http://user:secret@192.168.4.1:8080/",
  ]) assert.throws(() => validateRecoveryBaseUrl(value),
    /must be exact http:\/\/<private-ipv4>:8080\//);
});

test("remote exporter accepts only two bounded auth tokens from stdin", () => {
  assert.deepEqual(parseRecoveryAuthInput(
    "SESSION_TOKEN_1234567890\nCSRF_TOKEN_123456789012\n"), {
    sessionCookie: "SESSION_TOKEN_1234567890",
    csrfToken: "CSRF_TOKEN_123456789012",
  });
  for (const value of [
    "", "one-line-only\n", "a\nb\nextra\n",
    "short\nCSRF_TOKEN_123456789012\n",
    "SESSION_TOKEN_1234567890\nSESSION_TOKEN_1234567890\n",
  ]) assert.throws(() => parseRecoveryAuthInput(value), /Recovery/);
  assert.doesNotMatch(exporterSource, /accessCode|access-code-stdin/);
  assert.doesNotMatch(exporterSource, /\/api\/session/);
  assert.match(exporterSource, /--auth-stdin/);
});

test("output uses canonical parent plus leaf and is claimed exclusively", async () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-http-output-"));
  const realParent = join(scratch, "real-parent");
  const aliasParent = join(scratch, "alias-parent");
  mkdirSync(realParent, { mode: 0o700 });
  symlinkSync(realParent, aliasParent, "dir");
  const requested = join(aliasParent, "backup");
  const canonical = canonicalNewOutputPath(requested);
  assert.equal(canonical.parent, realpathSync(realParent));
  assert.equal(canonical.output, join(realpathSync(realParent), "backup"));

  mkdirSync(canonical.output, { mode: 0o700 });
  let fetched = false;
  const source = fixture();
  try {
    await assert.rejects(runRemoteRecoveryExport({
      baseUrl: "http://localhost:8080/",
      outputRoot: requested,
      ...source.auth,
      expected: source.expected,
      fetchImpl: async (...args) => {
        fetched = true;
        return source.fetchImpl(...args);
      },
    }), /output directory already exists/);
    assert.equal(fetched, false);
  } finally { rmSync(scratch, { recursive: true, force: true }); }
});
