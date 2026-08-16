import { t } from "./i18n-runtime";
export type CalibrationRgb = [number, number, number];
export type CalibrationPalette = Array<CalibrationRgb | null>;

export type CalibrationSample = {
  code: 0 | 1 | 2 | 3 | 5 | 6;
  key: "white" | "black" | "red" | "yellow" | "green" | "blue";
  label: string;
  expected: CalibrationRgb;
  measured: CalibrationRgb;
  deltaE: number;
};

export type DeviceColorCalibration = {
  version: 1;
  createdAt: string;
  palette: CalibrationPalette;
  averageDeltaE: number;
  quality: "excellent" | "good" | "weak";
  samples: CalibrationSample[];
  capture?: CalibrationCapture;
};

export type CalibrationCapture = {
  axis: "horizontal" | "vertical";
  reversed: boolean;
  rotation: 0 | 90 | 180 | 270;
  confidence: number;
  bounds: { x: number; y: number; width: number; height: number };
};

export const CALIBRATION_SWATCHES = [
  { code: 1 as const, key: "white" as const, label: t("白"), expected: [255, 255, 255] as CalibrationRgb },
  { code: 0 as const, key: "black" as const, label: t("黑"), expected: [0, 0, 0] as CalibrationRgb },
  { code: 3 as const, key: "red" as const, label: t("红"), expected: [255, 0, 0] as CalibrationRgb },
  { code: 2 as const, key: "yellow" as const, label: t("黄"), expected: [255, 255, 0] as CalibrationRgb },
  { code: 6 as const, key: "green" as const, label: t("绿"), expected: [0, 160, 0] as CalibrationRgb },
  { code: 5 as const, key: "blue" as const, label: t("蓝"), expected: [0, 0, 255] as CalibrationRgb },
] as const;

const clampByte = (value: number) => Math.max(0, Math.min(255, Math.round(value)));

type PixelBounds = { x: number; y: number; width: number; height: number };

function patchAverage(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  bounds: PixelBounds,
  axis: "horizontal" | "vertical",
  bandIndex: number,
): CalibrationRgb {
  const bandSize = (axis === "horizontal" ? bounds.height : bounds.width) / CALIBRATION_SWATCHES.length;
  const center = (bandIndex + 0.5) * bandSize;
  const startX = axis === "horizontal"
    ? Math.floor(bounds.x + bounds.width * 0.3)
    : Math.floor(bounds.x + center - bandSize * 0.16);
  const endX = axis === "horizontal"
    ? Math.ceil(bounds.x + bounds.width * 0.7)
    : Math.ceil(bounds.x + center + bandSize * 0.16);
  const startY = axis === "horizontal"
    ? Math.floor(bounds.y + center - bandSize * 0.16)
    : Math.floor(bounds.y + bounds.height * 0.3);
  const endY = axis === "horizontal"
    ? Math.ceil(bounds.y + center + bandSize * 0.16)
    : Math.ceil(bounds.y + bounds.height * 0.7);
  const stride = Math.max(1, Math.floor(Math.min(bounds.width, bounds.height) / 180));
  let red = 0;
  let green = 0;
  let blue = 0;
  let count = 0;
  for (let y = Math.max(0, startY); y < Math.min(height, endY); y += stride) {
    for (let x = Math.max(0, startX); x < Math.min(width, endX); x += stride) {
      const offset = (y * width + x) * 4;
      red += data[offset];
      green += data[offset + 1];
      blue += data[offset + 2];
      count += 1;
    }
  }
  if (!count) throw new Error(t("照片中没有检测到完整色卡"));
  return [red / count, green / count, blue / count].map(clampByte) as CalibrationRgb;
}

function srgbToLab([red, green, blue]: CalibrationRgb) {
  const linearize = (value: number) => {
    const normalized = value / 255;
    return normalized <= 0.04045
      ? normalized / 12.92
      : ((normalized + 0.055) / 1.055) ** 2.4;
  };
  const r = linearize(red);
  const g = linearize(green);
  const b = linearize(blue);
  const x = (r * 0.4124 + g * 0.3576 + b * 0.1805) / 0.95047;
  const y = r * 0.2126 + g * 0.7152 + b * 0.0722;
  const z = (r * 0.0193 + g * 0.1192 + b * 0.9505) / 1.08883;
  const pivot = (value: number) => value > 0.008856 ? value ** (1 / 3) : 7.787 * value + 16 / 116;
  const fx = pivot(x);
  const fy = pivot(y);
  const fz = pivot(z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)] as const;
}

