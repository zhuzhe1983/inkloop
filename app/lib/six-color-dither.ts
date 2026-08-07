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

type PalettePair = {
  start: number;
  end: number;
  startColor: SixColorRgb;
  deltaRed: number;
  deltaGreen: number;
  deltaBlue: number;
  denominator: number;
};

function makePalettePairs(indices: readonly number[]) {
  const pairs: PalettePair[] = [];
  for (let left = 0; left < indices.length; left += 1) {
    for (let right = left + 1; right < indices.length; right += 1) {
      const start = indices[left];
      const end = indices[right];
      const startColor = TODOO_NATIVE_PALETTE[start];
      const endColor = TODOO_NATIVE_PALETTE[end];
      const deltaRed = endColor[0] - startColor[0];
      const deltaGreen = endColor[1] - startColor[1];
      const deltaBlue = endColor[2] - startColor[2];
      pairs.push({
        start,
        end,
        startColor,
        deltaRed,
        deltaGreen,
        deltaBlue,
        denominator: deltaRed * deltaRed + deltaGreen * deltaGreen + deltaBlue * deltaBlue,
      });
    }
  }
  return pairs;
}

const FULL_PALETTE_PAIRS = makePalettePairs([0, 1, 2, 3, 4, 5]);
const NEUTRAL_PALETTE_PAIRS = makePalettePairs([0, 1]);
const MAGENTA_PALETTE_PAIRS = makePalettePairs([3, 4]);
const CYAN_PALETTE_PAIRS = makePalettePairs([4, 5]);

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

/**
 * Deterministic stochastic halftoning for Todoo's six native pigments.
 *
 * Each source colour is projected onto the best line segment between two
 * native pigments. A stable image-seeded random threshold then chooses one of
 * those two pigments. This preserves the local average while avoiding the long
 * directional feedback loops produced by raster Floyd-Steinberg on gradients.
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
      const chroma = maximum - Math.min(red, green, blue);
      const saturation = chroma / Math.max(1, maximum);
      const saturatedMagenta = saturation > 0.5 && Math.min(red, blue) - green > 24;
      const saturatedCyan = saturation > 0.5 && Math.min(green, blue) - red > 24;
      const pairs = protectNeutral && (chroma < 30 || saturation < 0.22)
        ? NEUTRAL_PALETTE_PAIRS
        : saturatedMagenta
          ? MAGENTA_PALETTE_PAIRS
          : saturatedCyan
            ? CYAN_PALETTE_PAIRS
            : FULL_PALETTE_PAIRS;
      let bestPair = pairs[0];
      let bestMix = 0;
      let bestDistance = Number.POSITIVE_INFINITY;

      for (const pair of pairs) {
        const relativeRed = red - pair.startColor[0];
        const relativeGreen = green - pair.startColor[1];
        const relativeBlue = blue - pair.startColor[2];
        const mix = Math.min(1, Math.max(0, (
          relativeRed * pair.deltaRed
          + relativeGreen * pair.deltaGreen
          + relativeBlue * pair.deltaBlue
        ) / pair.denominator));
        const projectedRed = pair.startColor[0] + pair.deltaRed * mix;
        const projectedGreen = pair.startColor[1] + pair.deltaGreen * mix;
        const projectedBlue = pair.startColor[2] + pair.deltaBlue * mix;
        const deltaRed = red - projectedRed;
        const deltaGreen = green - projectedGreen;
        const deltaBlue = blue - projectedBlue;
        const distance = deltaRed * deltaRed + deltaGreen * deltaGreen + deltaBlue * deltaBlue;
        if (distance < bestDistance) {
          bestDistance = distance;
          bestPair = pair;
          bestMix = mix;
        }
      }

      const selectedIndex = deterministicUnit(seed, pixelX, pixelY) < bestMix
        ? bestPair.end
        : bestPair.start;
      const selected = INKLOOP_PREVIEW_PALETTE[selectedIndex];
      output[offset] = selected[0];
      output[offset + 1] = selected[1];
      output[offset + 2] = selected[2];
      output[offset + 3] = 255;
    }
  }
  return output;
}
