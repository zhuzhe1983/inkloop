#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  closeSync,
  constants,
  fstatSync,
  fsyncSync,
  mkdirSync,
  openSync,
  readFileSync,
  readSync,
  realpathSync,
  renameSync,
  writeSync,
} from "node:fs";
import { basename, dirname, isAbsolute, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { parseIndex } from "./export_tf_album_recovery.mjs";

const HASH_PATTERN = /^[0-9a-f]{64}$/;
const SESSION_PATTERN = /^(?!0{32}$)[0-9a-f]{32}$/;
const TOKEN_PATTERN = /^[A-Za-z0-9_-]{16,64}$/;
const MAXIMUM_JSON_BYTES = 16 * 1024;
const MAXIMUM_INDEX_BYTES = 64 * 1024;
const MAXIMUM_ASSET_BYTES = 1_500_000;
const MINIMUM_ASSET_BYTES = 45;
const MAXIMUM_ASSETS = 3 * 96;
const INVENTORY_PAGE_ASSETS = 24;
const MAXIMUM_NETWORK_CHUNK_BYTES = 256 * 1024;
const MAXIMUM_TOTAL_BYTES = 3 * MAXIMUM_INDEX_BYTES +
  MAXIMUM_ASSETS * MAXIMUM_ASSET_BYTES;
const METADATA_TIMEOUT_MS = 30_000;
const FILE_TIMEOUT_MS = 120_000;
// Recovery performs a second full source hash after the bounded download.
// Match the device's finite custody session so the host does not abort a
// maximum-sized slow-card verification first.
const FINAL_VERIFICATION_TIMEOUT_MS = 30 * 60_000;
const SLOT_DEFINITIONS = Object.freeze([
  Object.freeze({ slot: "current", name: "index.json", item: 0, mask: 1 }),
  Object.freeze({ slot: "next", name: "index.next", item: 1, mask: 2 }),
  Object.freeze({ slot: "previous", name: "index.prev", item: 2, mask: 4 }),
]);

function fail(message) {
  throw new Error(message);
}

function requiredObject(value, label) {
  if (!value || typeof value !== "object" || Array.isArray(value))
    fail(`${label} is not an object`);
  return value;
}

function exactObject(value, keys, label) {
  requiredObject(value, label);
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  if (actual.length !== expected.length ||
      actual.some((key, at) => key !== expected[at]))
    fail(`${label} has an unexpected schema`);
  return value;
}

function safeInteger(value, minimum, maximum, label) {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    fail(`${label} is outside its bounded integer contract`);
  return value;
}

function exactHash(value, label) {
  if (typeof value !== "string" || !HASH_PATTERN.test(value))
    fail(`${label} is not a lowercase SHA-256`);
  return value;
}

export function validateRecoveryBaseUrl(value) {
  if (value === "http://localhost:8080/") return new URL(value);
  const match = /^http:\/\/([0-9]{1,3}(?:\.[0-9]{1,3}){3}):8080\/$/.exec(value);
  const octets = match?.[1].split(".").map((part) => Number(part));
  const canonical = match && octets.length === 4 &&
    octets.every((octet, at) => Number.isInteger(octet) && octet >= 0 &&
      octet <= 255 && String(octet) === match[1].split(".")[at]);
  const privateAddress = canonical && (octets[0] === 10 ||
    (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) ||
    (octets[0] === 192 && octets[1] === 168));
  if (!privateAddress) {
    fail("remote URL must be exact http://<private-ipv4>:8080/ " +
      "(http://localhost:8080/ is reserved for tests)");
  }
  return new URL(value);
}

function expectedHashes(value) {
  requiredObject(value, "expected hashes");
  const output = {};
  for (const definition of SLOT_DEFINITIONS)
    output[definition.slot] = exactHash(
      value[definition.slot], `expected ${definition.slot} SHA-256`);
  if (new Set(Object.values(output)).size !== SLOT_DEFINITIONS.length)
    fail("the three expected candidate indexes are not divergent");
  return Object.freeze(output);
}

function validateRecoveryAuth(sessionCookie, csrfToken) {
  if (typeof sessionCookie !== "string" ||
      !TOKEN_PATTERN.test(sessionCookie))
    fail("Recovery session cookie is outside its token contract");
  if (typeof csrfToken !== "string" || !TOKEN_PATTERN.test(csrfToken))
    fail("Recovery CSRF token is outside its token contract");
  if (sessionCookie === csrfToken)
    fail("Recovery session cookie and CSRF token must differ");
  return Object.freeze({ sessionCookie, csrfToken });
}

export function parseRecoveryAuthInput(value) {
  if (typeof value !== "string") fail("Recovery auth stdin must be text");
  const lines = value.split(/\r?\n/);
  if (lines.at(-1) === "") lines.pop();
  if (lines.length !== 2)
    fail("Recovery auth stdin must contain exactly two lines");
  return validateRecoveryAuth(lines[0], lines[1]);
}

export function canonicalNewOutputPath(value) {
  if (!isAbsolute(value)) fail("output must be an absolute path");
  const requested = resolve(value);
  const leaf = basename(requested);
  if (!leaf) fail("output must name a new child directory");
  const parent = realpathSync(dirname(requested));
  const output = join(parent, leaf);
  if (output === parent) fail("output must name a new child directory");
  return Object.freeze({ output, parent });
}

async function openWithTimeout(fetchImpl, url, init, timeoutMs) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetchImpl(url, {
      ...init,
      redirect: "error",
      signal: controller.signal,
    });
    return {
      response,
      dispose() { clearTimeout(timeout); },
    };
  } catch (error) {
    clearTimeout(timeout);
    throw error;
  }
}

