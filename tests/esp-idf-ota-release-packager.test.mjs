import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const root = process.cwd();
const packager = path.join(
  root, "firmware/inkloop-idf/tools/package_ota_release.py");
const source = fs.readFileSync(packager, "utf8");

function run(command, args, options = {}) {
  return spawnSync(command, args, { encoding: "utf8", ...options });
}

function fixture(prefix = "inkloop-ota-package-") {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  const privateKey = path.join(directory, "release.pem");
  const image = path.join(directory, "inkloop_idf.bin");
  const output = path.join(directory, "public", "ota");
  fs.mkdirSync(output, { recursive: true });
  assert.equal(
    run("openssl", ["genpkey", "-algorithm", "ED25519", "-out", privateKey]).status,
    0,
  );
  fs.chmodSync(privateKey, 0o600);
  fs.writeFileSync(image, Buffer.from("inkloop-release-image\0".repeat(257)));
  return { directory, privateKey, image, output };
}

function packageRelease(f, overrides = {}) {
  return run("python3", [
    packager,
    "--image", overrides.image ?? f.image,
    "--board-sku", overrides.boardSku ?? "m5-papercolor-c151",
    "--firmware-version", overrides.version ?? "0.4.0-beta.2",
    "--public-base-url", overrides.baseUrl ?? "https://updates.example.com/ota",
    "--private-key", overrides.privateKey ?? f.privateKey,
    "--output-root", overrides.output ?? f.output,
  ]);
}

function releaseDirectory(f, version = "0.4.0-beta.2") {
  return path.join(f.output, "m5-papercolor-c151", version.replace("+", "_build_"));
}

function verifyWithOpenSsl(directory, privateKey, manifest, image) {
  const board = Buffer.from(manifest.board_sku, "ascii");
  const version = Buffer.from(manifest.firmware_version, "ascii");
  const digest = Buffer.from(manifest.image_sha256, "hex");
  const policy = Buffer.from(manifest.signature_policy, "ascii");
  const size = Buffer.alloc(8);
  size.writeBigUInt64LE(BigInt(manifest.image_size));
  const signedMessage = Buffer.concat([
    Buffer.from("INKLOOP-OTA-MANIFEST-V1", "ascii"),
    Buffer.from([1, 0, board.length]),
    board,
    Buffer.from([version.length]),
    version,
    size,
    digest,
    Buffer.from([policy.length]),
    policy,
    digest,
  ]);
  assert.equal(
    digest.toString("hex"),
    crypto.createHash("sha256").update(fs.readFileSync(image)).digest("hex"),
  );
  const publicPem = path.join(directory, "release.pub.pem");
  const messagePath = path.join(directory, "signed-message.bin");
  const signaturePath = path.join(directory, "signature.bin");
  assert.equal(
    run("openssl", ["pkey", "-in", privateKey, "-pubout", "-out", publicPem]).status,
    0,
  );
  fs.writeFileSync(messagePath, signedMessage);
  fs.writeFileSync(signaturePath, Buffer.from(manifest.detached_signature, "hex"));
  const verified = run("openssl", [
    "pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", publicPem,
    "-sigfile", signaturePath, "-in", messagePath,
  ]);
  assert.equal(verified.status, 0, verified.stderr);
}

test("packager help and atomic publication contract are explicit", () => {
  const help = run("python3", [packager, "--help"]);
  assert.equal(help.status, 0, help.stderr);
  for (const option of [
    "--image", "--board-sku", "--firmware-version", "--public-base-url",
    "--private-key", "--output-root",
  ]) assert.match(help.stdout, new RegExp(option));
  assert.match(source, /os\.O_NOFOLLOW/);
  assert.match(source, /fcntl\.flock\(board_descriptor, fcntl\.LOCK_EX\)/);
  const receiptWrite = source.indexOf("_atomic_write(staging_directory / receipt_relative.name");
  const manifestRename = source.indexOf("os.replace(signed_manifest, staging_directory / manifest_relative.name)");
  const releaseRename = source.indexOf("os.rename(staging_directory, final_directory)");
  assert.ok(receiptWrite > 0 && receiptWrite < manifestRename);
  assert.ok(manifestRename < releaseRename);
  assert.doesNotMatch(source, /public-key-out|private_key.*read_(?:bytes|text)/);
});

