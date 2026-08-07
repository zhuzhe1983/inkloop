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
};

export const CALIBRATION_SWATCHES = [
  { code: 1 as const, key: "white" as const, label: "白", expected: [255, 255, 255] as CalibrationRgb },
  { code: 0 as const, key: "black" as const, label: "黑", expected: [0, 0, 0] as CalibrationRgb },
  { code: 3 as const, key: "red" as const, label: "红", expected: [255, 0, 0] as CalibrationRgb },
  { code: 2 as const, key: "yellow" as const, label: "黄", expected: [255, 255, 0] as CalibrationRgb },
  { code: 6 as const, key: "green" as const, label: "绿", expected: [0, 160, 0] as CalibrationRgb },
  { code: 5 as const, key: "blue" as const, label: "蓝", expected: [0, 0, 255] as CalibrationRgb },
] as const;

const clampByte = (value: number) => Math.max(0, Math.min(255, Math.round(value)));

function patchAverage(data: Uint8ClampedArray, width: number, height: number, bandIndex: number): CalibrationRgb {
  const bandHeight = height / CALIBRATION_SWATCHES.length;
  const centerY = (bandIndex + 0.5) * bandHeight;
  const startX = Math.floor(width * 0.34);
  const endX = Math.ceil(width * 0.66);
  const startY = Math.floor(centerY - bandHeight * 0.16);
  const endY = Math.ceil(centerY + bandHeight * 0.16);
  let red = 0;
  let green = 0;
  let blue = 0;
  let count = 0;
  for (let y = Math.max(0, startY); y < Math.min(height, endY); y += 2) {
    for (let x = startX; x < endX; x += 2) {
      const offset = (y * width + x) * 4;
      red += data[offset];
      green += data[offset + 1];
      blue += data[offset + 2];
      count += 1;
    }
  }
  if (!count) throw new Error("照片中没有检测到完整色卡");
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

export function analyzeCalibrationImage(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  createdAt = new Date().toISOString(),
): DeviceColorCalibration {
  if (width < 120 || height < 180 || data.length < width * height * 4) {
    throw new Error("照片分辨率太低，请靠近屏幕重新拍摄");
  }
  const raw = CALIBRATION_SWATCHES.map((_, index) => patchAverage(data, width, height, index));
  const white = raw[0];
  const black = raw[1];
  const whiteLuminance = white[0] * 0.2126 + white[1] * 0.7152 + white[2] * 0.0722;
  const blackLuminance = black[0] * 0.2126 + black[1] * 0.7152 + black[2] * 0.0722;
  if (whiteLuminance - blackLuminance < 48) {
    throw new Error("没有识别到清晰的黑白色带，请让屏幕填满取景框并避开反光");
  }

  const normalize = (sample: CalibrationRgb): CalibrationRgb => sample.map((value, channel) => {
    const range = Math.max(36, white[channel] - black[channel]);
    return clampByte(((value - black[channel]) / range) * 255);
  }) as CalibrationRgb;

  const samples = CALIBRATION_SWATCHES.map((swatch, index) => {
    const measured = normalize(raw[index]);
    return {
      ...swatch,
      measured,
      deltaE: Number(deltaE76(swatch.expected, measured).toFixed(1)),
    };
  });
  const chromaticSamples = samples.filter((sample) => ![0, 1].includes(sample.code));
  const averageDeltaE = Number((chromaticSamples.reduce((sum, sample) => sum + sample.deltaE, 0) / chromaticSamples.length).toFixed(1));
  const palette: CalibrationPalette = new Array(8).fill(null);
  samples.forEach((sample) => {
    palette[sample.code] = sample.measured;
  });
  return {
    version: 1,
    createdAt,
    palette,
    averageDeltaE,
    quality: averageDeltaE <= 18 ? "excellent" : averageDeltaE <= 35 ? "good" : "weak",
    samples,
  };
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