async function readBounded(response, maximum, label) {
  if (!response.body) fail(`${label} has no response body`);
  const reader = response.body.getReader();
  const chunks = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array) || value.byteLength === 0 ||
          value.byteLength > MAXIMUM_NETWORK_CHUNK_BYTES ||
          total > maximum - value.byteLength) {
        fail(`${label} exceeded its bounded response contract`);
      }
      total += value.byteLength;
      chunks.push(Buffer.from(value));
    }
  } catch (error) {
    try { await reader.cancel(); } catch {}
    throw error;
  }
  return Buffer.concat(chunks, total);
}

async function readJson(response, label) {
  const bytes = await readBounded(response, MAXIMUM_JSON_BYTES, label);
  let value;
  try { value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes)); }
  catch { fail(`${label} is not bounded UTF-8 JSON`); }
  if (!response.ok) {
    const error = value && typeof value.error === "string"
      ? value.error : `HTTP ${response.status}`;
    fail(`${label} was refused: ${error}`);
  }
  return value;
}

class RecoveryHttpClient {
  constructor(base, fetchImpl, sessionCookie, csrfToken) {
    this.base = base;
    this.fetchImpl = fetchImpl;
    this.cookie = sessionCookie;
    this.csrf = csrfToken;
  }

  url(path) { return new URL(path, this.base); }

  headers(contentType = false) {
    const output = {
      Origin: this.base.origin,
      Cookie: `inkloop_recovery_session=${this.cookie}`,
      "X-Inkloop-CSRF": this.csrf,
      "Cache-Control": "no-store",
    };
    if (contentType)
      output["Content-Type"] = "application/x-www-form-urlencoded";
    return output;
  }

  async json(path, method = "GET", fields = undefined,
             timeoutMs = METADATA_TIMEOUT_MS) {
    const opened = await openWithTimeout(this.fetchImpl, this.url(path), {
      method,
      headers: this.headers(fields !== undefined),
      body: fields === undefined ? undefined : new URLSearchParams(fields),
    }, timeoutMs);
    try { return await readJson(opened.response, path); }
    finally { opened.dispose(); }
  }

  async file(path) {
    return openWithTimeout(this.fetchImpl, this.url(path), {
      method: "GET",
      headers: this.headers(false),
    }, FILE_TIMEOUT_MS);
  }
}

