export const PAPER_COLOR_FLASH_SIZE = 0x1000000;
export const PAPER_COLOR_LITTLEFS_OFFSET = 0xC90000;
export const PAPER_COLOR_LITTLEFS_SIZE = 0x360000;
export const PAPER_COLOR_EMPTY_LITTLEFS_SHA256 =
  "24d8863ba58675b6d82f13514d3ac475599ee18c7e503a61603e7b23a6540b70";
export const PAPER_COLOR_SERVER_SLOT = Object.freeze({
  marker: "INKLOOP_API_URL_SLOT::",
  length: 192,
});

export const PAPER_COLOR_COMPLETE_FLASH_LAYOUT = Object.freeze([
  Object.freeze({ role: "bootloader", offset: 0x000000 }),
  Object.freeze({ role: "partitions", offset: 0x008000 }),
  Object.freeze({ role: "boot_app0", offset: 0x00E000 }),
  Object.freeze({ role: "app", offset: 0x010000 }),
  Object.freeze({ role: "littlefs", offset: PAPER_COLOR_LITTLEFS_OFFSET }),
]);

export const PAPER_COLOR_FIRMWARE_REGIONS = Object.freeze([
  Object.freeze({ role: "bootloader", offset: 0x000000, end: 0x008000 }),
  Object.freeze({ role: "partitions", offset: 0x008000, end: 0x009000 }),
  Object.freeze({ role: "boot_app0", offset: 0x00E000, end: 0x010000 }),
  Object.freeze({ role: "app", offset: 0x010000, end: 0x650000 }),
  Object.freeze({
    role: "littlefs",
    offset: PAPER_COLOR_LITTLEFS_OFFSET,
    end: 0xFF0000,
    exactSize: PAPER_COLOR_LITTLEFS_SIZE,
  }),
]);

const SHA256_PATTERN = /^[a-f0-9]{64}$/i;

function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}

function isSafeInteger(value) {
  return Number.isSafeInteger(value) && value >= 0;
}

function isSafeFirmwarePath(path) {
  return typeof path === "string" &&
    /^\/firmware\/m5-papercolor\/[a-zA-Z0-9._/-]+$/.test(path) &&
    !path.includes("//") &&
    !path.split("/").some((segment) => segment === "." || segment === "..");
}

const regionByRole = new Map(
  PAPER_COLOR_FIRMWARE_REGIONS.map((region) => [region.role, region]),
);
const regionByOffset = new Map(
  PAPER_COLOR_FIRMWARE_REGIONS.map((region) => [region.offset, region]),
);

function validateSegmentExtents(files) {
  const ordered = [...files].sort((left, right) => left.offset - right.offset);
  for (let index = 0; index < ordered.length; index += 1) {
    const current = ordered[index];
    const end = current.offset + current.size;
    if (!Number.isSafeInteger(end) || end > PAPER_COLOR_FLASH_SIZE) {
      fail("firmware_segment_out_of_bounds");
    }
    if (index + 1 < ordered.length && end > ordered[index + 1].offset) {
      fail("overlapping_firmware_segments");
    }
  }
  for (const file of files) {
    const region = regionByRole.get(file.role);
    if (!region || file.offset !== region.offset || file.offset + file.size > region.end) {
      fail("firmware_segment_outside_role_region");
    }
    if (region.exactSize !== undefined && file.size !== region.exactSize) {
      fail("invalid_littlefs_size");
    }
  }
}

/**
 * Validates both the published 0.2 four-segment schema and the complete-flash
 * v2 schema. Legacy manifests intentionally remain size/role optional.
 */
