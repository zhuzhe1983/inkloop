#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";

import {
  PAPER_COLOR_COMPLETE_FLASH_LAYOUT,
  PAPER_COLOR_EMPTY_LITTLEFS_SHA256,
  PAPER_COLOR_FLASH_SIZE,
  PAPER_COLOR_LITTLEFS_SIZE,
  PAPER_COLOR_SERVER_SLOT,
  validatePaperColorFirmwareManifest,
} from "../app/lib/esp32-firmware-layout.js";

const repositoryRoot = fileURLToPath(new URL("../", import.meta.url));
const firmwareRoot = resolve(repositoryRoot, "firmware/m5-papercolor");
const publicFirmwareRoot = resolve(repositoryRoot, "public/firmware/m5-papercolor");
const defaultBuildDir = resolve(firmwareRoot, ".pio/build/m5stack-papercolor");
const canonicalLittleFsTemplate = resolve(
  firmwareRoot,
  "assets/empty-littlefs.bin.gz.b64",
);
const partitionCsv = resolve(firmwareRoot, "default_16MB.csv");

function fail(message) {
  throw new Error(message);
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function parseNumber(raw, label) {
  const value = Number(raw.trim());
  if (!Number.isSafeInteger(value) || value < 0) fail(`invalid ${label}: ${raw}`);
  return value;
}

function parseArguments(argv) {
  const values = {};
  const allowed = new Set([
    "--version",
    "--build-dir",
    "--output-dir",
    "--base-path",
    "--boot-app0",
  ]);
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!allowed.has(key) || value === undefined) fail(`unknown or incomplete argument: ${key}`);
    values[key.slice(2)] = value;
  }
  const version = values.version?.trim();
  if (!version || !/^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$/.test(version)) {
    fail("--version is required and must be safe for a channel path");
  }
  const buildDir = resolve(repositoryRoot, values["build-dir"] || defaultBuildDir);
  const outputDir = resolve(
    repositoryRoot,
    values["output-dir"] || `outputs/firmware/m5-papercolor/test-channel/${version}`,
  );
  const basePath = values["base-path"] ||
    `/firmware/m5-papercolor/test-channel/${version}`;
  if (!/^\/[a-zA-Z0-9/._-]+$/.test(basePath) || basePath.endsWith("/")) {
    fail("--base-path must be an absolute URL path without a trailing slash");
  }
  return {
    version,
    buildDir,
    outputDir,
    basePath,
    bootApp0: resolve(repositoryRoot, values["boot-app0"] ||
      resolve(publicFirmwareRoot, "boot_app0.bin")),
  };
}