function validatePrepared(value, expected) {
  exactObject(value,
    ["ok", "session", "assetCount", "inventoryPages", "totalBytes", "candidates"],
    "export prepare response");
  if (value.ok !== true || !SESSION_PATTERN.test(value.session ?? "") ||
      !Array.isArray(value.candidates) || value.candidates.length !== 3)
    fail("export prepare response is invalid");
  const assetCount = safeInteger(value.assetCount, 0, MAXIMUM_ASSETS,
    "prepared asset count");
  const pages = safeInteger(value.inventoryPages, 0,
    Math.ceil(MAXIMUM_ASSETS / INVENTORY_PAGE_ASSETS), "inventory page count");
  if (pages !== Math.ceil(assetCount / INVENTORY_PAGE_ASSETS))
    fail("prepared inventory page count is inconsistent");
  const candidates = value.candidates.map((candidate, at) => {
    exactObject(candidate, ["item", "bytes", "sha256", "assetEntries"],
      `prepared candidate ${at}`);
    const definition = SLOT_DEFINITIONS[at];
    if (candidate.item !== at ||
        exactHash(candidate.sha256, `prepared candidate ${at} SHA-256`) !==
          expected[definition.slot]) fail(`prepared ${definition.slot} candidate changed`);
    return Object.freeze({
      ...definition,
      bytes: safeInteger(candidate.bytes, 1, MAXIMUM_INDEX_BYTES,
        `prepared candidate ${at} bytes`),
      sha256: candidate.sha256,
      assetEntries: safeInteger(candidate.assetEntries, 0, 96,
        `prepared candidate ${at} entries`),
    });
  });
  const totalBytes = safeInteger(value.totalBytes,
    candidates.reduce((sum, candidate) => sum + candidate.bytes, 0),
    MAXIMUM_TOTAL_BYTES, "prepared total bytes");
  return Object.freeze({
    session: value.session,
    assetCount,
    pages,
    totalBytes,
    candidates: Object.freeze(candidates),
  });
}

async function readInventory(client, prepared) {
  const assets = [];
  const hashes = new Set();
  for (let page = 0; page < prepared.pages; ++page) {
    const value = exactObject(await client.json(
      `/api/recovery/export/inventory/${prepared.session}/${page}`),
    ["ok", "session", "page", "assetOffset", "assets"],
    `inventory page ${page}`);
    const expectedOffset = page * INVENTORY_PAGE_ASSETS;
    if (value.ok !== true || value.session !== prepared.session ||
        value.page !== page || value.assetOffset !== expectedOffset ||
        !Array.isArray(value.assets)) fail(`inventory page ${page} is inconsistent`);
    const expectedCount = Math.min(INVENTORY_PAGE_ASSETS,
      prepared.assetCount - expectedOffset);
    if (value.assets.length !== expectedCount || expectedCount <= 0)
      fail(`inventory page ${page} has an invalid asset count`);
    for (let at = 0; at < value.assets.length; ++at) {
      const asset = exactObject(value.assets[at],
        ["ordinal", "item", "bytes", "sha256", "candidateMask"],
        `inventory asset ${expectedOffset + at}`);
      const ordinal = expectedOffset + at;
      const digest = exactHash(asset.sha256, `inventory asset ${ordinal} SHA-256`);
      if (asset.ordinal !== ordinal || asset.item !== ordinal + 3 ||
          hashes.has(digest)) fail(`inventory asset ${ordinal} is inconsistent`);
      hashes.add(digest);
      assets.push(Object.freeze({
        ordinal,
        item: asset.item,
        bytes: safeInteger(asset.bytes, MINIMUM_ASSET_BYTES,
          MAXIMUM_ASSET_BYTES, `inventory asset ${ordinal} bytes`),
        sha256: digest,
        candidateMask: safeInteger(asset.candidateMask, 1, 7,
          `inventory asset ${ordinal} candidate mask`),
      }));
    }
  }
  if (assets.length !== prepared.assetCount)
    fail("inventory did not contain the prepared asset count");
  const total = prepared.candidates.reduce((sum, value) => sum + value.bytes, 0) +
    assets.reduce((sum, value) => sum + value.bytes, 0);
  if (total !== prepared.totalBytes)
    fail("inventory byte total does not match the prepared snapshot");
  return Object.freeze(assets);
}

function syncDirectory(path) {
  const descriptor = openSync(path, constants.O_RDONLY);
  try { fsyncSync(descriptor); }
  finally { closeSync(descriptor); }
}

function writeExclusive(path, bytes) {
  const partial = `${path}.partial`;
  const descriptor = openSync(partial,
    constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL, 0o600);
  try {
    let offset = 0;
    while (offset < bytes.length) {
      const count = writeSync(descriptor, bytes, offset, bytes.length - offset);
      if (count <= 0) fail(`short destination write: ${path}`);
      offset += count;
    }
    fsyncSync(descriptor);
  } finally { closeSync(descriptor); }
  renameSync(partial, path);
  syncDirectory(dirname(path));
}

