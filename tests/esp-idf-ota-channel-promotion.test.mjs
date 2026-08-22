import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import test from "node:test";

const root = process.cwd();
const packager = path.join(
  root, "firmware/inkloop-idf/tools/package_ota_release.py");
const promoter = path.join(
  root, "firmware/inkloop-idf/tools/promote_ota_channel.py");
const promoterSource = fs.readFileSync(promoter, "utf8");
const boardSku = "m5-papercolor-c151";
const publicBase = "https://inkloop.mess.host/ota";

function run(command, args, options = {}) {
  return spawnSync(command, args, { encoding: "utf8", ...options });
}

function fixture(prefix = "inkloop-ota-promote-") {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  const privateKey = path.join(directory, "release.pem");
  const publicDer = path.join(directory, "release-public.der");
  const publicKey = path.join(directory, "reviewed-public-key.hex");
  const image = path.join(directory, "inkloop_idf.bin");
  const output = path.join(directory, "next", "public", "ota");
  fs.mkdirSync(output, { recursive: true });
  assert.equal(
    run("openssl", ["genpkey", "-algorithm", "ED25519", "-out", privateKey]).status,
    0,
  );
  fs.chmodSync(privateKey, 0o600);
  assert.equal(
    run("openssl", [
      "pkey", "-in", privateKey, "-pubout", "-outform", "DER", "-out", publicDer,
    ]).status,
    0,
  );
  const der = fs.readFileSync(publicDer);
  assert.equal(der.length, 44);
  fs.writeFileSync(publicKey, `${der.subarray(12).toString("hex")}\n`);
  fs.writeFileSync(image, Buffer.from("inkloop-promoted-image\0".repeat(257)));
  return { directory, privateKey, publicKey, image, output };
}

function versionPath(version) {
  return version.replace("+", "_build_");
}

function releaseDirectory(f, version) {
  return path.join(f.output, boardSku, versionPath(version));
}

function channelPath(f) {
  return path.join(f.output, boardSku, "manifest.json");
}

function packageRelease(f, version, options = {}) {
  if (options.imageBytes !== undefined) fs.writeFileSync(f.image, options.imageBytes);
  return run("python3", [
    packager,
    "--image", options.image ?? f.image,
    "--board-sku", options.boardSku ?? boardSku,
    "--firmware-version", version,
    "--public-base-url", options.baseUrl ?? publicBase,
    "--private-key", options.privateKey ?? f.privateKey,
    "--output-root", options.output ?? f.output,
  ]);
}

function promoteRelease(f, version, options = {}) {
  const commandArgs = [
    promoter,
    "--output-root", options.output ?? f.output,
    "--board-sku", options.boardSku ?? boardSku,
    "--firmware-version", version,
    "--public-base-url", options.baseUrl ?? publicBase,
    "--public-key", options.publicKey ?? f.publicKey,
  ];
  if (options.verifyOnly) commandArgs.push("--verify-only");
  return run("python3", commandArgs);
}

function releaseHashes(f, version) {
  const directory = releaseDirectory(f, version);
  return Object.fromEntries(fs.readdirSync(directory).sort().map((name) => [
    name,
    crypto.createHash("sha256").update(fs.readFileSync(path.join(directory, name))).digest("hex"),
  ]));
}

function rewriteReceiptForManifest(f, version) {
  const directory = releaseDirectory(f, version);
  const manifestPath = path.join(directory, "manifest.json");
  const receiptPath = path.join(directory, "release-receipt.json");
  const manifest = fs.readFileSync(manifestPath);
  const receipt = JSON.parse(fs.readFileSync(receiptPath, "utf8"));
  receipt.manifest_size = manifest.length;
  receipt.manifest_sha256 = crypto.createHash("sha256").update(manifest).digest("hex");
  fs.writeFileSync(receiptPath, `${JSON.stringify(receipt)}\n`);
}

test("promotion help and atomic single-channel contract are explicit", () => {
  const help = run("python3", [promoter, "--help"]);
  assert.equal(help.status, 0, help.stderr);
  for (const option of [
    "--output-root", "--board-sku", "--firmware-version",
    "--public-base-url", "--public-key", "--verify-only",
  ]) assert.match(help.stdout, new RegExp(option));
  assert.match(promoterSource, /fcntl\.LOCK_EX \| fcntl\.LOCK_NB/);
  assert.match(promoterSource, /os\.O_NOFOLLOW/);
  assert.match(promoterSource, /os\.replace\(temporary, channel_path\)/);
  assert.match(promoterSource, /os\.fsync\(board_descriptor\)/);
  assert.doesNotMatch(promoterSource, /private[_-]key|deploy|curl|flash/i);
});

