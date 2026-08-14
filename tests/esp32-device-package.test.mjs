import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

const publicRoot = new URL("../public/", import.meta.url);
const manifestUrl = new URL("firmware/m5-papercolor/manifest.json", publicRoot);

test("M5 PaperColor 刷机清单中的四个镜像均存在且哈希匹配", async () => {
  const manifest = JSON.parse(await readFile(manifestUrl, "utf8"));
  assert.equal(manifest.chipFamily, "ESP32-S3");
  assert.equal(manifest.files.length, 4);
  for (const file of manifest.files) {
    const bytes = await readFile(new URL(file.path.replace(/^\//, ""), publicRoot));
    assert.equal(createHash("sha256").update(bytes).digest("hex"), file.sha256, file.path);
  }
});

test("应用镜像只包含一个可安全覆盖的服务器地址槽位", async () => {
  const manifest = JSON.parse(await readFile(manifestUrl, "utf8"));
  const firmware = await readFile(new URL("firmware/m5-papercolor/firmware.bin", publicRoot));
  const marker = Buffer.from(manifest.serverSlot.marker);
  const first = firmware.indexOf(marker);
  assert.ok(first >= 0, "missing API URL slot");
  assert.equal(firmware.indexOf(marker, first + 1), -1, "API URL slot must be unique");
  const padding = firmware.subarray(first + marker.length, first + manifest.serverSlot.length);
  assert.ok(padding.every((byte) => byte === 0), "API URL slot must have zero-filled padding");
});