function hashLocalFile(path, expectedBytes, expectedDigest) {
  const noFollow = constants.O_NOFOLLOW ?? 0;
  const descriptor = openSync(path, constants.O_RDONLY | noFollow);
  try {
    const before = fstatSync(descriptor);
    if (!before.isFile() || before.size !== expectedBytes)
      fail(`destination file size changed: ${path}`);
    const hash = createHash("sha256");
    const buffer = Buffer.alloc(64 * 1024);
    let offset = 0;
    while (offset < expectedBytes) {
      const count = readSync(descriptor, buffer, 0,
        Math.min(buffer.length, expectedBytes - offset), offset);
      if (count <= 0) fail(`short destination read: ${path}`);
      hash.update(buffer.subarray(0, count));
      offset += count;
    }
    const after = fstatSync(descriptor);
    if (after.size !== before.size || after.mtimeMs !== before.mtimeMs ||
        hash.digest("hex") !== expectedDigest)
      fail(`destination verification failed: ${path}`);
  } finally { closeSync(descriptor); }
}

async function downloadVerified(client, prepared, item, destination,
                                expectedBytes, expectedDigest) {
  const opened = await client.file(
    `/api/recovery/export/file/${prepared.session}/${item}`);
  const response = opened.response;
  try {
  if (!response.ok) {
    await readJson(response, `export item ${item}`);
    fail(`export item ${item} was refused`);
  }
  if (response.headers.get("x-inkloop-sha256") !== expectedDigest ||
      response.headers.get("x-inkloop-bytes") !== String(expectedBytes) ||
      !/^application\/octet-stream(?:;|$)/i.test(
        response.headers.get("content-type") ?? ""))
    fail(`export item ${item} headers do not match its bound inventory`);
  if (!response.body) fail(`export item ${item} has no response body`);

  const partial = `${destination}.partial`;
  const descriptor = openSync(partial,
    constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL, 0o600);
  const hash = createHash("sha256");
  const reader = response.body.getReader();
  let received = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array) || value.byteLength === 0 ||
          value.byteLength > MAXIMUM_NETWORK_CHUNK_BYTES ||
          received > expectedBytes - value.byteLength)
        fail(`export item ${item} exceeded its bound size`);
      const bytes = Buffer.from(value);
      let offset = 0;
      while (offset < bytes.length) {
        const count = writeSync(descriptor, bytes, offset, bytes.length - offset);
        if (count <= 0) fail(`short destination write for export item ${item}`);
        offset += count;
      }
      hash.update(bytes);
      received += bytes.length;
    }
    if (received !== expectedBytes || hash.digest("hex") !== expectedDigest)
      fail(`export item ${item} body does not match its bound hash and size`);
    fsyncSync(descriptor);
  } catch (error) {
    try { await reader.cancel(); } catch {}
    throw error;
  } finally { closeSync(descriptor); }
  renameSync(partial, destination);
  syncDirectory(dirname(destination));
  hashLocalFile(destination, expectedBytes, expectedDigest);
  } finally { opened.dispose(); }
}

function readCandidate(path, expectedBytes, expectedDigest, label) {
  hashLocalFile(path, expectedBytes, expectedDigest);
  const bytes = readFileSync(path);
  if (bytes.length !== expectedBytes || bytes.length > MAXIMUM_INDEX_BYTES)
    fail(`${label} changed before schema verification`);
  return parseIndex(bytes, label);
}

function validateCandidateUnion(prepared, inventory, candidateDirectory) {
  const references = new Map();
  const parsed = prepared.candidates.map((candidate) => {
    const index = readCandidate(join(candidateDirectory, candidate.name),
      candidate.bytes, candidate.sha256, candidate.name);
    if (index.assets.length !== candidate.assetEntries)
      fail(`${candidate.name} entry count differs from the prepared snapshot`);
    for (const asset of index.assets) {
      const previous = references.get(asset.contentSha256);
      if (previous && previous.bytes !== asset.bytes)
        fail(`candidate indexes disagree on asset ${asset.contentSha256}`);
      references.set(asset.contentSha256, {
        bytes: asset.bytes,
        mask: (previous?.mask ?? 0) | candidate.mask,
      });
    }
    return Object.freeze({ ...candidate, current: index.current });
  });
  if (references.size !== inventory.length)
    fail("downloaded candidates and server inventory have different asset unions");
  for (const asset of inventory) {
    const reference = references.get(asset.sha256);
    if (!reference || reference.bytes !== asset.bytes ||
        reference.mask !== asset.candidateMask)
      fail(`inventory binding differs for asset ${asset.sha256}`);
  }
  return Object.freeze(parsed);
}

