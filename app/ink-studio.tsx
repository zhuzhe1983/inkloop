"use client";

import { useCallback, useEffect, useRef, useState, type ClipboardEvent, type CSSProperties } from "react";
import {
  featuredApps,
  generateInkApp,
  inferWeatherCity,
  intervalFor,
  scheduleLabel,
  starterApp,
  starterPrompt,
  type ArtworkSpec,
  type InkApp,
  type ScheduleMode,
  type ScreenSpec,
} from "./lib/app-model";
import { TodooCard, type TodooProgress } from "./lib/todoo-card";

type Tab = "studio" | "mine" | "explore" | "device";
type Toast = { tone: "success" | "error" | "info"; message: string } | null;
type ToastTone = NonNullable<Toast>["tone"];
type GeneratorStatus = "checking" | "online" | "local";
type PreviewStatus = "ready" | "loading" | "fallback";

const LOCAL_APPS_KEY = "inkloop-apps-v1";
const GALLERY_PREVIEW_DATE = new Date("2026-08-01T12:34:00+08:00");

const navItems: Array<{ id: Tab; label: string; glyph: string }> = [
  { id: "studio", label: "创作台", glyph: "✦" },
  { id: "mine", label: "我的应用", glyph: "▦" },
  { id: "explore", label: "发现", glyph: "◎" },
  { id: "device", label: "设备中心", glyph: "⌁" },
];

const samplePrompts = [
  "每天 8 点显示上海天气和带伞提醒",
  "显示新品发布倒计时",
  "彩虹背景，中间写一句今天也很棒",
  "美女时钟，每分钟换背景和字体",
  "每 15 分钟更新会议室状态",
  "每小时显示本月销售目标进度",
];

const guideExamples = [
  {
    level: "入门",
    number: "01",
    title: "贴图片或文字，帮我排版",
    description: "直接粘贴一张图片和要显示的文字，或点“选择图片”。生成器会把内容整理成适合六色墨水屏的版面。",
    prompt: "请用我贴入的图片做背景，把标题「今天也要开心」和小字「一步一步，慢慢来」排成简洁海报。",
    action: "套用排版示例",
  },
  {
    level: "进阶",
    number: "02",
    title: "让 LLM 生成主题随机内容",
    description: "只要说清主题、语气和用途，LLM 会生成文案、业务逻辑与配图关键词；预览不满意可继续换一张。",
    prompt: "生成一张露营主题的每日鼓励卡，每次刷新换一张自然风景，文案简短、有户外杂志感。",
    action: "套用主题示例",
  },
  {
    level: "高级",
    number: "03",
    title: "做会自动更新的屏幕",
    description: "例如美女时钟每分钟换背景和字体，或每天早上更新指定城市的天气，再选择对应的刷新计划。",
    prompt: "美女时钟，每分钟更新日期和时间、换一张时尚人像背景并随机字体，时间放在白色画板里。",
    action: "套用时钟示例",
  },
];

const accentColors = {
  red: "#dc3f2f",
  blue: "#2756c7",
  green: "#087c4e",
  yellow: "#e5c900",
};

function upgradeLegacyApp(savedApp: InkApp): InkApp {
  const prompt = savedApp.prompt || "";
  const wantsFullscreen = ["全屏", "铺满", "满屏"].some((term) => prompt.includes(term))
    && ["不要任何其他", "不要其他", "不要文字", "只有图片", "只要图片", "纯图片"].some((term) => prompt.includes(term));
  if (!wantsFullscreen || !savedApp.spec.artwork) return savedApp;
  const hasExplicitSchedule = /每\s*\d+\s*分钟|每(?:个)?小时|每天|每日|早上|上午|下午|晚上/.test(prompt);
  return {
    ...savedApp,
    scheduleMode: hasExplicitSchedule ? savedApp.scheduleMode : "once",
    spec: {
      ...savedApp.spec,
      eyebrow: "",
      title: "",
      value: "",
      unit: "",
      detail: "",
      footer: "",
      artwork: {
        ...savedApp.spec.artwork,
        layout: "fullscreen",
        rotateOnRefresh: /随机|每次换|换一张|轮换/.test(prompt) || savedApp.spec.artwork.rotateOnRefresh,
      },
    },
  };
}

function fitText(ctx: CanvasRenderingContext2D, text: string, maxWidth: number, startSize: number) {
  let size = startSize;
  while (size > 24) {
    ctx.font = `800 ${size}px Arial, "PingFang SC", sans-serif`;
    if (ctx.measureText(text).width <= maxWidth) break;
    size -= 2;
  }
  return size;
}

function randomArtworkSeed() {
  const values = new Uint32Array(1);
  crypto.getRandomValues(values);
  return (values[0] % 999_999) + 1;
}

function replaceTimeVariables(value: string, now: Date) {
  const pad = (part: number) => String(part).padStart(2, "0");
  const year = String(now.getFullYear());
  const month = pad(now.getMonth() + 1);
  const day = pad(now.getDate());
  const hour = pad(now.getHours());
  const minute = pad(now.getMinutes());
  const variables: Record<string, string> = {
    date: `${year}-${month}-${day}`,
    year,
    month,
    day,
    weekday: new Intl.DateTimeFormat("zh-CN", { weekday: "short" }).format(now),
    hour,
    minute,
    time: `${hour}:${minute}`,
  };
  const resolved = value.replace(
    /\{\{(date|year|month|day|weekday|hour|minute|time)\}\}/g,
    (_, key: string) => variables[key],
  );
  return /\{\{|\}\}/.test(resolved) ? "—" : resolved;
}

function resolveTimeVariables(spec: ScreenSpec, now = new Date()): ScreenSpec {
  return {
    ...spec,
    eyebrow: replaceTimeVariables(spec.eyebrow, now),
    title: replaceTimeVariables(spec.title, now),
    value: replaceTimeVariables(spec.value, now),
    unit: replaceTimeVariables(spec.unit, now),
    detail: replaceTimeVariables(spec.detail, now),
    footer: replaceTimeVariables(spec.footer, now),
  };
}

type WeatherPayload = {
  city?: string;
  temperature?: number;
  low?: number;
  high?: number;
  rainProbability?: number;
  condition?: string;
};

async function resolveRuntimeScreen(currentApp: InkApp, now = new Date()): Promise<ScreenSpec> {
  const resolved = resolveTimeVariables(currentApp.spec, now);
  if (resolved.kind !== "weather") return resolved;
  const city = resolved.city || inferWeatherCity(currentApp.prompt);
  try {
    const response = await fetch(`/api/weather?city=${encodeURIComponent(city)}`, { cache: "no-store" });
    if (!response.ok) throw new Error("weather unavailable");
    const weather = (await response.json()) as WeatherPayload;
    if (
      typeof weather.temperature !== "number"
      || typeof weather.low !== "number"
      || typeof weather.high !== "number"
    ) {
      throw new Error("weather incomplete");
    }
    const rainProbability = typeof weather.rainProbability === "number" ? weather.rainProbability : 0;
    const weekday = new Intl.DateTimeFormat("zh-CN", { weekday: "short" }).format(now);
    return {
      ...resolved,
      city: weather.city || city,
      eyebrow: `${weather.city || city} · ${weekday}`,
      title: "今日天气",
      value: String(Math.round(weather.temperature)),
      unit: "°C",
      detail: `${weather.condition || "天气多变"} · ${Math.round(weather.low)}—${Math.round(weather.high)}°C`,
      footer: rainProbability >= 45
        ? `降雨${Math.round(rainProbability)}% · 带伞 · Open-Meteo`
        : "少雨 · 适合出门 · Open-Meteo",
      accent: rainProbability >= 45 ? "red" : "yellow",
    };
  } catch {
    return {
      ...resolved,
      city,
      eyebrow: resolved.eyebrow === "—" ? `${city} · 今日` : resolved.eyebrow,
      title: resolved.title === "—" ? "今日天气" : resolved.title,
      value: resolved.value === "—" ? "--" : resolved.value,
      unit: "°C",
      detail: resolved.detail === "—" ? "天气数据暂不可用" : resolved.detail,
      footer: resolved.footer === "—" ? "稍后刷新重试" : resolved.footer,
    };
  }
}

