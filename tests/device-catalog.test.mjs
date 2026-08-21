import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

import {
  DEVICE_ADAPTERS,
  DEVICE_SKUS,
  deviceManufacturers,
  filterDeviceSkus,
  officialProductUrl,
  resolveDeviceTargetId,
} from "../app/lib/device-catalog.ts";

test("catalog keeps Chinese source copy and official product URLs", () => {
  const todoo = DEVICE_SKUS["todoo-card-3.7"];
  const paper = DEVICE_SKUS["m5-papercolor-c151"];

  assert.equal(todoo.sizeInches, 3.7);
  assert.equal(todoo.description, "528 × 792 六色蓝牙电子纸卡片");
  assert.equal(officialProductUrl(todoo, "zh"), "https://p.todoo.tech/?lang=zh");
  assert.equal(officialProductUrl(todoo, "en"), "https://p.todoo.tech/?lang=en");
  assert.equal(officialProductUrl(todoo, "ja"), "https://p.todoo.tech/?lang=ja");

  assert.equal(paper.sizeInches, 4);
  assert.equal(paper.description, "400 × 600 Spectra 6 Wi‑Fi 彩色电子纸");
  assert.equal(officialProductUrl(paper, "zh"), "https://docs.m5stack.com/zh_CN/core/PaperColor");
  assert.equal(officialProductUrl(paper, "en"), "https://docs.m5stack.com/en/core/PaperColor");
  assert.equal(officialProductUrl(paper, "ja"), "https://docs.m5stack.com/ja/core/PaperColor");

  assert.equal(DEVICE_ADAPTERS["todoo-card-3.7"].taskStatusCopy, "浏览器渲染 · GATT 分包写入");
  assert.equal(DEVICE_ADAPTERS["m5-papercolor-c151"].taskStatusCopy, "服务端 PNG · HTTPS 主动拉取");
});

test("filterDeviceSkus can slice by family and brand", () => {
  assert.deepEqual(deviceManufacturers().sort(), ["M5Stack", "Todoo"]);
  assert.equal(filterDeviceSkus().length, 2);
  assert.equal(filterDeviceSkus({ family: "all", manufacturer: "all" }).length, 2);
  assert.equal(filterDeviceSkus({ family: "bluetooth" })[0].id, "todoo-card-3.7");
  assert.equal(filterDeviceSkus({ family: "esp32" })[0].id, "m5-papercolor-c151");
  assert.equal(filterDeviceSkus({ manufacturer: "Todoo" })[0].id, "todoo-card-3.7");
  assert.equal(filterDeviceSkus({ manufacturer: "M5Stack" })[0].id, "m5-papercolor-c151");
  assert.equal(filterDeviceSkus({ family: "bluetooth", manufacturer: "M5Stack" }).length, 0);
});

test("device targets keep explicit focus across list refreshes and expose native resolution", () => {
  const initial = [{ id: "paper" }, { id: "todoo" }];
  const reordered = [{ id: "todoo" }, { id: "paper" }];

  assert.equal(resolveDeviceTargetId(initial, "paper", null), "paper");
  assert.equal(resolveDeviceTargetId(reordered, "paper", "todoo"), "paper");
  assert.equal(resolveDeviceTargetId(reordered, "missing", "paper"), "paper");
  assert.equal(resolveDeviceTargetId(reordered, "missing", "missing"), "todoo");
  assert.equal(resolveDeviceTargetId([], "paper", "paper"), null);

  assert.deepEqual(
    DEVICE_ADAPTERS["m5-papercolor-c151"].renderTarget("portrait"),
    { width: 400, height: 600 },
  );
  assert.deepEqual(
    DEVICE_ADAPTERS["m5-papercolor-c151"].renderTarget("landscape"),
    { width: 600, height: 400 },
  );
});

test("the studio has one write-target control while the sidebar only opens details", async () => {
  const studio = await readFile(new URL("../app/ink-studio.tsx", import.meta.url), "utf8");
  const start = studio.indexOf("const openDeviceCenter = useCallback");
  const end = studio.indexOf("const toggleSidebar", start);
  const deviceCenterHandler = studio.slice(start, end);

  assert.ok(start >= 0 && end > start);
  assert.doesNotMatch(deviceCenterHandler, /activateDevice|selectActiveDevice|setActiveDeviceId/);
  assert.match(studio, /className="run-device-target"[\s\S]*<select/);
  assert.match(studio, /查看设备详情，不切换写入设备/);
});