function referencedBy(mask) {
  return SLOT_DEFINITIONS.filter((value) => (mask & value.mask) !== 0)
    .map((value) => value.slot);
}

function durableJson(path, value) {
  writeExclusive(path, Buffer.from(`${JSON.stringify(value, null, 2)}\n`, "utf8"));
}

async function bestEffortAbort(client, session) {
  if (!session || !client.cookie || !client.csrf) return;
  try {
    await client.json("/api/recovery/export/abort", "POST", [
      ["session", session], ["confirm", "abort_export"],
    ]);
  } catch {}
}

export async function runRemoteRecoveryExport({
  baseUrl, outputRoot, expected, sessionCookie, csrfToken,
  fetchImpl = globalThis.fetch,
}) {
  if (typeof fetchImpl !== "function") fail("Fetch API is unavailable");
  const base = validateRecoveryBaseUrl(baseUrl);
  const hashes = expectedHashes(expected);
  const auth = validateRecoveryAuth(sessionCookie, csrfToken);
  const { output, parent } = canonicalNewOutputPath(outputRoot);

  const client = new RecoveryHttpClient(
    base, fetchImpl, auth.sessionCookie, auth.csrfToken);
  let prepared;
  let outputCreated = false;
  try {
    // Claim the canonical parent+leaf before the first network request. A
    // pre-existing directory or symlink fails atomically; later failures leave
    // an explicit INCOMPLETE marker in the directory this process claimed.
    try {
      mkdirSync(output, { mode: 0o700 });
    } catch (error) {
      if (error?.code === "EEXIST") fail("output directory already exists");
      throw error;
    }
    outputCreated = true;
    syncDirectory(parent);

    const preparedValue = await client.json(
      "/api/recovery/export/prepare", "POST", [
        ["current", hashes.current],
        ["next", hashes.next],
        ["previous", hashes.previous],
        ["confirm", "readonly_export"],
      ]);
    prepared = validatePrepared(preparedValue, hashes);
    const inventory = await readInventory(client, prepared);

    const candidateDirectory = join(output, "candidates");
    const assetDirectory = join(output, "assets");
    mkdirSync(candidateDirectory, { mode: 0o700 });
    mkdirSync(assetDirectory, { mode: 0o700 });
    syncDirectory(output);

    for (const candidate of prepared.candidates) {
      await downloadVerified(client, prepared, candidate.item,
        join(candidateDirectory, candidate.name), candidate.bytes,
        candidate.sha256);
    }
    const candidates = validateCandidateUnion(
      prepared, inventory, candidateDirectory);
    for (const asset of inventory) {
      await downloadVerified(client, prepared, asset.item,
        join(assetDirectory, `${asset.sha256}.png`), asset.bytes, asset.sha256);
    }

    const finished = exactObject(await client.json(
      "/api/recovery/export/finish", "POST", [
        ["session", prepared.session], ["confirm", "verify_export"],
      ], FINAL_VERIFICATION_TIMEOUT_MS), ["ok", "result"],
    "export finish response");
    if (finished.ok !== true || finished.result !== "verified")
      fail("Recovery did not verify the source snapshot after transfer");

    const files = [
      ...candidates.map((value) => ({
        path: `candidates/${value.name}`,
        bytes: value.bytes,
        sha256: value.sha256,
      })),
      ...inventory.map((value) => ({
        path: `assets/${value.sha256}.png`,
        bytes: value.bytes,
        sha256: value.sha256,
      })),
    ].sort((left, right) => left.path.localeCompare(right.path));
    for (const file of files)
      hashLocalFile(join(output, file.path), file.bytes, file.sha256);

    const manifest = {
      schema: 1,
      complete: true,
      readOnlySource: true,
      transport: "authenticated-recovery-http",
      recoveryOrigin: base.origin,
      recoverySession: prepared.session,
      createdAt: new Date().toISOString(),
      sourceVerifiedAfterTransfer: true,
      candidateCount: 3,
      candidates: candidates.map((value) => ({
        slot: value.slot,
        file: `candidates/${value.name}`,
        bytes: value.bytes,
        sha256: value.sha256,
        assetEntries: value.assetEntries,
        current: value.current,
      })),
      uniqueReferencedAssets: inventory.length,
      assets: inventory.map((value) => ({
        file: `assets/${value.sha256}.png`,
        bytes: value.bytes,
        sha256: value.sha256,
        referencedBy: referencedBy(value.candidateMask),
      })),
      totalBytes: prepared.totalBytes,
      files,
    };
    writeExclusive(join(output, "SHA256SUMS"), Buffer.from(
      `${files.map((value) => `${value.sha256}  ${value.path}`).join("\n")}\n`,
      "utf8"));
    // A complete manifest is written only after Recovery's final source
    // verification and a second local hash pass over every exported file.
    durableJson(join(output, "manifest.json"), manifest);
    return manifest;
  } catch (error) {
    await bestEffortAbort(client, prepared?.session);
    if (outputCreated) {
      try {
        durableJson(join(output, "INCOMPLETE.json"), {
          schema: 1,
          complete: false,
          error: error instanceof Error ? error.message : "unknown export failure",
        });
      } catch {}
    }
    throw error;
  }
}