function clockFontFamily(spec: ScreenSpec) {
  const fonts = {
    sans: 'Arial, "PingFang SC", sans-serif',
    serif: 'Georgia, "Songti SC", serif',
    rounded: '"Arial Rounded MT Bold", "PingFang SC", sans-serif',
    mono: '"Courier New", "SFMono-Regular", monospace',
    handwritten: '"Comic Sans MS", "Kaiti SC", cursive',
  } as const;
  const requested = spec.clock?.font ?? "sans";
  if (requested !== "random") return fonts[requested];
  const choices = Object.values(fonts);
  return choices[(spec.artwork?.seed ?? 0) % choices.length];
}

function fitClockText(
  ctx: CanvasRenderingContext2D,
  text: string,
  maxWidth: number,
  startSize: number,
  family: string,
) {
  let size = startSize;
  while (size > 28) {
    ctx.font = `900 ${size}px ${family}`;
    if (ctx.measureText(text).width <= maxWidth) break;
    size -= 2;
  }
  return size;
}

const ePaperPalette = [
  [21, 24, 22],
  [244, 240, 220],
  [229, 201, 0],
  [220, 63, 47],
  [39, 86, 199],
  [8, 124, 78],
] as const;

function artworkUrl(artwork: ArtworkSpec) {
  const params = new URLSearchParams({
    query: artwork.query,
    seed: String(artwork.seed),
  });
  return `/api/artwork?${params.toString()}`;
}

function loadArtwork(url: string) {
  return new Promise<HTMLImageElement>((resolve, reject) => {
    const image = new Image();
    const timeout = window.setTimeout(() => reject(new Error("图片素材加载超时")), 15_000);
    image.decoding = "async";
    image.onload = () => {
      window.clearTimeout(timeout);
      resolve(image);
    };
    image.onerror = () => {
      window.clearTimeout(timeout);
      reject(new Error("图片素材加载失败"));
    };
    image.src = url;
  });
}

async function prepareLocalImage(file: File) {
  if (!file.type.startsWith("image/")) throw new Error("请选择图片文件");
  if (file.size > 15 * 1024 * 1024) throw new Error("图片请控制在 15MB 以内");
  const source = await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error("读取图片失败"));
    reader.readAsDataURL(file);
  });
  const image = await loadArtwork(source);
  const canvas = document.createElement("canvas");
  canvas.width = 528;
  canvas.height = 792;
  const context = canvas.getContext("2d");
  if (!context) throw new Error("浏览器无法处理这张图片");
  context.fillStyle = "#f4f0dc";
  context.fillRect(0, 0, canvas.width, canvas.height);
  const scale = Math.max(canvas.width / image.naturalWidth, canvas.height / image.naturalHeight);
  const width = image.naturalWidth * scale;
  const height = image.naturalHeight * scale;
  context.drawImage(image, (canvas.width - width) / 2, (canvas.height - height) / 2, width, height);
  return canvas.toDataURL("image/jpeg", 0.78);
}

function drawImageContain(
  ctx: CanvasRenderingContext2D,
  image: HTMLImageElement,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  const scale = Math.min(width / image.naturalWidth, height / image.naturalHeight);
  const drawWidth = image.naturalWidth * scale;
  const drawHeight = image.naturalHeight * scale;
  ctx.fillStyle = "#f4f0dc";
  ctx.fillRect(x, y, width, height);
  ctx.drawImage(image, x + (width - drawWidth) / 2, y + (height - drawHeight) / 2, drawWidth, drawHeight);
}

function drawImageCover(
  ctx: CanvasRenderingContext2D,
  image: HTMLImageElement,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  const scale = Math.max(width / image.naturalWidth, height / image.naturalHeight);
  const drawWidth = image.naturalWidth * scale;
  const drawHeight = image.naturalHeight * scale;
  ctx.drawImage(image, x + (width - drawWidth) / 2, y + (height - drawHeight) / 2, drawWidth, drawHeight);
}

function quantizeRegion(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  const image = ctx.getImageData(x, y, width, height);
  const matrix = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
  for (let pixel = 0; pixel < image.data.length; pixel += 4) {
    const point = pixel / 4;
    const px = point % width;
    const py = Math.floor(point / width);
    const bias = (matrix[(py % 4) * 4 + (px % 4)] - 7.5) * 4;
    const red = Math.max(0, Math.min(255, image.data[pixel] + bias));
    const green = Math.max(0, Math.min(255, image.data[pixel + 1] + bias));
    const blue = Math.max(0, Math.min(255, image.data[pixel + 2] + bias));
    let best: readonly [number, number, number] = ePaperPalette[0];
    let bestDistance = Number.POSITIVE_INFINITY;
    for (const color of ePaperPalette) {
      const dr = red - color[0];
      const dg = green - color[1];
      const db = blue - color[2];
      const distance = dr * dr * 0.3 + dg * dg * 0.59 + db * db * 0.11;
      if (distance < bestDistance) {
        bestDistance = distance;
        best = color;
      }
    }
    image.data[pixel] = best[0];
    image.data[pixel + 1] = best[1];
    image.data[pixel + 2] = best[2];
    image.data[pixel + 3] = 255;
  }
  ctx.putImageData(image, x, y);
}

