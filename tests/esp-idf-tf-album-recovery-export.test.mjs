import assert from "node:assert/strict";
import childProcess from "node:child_process";
import { createHash } from "node:crypto";
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import {
  chmodSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { syncBuiltinESMExports } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  canonicalNewOutputPath,
  exportInspectedAlbumRecovery,
  inspectAlbumRecovery,
  parseMountTable,
  runCli,
} from "../firmware/inkloop-idf/tools/export_tf_album_recovery.mjs";

const repo = new URL("../", import.meta.url).pathname;
const tool = join(
  repo,
  "firmware/inkloop-idf/tools/export_tf_album_recovery.mjs",
);

function hash(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function asset(seed) {
  const bytes = Buffer.from(`asset-${seed}-${"x".repeat(64)}`, "utf8");
  return { bytes, sha256: hash(bytes) };
}

function index(entries, current = "") {
  return Buffer.from(JSON.stringify({
    schema: 1,
    current,
    currentRenderStrategy: current ? "official-quality" : "",
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

function fixture() {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-tf-export-"));
  const source = join(scratch, "source");
  const album = join(source, "inkloop-album");
  const output = join(scratch, "backup");
  mkdirSync(album, { recursive: true });
  const first = asset("first");
  const second = asset("second");
  const third = asset("third");
  for (const value of [first, second, third])
    writeFileSync(join(album, `${value.sha256}.png`), value.bytes);
  const candidates = {
    current: index([first, second], first.sha256),
    next: index([second, third], third.sha256),
    previous: index([first, third], first.sha256),
  };
  writeFileSync(join(album, "index.json"), candidates.current);
  writeFileSync(join(album, "index.next"), candidates.next);
  writeFileSync(join(album, "index.prev"), candidates.previous);
  return {
    scratch,
    source,
    album,
    output,
    assets: [first, second, third],
    candidates,
    expected: Object.fromEntries(Object.entries(candidates)
      .map(([name, bytes]) => [name, hash(bytes)])),
  };
}

function sourceHashes(value) {
  return {
    current: hash(readFileSync(join(value.album, "index.json"))),
    next: hash(readFileSync(join(value.album, "index.next"))),
    previous: hash(readFileSync(join(value.album, "index.prev"))),
    assets: value.assets.map((entry) =>
      hash(readFileSync(join(value.album, `${entry.sha256}.png`)))),
  };
}

test("read-only TF export preserves all divergent indexes and asset union", () => {
  const value = fixture();
  try {
    const before = sourceHashes(value);
    for (const path of [value.album,
      join(value.album, "index.json"), join(value.album, "index.next"),
      join(value.album, "index.prev"),
      ...value.assets.map((entry) => join(value.album, `${entry.sha256}.png`))])
      chmodSync(path, statSync(path).isDirectory() ? 0o500 : 0o400);

    const plan = inspectAlbumRecovery(value.source, value.expected);
    assert.equal(plan.candidates.length, 3);
    assert.equal(plan.assets.length, 3);
    const manifest = exportInspectedAlbumRecovery(plan, value.output);
    assert.equal(manifest.complete, true);
    assert.equal(manifest.candidateCount, 3);
    assert.equal(manifest.uniqueReferencedAssets, 3);
    assert.deepEqual(sourceHashes(value), before);

    assert.deepEqual(
      readFileSync(join(value.output, "candidates/index.json")),
      value.candidates.current,
    );
    assert.deepEqual(
      readFileSync(join(value.output, "candidates/index.next")),
      value.candidates.next,
    );
    assert.deepEqual(
      readFileSync(join(value.output, "candidates/index.prev")),
      value.candidates.previous,
    );
    for (const entry of value.assets) {
      assert.deepEqual(
        readFileSync(join(value.output, `assets/${entry.sha256}.png`)),
        entry.bytes,
      );
    }
    const persisted = JSON.parse(readFileSync(
      join(value.output, "manifest.json"), "utf8"));
    assert.equal(persisted.complete, true);
    assert.equal(persisted.files.length, 6);
    assert.match(readFileSync(join(value.output, "SHA256SUMS"), "utf8"),
      /candidates\/index\.json/);
  } finally {
    chmodSync(value.album, 0o700);
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("export refuses stale candidates and missing or altered assets before output", () => {
  for (const failure of ["stale", "missing", "altered"]) {
    const value = fixture();
    try {
      if (failure === "stale") value.expected.next = "0".repeat(64);
      if (failure === "missing")
        rmSync(join(value.album, `${value.assets[2].sha256}.png`));
      if (failure === "altered")
        writeFileSync(join(value.album, `${value.assets[1].sha256}.png`),
          Buffer.alloc(value.assets[1].bytes.length, 0x5a));
      assert.throws(
        () => inspectAlbumRecovery(value.source, value.expected),
        /digest changed|ENOENT|content digest/,
      );
      assert.equal(spawnSync("test", ["-e", value.output]).status, 1);
    } finally {
      rmSync(value.scratch, { recursive: true, force: true });
    }
  }
});

test("mount parsing accepts macOS and Linux read-only spellings", () => {
  assert.deepEqual(parseMountTable([
    "/dev/disk4s1 on /Volumes/INKLOOP TF (msdos, local, read-only, noowners)",
    "/dev/sdb1 on /mnt/inkloop type vfat (ro,nodev)",
    "malformed",
  ].join("\n")), [
    {
      device: "/dev/disk4s1",
      mountPoint: "/Volumes/INKLOOP TF",
      options: ["msdos", "local", "read-only", "noowners"],
    },
    {
      device: "/dev/sdb1",
      mountPoint: "/mnt/inkloop",
      options: ["ro", "nodev"],
    },
  ]);
});

test("CLI keeps the canonical output binding if the parent alias changes", () => {
  const value = fixture();
  const realParent = join(value.scratch, "real-parent");
  const redirectedParent = join(value.scratch, "redirected-parent");
  const aliasParent = join(value.scratch, "alias-parent");
  mkdirSync(realParent, { mode: 0o700 });
  mkdirSync(redirectedParent, { mode: 0o700 });
  symlinkSync(realParent, aliasParent, "dir");
  const requested = join(aliasParent, "backup");
  const canonical = canonicalNewOutputPath(requested);
  const canonicalSource = realpathSync(value.source);
  assert.equal(canonical.parent, realpathSync(realParent));
  assert.equal(canonical.output, join(realpathSync(realParent), "backup"));

  const originalExecFileSync = childProcess.execFileSync;
  const originalStatSync = fs.statSync;
  const originalStdoutWrite = process.stdout.write;
  let stdout = "";
  let aliasChanged = false;
  try {
    childProcess.execFileSync = () => {
      assert.equal(aliasChanged, false);
      rmSync(aliasParent);
      symlinkSync(redirectedParent, aliasParent, "dir");
      aliasChanged = true;
      return `/dev/inkloop-test on ${value.source} (read-only)\n`;
    };
    fs.statSync = (path, ...args) => {
      const result = originalStatSync(path, ...args);
      if (path === canonicalSource) result.dev = 101;
      if (path === canonical.parent) result.dev = 202;
      return result;
    };
    syncBuiltinESMExports();
    process.stdout.write = (chunk) => {
      stdout += chunk;
      return true;
    };
    runCli([
      "--source", value.source,
      "--output", requested,
      "--expect-device", "/dev/inkloop-test",
      "--expect-current-sha256", value.expected.current,
      "--expect-next-sha256", value.expected.next,
      "--expect-previous-sha256", value.expected.previous,
    ]);
  } finally {
    process.stdout.write = originalStdoutWrite;
    childProcess.execFileSync = originalExecFileSync;
    fs.statSync = originalStatSync;
    syncBuiltinESMExports();
  }

  try {
    assert.equal(aliasChanged, true);
    assert.equal(JSON.parse(stdout).output, canonical.output);
    assert.equal(JSON.parse(readFileSync(
      join(canonical.output, "manifest.json"), "utf8")).complete, true);
    assert.throws(() => statSync(join(redirectedParent, "backup")));
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("CLI revalidates the created output device before its first file write", () => {
  const value = fixture();
  const canonical = canonicalNewOutputPath(value.output);
  const canonicalSource = realpathSync(value.source);
  const originalExecFileSync = childProcess.execFileSync;
  const originalStatSync = fs.statSync;
  const originalLstatSync = fs.lstatSync;
  const originalFstatSync = fs.fstatSync;
  try {
    childProcess.execFileSync = () =>
      `/dev/inkloop-test on ${value.source} (read-only)\n`;
    fs.statSync = (path, ...args) => {
      const result = originalStatSync(path, ...args);
      if (path === canonicalSource || path === canonical.output) result.dev = 101;
      if (path === canonical.parent) result.dev = 202;
      return result;
    };
    fs.lstatSync = (path, ...args) => {
      const result = originalLstatSync(path, ...args);
      if (path === canonical.output) result.dev = 101;
      return result;
    };
    fs.fstatSync = (descriptor, ...args) => {
      const result = originalFstatSync(descriptor, ...args);
      if (result.isDirectory()) result.dev = 101;
      return result;
    };
    syncBuiltinESMExports();

    assert.throws(() => runCli([
      "--source", value.source,
      "--output", value.output,
      "--expect-device", "/dev/inkloop-test",
      "--expect-current-sha256", value.expected.current,
      "--expect-next-sha256", value.expected.next,
      "--expect-previous-sha256", value.expected.previous,
    ]), /created output is on the TF source filesystem/);
    assert.deepEqual(fs.readdirSync(canonical.output), []);
    assert.equal(fs.existsSync(join(canonical.output, "INCOMPLETE.json")), false);
  } finally {
    childProcess.execFileSync = originalExecFileSync;
    fs.statSync = originalStatSync;
    fs.lstatSync = originalLstatSync;
    fs.fstatSync = originalFstatSync;
    syncBuiltinESMExports();
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("CLI refuses a post-mkdir output alias outside the bound parent", () => {
  const value = fixture();
  const canonical = canonicalNewOutputPath(value.output);
  const canonicalSource = realpathSync(value.source);
  const redirected = join(value.scratch, "redirected");
  mkdirSync(redirected, { mode: 0o700 });
  const originalExecFileSync = childProcess.execFileSync;
  const originalStatSync = fs.statSync;
  const originalMkdirSync = fs.mkdirSync;
  try {
    childProcess.execFileSync = () =>
      `/dev/inkloop-test on ${value.source} (read-only)\n`;
    fs.statSync = (path, ...args) => {
      const result = originalStatSync(path, ...args);
      if (path === canonicalSource) result.dev = 101;
      if (path === canonical.parent) result.dev = 202;
      return result;
    };
    fs.mkdirSync = (path, ...args) => {
      const result = originalMkdirSync(path, ...args);
      if (path === canonical.output) {
        rmSync(path, { recursive: true });
        symlinkSync(redirected, path, "dir");
      }
      return result;
    };
    syncBuiltinESMExports();

    assert.throws(() => runCli([
      "--source", value.source,
      "--output", value.output,
      "--expect-device", "/dev/inkloop-test",
      "--expect-current-sha256", value.expected.current,
      "--expect-next-sha256", value.expected.next,
      "--expect-previous-sha256", value.expected.previous,
    ]), /created output resolved outside its bound parent/);
    assert.deepEqual(fs.readdirSync(redirected), []);
    assert.equal(fs.existsSync(join(redirected, "INCOMPLETE.json")), false);
  } finally {
    childProcess.execFileSync = originalExecFileSync;
    fs.statSync = originalStatSync;
    fs.mkdirSync = originalMkdirSync;
    syncBuiltinESMExports();
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("export claims the output leaf exclusively", () => {
  const value = fixture();
  const missingTarget = join(value.scratch, "missing-target");
  symlinkSync(missingTarget, value.output, "dir");
  try {
    const plan = inspectAlbumRecovery(value.source, value.expected);
    assert.throws(
      () => exportInspectedAlbumRecovery(plan, value.output),
      /output directory already exists/,
    );
    assert.equal(lstatSync(value.output).isSymbolicLink(), true);
    assert.throws(() => statSync(missingTarget));
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("CLI refuses a directory that is not an exact read-only mount", () => {
  const value = fixture();
  try {
    const result = spawnSync(process.execPath, [
      tool,
      "--source", value.source,
      "--output", value.output,
      "--expect-device", "/dev/not-the-source",
      "--expect-current-sha256", value.expected.current,
      "--expect-next-sha256", value.expected.next,
      "--expect-previous-sha256", value.expected.previous,
    ], { encoding: "utf8" });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /exact root of a mounted filesystem|mounted device mismatch/);
    assert.throws(() => statSync(value.output));
  } finally {
    rmSync(value.scratch, { recursive: true, force: true });
  }
});

test("export tool contains no source-side mutation primitives", () => {
  const source = readFileSync(tool, "utf8");
  assert.doesNotMatch(source, /unlinkSync|rmSync|rmdirSync|truncateSync|chmodSync|chownSync/);
  assert.match(source, /O_RDONLY/);
  assert.match(source, /source filesystem is not mounted read-only/);
  assert.match(source, /output must be on a different filesystem/);
  assert.match(source, /fsyncSync/);
  assert.ok(
    source.indexOf('writeExclusive(join(output, "SHA256SUMS")') <
      source.indexOf('durableJson(join(output, "manifest.json")'),
    "complete manifest must be the final durable export write",
  );
  execFileSync(process.execPath, [tool, "--help"], { stdio: "pipe" });
});