function parseArgs(argv) {
  if (argv.length === 1 && argv[0] === "--help") return { help: true };
  const options = new Set([
    "--remote-url", "--output", "--expect-current-sha256",
    "--expect-next-sha256", "--expect-previous-sha256",
  ]);
  const values = {};
  let authStdin = false;
  for (let at = 0; at < argv.length;) {
    const name = argv[at++];
    if (name === "--auth-stdin") {
      if (authStdin) fail(`duplicate argument: ${name}`);
      authStdin = true;
      continue;
    }
    if (!options.has(name) || at >= argv.length || argv[at].startsWith("--"))
      fail(`invalid argument: ${name ?? "<missing>"}`);
    if (values[name] !== undefined) fail(`duplicate argument: ${name}`);
    values[name] = argv[at++];
  }
  for (const name of options)
    if (values[name] === undefined) fail(`missing required argument: ${name}`);
  if (!authStdin) fail("--auth-stdin is required");
  return values;
}

function usage() {
  return [
    "Usage:",
    "  printf '%s\\n%s\\n' '<Recovery session cookie>' '<Recovery CSRF token>' | \\",
    "  node firmware/inkloop-idf/tools/export_tf_album_recovery_http.mjs \\",
    "    --remote-url http://192.168.4.1:8080/ \\",
    "    --output /absolute/new/backup-directory \\",
    "    --auth-stdin \\",
    "    --expect-current-sha256 <64 lowercase hex> \\",
    "    --expect-next-sha256 <64 lowercase hex> \\",
    "    --expect-previous-sha256 <64 lowercase hex>",
    "",
  ].join("\n");
}

export async function runCli(argv) {
  const args = parseArgs(argv);
  if (args.help) {
    process.stdout.write(usage());
    return;
  }
  const auth = parseRecoveryAuthInput(readFileSync(0, "utf8"));
  const canonicalOutput = canonicalNewOutputPath(args["--output"]).output;
  const manifest = await runRemoteRecoveryExport({
    baseUrl: args["--remote-url"],
    outputRoot: canonicalOutput,
    sessionCookie: auth.sessionCookie,
    csrfToken: auth.csrfToken,
    expected: {
      current: args["--expect-current-sha256"],
      next: args["--expect-next-sha256"],
      previous: args["--expect-previous-sha256"],
    },
  });
  process.stdout.write(`${JSON.stringify({
    ok: true,
    output: canonicalOutput,
    candidateCount: manifest.candidateCount,
    uniqueReferencedAssets: manifest.uniqueReferencedAssets,
  })}\n`);
}

const invoked = process.argv[1] &&
  import.meta.url === pathToFileURL(resolve(process.argv[1])).href;
if (invoked) {
  try { await runCli(process.argv.slice(2)); }
  catch (error) {
    process.stderr.write(`Remote TF album recovery export refused: ${
      error instanceof Error ? error.message : "unknown error"}\n`);
    process.exitCode = 1;
  }
}