function drawGeneratedArtwork(
  ctx: CanvasRenderingContext2D,
  artwork: ArtworkSpec,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  const colors = ["#dc3f2f", "#e5c900", "#087c4e", "#2756c7"];
  let state = artwork.seed || 1;
  const random = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
  ctx.save();
  ctx.beginPath();
  ctx.rect(x, y, width, height);
  ctx.clip();
  ctx.fillStyle = "#f4f0dc";
  ctx.fillRect(x, y, width, height);

  if (artwork.motif === "rainbow") {
    ctx.fillStyle = "#e5c900";
    ctx.fillRect(x, y, width, height);
    ctx.fillStyle = "#f4f0dc";
    for (let index = 0; index < 18; index += 1) {
      ctx.beginPath();
      ctx.arc(x + random() * width, y + random() * height, 4 + random() * 13, 0, Math.PI * 2);
      ctx.fill();
    }
    const centerX = x + width * (0.47 + random() * 0.06);
    const centerY = y + height * (0.72 + random() * 0.06);
    const baseRadius = Math.min(width * 0.48, height * 0.44) * (0.9 + random() * 0.1);
    colors.forEach((color, index) => {
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(26, baseRadius * 0.18);
      ctx.beginPath();
      ctx.arc(centerX, centerY, baseRadius - index * ctx.lineWidth * 0.78, Math.PI, Math.PI * 2);
      ctx.stroke();
    });
    ctx.fillStyle = "#f4f0dc";
    ctx.beginPath();
    ctx.arc(centerX, centerY, Math.max(34, baseRadius * 0.28), Math.PI, Math.PI * 2);
    ctx.fill();
  } else if (artwork.motif === "sunburst") {
    const centerX = x + width * (0.45 + random() * 0.14);
    const centerY = y + height * (0.36 + random() * 0.12);
    const rotation = random() * Math.PI * 2;
    for (let index = 0; index < 24; index += 1) {
      const start = rotation + (index / 24) * Math.PI * 2;
      const end = rotation + ((index + 1) / 24) * Math.PI * 2;
      ctx.fillStyle = colors[index % colors.length];
      ctx.beginPath();
      ctx.moveTo(centerX, centerY);
      ctx.arc(centerX, centerY, Math.max(width, height), start, end);
      ctx.closePath();
      ctx.fill();
    }
    ctx.fillStyle = "#e5c900";
    ctx.beginPath();
    ctx.arc(centerX, centerY, Math.min(width, height) * 0.16, 0, Math.PI * 2);
    ctx.fill();
  } else if (artwork.motif === "waves") {
    ctx.fillStyle = "#2756c7";
    ctx.fillRect(x, y, width, height);
    const phase = random() * Math.PI * 2;
    for (let row = 0; row < 8; row += 1) {
      ctx.strokeStyle = colors[row % colors.length];
      ctx.lineWidth = Math.max(14, height / 28);
      ctx.beginPath();
      for (let px = 0; px <= width; px += 8) {
        const py = y + 60 + row * (height / 9) + Math.sin(px / 34 + row + phase) * 24;
        if (px === 0) ctx.moveTo(x + px, py);
        else ctx.lineTo(x + px, py);
      }
      ctx.stroke();
    }
  } else if (artwork.motif === "confetti") {
    for (let index = 0; index < 90; index += 1) {
      ctx.save();
      ctx.translate(x + random() * width, y + random() * height);
      ctx.rotate(random() * Math.PI);
      ctx.fillStyle = colors[index % colors.length];
      ctx.fillRect(-8, -18, 16, 36);
      ctx.restore();
    }
  } else {
    const cell = Math.max(38, Math.floor(Math.min(width, height) / 7));
    const colorOffset = Math.floor(random() * colors.length);
    for (let py = y; py < y + height; py += cell) {
      for (let px = x; px < x + width; px += cell) {
        ctx.fillStyle = colors[(colorOffset + Math.floor((px - x) / cell) + Math.floor((py - y) / cell)) % colors.length];
        ctx.fillRect(px, py, cell, cell);
      }
    }
  }
  ctx.restore();
}

function drawClockCopy(ctx: CanvasRenderingContext2D, spec: ScreenSpec, accent: string) {
  const ink = "#151816";
  const paper = "#f4f0dc";
  const family = clockFontFamily(spec);
  const board = spec.clock?.board !== false;
  const valueSize = fitClockText(ctx, spec.value, board ? 380 : 438, board ? 112 : 126, family);

  ctx.textAlign = "center";
  if (board) {
    ctx.fillStyle = paper;
    ctx.strokeStyle = ink;
    ctx.lineWidth = 5;
    ctx.fillRect(48, 226, 432, 282);
    ctx.strokeRect(48, 226, 432, 282);
    ctx.fillStyle = accent;
    ctx.fillRect(72, 252, 76, 12);
    ctx.fillStyle = ink;
    ctx.font = '800 25px Arial, "PingFang SC", sans-serif';
    ctx.fillText(spec.title, 264, 306);
    ctx.font = `900 ${valueSize}px ${family}`;
    ctx.fillText(spec.value, 264, 424);
    ctx.font = '700 22px Arial, "PingFang SC", sans-serif';
    ctx.fillText(spec.detail.slice(0, 28), 264, 474);
  } else {
    ctx.lineJoin = "round";
    ctx.lineWidth = 14;
    ctx.strokeStyle = paper;
    ctx.font = `900 ${valueSize}px ${family}`;
    ctx.strokeText(spec.value, 264, 408);
    ctx.fillStyle = ink;
    ctx.fillText(spec.value, 264, 408);
    ctx.fillStyle = paper;
    ctx.strokeStyle = ink;
    ctx.lineWidth = 3;
    ctx.fillRect(62, 472, 404, 66);
    ctx.strokeRect(62, 472, 404, 66);
    ctx.fillStyle = ink;
    ctx.font = '700 23px Arial, "PingFang SC", sans-serif';
    ctx.fillText(spec.detail.slice(0, 28), 264, 514);
  }

  ctx.fillStyle = paper;
  ctx.strokeStyle = ink;
  ctx.lineWidth = 3;
  ctx.fillRect(48, 628, 432, 72);
  ctx.strokeRect(48, 628, 432, 72);
  ctx.fillStyle = accent;
  ctx.fillRect(70, 652, 20, 20);
  ctx.fillStyle = ink;
  ctx.font = '700 20px Arial, "PingFang SC", sans-serif';
  ctx.fillText(spec.footer.slice(0, 26), 286, 671);
  ctx.textAlign = "left";
}

function drawArtworkCopy(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  accent: string,
  layout: ArtworkSpec["layout"],
) {
  const ink = "#151816";
  const paper = "#f4f0dc";
  ctx.fillStyle = paper;
  ctx.strokeStyle = ink;
  ctx.lineWidth = 3;
  ctx.fillRect(38, 42, 452, 76);
  ctx.strokeRect(38, 42, 452, 76);
  ctx.fillStyle = ink;
  ctx.font = "700 18px Arial, sans-serif";
  ctx.fillText(spec.eyebrow.toUpperCase(), 56, 88);
  ctx.fillStyle = accent;
  ctx.fillRect(430, 65, 38, 14);

  if (spec.clock?.enabled) {
    drawClockCopy(ctx, spec, accent);
    ctx.fillStyle = ink;
    ctx.font = "700 15px Arial, sans-serif";
    ctx.fillText("INKLOOP / CLOCK", 48, 736);
    ctx.textAlign = "right";
    ctx.fillText("LIVE TIME", 480, 736);
    ctx.textAlign = "left";
    return;
  }

  if (layout === "background") {
    ctx.fillStyle = paper;
    ctx.fillRect(46, 188, 436, 198);
    ctx.strokeStyle = ink;
    ctx.strokeRect(46, 188, 436, 198);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 382, 38)}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.title, 72, 246);
    const valueSize = fitText(ctx, spec.value, 382, 76);
    ctx.font = `900 ${valueSize}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.value, 72, 340);
    if (spec.unit) {
      ctx.font = "800 28px Arial, sans-serif";
      ctx.fillText(spec.unit, 72, 374);
    }
  } else {
    ctx.fillStyle = paper;
    ctx.fillRect(38, 468, 452, 184);
    ctx.strokeStyle = ink;
    ctx.strokeRect(38, 468, 452, 184);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 402, 34)}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.title, 60, 516);
    const valueSize = fitText(ctx, spec.value, 402, 64);
    ctx.font = `900 ${valueSize}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.value, 60, 592);
    if (spec.unit) {
      ctx.font = "800 26px Arial, sans-serif";
      ctx.fillText(spec.unit, 60, 628);
    }
  }

  if (layout === "background") {
    ctx.fillStyle = paper;
    ctx.fillRect(38, 622, 452, 92);
    ctx.strokeStyle = ink;
    ctx.strokeRect(38, 622, 452, 92);
    ctx.fillStyle = ink;
    ctx.font = "700 20px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.detail.slice(0, 30), 58, 656);
    ctx.fillStyle = accent;
    ctx.fillRect(58, 674, 18, 18);
    ctx.fillStyle = ink;
    ctx.font = "700 19px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.footer.slice(0, 24), 90, 691);
  } else {
    ctx.fillStyle = paper;
    ctx.fillRect(38, 666, 452, 46);
    ctx.strokeStyle = ink;
    ctx.strokeRect(38, 666, 452, 46);
    ctx.fillStyle = ink;
    ctx.font = "700 19px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.detail.slice(0, 30), 58, 696);
  }

  ctx.fillStyle = ink;
  ctx.font = "700 15px Arial, sans-serif";
  ctx.fillText("INKLOOP / VISUAL", 48, 736);
  ctx.textAlign = "right";
  ctx.fillText("6-COLOR E-PAPER", 480, 736);
  ctx.textAlign = "left";
}