function parsePartitionCsv(source) {
  const entries = new Map();
  for (const rawLine of source.split(/\r?\n/)) {
    const line = rawLine.replace(/#.*/, "").trim();
    if (!line) continue;
    const [name, type, subtype, offsetRaw, sizeRaw] = line.split(",").map((part) => part.trim());
    if (!name || !type || !subtype || !offsetRaw || !sizeRaw || entries.has(name)) {
      fail(`invalid partition CSV row: ${rawLine}`);
    }
    entries.set(name, {
      name,
      type,
      subtype,
      offset: parseNumber(offsetRaw, `${name} offset`),
      size: parseNumber(sizeRaw, `${name} size`),
    });
  }
  return entries;
}

const expectedPartitions = Object.freeze([
  Object.freeze({ name: "nvs", type: "data", subtype: "nvs", offset: 0x9000, size: 0x5000 }),
  Object.freeze({ name: "otadata", type: "data", subtype: "ota", offset: 0xE000, size: 0x2000 }),
  Object.freeze({ name: "app0", type: "app", subtype: "ota_0", offset: 0x10000, size: 0x640000 }),
  Object.freeze({ name: "app1", type: "app", subtype: "ota_1", offset: 0x650000, size: 0x640000 }),
  Object.freeze({ name: "spiffs", type: "data", subtype: "spiffs", offset: 0xC90000, size: 0x360000 }),
  Object.freeze({ name: "coredump", type: "data", subtype: "coredump", offset: 0xFF0000, size: 0x10000 }),
]);

function validatePartitionCsv(entries) {
  if (entries.size !== expectedPartitions.length) fail("partition CSV has unexpected entries");
  for (const expected of expectedPartitions) {
    const actual = entries.get(expected.name);
    for (const field of ["type", "subtype", "offset", "size"]) {
      if (!actual || actual[field] !== expected[field]) {
        fail(`partition CSV mismatch: ${expected.name}.${field}`);
      }
    }
  }
}

const partitionBinaryTypes = Object.freeze({ app: 0x00, data: 0x01 });
const partitionBinarySubtypes = Object.freeze({
  nvs: 0x02,
  ota: 0x00,
  ota_0: 0x10,
  ota_1: 0x11,
  spiffs: 0x82,
  coredump: 0x03,
});

function parsePartitionBinary(bytes) {
  const entries = new Map();
  for (let offset = 0; offset + 32 <= bytes.length; offset += 32) {
    const magic = bytes.readUInt16LE(offset);
    if (magic === 0xFFFF || magic === 0xEBEB) break;
    if (magic !== 0x50AA) fail(`invalid partition binary magic at ${offset}`);
    const nameBytes = bytes.subarray(offset + 12, offset + 28);
    const terminator = nameBytes.indexOf(0);
    const name = nameBytes.subarray(0, terminator < 0 ? nameBytes.length : terminator).toString("ascii");
    if (!name || entries.has(name)) fail(`invalid partition binary label at ${offset}`);
    entries.set(name, {
      name,
      type: bytes[offset + 2],
      subtype: bytes[offset + 3],
      offset: bytes.readUInt32LE(offset + 4),
      size: bytes.readUInt32LE(offset + 8),
    });
  }
  return entries;
}

function validatePartitionBinary(bytes) {
  const entries = parsePartitionBinary(bytes);
  if (entries.size !== expectedPartitions.length) fail("partition binary has unexpected entries");
  for (const expected of expectedPartitions) {
    const actual = entries.get(expected.name);
    if (!actual || actual.type !== partitionBinaryTypes[expected.type] ||
        actual.subtype !== partitionBinarySubtypes[expected.subtype] ||
        actual.offset !== expected.offset || actual.size !== expected.size) {
      fail(`partition binary mismatch: ${expected.name}`);
    }
  }
}

function validateServerSlot(application) {
  const marker = Buffer.from(PAPER_COLOR_SERVER_SLOT.marker, "ascii");
  const first = application.indexOf(marker);
  if (first < 0 || application.indexOf(marker, first + 1) >= 0) {
    fail("application must contain exactly one server URL slot");
  }
  if (first + PAPER_COLOR_SERVER_SLOT.length > application.length ||
      !application.subarray(first + marker.length, first + PAPER_COLOR_SERVER_SLOT.length)
        .every((byte) => byte === 0)) {
    fail("application server URL slot must have zero-filled padding");
  }
}

async function loadCanonicalLittleFs() {
  const encoded = (await readFile(canonicalLittleFsTemplate, "utf8")).replace(/\s/g, "");
  const bytes = gunzipSync(Buffer.from(encoded, "base64"));
  if (bytes.length !== PAPER_COLOR_LITTLEFS_SIZE ||
      sha256(bytes) !== PAPER_COLOR_EMPTY_LITTLEFS_SHA256) {
    fail("canonical empty LittleFS template failed size/hash validation");
  }
  return bytes;
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  validatePartitionCsv(parsePartitionCsv(await readFile(partitionCsv, "utf8")));

  const inputs = new Map([
    ["bootloader", await readFile(resolve(options.buildDir, "bootloader.bin"))],
    ["partitions", await readFile(resolve(options.buildDir, "partitions.bin"))],
    ["boot_app0", await readFile(options.bootApp0)],
    ["app", await readFile(resolve(options.buildDir, "firmware.bin"))],
    ["littlefs", await loadCanonicalLittleFs()],
  ]);
  validatePartitionBinary(inputs.get("partitions"));
  validateServerSlot(inputs.get("app"));

  const outputNames = Object.freeze({
    bootloader: "bootloader.bin",
    partitions: "partitions.bin",
    boot_app0: "boot_app0.bin",
    app: "firmware.bin",
    littlefs: "littlefs.bin",
  });
  const files = PAPER_COLOR_COMPLETE_FLASH_LAYOUT.map(({ role, offset }) => {
    const bytes = inputs.get(role);
    return {
      role,
      path: `${options.basePath}/${outputNames[role]}`,
      offset,
      size: bytes.length,
      sha256: sha256(bytes),
    };
  });
  const manifest = {
    name: "Inkloop M5 PaperColor complete flash package",
    chipFamily: "ESP32-S3",
    version: options.version,
    baudRate: 460800,
    formatVersion: 2,
    completeFlash: true,
    flashSize: PAPER_COLOR_FLASH_SIZE,
    partitionTable: "default_16MB.csv",
    serverSlot: PAPER_COLOR_SERVER_SLOT,
    files,
  };
  validatePaperColorFirmwareManifest(manifest);

  await mkdir(options.outputDir, { recursive: true });
  await Promise.all(files.map((file) =>
    writeFile(resolve(options.outputDir, outputNames[file.role]), inputs.get(file.role)),
  ));
  await writeFile(
    resolve(options.outputDir, "manifest.json"),
    `${JSON.stringify(manifest, null, 2)}\n`,
  );
  process.stdout.write(`${resolve(options.outputDir, "manifest.json")}\n`);
}

await main();