test("real Ed25519 releases promote byte-exactly and packaging remains reusable", () => {
  const f = fixture();
  try {
    assert.equal(packageRelease(f, "0.4.0-beta.2").status, 0);
    const beta2Before = releaseHashes(f, "0.4.0-beta.2");
    const verified = promoteRelease(f, "0.4.0-beta.2", { verifyOnly: true });
    assert.equal(verified.status, 0, verified.stderr);
    assert.equal(JSON.parse(verified.stdout).operation, "verify");
    assert.equal(fs.existsSync(channelPath(f)), false);
    const first = promoteRelease(f, "0.4.0-beta.2");
    assert.equal(first.status, 0, first.stderr);
    const firstResult = JSON.parse(first.stdout);
    assert.equal(firstResult.operation, "promote");
    assert.equal(firstResult.channel_url, `${publicBase}/${boardSku}/manifest.json`);
    assert.equal(firstResult.firmware_version, "0.4.0-beta.2");
    assert.deepEqual(
      fs.readFileSync(channelPath(f)),
      fs.readFileSync(path.join(releaseDirectory(f, "0.4.0-beta.2"), "manifest.json")),
    );
    assert.deepEqual(releaseHashes(f, "0.4.0-beta.2"), beta2Before);

    const beta3Image = Buffer.from("inkloop-promoted-image-beta3\0".repeat(257));
    const packaged = packageRelease(f, "0.4.0-beta.3", { imageBytes: beta3Image });
    assert.equal(packaged.status, 0, packaged.stderr);
    const beta3Before = releaseHashes(f, "0.4.0-beta.3");
    const second = promoteRelease(f, "0.4.0-beta.3");
    assert.equal(second.status, 0, second.stderr);
    assert.deepEqual(
      fs.readFileSync(channelPath(f)),
      fs.readFileSync(path.join(releaseDirectory(f, "0.4.0-beta.3"), "manifest.json")),
    );
    assert.deepEqual(releaseHashes(f, "0.4.0-beta.2"), beta2Before);
    assert.deepEqual(releaseHashes(f, "0.4.0-beta.3"), beta3Before);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("receipt, image, URL, signature and reviewed public key corruption fail closed", () => {
  const cases = [
    {
      name: "receipt",
      mutate(f, version) {
        const receipt = path.join(releaseDirectory(f, version), "release-receipt.json");
        fs.appendFileSync(receipt, "x");
      },
      error: /invalid_receipt/,
    },
    {
      name: "image",
      mutate(f, version) {
        const manifest = JSON.parse(fs.readFileSync(
          path.join(releaseDirectory(f, version), "manifest.json"), "utf8"));
        const image = path.join(releaseDirectory(f, version), path.basename(manifest.image_url));
        fs.appendFileSync(image, "corrupt");
      },
      error: /image_manifest_mismatch/,
    },
    {
      name: "same-origin URL",
      mutate(f, version) {
        const manifestPath = path.join(releaseDirectory(f, version), "manifest.json");
        const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
        manifest.image_url = manifest.image_url.replace("inkloop.mess.host", "evil.example.com");
        fs.writeFileSync(manifestPath, `${JSON.stringify(manifest)}\n`);
        rewriteReceiptForManifest(f, version);
      },
      error: /invalid_manifest|ambiguous_release_history/,
    },
    {
      name: "signature",
      mutate(f, version) {
        const manifestPath = path.join(releaseDirectory(f, version), "manifest.json");
        const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
        manifest.detached_signature = `${manifest.detached_signature[0] === "0" ? "1" : "0"}${manifest.detached_signature.slice(1)}`;
        fs.writeFileSync(manifestPath, `${JSON.stringify(manifest)}\n`);
        rewriteReceiptForManifest(f, version);
      },
      error: /signature_verification_failed/,
    },
    {
      name: "wrong reviewed key",
      mutate(f) {
        const other = fixture("inkloop-ota-other-key-");
        fs.copyFileSync(other.publicKey, f.publicKey);
        fs.rmSync(other.directory, { recursive: true, force: true });
      },
      error: /signature_verification_failed/,
    },
    {
      name: "malformed reviewed key",
      mutate(f) { fs.writeFileSync(f.publicKey, `${"AB".repeat(32)}\n`); },
      error: /invalid_public_key/,
    },
  ];
  for (const entry of cases) {
    const f = fixture(`inkloop-ota-corrupt-${entry.name.replaceAll(" ", "-")}-`);
    try {
      const version = "0.4.0-beta.2";
      assert.equal(packageRelease(f, version).status, 0);
      entry.mutate(f, version);
      const result = promoteRelease(f, version);
      assert.equal(result.status, 2, `${entry.name}: ${result.stderr}`);
      assert.match(result.stderr, entry.error, entry.name);
      assert.equal(fs.existsSync(channelPath(f)), false, entry.name);
    } finally {
      fs.rmSync(f.directory, { recursive: true, force: true });
    }
  }
});

test("equal, downgrade and corrupt-next promotion preserve the current channel", () => {
  const f = fixture();
  try {
    assert.equal(packageRelease(f, "0.4.0-beta.2").status, 0);
    assert.equal(promoteRelease(f, "0.4.0-beta.2").status, 0);
    const beta2Channel = fs.readFileSync(channelPath(f));
    const equal = promoteRelease(f, "0.4.0-beta.2");
    assert.equal(equal.status, 2);
    assert.match(equal.stderr, /non_increasing_promotion/);
    assert.deepEqual(fs.readFileSync(channelPath(f)), beta2Channel);

    assert.equal(packageRelease(f, "0.4.0-beta.3", {
      imageBytes: Buffer.from("beta3-image\0".repeat(100)),
    }).status, 0);
    fs.appendFileSync(channelPath(f), "corrupt");
    const corruptChannel = fs.readFileSync(channelPath(f));
    const refusedCorruptChannel = promoteRelease(f, "0.4.0-beta.3");
    assert.equal(refusedCorruptChannel.status, 2);
    assert.match(refusedCorruptChannel.stderr, /invalid_channel_manifest/);
    assert.deepEqual(fs.readFileSync(channelPath(f)), corruptChannel);
    fs.writeFileSync(channelPath(f), beta2Channel);

    const beta3Receipt = path.join(
      releaseDirectory(f, "0.4.0-beta.3"), "release-receipt.json");
    fs.appendFileSync(beta3Receipt, "corrupt");
    const corrupt = promoteRelease(f, "0.4.0-beta.3");
    assert.equal(corrupt.status, 2);
    assert.deepEqual(fs.readFileSync(channelPath(f)), beta2Channel);
    fs.truncateSync(beta3Receipt, fs.statSync(beta3Receipt).size - "corrupt".length);
    assert.equal(promoteRelease(f, "0.4.0-beta.3").status, 0);
    const beta3Channel = fs.readFileSync(channelPath(f));
    const downgrade = promoteRelease(f, "0.4.0-beta.2");
    assert.equal(downgrade.status, 2);
    assert.match(downgrade.stderr, /non_increasing_promotion/);
    assert.deepEqual(fs.readFileSync(channelPath(f)), beta3Channel);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("symlink, traversal and ambiguous-tree attacks are refused", () => {
  const cases = [
    {
      name: "artifact symlink",
      mutate(f, version) {
        const receipt = path.join(releaseDirectory(f, version), "release-receipt.json");
        const moved = path.join(f.directory, "moved-receipt.json");
        fs.renameSync(receipt, moved);
        fs.symlinkSync(moved, receipt);
      },
      error: /invalid_receipt|ambiguous_release_history/,
    },
    {
      name: "version symlink",
      mutate(f, version) {
        const release = releaseDirectory(f, version);
        const moved = path.join(f.directory, "moved-release");
        fs.renameSync(release, moved);
        fs.symlinkSync(moved, release);
      },
      error: /ambiguous_release_history/,
    },
    {
      name: "ambiguous entry",
      mutate(f) { fs.writeFileSync(path.join(f.output, boardSku, "README"), "unexpected"); },
      error: /ambiguous_release_history/,
    },
  ];
  for (const entry of cases) {
    const f = fixture(`inkloop-ota-path-${entry.name.replaceAll(" ", "-")}-`);
    try {
      const version = "0.4.0-beta.2";
      assert.equal(packageRelease(f, version).status, 0);
      entry.mutate(f, version);
      const result = promoteRelease(f, version);
      assert.equal(result.status, 2, `${entry.name}: ${result.stderr}`);
      assert.match(result.stderr, entry.error);
      assert.equal(fs.existsSync(channelPath(f)), false);
    } finally {
      fs.rmSync(f.directory, { recursive: true, force: true });
    }
  }

  const f = fixture("inkloop-ota-path-inputs-");
  try {
    assert.equal(packageRelease(f, "0.4.0-beta.2").status, 0);
    const linkedOutput = path.join(f.directory, "linked-output");
    fs.symlinkSync(f.output, linkedOutput);
    const linkedKey = path.join(f.directory, "linked-key.hex");
    fs.symlinkSync(f.publicKey, linkedKey);
    const inputs = [
      { options: { output: linkedOutput }, error: /invalid_output_root/ },
      { options: { publicKey: linkedKey }, error: /invalid_public_key/ },
      { options: { output: "relative/output" }, error: /invalid_output_root_path/ },
      {
        options: { output: `${f.directory}/next/../next/public/ota` },
        error: /invalid_output_root_path/,
      },
      { options: { boardSku: "../other" }, error: /invalid_board_sku/ },
      { version: "1.0.0/../2.0.0", options: {}, error: /invalid_firmware_version/ },
    ];
    for (const entry of inputs) {
      const result = promoteRelease(f, entry.version ?? "0.4.0-beta.2", entry.options);
      assert.equal(result.status, 2, result.stderr);
      assert.match(result.stderr, entry.error);
    }
    const insideKey = path.join(f.output, "reviewed-public-key.hex");
    fs.copyFileSync(f.publicKey, insideKey);
    const inside = promoteRelease(f, "0.4.0-beta.2", { publicKey: insideKey });
    assert.equal(inside.status, 2);
    assert.match(inside.stderr, /public_key_inside_output/);
    assert.equal(fs.existsSync(channelPath(f)), false);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }

  const channelLink = fixture("inkloop-ota-channel-link-");
  try {
    const version = "0.4.0-beta.2";
    assert.equal(packageRelease(channelLink, version).status, 0);
    assert.equal(promoteRelease(channelLink, version).status, 0);
    fs.unlinkSync(channelPath(channelLink));
    fs.symlinkSync(
      path.join(releaseDirectory(channelLink, version), "manifest.json"),
      channelPath(channelLink),
    );
    const result = promoteRelease(channelLink, version);
    assert.equal(result.status, 2);
    assert.match(result.stderr, /invalid_channel_manifest/);
    assert.equal(fs.lstatSync(channelPath(channelLink)).isSymbolicLink(), true);
  } finally {
    fs.rmSync(channelLink.directory, { recursive: true, force: true });
  }
});

test("a concurrent board publisher is rejected without waiting or publishing", async () => {
  const f = fixture("inkloop-ota-concurrent-");
  let holder;
  try {
    assert.equal(packageRelease(f, "0.4.0-beta.2").status, 0);
    const board = path.join(f.output, boardSku);
    holder = spawn("python3", [
      "-c",
      "import fcntl,os,sys,time; fd=os.open(sys.argv[1],os.O_RDONLY); fcntl.flock(fd,fcntl.LOCK_EX); print('locked',flush=True); time.sleep(30)",
      board,
    ], { stdio: ["ignore", "pipe", "pipe"] });
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("lock holder timeout")), 5000);
      holder.stdout.once("data", (data) => {
        clearTimeout(timer);
        assert.match(data.toString(), /locked/);
        resolve();
      });
      holder.once("exit", (status) => reject(new Error(`lock holder exited ${status}`)));
    });
    const started = Date.now();
    const result = promoteRelease(f, "0.4.0-beta.2");
    assert.equal(result.status, 2, result.stderr);
    assert.match(result.stderr, /concurrent_publication/);
    assert.ok(Date.now() - started < 3000);
    assert.equal(fs.existsSync(channelPath(f)), false);
  } finally {
    if (holder) holder.kill("SIGTERM");
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});
