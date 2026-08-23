#!/usr/bin/env node

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  closeSync,
  constants,
  existsSync,
  fstatSync,
  fsyncSync,
  lstatSync,
  mkdirSync,
  openSync,
  readSync,
  realpathSync,
  renameSync,
  statSync,
  writeSync,
} from "node:fs";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from "node:path";
import { pathToFileURL } from "node:url";

const MAXIMUM_INDEX_BYTES = 64 * 1024;
const MAXIMUM_ASSET_BYTES = 1_500_000;
const MAXIMUM_ASSETS = 96;
const HASH_PATTERN = /^[0-9a-f]{64}$/;
const SLOT_DEFINITIONS = Object.freeze([
  Object.freeze({ slot: "current", name: "index.json" }),
  Object.freeze({ slot: "next", name: "index.next" }),
  Object.freeze({ slot: "previous", name: "index.prev" }),
]);

function fail(message) {
  throw new Error(message);
}

function inside(parent, child) {
  const offset = relative(parent, child);
  return offset === "" || (!offset.startsWith(`..${sep}`) && offset !== "..");
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

function identityOf(value) {
  return Object.freeze({ dev: value.dev, ino: value.ino });
}

function hasIdentity(value, identity) {
  return value.dev === identity.dev && value.ino === identity.ino;
}

// Node does not expose openat(2), so path-based writes retain an irreducible
// race. Bind every observable identity we do have and repeat this check at
// each durable write boundary. Once a changed binding is observed, it cannot
// receive a complete manifest (or even the best-effort INCOMPLETE marker).
function verifyCreatedOutputBinding(output, binding, createdIdentity) {
  if (output !== binding.output || dirname(output) !== binding.parent)
    fail("created output no longer matches its CLI binding");

  const source = realpathSync(binding.source);
  const sourceStatus = statSync(source);
  if (source !== binding.source || !sourceStatus.isDirectory() ||
      !hasIdentity(sourceStatus, binding.sourceIdentity))
    fail("source mount identity changed before destination write");

  const parent = realpathSync(dirname(output));
  const parentStatus = statSync(parent);
  if (parent !== binding.parent || !parentStatus.isDirectory() ||
      !hasIdentity(parentStatus, binding.parentIdentity))
    fail("output parent binding changed after directory creation");

  const target = realpathSync(output);
  if (target !== output || dirname(target) !== binding.parent ||
      !inside(binding.parent, target) || inside(binding.source, target))
    fail("created output resolved outside its bound parent");

  const leafStatus = lstatSync(output);
  const targetStatus = statSync(target);
  if (!leafStatus.isDirectory() || !targetStatus.isDirectory() ||
      leafStatus.isSymbolicLink() ||
      !hasIdentity(leafStatus, identityOf(targetStatus)))
    fail("created output is not one stable real directory");
  if (targetStatus.dev === sourceStatus.dev)
    fail("created output is on the TF source filesystem");

  const noFollow = constants.O_NOFOLLOW ?? 0;
  const directoryOnly = constants.O_DIRECTORY ?? 0;
  const descriptor = openSync(output,
    constants.O_RDONLY | noFollow | directoryOnly);
  try {
    const openedStatus = fstatSync(descriptor);
    if (!openedStatus.isDirectory() ||
        !hasIdentity(openedStatus, identityOf(targetStatus)) ||
        (createdIdentity && !hasIdentity(openedStatus, createdIdentity)))
      fail("created output directory identity changed");
  } finally {
    closeSync(descriptor);
  }

  if (realpathSync(output) !== target ||
      !hasIdentity(lstatSync(output), identityOf(targetStatus)))
    fail("created output directory identity changed");
  return identityOf(targetStatus);
}

function readOnlyFile(path, maximum = Number.MAX_SAFE_INTEGER) {
  const noFollow = constants.O_NOFOLLOW ?? 0;
  const descriptor = openSync(path, constants.O_RDONLY | noFollow);
  try {
    const before = fstatSync(descriptor);
    if (!before.isFile() || before.size < 0 || before.size > maximum)
      fail(`unsafe or oversized source file: ${path}`);
    const bytes = Buffer.alloc(before.size);
    let offset = 0;
    while (offset < bytes.length) {
      const count = readSync(descriptor, bytes, offset, bytes.length - offset, offset);
      if (count <= 0) fail(`short source read: ${path}`);
      offset += count;
    }
    const after = fstatSync(descriptor);
    if (after.size !== before.size || after.mtimeMs !== before.mtimeMs)
      fail(`source changed while reading: ${path}`);
    return bytes;
  } finally {
    closeSync(descriptor);
  }
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function requiredObject(value, label) {
  if (!value || typeof value !== "object" || Array.isArray(value))
    fail(`${label} is not an object`);
  return value;
}

export function parseIndex(bytes, label) {
  if (bytes.length === 0 || bytes.length > MAXIMUM_INDEX_BYTES)
    fail(`${label} has an invalid size`);
  let value;
  try {
    value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch {
    fail(`${label} is not valid UTF-8 JSON`);
  }
  requiredObject(value, label);
  if (value.schema !== 1 || typeof value.current !== "string" ||
      !Array.isArray(value.assets) || value.assets.length > MAXIMUM_ASSETS)
    fail(`${label} does not match album schema 1`);
  if (value.current !== "" && !HASH_PATTERN.test(value.current))
    fail(`${label} has an invalid current asset id`);

  const ids = new Set();
  const assets = [];
  for (let at = 0; at < value.assets.length; ++at) {
    const asset = requiredObject(value.assets[at], `${label} asset ${at}`);
    const content = asset.contentSha256 ?? asset.id;
    if (!HASH_PATTERN.test(asset.id) || !HASH_PATTERN.test(content) ||
        asset.path !== `/inkloop-album/${content}.png` ||
        !Number.isSafeInteger(asset.bytes) || asset.bytes < 45 ||
        asset.bytes > MAXIMUM_ASSET_BYTES || ids.has(asset.id))
      fail(`${label} asset ${at} violates the runtime path/size contract`);
    ids.add(asset.id);
    assets.push(Object.freeze({
      id: asset.id,
      contentSha256: content,
      path: asset.path,
      bytes: asset.bytes,
    }));
  }
  if (value.current !== "" && !ids.has(value.current))
    fail(`${label} current asset is absent`);
  return Object.freeze({ current: value.current, assets: Object.freeze(assets) });
}

function inspectCandidate(albumDirectory, definition, expectedDigest) {
  const path = join(albumDirectory, definition.name);
  if (!existsSync(path)) fail(`required ${definition.name} is missing`);
  const bytes = readOnlyFile(path, MAXIMUM_INDEX_BYTES);
  const digest = sha256(bytes);
  if (digest !== expectedDigest)
    fail(`${definition.name} digest changed: expected ${expectedDigest}, got ${digest}`);
  const parsed = parseIndex(bytes, definition.name);
  return Object.freeze({
    ...definition,
    sourcePath: path,
    bytes: bytes.length,
    sha256: digest,
    current: parsed.current,
    assets: parsed.assets,
  });
}

function inspectAsset(albumDirectory, contentSha256, expectedBytes, slots) {
  const path = join(albumDirectory, `${contentSha256}.png`);
  const bytes = readOnlyFile(path, MAXIMUM_ASSET_BYTES);
  if (bytes.length !== expectedBytes)
    fail(`asset ${contentSha256} size differs from its indexes`);
  const digest = sha256(bytes);
  if (digest !== contentSha256)
    fail(`asset ${contentSha256} content digest does not match its filename`);
  return Object.freeze({
    sourcePath: path,
    contentSha256,
    bytes: bytes.length,
    sha256: digest,
    referencedBy: Object.freeze([...slots].sort()),
  });
}

// Pure read-only inspection seam. The CLI performs mount/device safety checks
// before invoking it; tests can exercise the format contract using fixtures.
export function inspectAlbumRecovery(sourceRoot, expectedDigests) {
  const root = realpathSync(sourceRoot);
  const albumDirectory = realpathSync(join(root, "inkloop-album"));
  if (!inside(root, albumDirectory) || !lstatSync(albumDirectory).isDirectory())
    fail("inkloop-album is not a real directory inside the source root");
  const candidates = SLOT_DEFINITIONS.map((definition) => {
    const expected = expectedDigests?.[definition.slot];
    if (!HASH_PATTERN.test(expected ?? ""))
      fail(`expected ${definition.slot} SHA-256 is required`);
    return inspectCandidate(albumDirectory, definition, expected);
  });
  if (new Set(candidates.map((candidate) => candidate.sha256)).size !== 3)
    fail("the three candidate indexes are not divergent");

  const references = new Map();
  for (const candidate of candidates) {
    for (const asset of candidate.assets) {
      const existing = references.get(asset.contentSha256);
      if (existing && existing.bytes !== asset.bytes)
        fail(`candidate indexes disagree on asset ${asset.contentSha256} size`);
      const entry = existing ?? { bytes: asset.bytes, slots: new Set() };
      entry.slots.add(candidate.slot);
      references.set(asset.contentSha256, entry);
    }
  }
  const assets = [...references.entries()]
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([content, reference]) =>
      inspectAsset(albumDirectory, content, reference.bytes, reference.slots));
  return Object.freeze({
    sourceRoot: root,
    albumDirectory,
    candidates: Object.freeze(candidates),
    assets: Object.freeze(assets),
  });
}

function writeExclusive(path, bytes, verifyDestination) {
  verifyDestination?.();
  const partial = `${path}.partial`;
  const descriptor = openSync(partial, constants.O_WRONLY | constants.O_CREAT |
    constants.O_EXCL, 0o600);
  try {
    let offset = 0;
    while (offset < bytes.length) {
      const count = writeSync(descriptor, bytes, offset, bytes.length - offset);
      if (count <= 0) fail(`short destination write: ${path}`);
      offset += count;
    }
    fsyncSync(descriptor);
  } finally {
    closeSync(descriptor);
  }
  verifyDestination?.();
  renameSync(partial, path);
  verifyDestination?.();
  const directory = openSync(dirname(path), constants.O_RDONLY);
  try { fsyncSync(directory); }
  finally { closeSync(directory); }
}

function copyVerified(source, destination, expectedBytes, expectedDigest,
                      verifyDestination) {
  const bytes = readOnlyFile(source, expectedBytes);
  if (bytes.length !== expectedBytes || sha256(bytes) !== expectedDigest)
    fail(`source changed before export: ${source}`);
  writeExclusive(destination, bytes, verifyDestination);
  const copied = readOnlyFile(destination, expectedBytes);
  if (copied.length !== expectedBytes || sha256(copied) !== expectedDigest)
    fail(`destination verification failed: ${destination}`);
}

function durableJson(path, value, verifyDestination) {
  writeExclusive(path, Buffer.from(`${JSON.stringify(value, null, 2)}\n`, "utf8"),
    verifyDestination);
}

function snapshotIdentity(plan) {
  return {
    candidates: Object.fromEntries(plan.candidates.map((value) => [
      value.slot, value.sha256,
    ])),
    assets: Object.fromEntries(plan.assets.map((value) => [
      value.contentSha256, { bytes: value.bytes, sha256: value.sha256 },
    ])),
  };
}

// Writes only to a new destination. It re-inspects the source before and after
// copying, so a changed card/image never receives a "complete" manifest.
export function exportInspectedAlbumRecovery(plan, outputRoot,
                                             outputBinding = undefined) {
  const output = resolve(outputRoot);
  if (outputBinding && output !== outputBinding.output)
    fail("output does not match its CLI safety binding");
  if (existsSync(output)) fail("output directory already exists");
  const expected = snapshotIdentity(plan);
  const expectedDigests = expected.candidates;
  const before = inspectAlbumRecovery(plan.sourceRoot, expectedDigests);
  if (JSON.stringify(snapshotIdentity(before)) !== JSON.stringify(expected))
    fail("source changed after initial inspection");

  try {
    mkdirSync(output, { mode: 0o700 });
  } catch (error) {
    if (error?.code === "EEXIST") fail("output directory already exists");
    throw error;
  }
  const createdIdentity = outputBinding
    ? verifyCreatedOutputBinding(output, outputBinding) : undefined;
  const verifyDestination = outputBinding
    ? () => verifyCreatedOutputBinding(output, outputBinding, createdIdentity)
    : undefined;
  try {
    const outputParent = openSync(dirname(output), constants.O_RDONLY);
    try { fsyncSync(outputParent); }
    finally { closeSync(outputParent); }
    const candidateDirectory = join(output, "candidates");
    const assetDirectory = join(output, "assets");
    mkdirSync(candidateDirectory, { mode: 0o700 });
    mkdirSync(assetDirectory, { mode: 0o700 });
    const outputDescriptor = openSync(output, constants.O_RDONLY);
    try { fsyncSync(outputDescriptor); }
    finally { closeSync(outputDescriptor); }
    for (const candidate of before.candidates) {
      copyVerified(candidate.sourcePath, join(candidateDirectory, candidate.name),
        candidate.bytes, candidate.sha256, verifyDestination);
    }
    for (const asset of before.assets) {
      copyVerified(asset.sourcePath,
        join(assetDirectory, `${asset.contentSha256}.png`),
        asset.bytes, asset.sha256, verifyDestination);
    }

    const after = inspectAlbumRecovery(plan.sourceRoot, expectedDigests);
    if (JSON.stringify(snapshotIdentity(after)) !== JSON.stringify(expected))
      fail("source changed during export");
    const files = [
      ...before.candidates.map((value) => ({
        path: `candidates/${value.name}`,
        bytes: value.bytes,
        sha256: value.sha256,
      })),
      ...before.assets.map((value) => ({
        path: `assets/${value.contentSha256}.png`,
        bytes: value.bytes,
        sha256: value.sha256,
      })),
    ].sort((left, right) => left.path.localeCompare(right.path));
    const manifest = {
      schema: 1,
      complete: true,
      readOnlySource: true,
      createdAt: new Date().toISOString(),
      candidateCount: 3,
      candidates: before.candidates.map((value) => ({
        slot: value.slot,
        file: `candidates/${value.name}`,
        bytes: value.bytes,
        sha256: value.sha256,
        assetEntries: value.assets.length,
        current: value.current,
      })),
      uniqueReferencedAssets: before.assets.length,
      assets: before.assets.map((value) => ({
        file: `assets/${value.contentSha256}.png`,
        bytes: value.bytes,
        sha256: value.sha256,
        referencedBy: value.referencedBy,
      })),
      files,
    };
    writeExclusive(join(output, "SHA256SUMS"), Buffer.from(
      `${files.map((value) => `${value.sha256}  ${value.path}`).join("\n")}\n`,
      "utf8"), verifyDestination);
    // The complete manifest is the final durable write. A power cut or any
    // earlier failure therefore cannot leave a complete-looking export.
    durableJson(join(output, "manifest.json"), manifest, verifyDestination);
    return manifest;
  } catch (error) {
    try {
      verifyDestination?.();
      durableJson(join(output, "INCOMPLETE.json"), {
        schema: 1,
        complete: false,
        error: error instanceof Error ? error.message : "unknown export failure",
      }, verifyDestination);
    } catch {
      // Never follow a changed output binding merely to record the failure.
    }
    throw error;
  }
}

export function parseMountTable(text) {
  const rows = [];
  for (const line of text.split(/\r?\n/)) {
    const optionsAt = line.lastIndexOf(" (");
    const onAt = line.indexOf(" on ");
    if (onAt <= 0 || optionsAt <= onAt + 4 || !line.endsWith(")")) continue;
    const mounted = line.slice(onAt + 4, optionsAt);
    const typeAt = mounted.lastIndexOf(" type ");
    rows.push({
      device: line.slice(0, onAt),
      mountPoint: typeAt > 0 ? mounted.slice(0, typeAt) : mounted,
      options: line.slice(optionsAt + 2, -1)
        .split(",").map((value) => value.trim()).filter(Boolean),
    });
  }
  return rows;
}

function mountedSource(sourceRoot, expectedDevice) {
  let table;
  for (const command of ["/sbin/mount", "/bin/mount", "mount"]) {
    try {
      table = execFileSync(command, [], { encoding: "utf8" });
      break;
    } catch {
      // Try the next fixed command location.
    }
  }
  if (table === undefined) fail("cannot inspect the system mount table");
  const source = realpathSync(sourceRoot);
  const row = parseMountTable(table).find((candidate) => {
    try { return realpathSync(candidate.mountPoint) === source; }
    catch { return false; }
  });
  if (!row) fail("source must be the exact root of a mounted filesystem");
  if (row.device !== expectedDevice)
    fail(`mounted device mismatch: expected ${expectedDevice}, got ${row.device}`);
  if (!row.options.includes("ro") && !row.options.includes("read-only"))
    fail("source filesystem is not mounted read-only");
  return { ...row, mountPoint: source };
}

function parseArgs(argv) {
  if (argv.includes("--help")) return { help: true };
  const allowed = new Set([
    "--source", "--output", "--expect-device", "--expect-current-sha256",
    "--expect-next-sha256", "--expect-previous-sha256",
  ]);
  const values = {};
  for (let at = 0; at < argv.length; at += 2) {
    const name = argv[at];
    const value = argv[at + 1];
    if (!allowed.has(name) || value === undefined || value.startsWith("--"))
      fail(`invalid argument: ${name ?? "<missing>"}`);
    if (values[name] !== undefined) fail(`duplicate argument: ${name}`);
    values[name] = value;
  }
  for (const name of allowed)
    if (values[name] === undefined) fail(`missing required argument: ${name}`);
  return values;
}

function usage() {
  return [
    "Usage:",
    "  node firmware/inkloop-idf/tools/export_tf_album_recovery.mjs \\",
    "    --source /absolute/read-only/tf-mount \\",
    "    --output /absolute/new/backup-directory \\",
    "    --expect-device /dev/exact-mounted-partition \\",
    "    --expect-current-sha256 <64 lowercase hex> \\",
    "    --expect-next-sha256 <64 lowercase hex> \\",
    "    --expect-previous-sha256 <64 lowercase hex>",
    "",
  ].join("\n");
}

export function runCli(argv) {
  const args = parseArgs(argv);
  if (args.help) {
    process.stdout.write(usage());
    return;
  }
  const source = args["--source"];
  const requestedOutput = args["--output"];
  if (!isAbsolute(source) || !isAbsolute(requestedOutput))
    fail("source and output must be absolute paths");
  const { output, parent: outputParent } =
    canonicalNewOutputPath(requestedOutput);
  if (existsSync(output)) fail("output directory already exists");
  const mount = mountedSource(source, args["--expect-device"]);
  const sourceStatus = statSync(mount.mountPoint);
  const outputParentStatus = statSync(outputParent);
  if (!sourceStatus.isDirectory() || !outputParentStatus.isDirectory())
    fail("source mount and output parent must be directories");
  if (inside(mount.mountPoint, outputParent) ||
      sourceStatus.dev === outputParentStatus.dev)
    fail("output must be on a different filesystem from the TF source");
  const outputBinding = Object.freeze({
    source: mount.mountPoint,
    sourceIdentity: identityOf(sourceStatus),
    output,
    parent: outputParent,
    parentIdentity: identityOf(outputParentStatus),
  });
  const expected = {
    current: args["--expect-current-sha256"],
    next: args["--expect-next-sha256"],
    previous: args["--expect-previous-sha256"],
  };
  const plan = inspectAlbumRecovery(mount.mountPoint, expected);
  const manifest = exportInspectedAlbumRecovery(plan, output, outputBinding);
  process.stdout.write(`${JSON.stringify({
    ok: true,
    output,
    candidateCount: manifest.candidateCount,
    uniqueReferencedAssets: manifest.uniqueReferencedAssets,
  })}\n`);
}

const invoked = process.argv[1] &&
  import.meta.url === pathToFileURL(resolve(process.argv[1])).href;
if (invoked) {
  try {
    runCli(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`TF album recovery export refused: ${
      error instanceof Error ? error.message : "unknown error"}\n`);
    process.exitCode = 1;
  }
}