function deltaE76(left: CalibrationRgb, right: CalibrationRgb) {
  const a = srgbToLab(left);
  const b = srgbToLab(right);
  return Math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2);
}

function candidateBounds(width: number, height: number) {
  const candidates: PixelBounds[] = [{ x: 0, y: 0, width, height }];
  const seen = new Set<string>();
  const offsets = [-0.12, 0, 0.12];
  for (const aspect of [2 / 3, 3 / 2]) {
    const fitWidth = Math.min(width, height * aspect);
    const fitHeight = fitWidth / aspect;
    for (const scale of [0.98, 0.88, 0.78, 0.68, 0.58, 0.48, 0.38]) {
      const candidateWidth = fitWidth * scale;
      const candidateHeight = fitHeight * scale;
      const freeX = width - candidateWidth;
      const freeY = height - candidateHeight;
      for (const offsetX of offsets) {
        for (const offsetY of offsets) {
          const candidate = {
            x: freeX * (0.5 + offsetX),
            y: freeY * (0.5 + offsetY),
            width: candidateWidth,
            height: candidateHeight,
          };
          const key = [candidate.x, candidate.y, candidate.width, candidate.height]
            .map((value) => Math.round(value / 4)).join(":");
          if (!seen.has(key)) {
            seen.add(key);
            candidates.push(candidate);
          }
        }
      }
    }
  }
  return candidates;
}

function boundaryEnergy(data: Uint8ClampedArray, width: number, height: number, bounds: PixelBounds) {
  const luminanceAt = (x: number, y: number) => {
    const offset = (Math.max(0, Math.min(height - 1, y)) * width
      + Math.max(0, Math.min(width - 1, x))) * 4;
    return data[offset] * 0.2126 + data[offset + 1] * 0.7152 + data[offset + 2] * 0.0722;
  };
  const left = Math.round(bounds.x);
  const right = Math.round(bounds.x + bounds.width - 1);
  const top = Math.round(bounds.y);
  const bottom = Math.round(bounds.y + bounds.height - 1);
  const inset = Math.max(2, Math.round(Math.min(bounds.width, bounds.height) * 0.008));
  let energy = 0;
  let count = 0;
  for (let step = 1; step < 24; step += 1) {
    const ratio = step / 24;
    const x = Math.round(left + bounds.width * ratio);
    const y = Math.round(top + bounds.height * ratio);
    energy += Math.abs(luminanceAt(left + inset, y) - luminanceAt(left - inset, y));
    energy += Math.abs(luminanceAt(right - inset, y) - luminanceAt(right + inset, y));
    energy += Math.abs(luminanceAt(x, top + inset) - luminanceAt(x, top - inset));
    energy += Math.abs(luminanceAt(x, bottom - inset) - luminanceAt(x, bottom + inset));
    count += 4;
  }
  return count ? energy / count : 0;
}

type CalibrationCandidate = {
  bounds: PixelBounds;
  axis: "horizontal" | "vertical";
  reversed: boolean;
  raw: CalibrationRgb[];
  normalized: CalibrationRgb[];
  averageDeltaE: number;
  contrast: number;
  score: number;
};

function evaluateCandidate(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  bounds: PixelBounds,
  axis: "horizontal" | "vertical",
  reversed: boolean,
): CalibrationCandidate | null {
  const sampled = CALIBRATION_SWATCHES.map((_, index) => patchAverage(data, width, height, bounds, axis, index));
  const raw = reversed ? sampled.reverse() : sampled;
  const white = raw[0];
  const black = raw[1];
  const luminance = ([r, g, b]: CalibrationRgb) => r * 0.2126 + g * 0.7152 + b * 0.0722;
  const contrast = luminance(white) - luminance(black);
  if (contrast < 32) return null;
  const normalize = (sample: CalibrationRgb): CalibrationRgb => sample.map((value, channel) => {
    const range = Math.max(36, white[channel] - black[channel]);
    return clampByte(((value - black[channel]) / range) * 255);
  }) as CalibrationRgb;
  const normalized = raw.map(normalize);
  const deltas = normalized.map((sample, index) => deltaE76(CALIBRATION_SWATCHES[index].expected, sample));
  const averageDeltaE = deltas.slice(2).reduce((sum, value) => sum + value, 0) / 4;
  const blackWhitePenalty = (deltas[0] + deltas[1]) * 0.18;
  const contrastPenalty = Math.max(0, 72 - contrast) * 0.18;
  const edgeBonus = boundaryEnergy(data, width, height, bounds) * 0.06;
  const areaRatio = (bounds.width * bounds.height) / (width * height);
  const tinyPenalty = Math.max(0, 0.16 - areaRatio) * 80;
  return {
    bounds,
    axis,
    reversed,
    raw,
    normalized,
    averageDeltaE,
    contrast,
    score: averageDeltaE + blackWhitePenalty + contrastPenalty + tinyPenalty - edgeBonus,
  };
}

