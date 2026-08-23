import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { gunzipSync } from "node:zlib";

const manifestUrl = new URL(
  "../fixtures/host-cpp-dependencies.json",
  import.meta.url,
);
const archiveUrl = new URL(
  "../vendor/arduinojson-7.4.3-src.tar.gz",
  import.meta.url,
);
const TAR_BLOCK_BYTES = 512;
const MAXIMUM_EXPANDED_ARCHIVE_BYTES = 2 * 1024 * 1024;

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function requireExactDependency(manifest) {
  const dependency = manifest?.arduinoJson;
  if (
    manifest?.schema !== 1 ||
    dependency?.version !== "7.4.3" ||
    dependency?.tagCommit !== "77771d3c07668e01d8f52acb03910c1110bb373f" ||
    dependency?.upstreamUrl !==
      "https://github.com/bblanchon/ArduinoJson/releases/tag/v7.4.3" ||
    dependency?.archive !== "tests/vendor/arduinojson-7.4.3-src.tar.gz" ||
    dependency?.archiveBytes !== 60406 ||
    dependency?.archiveSha256 !==
      "2b4eb3accb35d2b2b9f688b4f52aeef0cb489236e441a0a77ae1a0e0dc56a739" ||
    dependency?.sourceFiles !== 141 ||
    dependency?.libraryJsonSha256 !==
      "45c8072598d097cf03031ee4f3230031105329903e022a2abd15b1c56157c770" ||
    dependency?.license !== "MIT" ||
    dependency?.licenseSha256 !==
      "4a7ee9c96b28cbf30c5bf7c2d211a0ef57179f0328e68ad7b7fa7d754b7da1a2"
  ) {
    throw new Error(
      "ArduinoJson host dependency manifest is not the reviewed source archive",
    );
  }
  return dependency;
}

function unsafeArchive(detail) {
  throw new Error(
    `ArduinoJson source archive has an unsafe member list: ${detail}`,
  );
}

function tarString(header, offset, length, label) {
  const field = header.subarray(offset, offset + length);
  const terminator = field.indexOf(0);
  const end = terminator === -1 ? field.length : terminator;
  if (terminator !== -1 &&
      field.subarray(terminator + 1).some((value) => value !== 0)) {
    unsafeArchive(`${label} has data after its NUL terminator`);
  }
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(
      field.subarray(0, end),
    );
  } catch {
    unsafeArchive(`${label} is not UTF-8`);
  }
}

function tarOctal(header, offset, length, label) {
  const field = header.subarray(offset, offset + length);
  if ((field[0] & 0x80) !== 0) unsafeArchive(`${label} is not canonical octal`);
  const terminator = field.indexOf(0);
  const end = terminator === -1 ? field.length : terminator;
  if (terminator !== -1 && field.subarray(terminator + 1).some(
    (value) => value !== 0 && value !== 0x20)) {
    unsafeArchive(`${label} has data after its NUL terminator`);
  }
  const digits = field.subarray(0, end).toString("ascii").trim();
  if (!/^[0-7]+$/.test(digits)) unsafeArchive(`${label} is not canonical octal`);
  let value = 0;
  for (const digit of digits) {
    value = value * 8 + Number(digit);
    if (!Number.isSafeInteger(value)) unsafeArchive(`${label} is too large`);
  }
  return value;
}

function verifyTarChecksum(header) {
  const expected = tarOctal(header, 148, 8, "tar checksum");
  let actual = 0;
  for (let at = 0; at < TAR_BLOCK_BYTES; ++at)
    actual += at >= 148 && at < 156 ? 0x20 : header[at];
  if (actual !== expected) unsafeArchive("tar header checksum is invalid");
}

function safeMemberName(header) {
  const name = tarString(header, 0, 100, "tar member name");
  const prefix = tarString(header, 345, 155, "tar member prefix");
  const joined = prefix ? `${prefix}/${name}` : name;
  const canonical = joined.endsWith("/") ? joined.slice(0, -1) : joined;
  const parts = canonical.split("/");
  if (!canonical || joined.startsWith("/") || joined.includes("\\") ||
      [...joined].some((character) => {
        const code = character.codePointAt(0);
        return code < 0x20 || code === 0x7f;
      }) || parts.some((part) => !part || part === "." || part === "..") ||
      !(canonical === "LICENSE.txt" || canonical === "library.json" ||
        canonical === "src" || canonical.startsWith("src/"))) {
    unsafeArchive(`unsafe tar member path: ${JSON.stringify(joined)}`);
  }
  return { canonical, joined };
}