export function validatePaperColorFirmwareManifest(manifest) {
  if (!manifest || typeof manifest !== "object" || manifest.chipFamily !== "ESP32-S3") {
    fail("invalid_manifest_identity");
  }
  if (typeof manifest.version !== "string" || manifest.version.trim() === "" ||
      !Number.isSafeInteger(manifest.baudRate) || manifest.baudRate <= 0 ||
      !manifest.serverSlot || manifest.serverSlot.marker !== PAPER_COLOR_SERVER_SLOT.marker ||
      manifest.serverSlot.length !== PAPER_COLOR_SERVER_SLOT.length) {
    fail("invalid_manifest_metadata");
  }
  if (!Array.isArray(manifest.files) || manifest.files.length === 0) {
    fail("missing_manifest_files");
  }
  const completeFlash = manifest.completeFlash === true || manifest.formatVersion >= 2;
  const seenPaths = new Set();
  const seenOffsets = new Set();
  for (const file of manifest.files) {
    if (!file || typeof file !== "object" || !isSafeFirmwarePath(file.path) ||
        seenPaths.has(file.path)) {
      fail("invalid_firmware_path");
    }
    seenPaths.add(file.path);
    if (!isSafeInteger(file.offset) || seenOffsets.has(file.offset)) {
      fail("invalid_firmware_offset");
    }
    seenOffsets.add(file.offset);
    if (typeof file.sha256 !== "string" || !SHA256_PATTERN.test(file.sha256)) {
      fail("invalid_firmware_sha256");
    }
    if (file.size !== undefined && (!isSafeInteger(file.size) || file.size === 0)) {
      fail("invalid_firmware_size");
    }
  }
  if (!completeFlash) {
    for (const file of manifest.files) {
      if (!regionByOffset.has(file.offset)) fail("invalid_legacy_firmware_offset");
    }
    return { completeFlash: false, files: manifest.files };
  }

  if (manifest.formatVersion !== 2 || manifest.completeFlash !== true ||
      manifest.flashSize !== PAPER_COLOR_FLASH_SIZE ||
      manifest.files.length !== PAPER_COLOR_COMPLETE_FLASH_LAYOUT.length) {
    fail("invalid_complete_flash_schema");
  }
  const expectedByRole = new Map(
    PAPER_COLOR_COMPLETE_FLASH_LAYOUT.map((entry) => [entry.role, entry]),
  );
  const seenRoles = new Set();
  for (const file of manifest.files) {
    if (typeof file.role !== "string" || seenRoles.has(file.role) ||
        !expectedByRole.has(file.role)) {
      fail("invalid_firmware_role");
    }
    seenRoles.add(file.role);
    const expected = expectedByRole.get(file.role);
    if (file.offset !== expected.offset || !isSafeInteger(file.size) || file.size === 0) {
      fail("invalid_complete_flash_segment");
    }
    if (file.role === "littlefs" && file.size !== PAPER_COLOR_LITTLEFS_SIZE) {
      fail("invalid_littlefs_size");
    }
  }
  if (seenRoles.size !== expectedByRole.size || seenRoles.has("nvs")) {
    fail("incomplete_flash_segments");
  }

  validateSegmentExtents(manifest.files);
  return { completeFlash: true, files: manifest.files };
}

export function validatePaperColorFirmwareFiles(manifest, actualSizes) {
  const { completeFlash } = validatePaperColorFirmwareManifest(manifest);
  if (!Array.isArray(actualSizes) || actualSizes.length !== manifest.files.length) {
    fail("invalid_actual_firmware_sizes");
  }
  const resolvedFiles = manifest.files.map((file, index) => {
    const actualSize = actualSizes[index];
    if (!Number.isSafeInteger(actualSize) || actualSize <= 0) {
      fail("invalid_actual_firmware_sizes");
    }
    if ((completeFlash || file.size !== undefined) && file.size !== actualSize) {
      fail("firmware_size_mismatch");
    }
    const role = completeFlash ? file.role : regionByOffset.get(file.offset)?.role;
    if (!role) fail("invalid_legacy_firmware_offset");
    return { ...file, role, size: actualSize };
  });
  validateSegmentExtents(resolvedFiles);
  return true;
}
