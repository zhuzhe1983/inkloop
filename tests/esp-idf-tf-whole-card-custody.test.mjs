import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Readable } from "node:stream";
import test from "node:test";

import {
  parseWholeDiskPath,
  runCli,
  validateDiskSnapshot,
} from "../firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs";

const repo = new URL("../", import.meta.url).pathname;
const tool = join(
  repo,
  "firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs",
);
const CARD_BYTES = 64 * 1024 * 1024;

function snapshot(overrides = {}) {
  const targetInfo = {
    DeviceIdentifier: "disk9",
    DeviceNode: "/dev/disk9",
    Whole: true,
    Internal: false,
    VirtualOrPhysical: "Physical",
    RemovableMedia: true,
    Ejectable: true,
    TotalSize: CARD_BYTES,
    DeviceBlockSize: 512,
    MediaName: "TEST TF CARD",
    DeviceTreePath: "IODeviceTree:/test/usb-reader/media-slot",
    IORegistryEntryName: "Test SD Card Reader Media",
    BusProtocol: "USB",
    MediaType: "Generic",
    Mounted: false,
    MountPoint: null,
    ...overrides.targetInfo,
  };
  const partitionInfo = {
    DeviceIdentifier: "disk9s1",
    ParentWholeDisk: "disk9",
    TotalSize: CARD_BYTES - 1024 * 1024,
    Content: "DOS_FAT_32",
    FilesystemType: "msdos",
    PartitionUUID: "11111111-2222-3333-4444-555555555555",
    VolumeUUID: "AAAA-BBBB",
    Mounted: false,
    MountPoint: null,
    ...overrides.partitionInfo,
  };
  return {
    targetInfo,
    targetList: {
      AllDisksAndPartitions: [{
        DeviceIdentifier: "disk9",
        Partitions: [{ DeviceIdentifier: "disk9s1" }],
      }],
      ...overrides.targetList,
    },
    memberInfos: overrides.memberInfos ?? [targetInfo, partitionInfo],
    rootInfos: overrides.rootInfos ?? [
      {
        DeviceIdentifier: "disk3s1s1",
        ParentWholeDisk: "disk3",
      },
      {
        DeviceIdentifier: "disk3s5",
        ParentWholeDisk: "disk3",
      },
    ],
  };
}

function outputFixture() {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-whole-card-test-"));
  return { scratch, output: join(scratch, "custody") };
}

function captureArgs(output, confirmation) {
  return [
    "--mode", "capture",
    "--disk", "/dev/disk9",
    "--output", output,
    "--expect-size-bytes", String(confirmation.totalSize),
    "--confirm-fingerprint", confirmation.fingerprint,
  ];
}

function memoryStdout() {
  let value = "";
  return {
    stream: { write(chunk) { value += String(chunk); return true; } },
    value: () => value,
  };
}

test("whole-device parser rejects raw paths, partitions and implicit targets", () => {
  assert.deepEqual(parseWholeDiskPath("/dev/disk9"), {
    blockPath: "/dev/disk9",
    rawPath: "/dev/rdisk9",
    identifier: "disk9",
  });
  for (const unsafe of [
    "", "disk9", "/dev/rdisk9", "/dev/disk9s1", "/dev/disk*",
    "/dev/disk9/../disk8", "/", "/dev/disk-1",
  ]) assert.throws(() => parseWholeDiskPath(unsafe), /explicit whole-device path/);
});

test("disk snapshot records a stable fingerprint for explicit removable media", () => {
  const first = validateDiskSnapshot(snapshot(), "/dev/disk9");
  const second = validateDiskSnapshot(snapshot(), "/dev/disk9");
  assert.equal(first.totalSize, CARD_BYTES);
  assert.match(first.fingerprint, /^[0-9a-f]{64}$/);
  assert.equal(first.fingerprint, second.fingerprint);
  assert.equal(first.identity.members.length, 2);

  const changed = snapshot({
    partitionInfo: { VolumeUUID: "CCCC-DDDD" },
  });
  assert.notEqual(
    validateDiskSnapshot(changed, "/dev/disk9").fingerprint,
    first.fingerprint,
  );
});

