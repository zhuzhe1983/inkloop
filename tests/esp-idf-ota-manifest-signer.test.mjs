import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const root = process.cwd();
const signer = path.join(root, "firmware/inkloop-idf/tools/sign_ota_manifest.py");

function run(command, args, options = {}) {
  return spawnSync(command, args, { encoding: "utf8", ...options });
}

test("release tool emits an exact verifiable Ed25519 OTA manifest", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "inkloop-ota-sign-"));
  try {
    const privateKey = path.join(directory, "release.pem");
    const publicKey = path.join(directory, "release.pub.hex");
    const image = path.join(directory, "inkloop.bin");
    const manifestPath = path.join(directory, "manifest.json");
    assert.equal(run("openssl", ["genpkey", "-algorithm", "ED25519", "-out", privateKey]).status, 0);
    fs.writeFileSync(image, crypto.randomBytes(8192));
    const signed = run("python3", [
      signer,
      "--image", image,
      "--image-url", "https://inkloop.mess.host/firmware/m5-papercolor/idf-test/0.4.0-beta.2/inkloop_idf.bin",
      "--board-sku", "m5-papercolor-c151",
      "--firmware-version", "0.4.0-beta.2",
      "--private-key", privateKey,
      "--manifest-out", manifestPath,
      "--public-key-out", publicKey,
    ]);
    assert.equal(signed.status, 0, signed.stderr);
    const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
    assert.deepEqual(Object.keys(manifest), [
      "schema_version", "board_sku", "firmware_version", "image_url",
      "image_size", "image_sha256", "signature_policy", "detached_signature",
    ]);
    assert.equal(manifest.schema_version, 1);
    assert.equal(manifest.image_size, 8192);
    assert.equal(manifest.image_sha256, crypto.createHash("sha256").update(fs.readFileSync(image)).digest("hex"));
    assert.match(manifest.detached_signature, /^[0-9a-f]{128}$/);
    assert.match(fs.readFileSync(publicKey, "utf8").trim(), /^[0-9a-f]{64}$/);

    const board = Buffer.from(manifest.board_sku, "ascii");
    const version = Buffer.from(manifest.firmware_version, "ascii");
    const policy = Buffer.from(manifest.signature_policy, "ascii");
    const size = Buffer.alloc(8);
    size.writeBigUInt64LE(BigInt(manifest.image_size));
    const canonical = Buffer.concat([
      Buffer.from("INKLOOP-OTA-MANIFEST-V1", "ascii"),
      Buffer.from([1, 0, board.length]), board,
      Buffer.from([version.length]), version,
      size, Buffer.from(manifest.image_sha256, "hex"),
      Buffer.from([policy.length]), policy,
    ]);
    const publicPem = path.join(directory, "release.pub.pem");
    assert.equal(run("openssl", ["pkey", "-in", privateKey, "-pubout", "-out", publicPem]).status, 0);
    const signedMessage = Buffer.concat([
      canonical,
      Buffer.from(manifest.image_sha256, "hex"),
    ]);
    const canonicalPath = path.join(directory, "manifest.canonical-and-digest");
    fs.writeFileSync(canonicalPath, signedMessage);
    fs.writeFileSync(path.join(directory, "signature.bin"), Buffer.from(manifest.detached_signature, "hex"));
    const verified = run("openssl", [
      "pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", publicPem,
      "-sigfile", path.join(directory, "signature.bin"),
      "-in", canonicalPath,
    ]);
    assert.equal(verified.status, 0, verified.stderr);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test("release tool fails closed on non-HTTPS URL and writes no manifest", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "inkloop-ota-reject-"));
  try {
    const privateKey = path.join(directory, "release.pem");
    const image = path.join(directory, "inkloop.bin");
    const manifestPath = path.join(directory, "manifest.json");
    assert.equal(run("openssl", ["genpkey", "-algorithm", "ED25519", "-out", privateKey]).status, 0);
    fs.writeFileSync(image, Buffer.from("image"));
    const rejected = run("python3", [
      signer, "--image", image, "--image-url", "http://inkloop.mess.host/image.bin",
      "--board-sku", "m5-papercolor-c151", "--firmware-version", "0.4.0-beta.2",
      "--private-key", privateKey, "--manifest-out", manifestPath,
    ]);
    assert.notEqual(rejected.status, 0);
    assert.equal(fs.existsSync(manifestPath), false);
    assert.doesNotMatch(rejected.stderr, /BEGIN PRIVATE KEY|[0-9a-f]{128}/i);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