function rotationFor(candidate: CalibrationCandidate): 0 | 90 | 180 | 270 {
  const landscape = candidate.bounds.width > candidate.bounds.height;
  if (!landscape) {
    return candidate.axis === "horizontal" && candidate.reversed ? 180 : 0;
  }
  if (candidate.axis === "vertical") return candidate.reversed ? 270 : 90;
  return candidate.reversed ? 180 : 0;
}

export function analyzeCalibrationCapture(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  createdAt = new Date().toISOString(),
): { profile: DeviceColorCalibration; detection: CalibrationCapture } {
  if (width < 120 || height < 120 || data.length < width * height * 4) {
    throw new Error(t("照片分辨率太低，请靠近屏幕重新拍摄"));
  }
  const candidates: CalibrationCandidate[] = [];
  for (const bounds of candidateBounds(width, height)) {
    for (const axis of ["horizontal", "vertical"] as const) {
      for (const reversed of [false, true]) {
        const candidate = evaluateCandidate(data, width, height, bounds, axis, reversed);
        if (candidate) candidates.push(candidate);
      }
    }
  }
  candidates.sort((left, right) => left.score - right.score);
  const best = candidates[0];
  if (!best) {
    throw new Error(t("没有识别到清晰的黑白色带与六色色卡，请让屏幕完整入镜并避开反光"));
  }
  if (best.averageDeltaE > 68) {
    throw new Error(t("六色色带没有正确对齐，请正对屏幕重拍；系统会自动旋转，无需手动处理照片"));
  }
  const confidence = Math.max(1, Math.min(99, Math.round(
    100 - best.averageDeltaE * 0.9 - Math.max(0, 70 - best.contrast) * 0.2,
  )));
  const detection: CalibrationCapture = {
    axis: best.axis,
    reversed: best.reversed,
    rotation: rotationFor(best),
    confidence,
    bounds: {
      x: Number((best.bounds.x / width).toFixed(4)),
      y: Number((best.bounds.y / height).toFixed(4)),
      width: Number((best.bounds.width / width).toFixed(4)),
      height: Number((best.bounds.height / height).toFixed(4)),
    },
  };
  const samples = CALIBRATION_SWATCHES.map((swatch, index) => ({
    ...swatch,
    measured: best.normalized[index],
    deltaE: Number(deltaE76(swatch.expected, best.normalized[index]).toFixed(1)),
  }));
  const averageDeltaE = Number((samples.slice(2).reduce((sum, sample) => sum + sample.deltaE, 0) / 4).toFixed(1));
  const palette: CalibrationPalette = new Array(8).fill(null);
  samples.forEach((sample) => { palette[sample.code] = sample.measured; });
  const profile: DeviceColorCalibration = {
    version: 1,
    createdAt,
    palette,
    averageDeltaE,
    quality: averageDeltaE <= 18 ? "excellent" : averageDeltaE <= 35 ? "good" : "weak",
    samples,
    capture: detection,
  };
  return { profile, detection };
}

export function analyzeCalibrationImage(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  createdAt = new Date().toISOString(),
): DeviceColorCalibration {
  return analyzeCalibrationCapture(data, width, height, createdAt).profile;
}

export function validCalibration(value: unknown): value is DeviceColorCalibration {
  if (!value || typeof value !== "object") return false;
  const candidate = value as Partial<DeviceColorCalibration>;
  return candidate.version === 1
    && typeof candidate.createdAt === "string"
    && typeof candidate.averageDeltaE === "number"
    && ["excellent", "good", "weak"].includes(candidate.quality || "")
    && Array.isArray(candidate.palette)
    && candidate.palette.length === 8
    && [0, 1, 2, 3, 5, 6].every((code) => {
      const color = candidate.palette?.[code];
      return Array.isArray(color) && color.length === 3 && color.every((part) => Number.isFinite(part));
    })
    && Array.isArray(candidate.samples)
    && candidate.samples.length === 6
    && candidate.samples.every((sample) => Boolean(
      sample
      && typeof sample === "object"
      && typeof sample.key === "string"
      && typeof sample.label === "string"
      && typeof sample.deltaE === "number"
      && Array.isArray(sample.expected)
      && sample.expected.length === 3
      && Array.isArray(sample.measured)
      && sample.measured.length === 3,
    ));
}