test("one release is deterministic, receipt-complete and OpenSSL-verifiable", () => {
  const first = fixture("inkloop-ota-deterministic-a-");
  const second = fixture("inkloop-ota-deterministic-b-");
  try {
    // Reuse byte-identical image and the same externally held signing key.
    fs.copyFileSync(first.image, second.image);
    fs.copyFileSync(first.privateKey, second.privateKey);
    fs.chmodSync(second.privateKey, 0o600);
    const one = packageRelease(first);
    const two = packageRelease(second);
    assert.equal(one.status, 0, one.stderr);
    assert.equal(two.status, 0, two.stderr);

    const firstRelease = releaseDirectory(first);
    const secondRelease = releaseDirectory(second);
    const imageName = "inkloop-idf-m5-papercolor-c151-0.4.0-beta.2.bin";
    const expectedNames = [imageName, "manifest.json", "release-receipt.json"];
    assert.deepEqual(fs.readdirSync(firstRelease).sort(), expectedNames.sort());
    for (const name of expectedNames) {
      assert.deepEqual(
        fs.readFileSync(path.join(firstRelease, name)),
        fs.readFileSync(path.join(secondRelease, name)),
      );
    }

    const imagePath = path.join(firstRelease, imageName);
    const manifestPath = path.join(firstRelease, "manifest.json");
    const receiptPath = path.join(firstRelease, "release-receipt.json");
    const manifestBytes = fs.readFileSync(manifestPath);
    const manifest = JSON.parse(manifestBytes);
    const receiptBytes = fs.readFileSync(receiptPath);
    const receipt = JSON.parse(receiptBytes);
    assert.deepEqual(Object.keys(manifest), [
      "schema_version", "board_sku", "firmware_version", "image_url",
      "image_size", "image_sha256", "signature_policy", "detached_signature",
    ]);
    assert.equal(
      manifest.image_url,
      `https://updates.example.com/ota/m5-papercolor-c151/0.4.0-beta.2/${imageName}`,
    );
    assert.equal(receipt.image_path, `m5-papercolor-c151/0.4.0-beta.2/${imageName}`);
    assert.equal(receipt.manifest_path, "m5-papercolor-c151/0.4.0-beta.2/manifest.json");
    assert.equal(receipt.image_size, fs.statSync(imagePath).size);
    assert.equal(receipt.manifest_size, manifestBytes.length);
    assert.equal(
      receipt.image_sha256,
      crypto.createHash("sha256").update(fs.readFileSync(imagePath)).digest("hex"),
    );
    assert.equal(
      receipt.manifest_sha256,
      crypto.createHash("sha256").update(manifestBytes).digest("hex"),
    );
    assert.deepEqual(JSON.parse(one.stdout), receipt);
    assert.doesNotMatch(receiptBytes.toString("utf8"), /private|BEGIN|key|pem/i);
    assert.doesNotMatch(one.stdout + one.stderr, /BEGIN PRIVATE KEY/);
    verifyWithOpenSsl(first.directory, first.privateKey, manifest, imagePath);
  } finally {
    fs.rmSync(first.directory, { recursive: true, force: true });
    fs.rmSync(second.directory, { recursive: true, force: true });
  }
});

