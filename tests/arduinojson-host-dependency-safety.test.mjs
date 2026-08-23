import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import test from "node:test";
import { gzipSync } from "node:zlib";

import {
  materializeArduinoJson,
  validateArchiveMembers,
} from "./support/arduinojson-host-dependency.mjs";

const TAR_BLOCK_BYTES = 512;

function writeString(header, offset, length, value) {
  const bytes = Buffer.from(value, "utf8");
  assert.ok(bytes.length <= length);
  bytes.copy(header, offset);
}

function writeOctal(header, offset, length, value) {
  const encoded = value.toString(8).padStart(length - 1, "0");
  assert.equal(encoded.length, length - 1);
  header.write(encoded, offset, length - 1, "ascii");
  header[offset + length - 1] = 0;
}

function tarEntry({ name, type, contents = Buffer.alloc(0), linkName = "" }) {
  const header = Buffer.alloc(TAR_BLOCK_BYTES);
  writeString(header, 0, 100, name);
  writeOctal(header, 100, 8, type === "5" ? 0o755 : 0o644);
  writeOctal(header, 108, 8, 0);
  writeOctal(header, 116, 8, 0);
  writeOctal(header, 124, 12, contents.length);
  writeOctal(header, 136, 12, 0);
  header.fill(0x20, 148, 156);
  header[156] = type === "\0" ? 0 : type.charCodeAt(0);
  if (linkName) writeString(header, 157, 100, linkName);
  writeString(header, 257, 6, "ustar");
  writeString(header, 263, 2, "00");

  let checksum = 0;
  for (const value of header) checksum += value;
  const encodedChecksum = checksum.toString(8).padStart(6, "0");
  assert.equal(encodedChecksum.length, 6);
  header.write(encodedChecksum, 148, 6, "ascii");
  header[154] = 0;
  header[155] = 0x20;

  const padding = Buffer.alloc(
    Math.ceil(contents.length / TAR_BLOCK_BYTES) * TAR_BLOCK_BYTES -
      contents.length,
  );
  return Buffer.concat([header, contents, padding]);
}

function archiveWith(extra = []) {
  const entries = [
    { name: "src/", type: "5" },
    {
      name: "src/ArduinoJson.h",
      type: "0",
      contents: Buffer.from("// reviewed header\n"),
    },
    {
      name: "src/ArduinoJson.hpp",
      type: "\0",
      contents: Buffer.from("// reviewed implementation\n"),
    },
    { name: "LICENSE.txt", type: "0", contents: Buffer.from("MIT\n") },
    { name: "library.json", type: "0", contents: Buffer.from("{}\n") },
    ...extra,
  ];
  return gzipSync(Buffer.concat([
    ...entries.map(tarEntry),
    Buffer.alloc(2 * TAR_BLOCK_BYTES),
  ]));
}

test("reviewed ArduinoJson archive remains content-addressed and safe", async () => {
  const dependency = await materializeArduinoJson();
  try {
    assert.equal(dependency.version, "7.4.3");
    assert.equal(
      dependency.sha256,
      "2b4eb3accb35d2b2b9f688b4f52aeef0cb489236e441a0a77ae1a0e0dc56a739",
    );
  } finally {
    await dependency.cleanup();
  }
});

test("archive validator permits only regular files and directories", () => {
  assert.deepEqual(new Set(validateArchiveMembers(archiveWith())), new Set([
    "src",
    "src/ArduinoJson.h",
    "src/ArduinoJson.hpp",
    "LICENSE.txt",
    "library.json",
  ]));
});

test("archive validator binds the manifest source-file count", () => {
  const archive = archiveWith();
  assert.doesNotThrow(() => validateArchiveMembers(archive, 2));
  assert.throws(
    () => validateArchiveMembers(archive, 3),
    /expected 3 source files, found 2/,
  );
});

for (const { label, type, linkName = "" } of [
  { label: "hard link", type: "1", linkName: "src/ArduinoJson.h" },
  { label: "symbolic link", type: "2", linkName: "../outside" },
  { label: "character device", type: "3" },
  { label: "block device", type: "4" },
  { label: "FIFO", type: "6" },
  { label: "socket", type: "s" },
  { label: "PAX extension record", type: "x" },
]) {
  test(`archive validator rejects a ${label} before extraction`, () => {
    const malicious = archiveWith([{
      name: `src/malicious-${type}`,
      type,
      linkName,
    }]);
    assert.throws(
      () => validateArchiveMembers(malicious),
      /tar member type .* is not a regular file or directory/,
    );
  });
}
