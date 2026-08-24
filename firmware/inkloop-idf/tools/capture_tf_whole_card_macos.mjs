#!/usr/bin/env node

// Fail-closed, macOS-only custody capture for a physically removed TF card.
//
// The tool has two explicit phases. `inspect` reads diskutil metadata only and
// prints a confirmation fingerprint. `capture` requires that fingerprint and
// the exact byte size, refuses mounted/internal/system/virtual media, then
// reads the raw device twice. It never unmounts, ejects, formats, repairs or
// writes the source device, and it never invokes sudo.

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  closeSync,
  constants,
  createReadStream,
  existsSync,
  fstatSync,
  fsyncSync,
  lstatSync,
  mkdirSync,
  openSync,
  readSync,
  realpathSync,
  renameSync,
  statfsSync,
  statSync,
  writeSync,
} from "node:fs";
import { homedir } from "node:os";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const DISK_PATTERN = /^\/dev\/disk(0|[1-9][0-9]*)$/;
const IDENTIFIER_PATTERN = /^disk(0|[1-9][0-9]*)(?:s[0-9]+)*$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;
// Match the physical staging gate so the operator cannot finish an otherwise
// valid custody capture only to discover that the release gate rejects its
// capacity. Cards below 64 MiB are outside the supported PaperColor TF range.
const MINIMUM_CARD_BYTES = 64 * 1024 * 1024;
const MAXIMUM_CARD_BYTES = 4 * 1024 * 1024 * 1024 * 1024;
const COPY_CHUNK_BYTES = 4 * 1024 * 1024;
const REPOSITORY_ROOT = realpathSync(resolve(
  dirname(fileURLToPath(import.meta.url)), "../../..",
));

function fail(message) {
  throw new Error(message);
}

function inside(parent, child) {
  const offset = relative(parent, child);
  return offset === "" || (!offset.startsWith(`..${sep}`) && offset !== "..");
}