test("disk snapshot fails closed for internal, boot, virtual, mounted and system media", () => {
  const cases = [
    [snapshot({ targetInfo: { Internal: true } }), /internal/],
    [snapshot({ targetInfo: { VirtualOrPhysical: "Virtual" } }), /virtual/],
    [snapshot({
      rootInfos: [{ DeviceIdentifier: "disk9s1", ParentWholeDisk: "disk9" }],
    }), /boot\/system/],
    [snapshot({ partitionInfo: { Mounted: true, MountPoint: "/Volumes/TF" } }), /mounted/],
    [snapshot({ partitionInfo: { Mounted: undefined, MountPoint: null } }), /indeterminate/],
    [snapshot({ partitionInfo: { Content: "Apple_APFS" } }), /APFS/],
    [snapshot({ targetInfo: { RemovableMedia: false, Ejectable: false } }), /removable/],
    [snapshot({ targetInfo: { TotalSize: 32 * 1024 * 1024 } }), /approved TF range/],
    [snapshot({
      targetInfo: { DeviceTreePath: null, IORegistryEntryName: null },
    }), /stable media identity/],
  ];
  for (const [value, pattern] of cases)
    assert.throws(() => validateDiskSnapshot(value, "/dev/disk9"), pattern);
});

test("inspect is metadata-only and returns explicit capture confirmations", async () => {
  const value = outputFixture();
  const stdout = memoryStdout();
  let rawReads = 0;
  try {
    const result = await runCli([
      "--mode", "inspect",
      "--disk", "/dev/disk9",
      "--output", value.output,
    ], {
      platform: "darwin",
      takeSnapshot: () => snapshot(),
      openRaw: () => { rawReads += 1; throw new Error("must not read raw media"); },
      stdout: stdout.stream,
    });
    assert.equal(rawReads, 0);
    assert.equal(result.noRawDeviceRead, true);
    assert.equal(result.expectSizeBytes, CARD_BYTES);
    assert.match(result.confirmFingerprint, /^[0-9a-f]{64}$/);
    assert.equal(existsSync(value.output), false);
    assert.match(stdout.value(), /"mode": "inspect"/);
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("capture performs two identical raw reads and writes complete external custody", async () => {
  const value = outputFixture();
  const confirmation = validateDiskSnapshot(snapshot(), "/dev/disk9");
  const source = Buffer.alloc(CARD_BYTES);
  for (let at = 0; at < source.length; at += 4096)
    source.writeUInt32LE(at >>> 0, at);
  const expectedHash = createHash("sha256").update(source).digest("hex");
  const stdout = memoryStdout();
  let rawReads = 0;
  let snapshots = 0;
  try {
    const manifest = await runCli(captureArgs(value.output, confirmation), {
      platform: "darwin",
      takeSnapshot: () => { snapshots += 1; return snapshot(); },
      openRaw: (path) => {
        assert.equal(path, "/dev/rdisk9");
        rawReads += 1;
        return Readable.from([
          source.subarray(0, 5 * 1024 * 1024),
          source.subarray(5 * 1024 * 1024, 11 * 1024 * 1024),
          source.subarray(11 * 1024 * 1024),
        ]);
      },
      statfs: () => ({ bavail: 1024 * 1024, bsize: 4096 }),
      now: () => new Date("2026-08-24T00:00:00.000Z"),
      stdout: stdout.stream,
    });
    assert.equal(rawReads, 2);
    assert.equal(snapshots, 4);
    assert.equal(manifest.complete, true);
    assert.equal(manifest.sourceWritesPerformed, false);
    assert.equal(manifest.automaticUnmountOrEjectPerformed, false);
    assert.equal(manifest.implicitPrivilegeEscalationPerformed, false);
    assert.equal(manifest.sha256, expectedHash);
    assert.equal(readFileSync(join(value.output, "tf-whole-card.img")).length,
      CARD_BYTES);
    assert.equal(
      readFileSync(join(value.output, "SHA256SUMS"), "utf8"),
      `${expectedHash}  tf-whole-card.img\n`,
    );
    const custody = JSON.parse(readFileSync(
      join(value.output, "custody.json"), "utf8"));
    assert.equal(custody.complete, true);
    assert.equal(custody.sourcePasses.length, 2);
    assert.equal(custody.sourcePasses[0].sha256, expectedHash);
    assert.equal(custody.sourcePasses[1].sha256, expectedHash);
    assert.equal(JSON.parse(readFileSync(
      join(value.output, "diskutil-info-before.json"), "utf8"))
      .DeviceIdentifier, "disk9");
    assert.equal(JSON.parse(readFileSync(
      join(value.output, "diskutil-info-after.json"), "utf8"))
      .DeviceIdentifier, "disk9");
    assert.equal(JSON.parse(readFileSync(
      join(value.output, "diskutil-members-before.json"), "utf8")).length, 2);
    assert.equal(JSON.parse(readFileSync(
      join(value.output, "diskutil-info-between.json"), "utf8"))
      .DeviceIdentifier, "disk9");
    assert.deepEqual(new Set(Object.values(custody.snapshotFingerprints)),
      new Set([confirmation.fingerprint]));
    assert.match(stdout.value(), /"mode":"capture"/);
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("capture fails closed when diskutil identity changes between raw passes", async () => {
  const value = outputFixture();
  const confirmation = validateDiskSnapshot(snapshot(), "/dev/disk9");
  const source = Buffer.alloc(CARD_BYTES, 0x33);
  let snapshotAt = 0;
  let rawReads = 0;
  try {
    await assert.rejects(() => runCli(captureArgs(value.output, confirmation), {
      platform: "darwin",
      takeSnapshot: () => {
        snapshotAt += 1;
        return snapshotAt < 3
          ? snapshot()
          : snapshot({ targetInfo: { MediaName: "REPLACED TEST CARD" } });
      },
      openRaw: () => { rawReads += 1; return Readable.from([source]); },
      statfs: () => ({ bavail: 1024 * 1024, bsize: 4096 }),
      now: () => new Date("2026-08-24T00:00:00.000Z"),
      stdout: { write() { return true; } },
    }), /identity changed after the image read/);
    assert.equal(rawReads, 1);
    const custody = JSON.parse(readFileSync(
      join(value.output, "custody.json"), "utf8"));
    assert.equal(custody.complete, false);
    assert.match(custody.error, /identity changed/);
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("capture leaves complete=false when a second raw read differs", async () => {
  const value = outputFixture();
  const confirmation = validateDiskSnapshot(snapshot(), "/dev/disk9");
  const first = Buffer.alloc(CARD_BYTES, 0x11);
  const second = Buffer.alloc(CARD_BYTES, 0x22);
  let pass = 0;
  try {
    await assert.rejects(() => runCli(captureArgs(value.output, confirmation), {
      platform: "darwin",
      takeSnapshot: () => snapshot(),
      openRaw: () => Readable.from([pass++ === 0 ? first : second]),
      statfs: () => ({ bavail: 1024 * 1024, bsize: 4096 }),
      now: () => new Date("2026-08-24T00:00:00.000Z"),
      stdout: { write() { return true; } },
    }), /do not match/);
    const custody = JSON.parse(readFileSync(
      join(value.output, "custody.json"), "utf8"));
    assert.equal(custody.complete, false);
    assert.match(custody.error, /do not match/);
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("capture refuses stale confirmations before creating an output directory", async () => {
  for (const stale of ["size", "fingerprint"]) {
    const value = outputFixture();
    const confirmation = validateDiskSnapshot(snapshot(), "/dev/disk9");
    const args = captureArgs(value.output, confirmation);
    if (stale === "size") args[args.indexOf("--expect-size-bytes") + 1] = String(CARD_BYTES + 1);
    else args[args.indexOf("--confirm-fingerprint") + 1] = "0".repeat(64);
    try {
      await assert.rejects(() => runCli(args, {
        platform: "darwin",
        takeSnapshot: () => snapshot(),
        stdout: { write() { return true; } },
      }), /confirmed byte size|confirmed fingerprint/);
      assert.equal(existsSync(value.output), false);
    } finally {
      rmSync(value.scratch, { recursive: true, force: true });
    }
  }
});

test("inspect refuses repository-local evidence before any diskutil access", async () => {
  const output = join(repo, "never-create-whole-card-custody-here");
  let snapshots = 0;
  await assert.rejects(() => runCli([
    "--mode", "inspect",
    "--disk", "/dev/disk9",
    "--output", output,
  ], {
    platform: "darwin",
    takeSnapshot: () => { snapshots += 1; return snapshot(); },
    stdout: { write() { return true; } },
  }), /outside the repository/);
  assert.equal(snapshots, 0);
  assert.equal(existsSync(output), false);
});

test("CLI has no default disk, implicit sudo, auto-unmount or source write path", () => {
  const source = readFileSync(tool, "utf8");
  assert.doesNotMatch(source, /execFileSync\([^\n]*sudo|["']sudo["']/);
  assert.doesNotMatch(source, /unmountDisk|eraseDisk|partitionDisk|repairDisk/);
  assert.match(source, /openSync\(rawPath, constants\.O_RDONLY/);
  assert.doesNotMatch(source, /openSync\(rawPath,[^\n]*O_WRONLY/);

  const help = spawnSync(process.execPath, [tool, "--help"], { encoding: "utf8" });
  assert.equal(help.status, 0);
  assert.match(help.stdout, /--disk \/dev\/diskN/);

  const missing = spawnSync(process.execPath, [
    tool, "--mode", "inspect", "--output", "/tmp/never-created-by-test",
  ], { encoding: "utf8" });
  assert.notEqual(missing.status, 0);
  assert.match(missing.stderr, /missing required argument: --disk/);
});
