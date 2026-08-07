import test from "node:test";
import assert from "node:assert/strict";

import {
  CALIBRATION_SWATCHES,
  analyzeCalibrationImage,
  validCalibration,
} from "../app/lib/device-calibration.ts";

function calibrationImage(width, height, colors) {
  const data = new Uint8ClampedArray(width * height * 4);
  const bandHeight = height / colors.length;
  for (let y = 0; y < height; y += 1) {
    const color = colors[Math.min(colors.length - 1, Math.floor(y / bandHeight))];
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      data[offset] = color[0];
      data[offset + 1] = color[1];
      data[offset + 2] = color[2];
      data[offset + 3] = 255;
    }
  }
  return data;
}

test("标准六色色带生成可用于设备编码的 8 项色板", () => {
  const width = 240;
  const height = 360;
  const colors = CALIBRATION_SWATCHES.map((swatch) => swatch.expected);
  const profile = analyzeCalibrationImage(calibrationImage(width, height, colors), width, height, "2026-08-07T00:00:00.000Z");
  assert.equal(profile.averageDeltaE, 0);
  assert.equal(profile.quality, "excellent");
  assert.deepEqual(profile.palette[0], [0, 0, 0]);
  assert.deepEqual(profile.palette[1], [255, 255, 255]);
  assert.deepEqual(profile.palette[6], [0, 160, 0]);
  assert.equal(validCalibration(profile), true);
});

test("相机曝光与白平衡先由黑白色带归一化", () => {
  const width = 240;
  const height = 360;
  const colors = CALIBRATION_SWATCHES.map((swatch) => swatch.expected.map((part) => Math.round(24 + part * 0.72)));
  const profile = analyzeCalibrationImage(calibrationImage(width, height, colors), width, height);
  assert.ok(profile.averageDeltaE < 8);
  assert.deepEqual(profile.palette[3], [255, 0, 0]);
});

test("缺少清晰黑白对比时拒绝生成错误 Profile", () => {
  const width = 240;
  const height = 360;
  const flat = CALIBRATION_SWATCHES.map(() => [120, 120, 120]);
  assert.throws(
    () => analyzeCalibrationImage(calibrationImage(width, height, flat), width, height),
    /黑白色带/,
  );
});
