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