function canonicalJson(value) {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.keys(value).sort().map((key) =>
      `${JSON.stringify(key)}:${canonicalJson(value[key])}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

function sha256Bytes(value) {
  return createHash("sha256").update(value).digest("hex");
}

function safeString(value) {
  return typeof value === "string" && value.length > 0 ? value : null;
}

function safeBoolean(value) {
  return typeof value === "boolean" ? value : null;
}

function safeInteger(value) {
  return Number.isSafeInteger(value) ? value : null;
}

export function parseWholeDiskPath(value) {
  const match = DISK_PATTERN.exec(value ?? "");
  if (!match)
    fail("disk must be one explicit whole-device path like /dev/disk4");
  const identifier = `disk${match[1]}`;
  return Object.freeze({
    blockPath: `/dev/${identifier}`,
    rawPath: `/dev/r${identifier}`,
    identifier,
  });
}

function wholeIdentifier(value) {
  const match = /^(disk(?:0|[1-9][0-9]*))/.exec(value ?? "");
  return match?.[1] ?? null;
}

function collectDeviceIdentifiers(value, output = new Set()) {
  if (Array.isArray(value)) {
    for (const item of value) collectDeviceIdentifiers(item, output);
    return output;
  }
  if (!value || typeof value !== "object") return output;
  for (const [key, item] of Object.entries(value)) {
    if ((key === "DeviceIdentifier" || key === "ParentWholeDisk" ||
         key === "APFSPhysicalStore" || key === "ContainerReference") &&
        typeof item === "string" && IDENTIFIER_PATTERN.test(item))
      output.add(item);
    collectDeviceIdentifiers(item, output);
  }
  return output;
}

function normalizedMember(info) {
  return Object.freeze({
    deviceIdentifier: safeString(info.DeviceIdentifier),
    parentWholeDisk: safeString(info.ParentWholeDisk),
    totalSize: safeInteger(info.TotalSize),
    content: safeString(info.Content),
    filesystemType: safeString(info.FilesystemType),
    partitionUUID: safeString(info.PartitionUUID),
    volumeUUID: safeString(info.VolumeUUID),
    diskUUID: safeString(info.DiskUUID),
    mediaUUID: safeString(info.MediaUUID),
    mounted: safeBoolean(info.Mounted),
    mountPoint: safeString(info.MountPoint),
    volumeRoles: Array.isArray(info.APFSVolumeRole)
      ? [...info.APFSVolumeRole].filter((entry) => typeof entry === "string").sort()
      : safeString(info.APFSVolumeRole),
  });
}

function normalizedIdentity(targetInfo, memberInfos, disk) {
  const members = memberInfos.map(normalizedMember)
    .sort((left, right) =>
      (left.deviceIdentifier ?? "").localeCompare(right.deviceIdentifier ?? ""));
  return Object.freeze({
    deviceIdentifier: disk.identifier,
    deviceNode: disk.blockPath,
    totalSize: safeInteger(targetInfo.TotalSize),
    deviceBlockSize: safeInteger(targetInfo.DeviceBlockSize),
    mediaName: safeString(targetInfo.MediaName),
    mediaUUID: safeString(targetInfo.MediaUUID),
    diskUUID: safeString(targetInfo.DiskUUID),
    deviceTreePath: safeString(targetInfo.DeviceTreePath),
    ioRegistryEntryName: safeString(targetInfo.IORegistryEntryName),
    busProtocol: safeString(targetInfo.BusProtocol),
    mediaType: safeString(targetInfo.MediaType),
    removableMedia: safeBoolean(targetInfo.RemovableMedia),
    ejectable: safeBoolean(targetInfo.Ejectable),
    internal: safeBoolean(targetInfo.Internal),
    virtualOrPhysical: safeString(targetInfo.VirtualOrPhysical),
    members,
  });
}

function hasSystemRole(member) {
  const roles = Array.isArray(member.volumeRoles)
    ? member.volumeRoles : [member.volumeRoles];
  return roles.some((role) =>
    ["System", "Data", "Preboot", "Recovery", "VM"].includes(role));
}

export function validateDiskSnapshot(snapshot, requestedDisk) {
  const disk = typeof requestedDisk === "string"
    ? parseWholeDiskPath(requestedDisk) : requestedDisk;
  const targetInfo = snapshot?.targetInfo;
  const targetList = snapshot?.targetList;
  const memberInfos = snapshot?.memberInfos;
  const rootInfos = snapshot?.rootInfos;
  if (!targetInfo || !targetList || !Array.isArray(memberInfos) ||
      !Array.isArray(rootInfos) || rootInfos.length === 0)
    fail("diskutil snapshot is incomplete");
  if (targetInfo.DeviceIdentifier !== disk.identifier ||
      targetInfo.DeviceNode !== disk.blockPath || targetInfo.Whole !== true)
    fail("diskutil did not resolve the exact requested whole disk");
  if (targetInfo.Internal !== false)
    fail("internal or indeterminate media is forbidden");
  if (targetInfo.VirtualOrPhysical !== "Physical")
    fail("virtual or indeterminate media is forbidden");
  if (targetInfo.RemovableMedia !== true && targetInfo.Ejectable !== true)
    fail("target is not positively identified as removable/ejectable media");
  if (!Number.isSafeInteger(targetInfo.TotalSize) ||
      targetInfo.TotalSize < MINIMUM_CARD_BYTES ||
      targetInfo.TotalSize > MAXIMUM_CARD_BYTES)
    fail("target capacity is missing or outside the approved TF range");
  if (!Number.isSafeInteger(targetInfo.DeviceBlockSize) ||
      ![512, 1024, 2048, 4096].includes(targetInfo.DeviceBlockSize))
    fail("target block size is missing or unsupported");

  const listed = [...collectDeviceIdentifiers(targetList)]
    .filter((identifier) => wholeIdentifier(identifier) === disk.identifier);
  if (!listed.includes(disk.identifier))
    fail("diskutil list does not contain the requested whole disk");
  const memberIdentifiers = new Set(memberInfos.map((info) => info.DeviceIdentifier));
  for (const identifier of listed) {
    if (!memberIdentifiers.has(identifier))
      fail(`missing diskutil info for target member ${identifier}`);
  }
  for (const info of memberInfos) {
    if (!IDENTIFIER_PATTERN.test(info.DeviceIdentifier ?? "") ||
        wholeIdentifier(info.DeviceIdentifier) !== disk.identifier)
      fail("diskutil member info escaped the requested whole disk");
    if (info.Mounted !== false || safeString(info.MountPoint))
      fail(`target member ${info.DeviceIdentifier} is mounted or mount state is indeterminate; unmount it manually first`);
  }

  const rootWholeDisks = new Set();
  for (const info of rootInfos) {
    for (const identifier of collectDeviceIdentifiers(info)) {
      const whole = wholeIdentifier(identifier);
      if (whole) rootWholeDisks.add(whole);
    }
  }
  if (rootWholeDisks.has(disk.identifier))
    fail("boot/system disk is forbidden");

  const normalizedMembers = memberInfos.map(normalizedMember);
  if (normalizedMembers.some((member) =>
    /Apple_APFS|Apple_CoreStorage/i.test(member.content ?? "") ||
    hasSystemRole(member)))
    fail("APFS/CoreStorage/system-role media is forbidden for TF custody capture");

  const identity = normalizedIdentity(targetInfo, memberInfos, disk);
  if (!identity.mediaName || !identity.busProtocol ||
      !(identity.deviceTreePath || identity.ioRegistryEntryName ||
        identity.mediaUUID || identity.diskUUID))
    fail("diskutil lacks enough stable media identity fields");
  const fingerprint = sha256Bytes(Buffer.from(canonicalJson(identity), "utf8"));
  return Object.freeze({ disk, identity, fingerprint, totalSize: targetInfo.TotalSize });
}

function plist(command, args, input = undefined) {
  const bytes = execFileSync(command, args, {
    encoding: null,
    input,
    maxBuffer: 16 * 1024 * 1024,
    stdio: [input === undefined ? "ignore" : "pipe", "pipe", "pipe"],
  });
  return bytes;
}

function parsePlist(bytes) {
  const json = plist("/usr/bin/plutil", ["-convert", "json", "-o", "-", "-"], bytes);
  return JSON.parse(json.toString("utf8"));
}

function diskutilPlist(args) {
  return parsePlist(plist("/usr/sbin/diskutil", args));
}

function productionSnapshot(disk) {
  const targetInfo = diskutilPlist(["info", "-plist", disk.blockPath]);
  const targetList = diskutilPlist(["list", "-plist", disk.blockPath]);
  const listed = [...collectDeviceIdentifiers(targetList)]
    .filter((identifier) => wholeIdentifier(identifier) === disk.identifier)
    .sort();
  const memberInfos = listed.map((identifier) =>
    diskutilPlist(["info", "-plist", `/dev/${identifier}`]));
  // Both roots are required on the supported macOS target. This intentionally
  // fails closed rather than guessing around an unfamiliar boot layout.
  const rootInfos = [
    diskutilPlist(["info", "-plist", "/"]),
    diskutilPlist(["info", "-plist", "/System/Volumes/Data"]),
  ];
  return Object.freeze({ targetInfo, targetList, memberInfos, rootInfos });
}

function canonicalNewOutput(value) {
  if (!isAbsolute(value)) fail("output must be an absolute path");
  const requested = resolve(value);
  const leaf = basename(requested);
  const parent = realpathSync(dirname(requested));
  const output = join(parent, leaf);
  if (!leaf || output === parent || parent === "/" || output === homedir())
    fail("output must be a narrowly scoped new child directory");
  if (existsSync(output)) fail("output directory already exists");
  if (inside(REPOSITORY_ROOT, output))
    fail("whole-card evidence must be stored outside the repository");
  const parentStatus = statSync(parent);
  if (!parentStatus.isDirectory() || lstatSync(parent).isSymbolicLink())
    fail("output parent must resolve to a real directory");
  return Object.freeze({ output, parent, parentIdentity: {
    dev: parentStatus.dev, ino: parentStatus.ino,
  } });
}

function verifyOutputBinding(binding, createdIdentity = undefined) {
  const parent = realpathSync(dirname(binding.output));
  const parentStatus = statSync(parent);
  if (parent !== binding.parent || parentStatus.dev !== binding.parentIdentity.dev ||
      parentStatus.ino !== binding.parentIdentity.ino)
    fail("output parent identity changed");
  if (!existsSync(binding.output)) return null;
  const leaf = lstatSync(binding.output);
  if (!leaf.isDirectory() || leaf.isSymbolicLink())
    fail("output binding is no longer a real directory");
  const target = realpathSync(binding.output);
  if (target !== binding.output || dirname(target) !== binding.parent ||
      !inside(binding.parent, target) || inside(REPOSITORY_ROOT, target))
    fail("output binding escaped its approved parent");
  if (createdIdentity && (leaf.dev !== createdIdentity.dev || leaf.ino !== createdIdentity.ino))
    fail("output directory identity changed");
  return Object.freeze({ dev: leaf.dev, ino: leaf.ino });
}

function writeAll(descriptor, bytes) {
  let offset = 0;
  while (offset < bytes.length) {
    const count = writeSync(descriptor, bytes, offset, bytes.length - offset);
    if (count <= 0) fail("short destination write");
    offset += count;
  }
}

function durableReplace(path, bytes, verify) {
  verify();
  const next = `${path}.next`;
  const descriptor = openSync(next, constants.O_WRONLY | constants.O_CREAT |
    constants.O_EXCL | (constants.O_NOFOLLOW ?? 0), 0o600);
  try {
    writeAll(descriptor, bytes);
    fsyncSync(descriptor);
  } finally {
    closeSync(descriptor);
  }
  verify();
  renameSync(next, path);
  const directory = openSync(dirname(path), constants.O_RDONLY |
    (constants.O_DIRECTORY ?? 0));
  try { fsyncSync(directory); }
  finally { closeSync(directory); }
  verify();
}

function durableJson(path, value, verify) {
  durableReplace(path, Buffer.from(`${JSON.stringify(value, null, 2)}\n`, "utf8"), verify);
}

function productionOpenRaw(rawPath) {
  const descriptor = openSync(rawPath, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  const status = fstatSync(descriptor);
  if (!status.isCharacterDevice() && !status.isBlockDevice()) {
    closeSync(descriptor);
    fail("raw source is not a character/block device");
  }
  return createReadStream(null, {
    fd: descriptor,
    autoClose: true,
    highWaterMark: COPY_CHUNK_BYTES,
  });
}

async function readRawPass({ rawPath, expectedBytes, imagePath, openRaw, verify }) {
  verify?.();
  const source = openRaw(rawPath);
  const imageDescriptor = imagePath === undefined ? null : openSync(
    imagePath,
    constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL |
      (constants.O_NOFOLLOW ?? 0),
    0o600,
  );
  const digest = createHash("sha256");
  let bytes = 0;
  try {
    for await (const value of source) {
      const chunk = Buffer.isBuffer(value) ? value : Buffer.from(value);
      bytes += chunk.length;
      if (bytes > expectedBytes) fail("raw source exceeded diskutil capacity");
      digest.update(chunk);
      if (imageDescriptor !== null) writeAll(imageDescriptor, chunk);
    }
    if (bytes !== expectedBytes)
      fail(`raw source length mismatch: expected ${expectedBytes}, got ${bytes}`);
    if (imageDescriptor !== null) fsyncSync(imageDescriptor);
  } finally {
    if (imageDescriptor !== null) closeSync(imageDescriptor);
    if (!source.destroyed) source.destroy();
  }
  verify?.();
  return Object.freeze({ bytes, sha256: digest.digest("hex") });
}

function hashImage(path, expectedBytes) {
  const descriptor = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  const digest = createHash("sha256");
  const buffer = Buffer.allocUnsafe(COPY_CHUNK_BYTES);
  let bytes = 0;
  try {
    const before = fstatSync(descriptor);
    if (!before.isFile() || before.size !== expectedBytes)
      fail("captured image is missing, unsafe or has the wrong size");
    while (bytes < expectedBytes) {
      const wanted = Math.min(buffer.length, expectedBytes - bytes);
      const count = readSync(descriptor, buffer, 0, wanted, bytes);
      if (count <= 0) fail("captured image changed while hashing");
      digest.update(buffer.subarray(0, count));
      bytes += count;
    }
    if (readSync(descriptor, buffer, 0, 1, bytes) !== 0)
      fail("captured image grew while hashing");
    const after = fstatSync(descriptor);
    if (after.dev !== before.dev || after.ino !== before.ino ||
        after.size !== before.size || after.mtimeMs !== before.mtimeMs)
      fail("captured image changed while hashing");
  } finally {
    closeSync(descriptor);
  }
  return digest.digest("hex");
}

function sameCaptureIdentity(left, right) {
  return left.totalSize === right.totalSize && left.fingerprint === right.fingerprint;
}

function ensureCapacity(parent, requiredBytes, statfs = statfsSync) {
  const status = statfs(parent);
  const available = Number(status.bavail) * Number(status.bsize);
  const margin = Math.max(64 * 1024 * 1024, Math.ceil(requiredBytes / 100));
  if (!Number.isSafeInteger(available) || available < requiredBytes + margin)
    fail("output filesystem does not have image size plus safety margin available");
  return Object.freeze({ availableBytes: available, requiredBytes, safetyMarginBytes: margin });
}

function parseArgs(argv) {
  if (argv.includes("--help")) return { help: true };
  const allowed = new Set([
    "--mode", "--disk", "--output", "--expect-size-bytes", "--confirm-fingerprint",
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
  for (const required of ["--mode", "--disk", "--output"])
    if (values[required] === undefined) fail(`missing required argument: ${required}`);
  if (!["inspect", "capture"].includes(values["--mode"]))
    fail("mode must be inspect or capture");
  if (values["--mode"] === "inspect" &&
      (values["--expect-size-bytes"] !== undefined ||
       values["--confirm-fingerprint"] !== undefined))
    fail("inspect mode does not accept capture confirmations");
  if (values["--mode"] === "capture") {
    for (const required of ["--expect-size-bytes", "--confirm-fingerprint"])
      if (values[required] === undefined) fail(`missing required argument: ${required}`);
    if (!/^[1-9][0-9]*$/.test(values["--expect-size-bytes"]))
      fail("expected size must be an exact base-10 byte count");
    if (!SHA256_PATTERN.test(values["--confirm-fingerprint"]))
      fail("confirmation fingerprint must be 64 lowercase hex characters");
  }
  return values;
}

function usage() {
  return [
    "Inspect (metadata only; card must already be manually unmounted):",
    "  node firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs \\",
    "    --mode inspect --disk /dev/diskN \\",
    "    --output /absolute/new/directory-outside-repository",
    "",
    "Capture (copy exact values printed by inspect):",
    "  node firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs \\",
    "    --mode capture --disk /dev/diskN \\",
    "    --output /absolute/new/directory-outside-repository \\",
    "    --expect-size-bytes <exact-decimal-bytes> \\",
    "    --confirm-fingerprint <64-lowercase-hex>",
    "",
  ].join("\n");
}

export async function runCli(argv, runtime = {}) {
  const args = parseArgs(argv);
  if (args.help) {
    (runtime.stdout ?? process.stdout).write(usage());
    return null;
  }
  if ((runtime.platform ?? process.platform) !== "darwin")
    fail("whole-card custody capture is supported only on macOS");
  const disk = parseWholeDiskPath(args["--disk"]);
  const binding = canonicalNewOutput(args["--output"]);
  const takeSnapshot = runtime.takeSnapshot ?? productionSnapshot;
  const beforeRaw = takeSnapshot(disk);
  const before = validateDiskSnapshot(beforeRaw, disk);
  const stdout = runtime.stdout ?? process.stdout;
  if (args["--mode"] === "inspect") {
    const result = {
      ok: true,
      mode: "inspect",
      noRawDeviceRead: true,
      disk: disk.blockPath,
      rawDeviceForCapture: disk.rawPath,
      output: binding.output,
      expectSizeBytes: before.totalSize,
      confirmFingerprint: before.fingerprint,
      identity: before.identity,
      operatorAction: "Manually verify the media identity and copy both confirmation values into capture mode.",
    };
    stdout.write(`${JSON.stringify(result, null, 2)}\n`);
    return result;
  }

  const expectedSize = Number(args["--expect-size-bytes"]);
  if (!Number.isSafeInteger(expectedSize) || expectedSize !== before.totalSize)
    fail("confirmed byte size does not match the current diskutil snapshot");
  if (args["--confirm-fingerprint"] !== before.fingerprint)
    fail("confirmed fingerprint does not match the current diskutil snapshot");
  const capacity = ensureCapacity(binding.parent, expectedSize,
    runtime.statfs ?? statfsSync);

  mkdirSync(binding.output, { mode: 0o700 });
  const createdIdentity = verifyOutputBinding(binding);
  const verify = () => verifyOutputBinding(binding, createdIdentity);
  const startedAt = (runtime.now ?? (() => new Date()))().toISOString();
  const custodyPath = join(binding.output, "custody.json");
  durableJson(custodyPath, {
    schema: 1,
    complete: false,
    startedAt,
    disk: disk.blockPath,
    expectedBytes: expectedSize,
    confirmedFingerprint: before.fingerprint,
  }, verify);
  try {
    durableJson(join(binding.output, "diskutil-info-before.json"),
      beforeRaw.targetInfo, verify);
    durableJson(join(binding.output, "diskutil-list-before.json"),
      beforeRaw.targetList, verify);
    durableJson(join(binding.output, "diskutil-members-before.json"),
      beforeRaw.memberInfos, verify);

    const preReadRaw = takeSnapshot(disk);
    const preRead = validateDiskSnapshot(preReadRaw, disk);
    if (!sameCaptureIdentity(before, preRead))
      fail("source identity changed before the image read");
    durableJson(join(binding.output, "diskutil-info-pre-read.json"),
      preReadRaw.targetInfo, verify);
    durableJson(join(binding.output, "diskutil-members-pre-read.json"),
      preReadRaw.memberInfos, verify);

    const partialImage = join(binding.output, "tf-whole-card.img.partial");
    const finalImage = join(binding.output, "tf-whole-card.img");
    const first = await readRawPass({
      rawPath: disk.rawPath,
      expectedBytes: expectedSize,
      imagePath: partialImage,
      openRaw: runtime.openRaw ?? productionOpenRaw,
      verify,
    });
    verify();
    renameSync(partialImage, finalImage);
    const outputDirectory = openSync(binding.output, constants.O_RDONLY |
      (constants.O_DIRECTORY ?? 0));
    try { fsyncSync(outputDirectory); }
    finally { closeSync(outputDirectory); }
    verify();

    const betweenRaw = takeSnapshot(disk);
    const between = validateDiskSnapshot(betweenRaw, disk);
    if (!sameCaptureIdentity(before, between))
      fail("source identity changed after the image read");
    durableJson(join(binding.output, "diskutil-info-between.json"),
      betweenRaw.targetInfo, verify);
    durableJson(join(binding.output, "diskutil-members-between.json"),
      betweenRaw.memberInfos, verify);
    const second = await readRawPass({
      rawPath: disk.rawPath,
      expectedBytes: expectedSize,
      openRaw: runtime.openRaw ?? productionOpenRaw,
      verify,
    });
    const afterRaw = takeSnapshot(disk);
    const after = validateDiskSnapshot(afterRaw, disk);
    if (!sameCaptureIdentity(before, after))
      fail("source identity changed during the verification read");
    const imageSha256 = hashImage(finalImage, expectedSize);
    if (first.sha256 !== second.sha256 || first.sha256 !== imageSha256)
      fail("source pass 1, source pass 2 and captured image SHA-256 do not match");

    durableJson(join(binding.output, "diskutil-info-after.json"),
      afterRaw.targetInfo, verify);
    durableJson(join(binding.output, "diskutil-members-after.json"),
      afterRaw.memberInfos, verify);
    durableReplace(join(binding.output, "SHA256SUMS"), Buffer.from(
      `${imageSha256}  tf-whole-card.img\n`, "utf8"), verify);
    const completedAt = (runtime.now ?? (() => new Date()))().toISOString();
    const manifest = {
      schema: 1,
      complete: true,
      platform: "macOS",
      sourceAccess: "two full read-only raw-device passes",
      sourceWritesPerformed: false,
      automaticUnmountOrEjectPerformed: false,
      implicitPrivilegeEscalationPerformed: false,
      startedAt,
      completedAt,
      disk: disk.blockPath,
      rawDisk: disk.rawPath,
      bytes: expectedSize,
      sha256: imageSha256,
      image: "tf-whole-card.img",
      fingerprint: before.fingerprint,
      identity: before.identity,
      identityStableAcrossSnapshots: true,
      snapshotFingerprints: {
        before: before.fingerprint,
        preRead: preRead.fingerprint,
        betweenPasses: between.fingerprint,
        after: after.fingerprint,
      },
      sourcePasses: [first, second],
      outputCapacity: capacity,
      effectiveUserId: typeof process.geteuid === "function" ? process.geteuid() : null,
    };
    // The complete=true custody record is the final durable write. Until this
    // atomic replacement succeeds, custody.json remains complete=false.
    durableJson(custodyPath, manifest, verify);
    stdout.write(`${JSON.stringify({
      ok: true,
      mode: "capture",
      output: binding.output,
      bytes: expectedSize,
      sha256: imageSha256,
      fingerprint: before.fingerprint,
    })}\n`);
    return manifest;
  } catch (error) {
    try {
      durableJson(custodyPath, {
        schema: 1,
        complete: false,
        startedAt,
        failedAt: (runtime.now ?? (() => new Date()))().toISOString(),
        disk: disk.blockPath,
        expectedBytes: expectedSize,
        confirmedFingerprint: before.fingerprint,
        error: error instanceof Error ? error.message : "unknown capture failure",
      }, verify);
    } catch {
      // Never follow a changed destination binding just to record an error.
    }
    throw error;
  }
}

const invoked = process.argv[1] &&
  import.meta.url === pathToFileURL(resolve(process.argv[1])).href;
if (invoked) {
  try {
    await runCli(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`TF whole-card custody refused: ${
      error instanceof Error ? error.message : "unknown error"}\n`);
    process.exitCode = 1;
  }
}
