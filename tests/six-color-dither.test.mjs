import test from "node:test";
import assert from "node:assert/strict";

import {
  INKLOOP_PREVIEW_PALETTE,
  stochasticSixColorDither,
} from "../app/lib/six-color-dither.ts";

function solidImage(width, height, color) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let pixel = 0; pixel < width * height; pixel += 1) {
    data.set([...color, 255], pixel * 4);
  }
  return data;
}

test("随机六色抖动对同一图片保持稳定，并只输出设备六色", () => {
  const input = solidImage(96, 64, [118, 172, 219]);
  const first = stochasticSixColorDither(input, 96, 64);
  const second = stochasticSixColorDither(input, 96, 64);
  assert.deepEqual(first, second);
  const allowed = new Set(INKLOOP_PREVIEW_PALETTE.map((color) => color.join(",")));
  for (let offset = 0; offset < first.length; offset += 4) {
    assert.equal(allowed.has(`${first[offset]},${first[offset + 1]},${first[offset + 2]}`), true);
    assert.equal(first[offset + 3], 255);
  }
});

test("随机网点保持灰阶平均亮度且不会形成单向误差扩散长条", () => {
  const width = 528;
  const output = stochasticSixColorDither(solidImage(width, 24, [128, 128, 128]), width, 24, {
    protectNeutral: true,
  });
  let whitePixels = 0;
  let horizontalTransitions = 0;
  for (let y = 0; y < 24; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      if (output[offset] === INKLOOP_PREVIEW_PALETTE[1][0]) whitePixels += 1;
      if (x > 0 && output[offset] !== output[offset - 4]) horizontalTransitions += 1;
    }
  }
  const ratio = whitePixels / (width * 24);
  assert.ok(ratio > 0.46 && ratio < 0.54, `白色占比异常：${ratio}`);
  assert.ok(horizontalTransitions > width * 5, `随机网点不足：${horizontalTransitions}`);
});

test("设备原生纯色不会被随机化", () => {
  const nativeToPreview = [
    [[0, 0, 0], 0],
    [[255, 255, 255], 1],
    [[255, 255, 0], 2],
    [[255, 0, 0], 3],
    [[0, 0, 255], 4],
    [[0, 255, 0], 5],
  ];
  for (const [source, expectedIndex] of nativeToPreview) {
    const output = stochasticSixColorDither(solidImage(12, 8, source), 12, 8);
    for (let offset = 0; offset < output.length; offset += 4) {
      assert.deepEqual(
        Array.from(output.slice(offset, offset + 3)),
        INKLOOP_PREVIEW_PALETTE[expectedIndex],
      );
    }
  }
});

test("高饱和品红使用红蓝网点，不会退化成蓝白网点", () => {
  const output = stochasticSixColorDither(solidImage(128, 64, [128, 0, 255]), 128, 64);
  const counts = new Map(INKLOOP_PREVIEW_PALETTE.map((color) => [color.join(","), 0]));
  for (let offset = 0; offset < output.length; offset += 4) {
    const key = `${output[offset]},${output[offset + 1]},${output[offset + 2]}`;
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  assert.ok(counts.get(INKLOOP_PREVIEW_PALETTE[3].join(",")) > 0, "缺少红色网点");
  assert.ok(counts.get(INKLOOP_PREVIEW_PALETTE[4].join(",")) > 0, "缺少蓝色网点");
  assert.equal(counts.get(INKLOOP_PREVIEW_PALETTE[1].join(",")), 0, "不应使用白色替代红色");
});

test("高饱和青色使用蓝绿网点，不会退化成蓝白网点", () => {
  const output = stochasticSixColorDither(solidImage(128, 64, [0, 128, 255]), 128, 64);
  const colors = new Set();
  for (let offset = 0; offset < output.length; offset += 4) {
    colors.add(`${output[offset]},${output[offset + 1]},${output[offset + 2]}`);
  }
  assert.deepEqual(
    colors,
    new Set([
      INKLOOP_PREVIEW_PALETTE[4].join(","),
      INKLOOP_PREVIEW_PALETTE[5].join(","),
    ]),
  );
});

test("青色过渡连续增加绿色网点，不会在阈值处突变", () => {
  const greenRatios = [64, 128, 192].map((green) => {
    const output = stochasticSixColorDither(solidImage(256, 96, [0, green, 255]), 256, 96);
    let greenPixels = 0;
    for (let offset = 0; offset < output.length; offset += 4) {
      if (
        output[offset] === INKLOOP_PREVIEW_PALETTE[5][0]
        && output[offset + 1] === INKLOOP_PREVIEW_PALETTE[5][1]
        && output[offset + 2] === INKLOOP_PREVIEW_PALETTE[5][2]
      ) greenPixels += 1;
    }
    return greenPixels / (256 * 96);
  });
  assert.ok(greenRatios[0] < greenRatios[1]);
  assert.ok(greenRatios[1] < greenRatios[2]);
});