async function drawScreen(canvas: HTMLCanvasElement, spec: ScreenSpec, localImage?: string) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return false;
  const width = 528;
  const height = 792;
  const ink = "#151816";
  const paper = "#f4f0dc";
  const accent = accentColors[spec.accent];

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = paper;
  ctx.fillRect(0, 0, width, height);
  const artwork = spec.artwork ?? (localImage
    ? {
        mode: "web" as const,
        motif: "grid" as const,
        query: "local image",
        layout: "background" as const,
        seed: 1,
      }
    : undefined);
  if (artwork) {
    const area = artwork.layout === "fullscreen"
      ? { x: 0, y: 0, width, height }
      : artwork.layout === "hero"
      ? { x: 48, y: 132, width: 432, height: 314 }
      : { x: 22, y: 22, width: width - 44, height: height - 44 };
    try {
      if (localImage || artwork.mode === "web") {
        const image = await loadArtwork(localImage || artworkUrl(artwork));
        if (artwork.layout === "fullscreen") {
          drawImageCover(ctx, image, area.x, area.y, area.width, area.height);
        } else {
          drawImageContain(ctx, image, area.x, area.y, area.width, area.height);
        }
        quantizeRegion(ctx, area.x, area.y, area.width, area.height);
      } else {
        drawGeneratedArtwork(ctx, artwork, area.x, area.y, area.width, area.height);
      }
      if (artwork.layout === "fullscreen") return true;
      drawArtworkCopy(ctx, spec, accent, artwork.layout);
      ctx.strokeStyle = ink;
      ctx.lineWidth = 3;
      ctx.strokeRect(22, 22, width - 44, height - 44);
      return true;
    } catch {
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = paper;
      ctx.fillRect(0, 0, width, height);
      if (artwork.layout === "fullscreen") return false;
    }
  }

  ctx.strokeStyle = ink;
  ctx.lineWidth = 3;
  ctx.strokeRect(22, 22, width - 44, height - 44);

  ctx.fillStyle = ink;
  ctx.font = "700 18px Arial, sans-serif";
  ctx.letterSpacing = "2px";
  ctx.fillText(spec.eyebrow.toUpperCase(), 48, 74);
  ctx.fillStyle = accent;
  ctx.fillRect(48, 96, 70, 10);
  ctx.fillStyle = ink;
  ctx.fillRect(124, 96, 356, 2);

  if (spec.kind === "weather") {
    ctx.fillStyle = "#e5c900";
    ctx.beginPath();
    ctx.arc(398, 208, 54, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = ink;
    ctx.lineWidth = 8;
    for (let angle = 0; angle < Math.PI * 2; angle += Math.PI / 4) {
      ctx.beginPath();
      ctx.moveTo(398 + Math.cos(angle) * 72, 208 + Math.sin(angle) * 72);
      ctx.lineTo(398 + Math.cos(angle) * 90, 208 + Math.sin(angle) * 90);
      ctx.stroke();
    }
    ctx.fillStyle = ink;
    ctx.font = "700 31px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 182);
    ctx.font = "800 164px Arial, sans-serif";
    ctx.fillText(spec.value, 42, 370);
    ctx.font = "800 62px Arial, sans-serif";
    ctx.fillText(spec.unit, 264, 280);
  } else if (spec.kind === "countdown") {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.font = "900 238px Arial, sans-serif";
    ctx.fillText(spec.value, 34, 438);
    ctx.fillStyle = ink;
    ctx.font = "800 58px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.unit, 380, 408);
  } else if (spec.kind === "meeting") {
    ctx.fillStyle = accent;
    ctx.fillRect(48, 140, 432, 112);
    ctx.fillStyle = "#ffffff";
    ctx.font = "800 54px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.value, 74, 215);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 430, 70)}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.title, 48, 360);
  } else if (spec.kind === "metric") {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 178);
    ctx.font = "900 176px Arial, sans-serif";
    ctx.fillText(spec.value, 40, 370);
    ctx.font = "800 62px Arial, sans-serif";
    ctx.fillText(spec.unit, 330, 350);
    ctx.strokeStyle = ink;
    ctx.lineWidth = 5;
    ctx.strokeRect(48, 432, 432, 46);
    ctx.fillStyle = accent;
    ctx.fillRect(57, 441, 316, 28);
  } else {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.fillRect(48, 226, 432, 206);
    ctx.fillStyle = "#ffffff";
    const size = fitText(ctx, spec.value, 380, 74);
    ctx.font = `900 ${size}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.value, 72, 346);
  }

  ctx.fillStyle = ink;
  ctx.font = "700 24px Arial, \"PingFang SC\", sans-serif";
  ctx.fillText(spec.detail, 48, 552);
  ctx.fillRect(48, 590, 432, 3);
  ctx.fillStyle = accent;
  ctx.beginPath();
  ctx.arc(68, 646, 18, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = ink;
  ctx.font = "700 27px Arial, \"PingFang SC\", sans-serif";
  const footer = spec.footer.length > 24 ? `${spec.footer.slice(0, 24)}…` : spec.footer;
  ctx.fillText(footer, 100, 655);

  ctx.font = "700 16px Arial, sans-serif";
  ctx.fillText("INKLOOP / TODOO 3.7", 48, 724);
  ctx.textAlign = "right";
  ctx.fillText("6-COLOR E-PAPER", 480, 724);
  ctx.textAlign = "left";
  return false;
}

async function renderScreenToCanvas(canvas: HTMLCanvasElement, spec: ScreenSpec, localImage?: string) {
  const staging = document.createElement("canvas");
  staging.width = 528;
  staging.height = 792;
  const usedArtwork = await drawScreen(staging, spec, localImage);
  const context = canvas.getContext("2d");
  if (!context) return usedArtwork;
  context.clearRect(0, 0, canvas.width, canvas.height);
  context.drawImage(staging, 0, 0);
  return usedArtwork;
}

function MiniScreen({ app }: { app: InkApp }) {
  const spec = resolveTimeVariables(app.spec, GALLERY_PREVIEW_DATE);
  const artwork = spec.artwork;
  const generatedBackgrounds: Record<ArtworkSpec["motif"], string> = {
    rainbow: "linear-gradient(135deg, #dc3f2f 0 22%, #e5c900 22% 46%, #087c4e 46% 70%, #2756c7 70%)",
    sunburst: "conic-gradient(from 12deg, #e5c900, #dc3f2f, #f4f0dc, #2756c7, #e5c900)",
    confetti: "repeating-linear-gradient(115deg, #f4f0dc 0 14px, #dc3f2f 14px 20px, #e5c900 20px 34px, #2756c7 34px 40px)",
    waves: "repeating-linear-gradient(0deg, #2756c7 0 18px, #f4f0dc 18px 32px, #087c4e 32px 48px)",
    grid: "conic-gradient(#dc3f2f 25%, #e5c900 0 50%, #087c4e 0 75%, #2756c7 0) 0 0 / 40px 40px",
  };
  const style: CSSProperties | undefined = artwork
    ? {
        backgroundImage: artwork.mode === "web"
          ? `url("${artworkUrl(artwork)}")`
          : generatedBackgrounds[artwork.motif],
        backgroundSize: "cover",
        backgroundPosition: "center",
      }
    : undefined;
  if (artwork?.layout === "fullscreen") {
    return <div className="mini-screen image-only" style={style} aria-label={`${app.title} 全屏图片预览`} />;
  }
  return (
    <div className={`mini-screen mini-${spec.accent}${artwork ? " has-artwork" : ""}`} style={style}>
      <span className="mini-eyebrow">{spec.eyebrow}</span>
      <i />
      <b>{spec.value}</b>
      <em>{spec.unit}</em>
      <small>{spec.title}</small>
      <span className="mini-footer">{spec.detail}</span>
    </div>
  );
}

function AppCard({ app, onUse, local }: { app: InkApp; onUse: () => void; local?: boolean }) {
  return (
    <article className="app-card">
      <MiniScreen app={app} />
      <div className="app-card-copy">
        <div className="app-card-meta">
          <span>{local ? "本机" : `by ${app.author}`}</span>
          <span>{scheduleLabel(app)}</span>
        </div>
        <h3>{app.title}</h3>
        <p>{app.description}</p>
        <button type="button" onClick={onUse}>
          使用此应用 <span>→</span>
        </button>
      </div>
    </article>
  );
}

export default function InkStudio() {
  const [tab, setTab] = useState<Tab>("studio");
  const [prompt, setPrompt] = useState(starterPrompt);
  const [app, setApp] = useState<InkApp>(starterApp);
  const [localApps, setLocalApps] = useState<InkApp[]>([]);
  const [publicApps, setPublicApps] = useState<InkApp[]>(featuredApps);
  const [generating, setGenerating] = useState(false);
  const [generatorStatus, setGeneratorStatus] = useState<GeneratorStatus>("checking");
  const [generatorModel, setGeneratorModel] = useState("auto");
  const [previewStatus, setPreviewStatus] = useState<PreviewStatus>("ready");
  const [clockTick, setClockTick] = useState(0);
  const [codeOpen, setCodeOpen] = useState(false);
  const [guideOpen, setGuideOpen] = useState(false);
  const [toast, setToast] = useState<Toast>(null);
  const [deviceName, setDeviceName] = useState<string | null>(null);
  const [deviceStatus, setDeviceStatus] = useState<"idle" | "ready" | "writing" | "scheduled" | "error">("idle");
  const [progress, setProgress] = useState<TodooProgress | null>(null);
  const [scheduleActive, setScheduleActive] = useState(false);
  const [nextRun, setNextRun] = useState<Date | null>(null);
  const [bluetoothSupported, setBluetoothSupported] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const previewVersionRef = useRef(0);
  const driverRef = useRef<TodooCard | null>(null);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const showToast = useCallback((message: string, tone: ToastTone = "info") => {
    setToast({ message, tone });
    setTimeout(() => setToast(null), 3400);
  }, []);

  useEffect(() => {
    try {
      const stored = JSON.parse(localStorage.getItem(LOCAL_APPS_KEY) ?? "[]") as InkApp[];
      if (Array.isArray(stored)) setLocalApps(stored.map(upgradeLegacyApp));
    } catch {
      localStorage.removeItem(LOCAL_APPS_KEY);
    }
  }, []);

  useEffect(() => {
    if (!guideOpen) return;
    const previousOverflow = document.body.style.overflow;
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") setGuideOpen(false);
    };
    document.body.style.overflow = "hidden";
    window.addEventListener("keydown", closeOnEscape);
    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener("keydown", closeOnEscape);
    };
  }, [guideOpen]);

  useEffect(() => {
    fetch("/api/generate")
      .then(async (response) => {
        if (!response.ok) throw new Error("generator unavailable");
        return (await response.json()) as { configured?: boolean; model?: string };
      })
      .then((data) => {
        setGeneratorStatus(data.configured ? "online" : "local");
        setGeneratorModel(data.model || "auto");
      })
      .catch(() => setGeneratorStatus("local"));
  }, []);

  useEffect(() => {
    fetch("/api/apps")
      .then(async (response) => {
        if (!response.ok) throw new Error("gallery unavailable");
        return (await response.json()) as { apps?: InkApp[] };
      })
      .then((data) => {
        if (data.apps?.length) setPublicApps([...data.apps.map(upgradeLegacyApp), ...featuredApps]);
      })
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    setBluetoothSupported(
      Boolean((navigator as Navigator & { bluetooth?: unknown }).bluetooth && globalThis.isSecureContext),
    );
    const driver = new TodooCard(setProgress);
    driverRef.current = driver;
    driver
      .restoreAuthorizedDevice()
      .then((device) => {
        if (device) {
          setDeviceName(device.name ?? "已授权设备");
          setDeviceStatus("ready");
        }
      })
      .catch(() => undefined);
    return () => driver.disconnect();
  }, []);

  useEffect(() => {
    let interval: ReturnType<typeof setInterval> | undefined;
    const delay = 60_050 - (Date.now() % 60_000);
    const timeout = setTimeout(() => {
      setClockTick((tick) => tick + 1);
      interval = setInterval(() => setClockTick((tick) => tick + 1), 60_000);
    }, delay);
    return () => {
      clearTimeout(timeout);
      if (interval) clearInterval(interval);
    };
  }, []);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const version = ++previewVersionRef.current;
    const staging = document.createElement("canvas");
    staging.width = 528;
    staging.height = 792;
    const hasArtwork = Boolean(app.spec.artwork || app.localImage);
    setPreviewStatus(hasArtwork ? "loading" : "ready");
    resolveRuntimeScreen(app, new Date()).then((runtimeSpec) => drawScreen(staging, runtimeSpec, app.localImage)).then((usedArtwork) => {
      if (version !== previewVersionRef.current) return;
      const context = canvas.getContext("2d");
      if (!context) return;
      context.clearRect(0, 0, canvas.width, canvas.height);
      context.drawImage(staging, 0, 0);
      setPreviewStatus(hasArtwork && !usedArtwork ? "fallback" : "ready");
    });
  }, [app.spec, app.localImage, app.prompt, clockTick]);

  const attachLocalImage = async (file?: File) => {
    if (!file) return;
    try {
      const localImage = await prepareLocalImage(file);
      setApp((current) => ({ ...current, localImage }));
      showToast("图片已贴入，生成后会自动参与排版", "success");
    } catch (error) {
      showToast(error instanceof Error ? error.message : "图片处理失败", "error");
    }
  };

  const handlePromptPaste = (event: ClipboardEvent<HTMLTextAreaElement>) => {
    const imageItem = Array.from(event.clipboardData.items).find((item) => item.type.startsWith("image/"));
    if (imageItem) void attachLocalImage(imageItem.getAsFile() ?? undefined);
  };

  const applyGuideExample = (value: string) => {
    setPrompt(value);
    setTab("studio");
    setGuideOpen(false);
    window.setTimeout(() => document.getElementById("app-prompt")?.focus(), 0);
  };

  const generate = async () => {
    if (!prompt.trim()) {
      showToast("先描述你想让屏幕显示什么", "error");
      return;
    }
    setGenerating(true);
    try {
      const response = await fetch("/api/generate", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ prompt }),
      });
      if (!response.ok) throw new Error("生成服务暂时不可用");
      const result = (await response.json()) as {
        app?: InkApp;
        mode?: "llm" | "local";
        model?: string | null;
        warning?: string;
      };
      if (!result.app) throw new Error("生成结果不完整");
      setApp({ ...result.app, localImage: app.localImage });
      if (result.mode === "llm") {
        setGeneratorStatus("online");
        setGeneratorModel(result.model || "auto");
        showToast(`已由 ${result.model || "在线模型"} 生成应用`, "success");
      } else {
        setGeneratorStatus("local");
        showToast(result.warning || "已使用本地模板生成", "info");
      }
    } catch (error) {
      setApp({ ...generateInkApp(prompt), localImage: app.localImage });
      setGeneratorStatus("local");
      showToast(error instanceof Error ? `${error.message}，已使用本地模板` : "已使用本地模板", "info");
    } finally {
      setGenerating(false);
    }
  };

  const updateSchedule = (scheduleMode: ScheduleMode) => {
    setApp((current) => ({ ...current, scheduleMode }));
  };

  const regeneratePreviewArtwork = () => {
    const seed = randomArtworkSeed();
    setApp((current) => current.spec.artwork
      ? {
          ...current,
          spec: {
            ...current.spec,
            artwork: { ...current.spec.artwork, seed },
          },
        }
      : current);
    showToast("正在按原主题重新生成图片素材", "info");
  };

  const saveApp = async () => {
    const saved = { ...app, id: app.id.startsWith("starter") ? `app-${Date.now()}` : app.id };
    const next = [saved, ...localApps.filter((item) => item.id !== saved.id)].slice(0, 30);
    setApp(saved);
    setLocalApps(next);
    localStorage.setItem(LOCAL_APPS_KEY, JSON.stringify(next));

    if (saved.isPublic) {
      try {
        const publicPayload = { ...saved, localImage: undefined };
        const response = await fetch("/api/apps", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify(publicPayload),
        });
        if (!response.ok) throw new Error("publish failed");
        const data = (await response.json()) as { app: InkApp };
        setPublicApps((items) => [data.app, ...items.filter((item) => item.id !== data.app.id)]);
        showToast("已保存到本机，并发布到发现页", "success");
      } catch {
        showToast("已保存到本机；公开发布暂时不可用", "info");
      }
    } else {
      showToast("应用已保存在这台设备上", "success");
    }
  };

  const useApp = (selected: InkApp) => {
    const upgraded = upgradeLegacyApp(selected);
    const cloned = {
      ...upgraded,
      id: `app-${Date.now()}`,
      author: "我",
      isPublic: false,
      createdAt: new Date().toISOString(),
    };
    setApp(cloned);
    setPrompt(cloned.prompt);
    setTab("studio");
    showToast("已复制到创作台，可以继续调整", "success");
  };

  const calculateNextDelay = useCallback((current: InkApp) => {
    if (current.scheduleMode !== "daily") return intervalFor(current);
    const [hour, minute] = current.dailyTime.split(":").map(Number);
    const now = new Date();
    const next = new Date(now);
    next.setHours(hour, minute, 0, 0);
    if (next <= now) next.setDate(next.getDate() + 1);
    return next.getTime() - now.getTime();
  }, []);

  const stopSchedule = useCallback(() => {
    if (timerRef.current) clearTimeout(timerRef.current);
    timerRef.current = null;
    setScheduleActive(false);
    setNextRun(null);
    setDeviceStatus(deviceName ? "ready" : "idle");
    driverRef.current?.disconnect();
  }, [deviceName]);

  const runTransfer = useCallback(async () => {
    const driver = driverRef.current;
    const canvas = canvasRef.current;
    if (!driver || !canvas) return false;
    if (previewStatus === "loading") {
      showToast("图片素材仍在加载，请稍候再写入", "info");
      return false;
    }
    if (document.visibilityState !== "visible") {
      showToast("页面在后台，已推迟到重新打开后写入", "info");
      return false;
    }
    setDeviceStatus("writing");
    try {
      let runtimeSpec = await resolveRuntimeScreen(app, new Date());
      let nextSeed: number | null = null;
      if (runtimeSpec.artwork?.rotateOnRefresh) {
        nextSeed = randomArtworkSeed();
        runtimeSpec = {
          ...runtimeSpec,
          artwork: { ...runtimeSpec.artwork, seed: nextSeed },
        };
      }
      const hasArtwork = Boolean(runtimeSpec.artwork || app.localImage);
      setPreviewStatus(hasArtwork ? "loading" : "ready");
      const usedArtwork = await renderScreenToCanvas(canvas, runtimeSpec, app.localImage);
      setPreviewStatus(hasArtwork && !usedArtwork ? "fallback" : "ready");
      await driver.writeCanvas(canvas, true);
      if (nextSeed !== null) {
        setApp((current) => current.spec.artwork
          ? {
              ...current,
              spec: {
                ...current.spec,
                artwork: { ...current.spec.artwork, seed: nextSeed },
              },
            }
          : current);
      }
      setDeviceStatus("ready");
      showToast("帧已发送，墨水屏可能还会显色几分钟", "success");
      return true;
    } catch (error) {
      const message = error instanceof Error ? error.message : "写入失败";
      setDeviceStatus("error");
      showToast(message, "error");
      return false;
    }
  }, [app.localImage, app.prompt, app.spec, previewStatus, showToast]);

  const scheduleFollowingRun = useCallback(() => {
    const delay = calculateNextDelay(app);
    if (!delay) {
      setScheduleActive(false);
      return;
    }
    const due = new Date(Date.now() + delay);
    setNextRun(due);
    setScheduleActive(true);
    setDeviceStatus("scheduled");
    timerRef.current = setTimeout(async () => {
      const wrote = await runTransfer();
      if (!wrote && document.visibilityState !== "visible") {
        timerRef.current = setTimeout(scheduleFollowingRun, 60_000);
        return;
      }
      scheduleFollowingRun();
    }, delay);
  }, [app, calculateNextDelay, runTransfer]);

  const start = async () => {
    const driver = driverRef.current;
    if (!driver?.supported) {
      showToast("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站", "error");
      setDeviceStatus("error");
      return;
    }
    try {
      let device = driver.selectedDevice;
      if (!device) device = await driver.restoreAuthorizedDevice();
      if (!device) device = await driver.requestDevice();
      setDeviceName(device.name ?? "TodooCard");
      const wrote = await runTransfer();
      if (wrote && app.scheduleMode !== "once") scheduleFollowingRun();
    } catch (error) {
      const message = error instanceof Error ? error.message : "没有选择设备";
      showToast(message, "error");
      setDeviceStatus("error");
    }
  };

  const contentTitle = tab === "mine" ? "我的应用" : tab === "explore" ? "发现灵感" : tab === "device" ? "设备中心" : null;

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <button className="brand" type="button" onClick={() => setTab("studio")} aria-label="返回创作台">
          <span className="brand-mark">I</span>
          <span>Inkloop</span>
        </button>
        <nav aria-label="主导航">
          {navItems.map((item) => (
            <button
              type="button"
              key={item.id}
              className={tab === item.id ? "active" : ""}
              onClick={() => setTab(item.id)}
            >
              <span>{item.glyph}</span>
              {item.label}
            </button>
          ))}
        </nav>
        <div className="sidebar-device">
          <span className={`status-dot ${deviceStatus}`} />
          <div>
            <strong>{deviceName ?? "未连接设备"}</strong>
            <small>{deviceName ? "已记住，可自动重连" : "TodooCard · BLE"}</small>
          </div>
        </div>
        <a className="product-link" href="https://p.todoo.tech/?lang=zh" target="_blank" rel="noreferrer">
          产品信息 <span>↗</span>
        </a>
      </aside>

      <section className="workspace">
        <header className="topbar">
          <div>
            <span className="eyebrow">TODOO · 6-COLOR E-PAPER</span>
            <strong>{contentTitle ?? app.title}</strong>
          </div>
          <div className="topbar-actions">
            <button type="button" className="guide-button" onClick={() => setGuideOpen(true)}>
              <span>?</span> 使用说明
            </button>
            <span className={`support-chip ${bluetoothSupported ? "ok" : "warn"}`}>
              <i /> {bluetoothSupported ? "蓝牙可用" : "请使用 Chromium"}
            </span>
            {tab === "studio" && (
              <button type="button" className="save-button" onClick={saveApp}>
                保存应用
              </button>
            )}
          </div>
        </header>

        {tab === "studio" && (
          <>
            <div className="studio-grid">
              <section className="prompt-panel panel">
                <div className="panel-heading">
                  <span className="step-number">01</span>
                  <div>
                    <h2>描述你想看到的内容</h2>
                    <p>说人话就好，生成器会补全数据与排版逻辑。</p>
                  </div>
                </div>
                <label htmlFor="app-prompt">应用需求</label>
                <div className="prompt-box">
                  <textarea
                    id="app-prompt"
                    value={prompt}
                    onChange={(event) => setPrompt(event.target.value)}
                    onPaste={handlePromptPaste}
                    placeholder="例如：每天早上 8 点显示上海天气…"
                    rows={7}
                  />
                  <div className="prompt-counter">{prompt.length} / 300</div>
                </div>
                <div className="prompt-attachment">
                  <input
                    ref={fileInputRef}
                    type="file"
                    accept="image/*"
                    onChange={(event) => {
                      void attachLocalImage(event.target.files?.[0]);
                      event.target.value = "";
                    }}
                  />
                  {app.localImage ? (
                    <div className="attached-image">
                      <span
                        className="attached-image-thumb"
                        role="img"
                        aria-label="已贴入的图片缩略图"
                        style={{ backgroundImage: `url(${app.localImage})` }}
                      />
                      <span className="attached-image-copy"><strong>图片已贴入</strong><small>将转换成六色参与排版</small></span>
                      <button type="button" onClick={() => setApp((current) => ({ ...current, localImage: undefined }))}>移除</button>
                    </div>
                  ) : (
                    <button type="button" className="attach-button" onClick={() => fileInputRef.current?.click()}>
                      <span>＋</span> 选择图片 <small>也可直接粘贴</small>
                    </button>
                  )}
                </div>
                <div className="suggestions">
                  <span>试试这些</span>
                  {samplePrompts.map((sample) => (
                    <button type="button" key={sample} onClick={() => setPrompt(sample)}>
                      {sample.replace("每天 8 点", "天气").replace("显示", "").slice(0, 10)}
                    </button>
                  ))}
                </div>
                <button className="generate-button" type="button" onClick={generate} disabled={generating}>
                  <span>{generating ? (generatorStatus === "online" ? "模型编码中" : "生成中") : "✦ 生成应用"}</span>
                  <i>{generating ? "•••" : "→"}</i>
                </button>
                <div className="generator-note">
                  <span className={generatorStatus === "online" ? "online" : ""}>LLM</span>
                  <p>
                    {generatorStatus === "checking"
                      ? "正在检查在线编码服务…"
                      : generatorStatus === "online"
                        ? `Tsingfly 在线编码已就绪 · ${generatorModel === "auto" ? "自动选择模型" : generatorModel}`
                        : "等待配置 LLM_API_KEY · 当前自动使用本地模板"}
                  </p>
                </div>
              </section>

              <section className="preview-panel panel">
                <div className="preview-toolbar">
                  <div>
                    <span className="step-number">02</span>
                    <div>
                      <h2>屏幕预览</h2>
                      <p>
                        528 × 792 · {previewStatus === "loading"
                          ? "正在获取并转换图片素材"
                          : previewStatus === "fallback"
                            ? app.spec.artwork?.layout === "fullscreen"
                              ? "图片暂不可用，已保持纯图片模式"
                              : "素材暂不可用，已使用图形排版"
                            : app.localImage
                              ? "本机图片已转换为实际六色"
                              : app.spec.artwork
                                ? "图片已转换为实际六色"
                              : "实际六色色板"}
                      </p>
                    </div>
                  </div>
                  <div className="preview-actions">
                    <button
                      className="regenerate-preview"
                      type="button"
                      onClick={regeneratePreviewArtwork}
                      disabled={!app.spec.artwork || Boolean(app.localImage) || previewStatus === "loading"}
                      title={app.localImage ? "当前使用的是你贴入的图片" : app.spec.artwork ? `保持主题：${app.spec.artwork.query}` : "当前应用没有图片素材"}
                    >
                      ↻ 重新生成
                    </button>
                    <span className="scale-chip">50%</span>
                  </div>
                </div>
                <div className="canvas-stage">
                  <div className="device-shadow" />
                  <div className="device-frame">
                    <div className="device-label">TODOO</div>
                    <canvas ref={canvasRef} width={528} height={792} aria-label="电子墨水屏预览" />
                    <div className="device-port" />
                  </div>
                </div>
                <div className="palette-strip" aria-label="屏幕支持六种颜色">
                  {[
                    ["黑", "#111"],
                    ["白", "#f6f2df"],
                    ["黄", "#e5c900"],
                    ["红", "#dc3f2f"],
                    ["蓝", "#2756c7"],
                    ["绿", "#087c4e"],
                  ].map(([label, color]) => (
                    <span key={label}><i style={{ background: color }} />{label}</span>
                  ))}
                </div>
              </section>

              <section className="settings-panel panel">
                <div className="panel-heading compact">
                  <span className="step-number">03</span>
                  <div>
                    <h2>保存与刷新</h2>
                    <p>选择写入频率与分享范围。</p>
                  </div>
                </div>
                <label>刷新计划</label>
                <div className="schedule-options">
                  {[
                    ["once", "单次写入", "立即执行一次"],
                    ["hourly", "每小时", "整点后循环"],
                    ["daily", "每天", app.dailyTime],
                    ["custom", "自定义", `${app.customMinutes} 分钟`],
                  ].map(([value, title, detail]) => (
                    <button
                      type="button"
                      key={value}
                      onClick={() => updateSchedule(value as ScheduleMode)}
                      className={app.scheduleMode === value ? "selected" : ""}
                    >
                      <i>{app.scheduleMode === value ? "●" : "○"}</i>
                      <span><strong>{title}</strong><small>{detail}</small></span>
                    </button>
                  ))}
                </div>
                {app.scheduleMode === "daily" && (
                  <div className="inline-field">
                    <label htmlFor="daily-time">每天执行时间</label>
                    <input
                      id="daily-time"
                      type="time"
                      value={app.dailyTime}
                      onChange={(event) => setApp((current) => ({ ...current, dailyTime: event.target.value }))}
                    />
                  </div>
                )}
                {app.scheduleMode === "custom" && (
                  <div className="inline-field">
                    <label htmlFor="custom-minutes">间隔分钟（最短 1 分钟）</label>
                    <input
                      id="custom-minutes"
                      type="number"
                      min={1}
                      step={1}
                      value={app.customMinutes}
                      onChange={(event) =>
                        setApp((current) => ({
                          ...current,
                          customMinutes: Math.max(1, Math.round(Number(event.target.value))),
                        }))
                      }
                    />
                  </div>
                )}
                <div className="sharing-row">
                  <div>
                    <strong>公开到发现页</strong>
                    <small>其他人可以复制并使用</small>
                  </div>
                  <button
                    type="button"
                    role="switch"
                    aria-checked={app.isPublic}
                    className={`switch ${app.isPublic ? "on" : ""}`}
                    onClick={() => setApp((current) => ({ ...current, isPublic: !current.isPublic }))}
                  >
                    <span />
                  </button>
                </div>
                <button type="button" className="code-toggle" onClick={() => setCodeOpen((open) => !open)}>
                  <span><i>&lt;/&gt;</i> 查看生成逻辑</span><b>{codeOpen ? "−" : "+"}</b>
                </button>
                {codeOpen && <pre className="code-preview"><code>{app.code}</code></pre>}
              </section>
            </div>

            <div className="run-dock">
              <div className="run-status">
                <span className={`run-icon ${deviceStatus}`}>{deviceStatus === "writing" ? "↻" : "⌁"}</span>
                <div>
                  <strong>
                    {deviceStatus === "writing" ? progress?.message ?? "正在写入" : scheduleActive ? "定时任务运行中" : deviceName ?? "准备写入 TodooCard"}
                  </strong>
                  <small>
                    {nextRun
                      ? `下次执行 ${nextRun.toLocaleString("zh-CN", { hour: "2-digit", minute: "2-digit" })}`
                      : deviceName
                        ? "已授权设备不会再次弹出选择器"
                        : "首次需要手动选择设备 · 之后自动重连"}
                  </small>
                </div>
              </div>
              {deviceStatus === "writing" && (
                <div className="transfer-progress"><i style={{ width: `${progress?.percent ?? 0}%` }} /></div>
              )}
              <div className="run-actions">
                {scheduleActive && <button type="button" className="stop-button" onClick={stopSchedule}>停止任务</button>}
                <button type="button" className="start-button" onClick={start} disabled={deviceStatus === "writing"}>
                  <span>{deviceStatus === "writing" ? "正在写入" : scheduleActive ? "立即再写一次" : "开始写入"}</span>
                  <i>→</i>
                </button>
              </div>
            </div>
          </>
        )}

        {tab === "mine" && (
          <section className="collection-view">
            <div className="collection-hero">
              <span className="eyebrow">LOCAL LIBRARY</span>
              <h1>留在你设备里的应用</h1>
              <p>这些应用保存在浏览器本机，不上传个人数据。清理浏览器数据会一并删除。</p>
              <button type="button" onClick={() => setTab("studio")}>＋ 创建新应用</button>
            </div>
            {localApps.length ? (
              <div className="card-grid">
                {localApps.map((item) => <AppCard key={item.id} app={item} local onUse={() => useApp(item)} />)}
              </div>
            ) : (
              <div className="empty-state">
                <span>▦</span><h2>还没有保存的应用</h2><p>在创作台生成并保存，第一个应用就会出现在这里。</p>
              </div>
            )}
          </section>
        )}

        {tab === "explore" && (
          <section className="collection-view explore-view">
            <div className="collection-hero split">
              <div>
                <span className="eyebrow">PUBLIC GALLERY</span>
                <h1>把别人的灵感，变成你的屏幕</h1>
                <p>所有应用都能一键复制到创作台，再按自己的数据与频率修改。</p>
              </div>
              <div className="gallery-stat"><b>{publicApps.length}</b><span>公开应用</span></div>
            </div>
            <div className="filter-row"><button className="active">精选</button><button>生活</button><button>效率</button><button>数据</button></div>
            <div className="card-grid">
              {publicApps.map((item, index) => <AppCard key={`${item.id}-${index}`} app={item} onUse={() => useApp(item)} />)}
            </div>
          </section>
        )}

        {tab === "device" && (
          <section className="device-view">
            <div className="device-hero">
              <div>
                <span className="eyebrow">WEB BLUETOOTH · FEF0 / FEF1 / FEF2</span>
                <h1>一次选择，页面打开时自动写入。</h1>
                <p>首选 Android 或桌面版 Chromium。首次必须由点击唤起设备选择；授权后本页面会保留设备，并优先恢复历史授权。</p>
              </div>
              <div className="connection-card">
                <span className={`status-orb ${deviceName ? "connected" : ""}`}>⌁</span>
                <strong>{deviceName ?? "TodooCard 未连接"}</strong>
                <small>{deviceName ? "授权已保存 · 等待写入" : "NEMR99803797 / PICKSMART · 528 × 792"}</small>
                <button type="button" onClick={start}>{deviceName ? "测试写入" : "选择设备"}</button>
              </div>
            </div>
            <div className="feasibility-grid">
              <article className="verdict-card yes">
                <span>可以做到</span>
                <h2>同一会话自动重连</h2>
                <p>保留 BluetoothDevice，定时到点后连接 GATT、写入、断开。支持 getDevices() 时，下次访问也可找回已授权设备。</p>
              </article>
              <article className="verdict-card caution">
                <span>有条件</span>
                <h2>使用 setTimeout 调度</h2>
                <p>每次传输完成后再计算下一次时间，避免 setInterval 在写屏耗时较长时产生重叠任务。</p>
              </article>
              <article className="verdict-card no">
                <span>无法保证</span>
                <h2>浏览器关闭后无人值守</h2>
                <p>后台节流、系统休眠和设备唤醒都会打断任务；Service Worker 也不能在后台调用 Web Bluetooth。</p>
              </article>
            </div>
            <div className="protocol-table">
              <div><span>传输协议</span><strong>BLE GATT</strong></div>
              <div><span>设备服务</span><strong>FEF0 · FEF1 · FEF2</strong></div>
              <div><span>单次数据</span><strong>219,120 bytes · 913 包</strong></div>
              <div><span>显色时间</span><strong>复杂画面可能约 3 分钟</strong></div>
            </div>
            <div className="reliability-note">
              <span>长期无人值守建议</span>
              <p>如果必须在浏览器关闭后也准时刷新，建议把调度与蓝牙下沉到常开网关（树莓派 / ESP32 / 原生 App），网页只负责编辑应用与下发计划。</p>
            </div>
          </section>
        )}
      </section>

      {guideOpen && (
        <div className="guide-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) setGuideOpen(false);
        }}>
          <section className="guide-dialog" role="dialog" aria-modal="true" aria-labelledby="guide-title">
            <div className="guide-header">
              <div>
                <span className="eyebrow">FROM FIRST SCREEN TO AUTOMATION</span>
                <h2 id="guide-title">三步用好 Inkloop</h2>
                <p>先做一张满意的画面，再保存应用、选择刷新时间，最后写入屏幕。</p>
              </div>
              <button type="button" className="guide-close" onClick={() => setGuideOpen(false)} aria-label="关闭使用说明">×</button>
            </div>

            <div className="guide-levels">
              {guideExamples.map((item) => (
                <article key={item.number} className="guide-level">
                  <div className="guide-level-top"><span>{item.number}</span><i>{item.level}</i></div>
                  <h3>{item.title}</h3>
                  <p>{item.description}</p>
                  <button type="button" onClick={() => applyGuideExample(item.prompt)}>{item.action} <span>→</span></button>
                </article>
              ))}
            </div>

            <div className="bluetooth-guide">
              <div className="bluetooth-guide-icon">⌁</div>
              <div>
                <strong>写入前，把设备放在电脑附近</strong>
                <p>建议保持在 1–3 米蓝牙范围内并唤醒屏幕。首次点击“开始写入”后选择 PICKSMART；之后保持这个页面打开、电脑不要休眠，就能按计划自动重连。</p>
              </div>
              <button type="button" onClick={() => { setGuideOpen(false); setTab("device"); }}>查看设备说明</button>
            </div>

            <p className="guide-footnote">本机图片随应用保存在当前浏览器；公开分享时不会上传你的私人图片，而会保留可复用的版式和主题。</p>
          </section>
        </div>
      )}

      {toast && <div className={`toast ${toast.tone}`} role="status"><span>{toast.tone === "success" ? "✓" : toast.tone === "error" ? "!" : "i"}</span>{toast.message}</div>}
    </main>
  );
}
