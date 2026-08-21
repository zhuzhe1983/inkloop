import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import test from "node:test";

import {
  PAPER_COLOR_COMPLETE_FLASH_LAYOUT,
  PAPER_COLOR_EMPTY_LITTLEFS_SHA256,
  PAPER_COLOR_FLASH_SIZE,
  PAPER_COLOR_LITTLEFS_OFFSET,
  PAPER_COLOR_LITTLEFS_SIZE,
  validatePaperColorFirmwareFiles,
  validatePaperColorFirmwareManifest,
} from "../app/lib/esp32-firmware-layout.js";

const execFileAsync = promisify(execFile);
const repositoryRoot = new URL("../", import.meta.url);
const publicRoot = new URL("../public/", import.meta.url);
const stableManifestUrl = new URL("firmware/m5-papercolor/manifest.json", publicRoot);
const betaManifestUrl = new URL(
  "firmware/m5-papercolor/test-channel/0.3.0-beta.1/manifest.json",
  publicRoot,
);
const zeroHash = "0".repeat(64);

function completeManifest() {
  const sizes = {
    bootloader: 0x4000,
    partitions: 0x1000,
    boot_app0: 0x2000,
    app: 0x200000,
    littlefs: PAPER_COLOR_LITTLEFS_SIZE,
  };
  return {
    name: "fixture",
    chipFamily: "ESP32-S3",
    version: "test",
    baudRate: 460800,
    formatVersion: 2,
    completeFlash: true,
    flashSize: PAPER_COLOR_FLASH_SIZE,
    serverSlot: { marker: "INKLOOP_API_URL_SLOT::", length: 192 },
    files: PAPER_COLOR_COMPLETE_FLASH_LAYOUT.map(({ role, offset }) => ({
      role,
      path: `/firmware/m5-papercolor/test/${role}.bin`,
      offset,
      size: sizes[role],
      sha256: zeroHash,
    })),
  };
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

test("published 0.2 manifest remains a compatible four-segment legacy package", async () => {
  const manifest = JSON.parse(await readFile(stableManifestUrl, "utf8"));
  assert.equal(manifest.version, "0.2.0");
  assert.equal(manifest.files.length, 4);
  assert.equal(manifest.formatVersion, undefined);
  assert.equal(manifest.completeFlash, undefined);
  assert.equal(validatePaperColorFirmwareManifest(manifest).completeFlash, false);
  const actualSizes = await Promise.all(manifest.files.map(async (file) =>
    (await readFile(new URL(file.path.replace(/^\//, ""), publicRoot))).length,
  ));
  assert.doesNotThrow(() => validatePaperColorFirmwareFiles(manifest, actualSizes));
});

test("published beta is a complete five-segment package and excludes NVS", async () => {
  const manifest = JSON.parse(await readFile(betaManifestUrl, "utf8"));
  assert.equal(manifest.version, "0.3.0-beta.1");
  assert.equal(validatePaperColorFirmwareManifest(manifest).completeFlash, true);
  assert.equal(manifest.files.length, 5);
  assert.equal(manifest.files.some((file) => file.role === "nvs"), false);
  const actualSizes = [];
  for (const file of manifest.files) {
    const bytes = await readFile(new URL(file.path.replace(/^\//, ""), publicRoot));
    actualSizes.push(bytes.length);
    assert.equal(createHash("sha256").update(bytes).digest("hex"), file.sha256, file.role);
  }
  assert.doesNotThrow(() => validatePaperColorFirmwareFiles(manifest, actualSizes));

  const catalog = await readFile(new URL("../app/lib/device-catalog.ts", import.meta.url), "utf8");
  assert.match(catalog, /firmwareManifest: "\/firmware\/m5-papercolor\/test-channel\/0\.3\.0-beta\.1\/manifest\.json"/);
});

test("complete-flash v2 requires the exact five hashed and sized segments", () => {
  const valid = completeManifest();
  assert.equal(validatePaperColorFirmwareManifest(valid).completeFlash, true);

  const missingLittleFs = clone(valid);
  missingLittleFs.files.pop();
  assert.throws(
    () => validatePaperColorFirmwareManifest(missingLittleFs),
    /invalid_complete_flash_schema/,
  );

  const wrongOffset = clone(valid);
  wrongOffset.files.find((file) => file.role === "littlefs").offset += 0x1000;
  assert.throws(
    () => validatePaperColorFirmwareManifest(wrongOffset),
    /invalid_complete_flash_segment/,
  );

  const missingSize = clone(valid);
  delete missingSize.files.find((file) => file.role === "app").size;
  assert.throws(
    () => validatePaperColorFirmwareManifest(missingSize),
    /invalid_complete_flash_segment/,
  );

  const badHash = clone(valid);
  badHash.files.find((file) => file.role === "partitions").sha256 = "not-a-hash";
  assert.throws(() => validatePaperColorFirmwareManifest(badHash), /invalid_firmware_sha256/);

  const traversal = clone(valid);
  traversal.files.find((file) => file.role === "app").path =
    "/firmware/m5-papercolor/test/../firmware.bin";
  assert.throws(() => validatePaperColorFirmwareManifest(traversal), /invalid_firmware_path/);

  const overlap = clone(valid);
  overlap.files.find((file) => file.role === "app").size =
    PAPER_COLOR_LITTLEFS_OFFSET - 0x10000 + 1;
  assert.throws(() => validatePaperColorFirmwareManifest(overlap), /overlapping_firmware_segments/);

  const partitionsCoverNvs = clone(valid);
  partitionsCoverNvs.files.find((file) => file.role === "partitions").size = 0x6000;
  assert.throws(
    () => validatePaperColorFirmwareManifest(partitionsCoverNvs),
    /firmware_segment_outside_role_region/,
  );

  const appCoversOtaSlot = clone(valid);
  appCoversOtaSlot.files.find((file) => file.role === "app").size = 0x640001;
  assert.throws(
    () => validatePaperColorFirmwareManifest(appCoversOtaSlot),
    /firmware_segment_outside_role_region/,
  );

  const nvsSegment = clone(valid);
  nvsSegment.files.find((file) => file.role === "littlefs").role = "nvs";
  assert.throws(() => validatePaperColorFirmwareManifest(nvsSegment), /invalid_firmware_role/);

  const actualSizes = valid.files.map((file) => file.size);
  actualSizes[0] += 1;
  assert.throws(() => validatePaperColorFirmwareFiles(valid, actualSizes), /firmware_size_mismatch/);
});

test("legacy actual byte lengths cannot cover NVS, adjacent segments, app1, or flash bounds", async () => {
  const manifest = JSON.parse(await readFile(stableManifestUrl, "utf8"));
  const stableSizes = await Promise.all(manifest.files.map(async (file) =>
    (await readFile(new URL(file.path.replace(/^\//, ""), publicRoot))).length,
  ));

  const partitionsCoverNvs = [...stableSizes];
  partitionsCoverNvs[1] = 0x1001;
  assert.throws(
    () => validatePaperColorFirmwareFiles(manifest, partitionsCoverNvs),
    /firmware_segment_outside_role_region/,
  );

  const bootloaderCoversAdjacent = [...stableSizes];
  bootloaderCoversAdjacent[0] = 0x8001;
  assert.throws(
    () => validatePaperColorFirmwareFiles(manifest, bootloaderCoversAdjacent),
    /overlapping_firmware_segments/,
  );

  const bootAppCoversAdjacent = [...stableSizes];
  bootAppCoversAdjacent[2] = 0x2001;
  assert.throws(
    () => validatePaperColorFirmwareFiles(manifest, bootAppCoversAdjacent),
    /overlapping_firmware_segments/,
  );

  const appCoversOtaSlot = [...stableSizes];
  appCoversOtaSlot[3] = 0x640001;
  assert.throws(
    () => validatePaperColorFirmwareFiles(manifest, appCoversOtaSlot),
    /firmware_segment_outside_role_region/,
  );

  const appExceedsFlash = [...stableSizes];
  appExceedsFlash[3] = PAPER_COLOR_FLASH_SIZE;
  assert.throws(
    () => validatePaperColorFirmwareFiles(manifest, appExceedsFlash),
    /firmware_segment_out_of_bounds/,
  );
});

test("local packager emits the deterministic empty LittleFS segment and full hashes", async () => {
  const temporaryRoot = await mkdtemp(join(tmpdir(), "inkloop-papercolor-package-"));
  const firstOutput = join(temporaryRoot, "first");
  const secondOutput = join(temporaryRoot, "second");
  const script = new URL("../scripts/package-papercolor-test-channel.mjs", import.meta.url);
  const stableBuild = new URL("../public/firmware/m5-papercolor", import.meta.url);
  const commonArguments = [
    fileURLToPath(script),
    "--version", "test-fixture",
    "--build-dir", fileURLToPath(stableBuild),
    "--base-path", "/firmware/m5-papercolor/test-channel/test-fixture",
  ];
  try {
    await execFileAsync(process.execPath, [...commonArguments, "--output-dir", firstOutput], {
      cwd: fileURLToPath(repositoryRoot),
    });
    await execFileAsync(process.execPath, [...commonArguments, "--output-dir", secondOutput], {
      cwd: fileURLToPath(repositoryRoot),
    });

    const firstManifestBytes = await readFile(join(firstOutput, "manifest.json"));
    const secondManifestBytes = await readFile(join(secondOutput, "manifest.json"));
    assert.deepEqual(firstManifestBytes, secondManifestBytes, "manifest must be deterministic");
    const manifest = JSON.parse(firstManifestBytes);
    assert.equal(validatePaperColorFirmwareManifest(manifest).completeFlash, true);
    assert.equal(manifest.files.length, 5);
    assert.deepEqual(
      manifest.files.map(({ role, offset }) => ({ role, offset })),
      PAPER_COLOR_COMPLETE_FLASH_LAYOUT,
    );
    assert.equal(manifest.files.some((file) => file.role === "nvs"), false);

    const ordered = [...manifest.files].sort((left, right) => left.offset - right.offset);
    for (let index = 0; index < ordered.length; index += 1) {
      const file = ordered[index];
      const name = file.path.split("/").at(-1);
      const firstBytes = await readFile(join(firstOutput, name));
      const secondBytes = await readFile(join(secondOutput, name));
      assert.deepEqual(firstBytes, secondBytes, `${file.role} must be deterministic`);
      assert.equal(firstBytes.length, file.size, `${file.role} size`);
      assert.equal(createHash("sha256").update(firstBytes).digest("hex"), file.sha256, file.role);
      if (index + 1 < ordered.length) {
        assert.ok(file.offset + file.size <= ordered[index + 1].offset, `${file.role} overlap`);
      }
    }

    const filesystem = manifest.files.find((file) => file.role === "littlefs");
    const littleFs = await readFile(join(firstOutput, "littlefs.bin"));
    assert.equal(filesystem.offset, PAPER_COLOR_LITTLEFS_OFFSET);
    assert.equal(filesystem.size, PAPER_COLOR_LITTLEFS_SIZE);
    assert.equal(filesystem.sha256, PAPER_COLOR_EMPTY_LITTLEFS_SHA256);
    assert.equal(littleFs.subarray(8, 17).toString("ascii"), "littlefs/");
    assert.equal(littleFs.subarray(0x1008, 0x1011).toString("ascii"), "littlefs/");
  } finally {
    await rm(temporaryRoot, { recursive: true, force: true });
  }
});

test("WebSerial validates manifest and byte sizes before writing every declared segment", async () => {
  const source = await readFile(new URL("../app/lib/esp32-device.ts", import.meta.url), "utf8");
  const flash = source.slice(source.indexOf("export async function flashM5PaperColor"));
  const manifestValidation = flash.indexOf("validatePaperColorFirmwareManifest(manifest)");
  const fileMapping = flash.indexOf("manifest.files.map(");
  const sizeValidation = flash.indexOf("validatePaperColorFirmwareFiles(");
  const hashValidation = flash.indexOf("await sha256Hex(bytes)");
  const fileArray = flash.indexOf("fileArray: files");

  assert.ok(manifestValidation >= 0 && manifestValidation < fileMapping);
  assert.ok(fileMapping < sizeValidation && sizeValidation < hashValidation);
  assert.ok(hashValidation < fileArray);
  assert.match(flash, /return \{ data: bytes, address: file\.offset \}/);
});