export function validateArchiveMembers(
  compressedArchive,
  expectedSourceFiles = undefined,
) {
  let expanded;
  try {
    expanded = gunzipSync(compressedArchive, {
      maxOutputLength: MAXIMUM_EXPANDED_ARCHIVE_BYTES,
    });
  } catch {
    unsafeArchive("archive is not bounded gzip data");
  }
  if (expanded.length < 2 * TAR_BLOCK_BYTES ||
      expanded.length % TAR_BLOCK_BYTES !== 0) {
    unsafeArchive("tar data is truncated");
  }

  const members = new Set();
  let sourceFiles = 0;
  let offset = 0;
  let zeroBlocks = 0;
  while (offset < expanded.length) {
    const header = expanded.subarray(offset, offset + TAR_BLOCK_BYTES);
    if (header.every((value) => value === 0)) {
      ++zeroBlocks;
      offset += TAR_BLOCK_BYTES;
      if (zeroBlocks < 2) continue;
      if (expanded.subarray(offset).some((value) => value !== 0))
        unsafeArchive("tar data follows the end marker");
      break;
    }
    if (zeroBlocks !== 0) unsafeArchive("tar has a partial end marker");

    verifyTarChecksum(header);
    const type = header[156];
    if (type !== 0 && type !== 0x30 && type !== 0x35) {
      const printableType = type >= 0x20 && type <= 0x7e
        ? String.fromCharCode(type) : `0x${type.toString(16).padStart(2, "0")}`;
      unsafeArchive(
        `tar member type ${JSON.stringify(printableType)} is not a regular ` +
        "file or directory",
      );
    }
    if (tarString(header, 157, 100, "tar link name"))
      unsafeArchive("regular file or directory has a link target");

    const { canonical, joined } = safeMemberName(header);
    if (members.has(canonical)) unsafeArchive(`duplicate tar member: ${canonical}`);
    members.add(canonical);

    const size = tarOctal(header, 124, 12, "tar member size");
    if (type === 0x35 && (size !== 0 || !joined.endsWith("/")))
      unsafeArchive(`directory member is not canonical: ${canonical}`);
    if (type !== 0x35 && joined.endsWith("/"))
      unsafeArchive(`regular-file member is not canonical: ${canonical}`);
    if (type !== 0x35 && canonical.startsWith("src/")) ++sourceFiles;
    const payloadBlocks = Math.ceil(size / TAR_BLOCK_BYTES);
    if (payloadBlocks >
        Math.floor((expanded.length - offset - TAR_BLOCK_BYTES) /
          TAR_BLOCK_BYTES)) {
      unsafeArchive(`tar member is truncated: ${canonical}`);
    }
    offset += TAR_BLOCK_BYTES + payloadBlocks * TAR_BLOCK_BYTES;
  }

  if (zeroBlocks < 2 || members.size === 0 ||
      !members.has("src/ArduinoJson.h") ||
      !members.has("src/ArduinoJson.hpp") ||
      !members.has("LICENSE.txt") ||
      !members.has("library.json")) {
    unsafeArchive("required reviewed members are missing");
  }
  if (expectedSourceFiles !== undefined &&
      sourceFiles !== expectedSourceFiles) {
    unsafeArchive(
      `expected ${expectedSourceFiles} source files, found ${sourceFiles}`,
    );
  }
  return Object.freeze([...members]);
}

export async function materializeArduinoJson() {
  const manifest = JSON.parse(await readFile(manifestUrl, "utf8"));
  const dependency = requireExactDependency(manifest);
  const archive = await readFile(archiveUrl);
  if (
    archive.length !== dependency.archiveBytes ||
    sha256(archive) !== dependency.archiveSha256
  ) {
    throw new Error("ArduinoJson source archive failed its content address");
  }
  validateArchiveMembers(archive, dependency.sourceFiles);

  const extractionRoot = await mkdtemp(
    join(tmpdir(), "inkloop-arduinojson-7.4.3-"),
  );
  try {
    execFileSync("tar", ["-xzf", "-", "-C", extractionRoot], {
      input: archive,
      stdio: "pipe",
    });
    const [libraryBytes, licenseBytes] = await Promise.all([
      readFile(join(extractionRoot, "library.json")),
      readFile(join(extractionRoot, "LICENSE.txt")),
    ]);
    const library = JSON.parse(libraryBytes.toString("utf8"));
    if (
      library?.name !== "ArduinoJson" ||
      library?.version !== dependency.version ||
      sha256(libraryBytes) !== dependency.libraryJsonSha256 ||
      sha256(licenseBytes) !== dependency.licenseSha256
    ) {
      throw new Error("ArduinoJson extracted metadata is not the reviewed release");
    }
  } catch (error) {
    await rm(extractionRoot, { recursive: true, force: true });
    throw error;
  }

  let cleaned = false;
  return {
    version: dependency.version,
    sha256: dependency.archiveSha256,
    includeDirectory: join(extractionRoot, "src"),
    headerPath: join(extractionRoot, "src", "ArduinoJson.h"),
    async cleanup() {
      if (cleaned) return;
      cleaned = true;
      await rm(extractionRoot, { recursive: true, force: true });
    },
  };
}
