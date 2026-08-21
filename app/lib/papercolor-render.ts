export const PAPER_COLOR_RENDER_STRATEGIES = [
  "official-quality",
  "classic-six-color",
  "reflectance-photo",
  "solid-clean",
] as const;

export type PaperColorRenderStrategy = typeof PAPER_COLOR_RENDER_STRATEGIES[number];

export const PAPER_COLOR_RENDER_OPTIONS: ReadonlyArray<{
  value: PaperColorRenderStrategy;
  label: string;
  description: string;
}> = [
  {
    value: "official-quality",
    label: "官方画质",
    description: "交给 M5GFX 官方高质量六色渲染，兼容性最好。",
  },
  {
    value: "classic-six-color",
    label: "经典六色抖色",
    description: "RGB Floyd–Steinberg 六色误差扩散，纹理明显、速度稳定。",
  },
  {
    value: "reflectance-photo",
    label: "反射率照片",
    description: "按 PaperColor 实测色与反射率扩散，适合照片、插画和渐变。",
  },
  {
    value: "solid-clean",
    label: "纯色清晰",
    description: "关闭误差扩散，适合大色块、文字、二维码、表格和信息卡。",
  },
];

export function isPaperColorRenderStrategy(
  value: unknown,
): value is PaperColorRenderStrategy {
  return typeof value === "string"
    && PAPER_COLOR_RENDER_STRATEGIES.includes(value as PaperColorRenderStrategy);
}

export function normalizePaperColorRenderStrategy(
  value: unknown,
): PaperColorRenderStrategy {
  if (isPaperColorRenderStrategy(value)) return value;
  if (value === "official") return "official-quality";
  if (value === "experimental-six-color") return "classic-six-color";
  if (value === "inkloop-text") return "solid-clean";
  return "official-quality";
}

export function paperColorStrategyForScreenMode(
  mode: string | null | undefined,
): PaperColorRenderStrategy {
  if (mode === "classic-six-color" || mode === "reflectance-photo") return mode;
  if (mode === "inkloop-text" || mode === "solid-clean") return "solid-clean";
  return "official-quality";
}
