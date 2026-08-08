export type SixColorRgb = readonly [number, number, number];

export const TODOO_NATIVE_PALETTE: readonly SixColorRgb[] = [
  [0, 0, 0],
  [255, 255, 255],
  [255, 255, 0],
  [255, 0, 0],
  [0, 0, 255],
  [0, 255, 0],
] as const;

export const INKLOOP_PREVIEW_PALETTE: readonly SixColorRgb[] = [
  [21, 24, 22],
  [250, 250, 248],
  [229, 201, 0],
  [220, 63, 47],
  [39, 86, 199],
  [8, 124, 78],
] as const;

const HUE_STOPS = [
  { degrees: 0, paletteIndex: 3 },
  { degrees: 60, paletteIndex: 2 },
  { degrees: 120, paletteIndex: 5 },
  { degrees: 240, paletteIndex: 4 },
  { degrees: 360, paletteIndex: 3 },
] as const;

function sourceSeed(data: Uint8ClampedArray, width: number, height: number) {
  let seed = (0x811c9dc5 ^ width ^ Math.imul(height, 0x9e3779b1)) >>> 0;
  const pixelCount = width * height;
  const step = Math.max(1, Math.floor(pixelCount / 97));
  for (let pixel = 0; pixel < pixelCount; pixel += step) {
    const offset = pixel * 4;
    seed ^= data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
    seed = Math.imul(seed, 0x01000193) >>> 0;
  }
  return seed;
}

function deterministicUnit(seed: number, x: number, y: number) {
  let value = (
    seed
    ^ Math.imul(x + 1, 0x1f123bb5)
    ^ Math.imul(y + 1, 0x5f356495)
  ) >>> 0;
  value ^= value >>> 16;
  value = Math.imul(value, 0x7feb352d) >>> 0;
  value ^= value >>> 15;
  value = Math.imul(value, 0x846ca68b) >>> 0;
  value ^= value >>> 16;
  return (value >>> 0) / 4294967296;
}

function sourceHue(red: number, green: number, blue: number, maximum: number, chroma: number) {
  if (chroma === 0) return 0;
  let sector = maximum === red
    ? (green - blue) / chroma
    : maximum === green
      ? (blue - red) / chroma + 2
      : (red - green) / chroma + 4;
  if (sector < 0) sector += 6;
  return sector * 60;
}

/**
 * Deterministic stochastic halftoning for Todoo's six native pigments.
 *
 * Brightness is decomposed into black/white weights, while chroma is spread
 * continuously across the neighbouring native hue pigments. A stable
 * image-seeded threshold then selects a pigment for each pixel. This keeps
 * cyan as a blue/green mix and magenta as a blue/red mix without hard
 * thresholds or Floyd-Steinberg's directional feedback loops.
 */
export function stochasticSixColorDither(
  source: Uint8ClampedArray,
  width: number,
  height: number,
  options: { protectNeutral?: boolean } = {},
) {
  if (width <= 0 || height <= 0 || source.length < width * height * 4) {
    throw new Error("六色抖动输入尺寸无效");
  }
  const output = new Uint8ClampedArray(width * height * 4);
  const seed = sourceSeed(source, width, height);
  const protectNeutral = options.protectNeutral === true;

  for (let pixelY = 0; pixelY < height; pixelY += 1) {
    for (let pixelX = 0; pixelX < width; pixelX += 1) {
      const offset = (pixelY * width + pixelX) * 4;
      const red = source[offset];
      const green = source[offset + 1];
      const blue = source[offset + 2];
      const maximum = Math.max(red, green, blue);
      const minimum = Math.min(red, green, blue);
      const chroma = maximum - minimum;
      const saturation = chroma / Math.max(1, maximum);
      const weights = new Float32Array(TODOO_NATIVE_PALETTE.length);

      if (protectNeutral && (chroma < 30 || saturation < 0.22)) {
        const luminance = (red * 0.2126 + green * 0.7152 + blue * 0.0722) / 255;
        weights[0] = 1 - luminance;
        weights[1] = luminance;
      } else {
        weights[0] = 1 - maximum / 255;
        weights[1] = minimum / 255;
        const chromaWeight = chroma / 255;
        if (chromaWeight > 0) {
          const hue = sourceHue(red, green, blue, maximum, chroma);
          for (let index = 0; index < HUE_STOPS.length - 1; index += 1) {
            const start = HUE_STOPS[index];
            const end = HUE_STOPS[index + 1];
            if (hue < start.degrees || hue > end.degrees) continue;
            const mix = (hue - start.degrees) / (end.degrees - start.degrees);
            weights[start.paletteIndex] += chromaWeight * (1 - mix);
            weights[end.paletteIndex] += chromaWeight * mix;
            break;
          }
        }
      }

      const threshold = deterministicUnit(seed, pixelX, pixelY);
      let cumulative = 0;
      let selectedIndex = 0;
      for (let paletteIndex = 0; paletteIndex < weights.length; paletteIndex += 1) {
        cumulative += weights[paletteIndex];
        if (threshold < cumulative || paletteIndex === weights.length - 1) {
          selectedIndex = paletteIndex;
          break;
        }
      }
      const selected = INKLOOP_PREVIEW_PALETTE[selectedIndex];
      output[offset] = selected[0];
      output[offset + 1] = selected[1];
      output[offset + 2] = selected[2];
      output[offset + 3] = 255;
    }
  }
  return output;
}