test("filesystem aliases, symlinks, weak keys and oversized images fail closed", () => {
  const cases = [];
  try {
    {
      const f = fixture();
      const linked = path.join(f.directory, "image-link.bin");
      fs.symlinkSync(f.image, linked);
      cases.push(f);
      const result = packageRelease(f, { image: linked });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /invalid_image/);
    }
    {
      const f = fixture();
      const linked = path.join(f.directory, "key-link.pem");
      fs.symlinkSync(f.privateKey, linked);
      cases.push(f);
      const result = packageRelease(f, { privateKey: linked });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /invalid_private_key/);
    }
    {
      const f = fixture();
      const realOutput = path.join(f.directory, "real-public");
      const linkedOutput = path.join(f.directory, "linked-public");
      fs.mkdirSync(realOutput);
      fs.symlinkSync(realOutput, linkedOutput);
      cases.push(f);
      const result = packageRelease(f, { output: linkedOutput });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /invalid_output_root/);
    }
    {
      const f = fixture();
      fs.chmodSync(f.privateKey, 0o644);
      cases.push(f);
      const result = packageRelease(f);
      assert.equal(result.status, 2);
      assert.match(result.stderr, /weak_private_key_permissions/);
      assert.doesNotMatch(result.stderr, /BEGIN PRIVATE KEY/);
    }
    {
      const f = fixture();
      const insideKey = path.join(f.output, "release.pem");
      fs.copyFileSync(f.privateKey, insideKey);
      fs.chmodSync(insideKey, 0o600);
      cases.push(f);
      const result = packageRelease(f, { privateKey: insideKey });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /private_key_inside_output/);
    }
    {
      const f = fixture();
      const huge = path.join(f.directory, "huge.bin");
      fs.closeSync(fs.openSync(huge, "w"));
      fs.truncateSync(huge, 8 * 1024 * 1024 + 1);
      cases.push(f);
      const result = packageRelease(f, { image: huge });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /invalid_image/);
    }
    {
      const f = fixture();
      const destination = path.join(
        releaseDirectory(f),
        "inkloop-idf-m5-papercolor-c151-0.4.0-beta.2.bin",
      );
      fs.mkdirSync(path.dirname(destination), { recursive: true });
      fs.copyFileSync(f.image, destination);
      cases.push(f);
      const result = packageRelease(f, { image: destination });
      assert.equal(result.status, 2);
      assert.match(result.stderr, /source_output_alias/);
    }
  } finally {
    for (const f of cases) fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("invalid URL, SKU, SemVer and relative output paths publish nothing", () => {
  const f = fixture();
  try {
    const invalid = [
      { baseUrl: "http://updates.example.com/ota", error: "invalid_public_base_url" },
      { baseUrl: "https://Updates.example.com/ota", error: "invalid_public_base_url" },
      { baseUrl: "https://user@updates.example.com/ota", error: "invalid_public_base_url" },
      { baseUrl: "https://127.0.0.1/ota", error: "invalid_public_base_url" },
      { baseUrl: "https://localhost/ota", error: "invalid_public_base_url" },
      { baseUrl: "https://updates.example.com/ota/../release", error: "invalid_public_base_url" },
      { baseUrl: "https://updates.example.com/ota?token=x", error: "invalid_public_base_url" },
      { boardSku: "../c151", error: "invalid_board_sku" },
      { boardSku: ".", error: "invalid_board_sku" },
      { version: "01.0.0", error: "invalid_firmware_version" },
      { version: "1.0.0-01", error: "invalid_firmware_version" },
      { version: "4294967296.0.0", error: "invalid_firmware_version" },
      { output: "relative/public", error: "invalid_output_root_path" },
    ];
    for (const entry of invalid) {
      const result = packageRelease(f, entry);
      assert.equal(result.status, 2, `${JSON.stringify(entry)}: ${result.stderr}`);
      assert.match(result.stderr, new RegExp(entry.error));
    }
    assert.deepEqual(fs.readdirSync(f.output), []);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("overwrite, downgrade, ambiguous history and signer failure are non-publishing", () => {
  const f = fixture();
  const broken = fixture("inkloop-ota-broken-signer-");
  const ambiguous = fixture("inkloop-ota-ambiguous-");
  try {
    const beta2 = packageRelease(f);
    assert.equal(beta2.status, 0, beta2.stderr);
    const before = fs.readFileSync(path.join(releaseDirectory(f), "manifest.json"));
    const repeated = packageRelease(f);
    assert.equal(repeated.status, 2);
    assert.match(repeated.stderr, /non_increasing_release/);
    const downgrade = packageRelease(f, { version: "0.4.0-beta.1" });
    assert.equal(downgrade.status, 2);
    assert.match(downgrade.stderr, /non_increasing_release/);
    assert.deepEqual(fs.readFileSync(path.join(releaseDirectory(f), "manifest.json")), before);
    const beta3 = packageRelease(f, { version: "0.4.0-beta.3" });
    assert.equal(beta3.status, 0, beta3.stderr);

    const ambiguousBoard = path.join(ambiguous.output, "m5-papercolor-c151");
    fs.mkdirSync(path.join(ambiguousBoard, "0.4.0-beta.1"), { recursive: true });
    const refused = packageRelease(ambiguous);
    assert.equal(refused.status, 2);
    assert.match(refused.stderr, /existing_manifest|ambiguous_existing_release/);
    assert.equal(fs.existsSync(releaseDirectory(ambiguous)), false);

    const rsaKey = path.join(broken.directory, "rsa.pem");
    assert.equal(run("openssl", ["genpkey", "-algorithm", "RSA", "-out", rsaKey]).status, 0);
    fs.chmodSync(rsaKey, 0o600);
    const signerFailure = packageRelease(broken, { privateKey: rsaKey });
    assert.equal(signerFailure.status, 2);
    assert.match(signerFailure.stderr, /signer_failed/);
    const board = path.join(broken.output, "m5-papercolor-c151");
    assert.deepEqual(fs.existsSync(board) ? fs.readdirSync(board) : [], []);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
    fs.rmSync(broken.directory, { recursive: true, force: true });
    fs.rmSync(ambiguous.directory, { recursive: true, force: true });
  }
});
