"use client";

import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ClipboardEvent,
  type KeyboardEvent as ReactKeyboardEvent,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { create as createQrCode } from "qrcode";
import {
  DEFAULT_ELEMENT_POSITIONS,
  DEFAULT_ELEMENT_SIZES,
  displaySettings,
  featuredApps,
  generateInkApp,
  inferWeatherCity,
  intervalFor,
  scheduleLabel,
  starterApp,
  starterPrompt,
  type ArtworkSpec,
  type AgendaEvent,
  type AgendaRangeMode,
  type AgendaView,
  type CalendarEvent,
  type InkApp,
  type ScheduleMode,
  type ScreenDisplay,
  type ScreenElementKey,
  type ScreenFont,
  type ScreenRenderMode,
  type ScreenOrientation,
  type ScreenSpec,
} from "./lib/app-model";
import { TodooCard, type TodooProgress } from "./lib/todoo-card";

type Tab = "studio" | "mine" | "explore" | "device";
type Toast = { tone: "success" | "error" | "info"; message: string } | null;
type ToastTone = NonNullable<Toast>["tone"];
type GeneratorStatus = "checking" | "online" | "local";
type PreviewStatus = "ready" | "loading" | "fallback";
type DeviceTaskStatus = "scheduled" | "writing" | "error";
type ArtworkCredit = { provider: string; url: string };
type CalendarPreferences = {
  customUrl: string;
  chinaHolidays: boolean;
  lunar: boolean;
};
type CalendarFeedPayload = {
  events?: CalendarEvent[];
  timedEvents?: AgendaEvent[];
  warnings?: string[];
};

function agendaWindow(table: Extract<NonNullable<ScreenSpec["table"]>, { type: "agenda" }>, now: Date) {
  if (table.rangeMode === "custom") {
    const start = table.customStart ? new Date(table.customStart) : now;
    const end = table.customEnd ? new Date(table.customEnd) : new Date(start.getTime() + table.rangeHours * 60 * 60 * 1000);
    if (Number.isFinite(start.getTime()) && Number.isFinite(end.getTime()) && end > start) return { start, end };
  }
  if (table.rangeMode === "today") {
    const start = new Date(now);
    start.setHours(0, 0, 0, 0);
    const end = new Date(start);
    end.setDate(end.getDate() + 1);
    return { start, end };
  }
  if (table.view === "workweek") {
    const start = new Date(now);
    start.setHours(0, 0, 0, 0);
    const weekday = start.getDay() || 7;
    start.setDate(start.getDate() - weekday + 1);
    const end = new Date(start);
    end.setDate(end.getDate() + 5);
    return { start, end };
  }
  return { start: now, end: new Date(now.getTime() + table.rangeHours * 60 * 60 * 1000) };
}

type DeviceProfile = {
  id: string;
  name: string;
};

type AuthorizedBluetoothDevice = {
  id: string;
  name?: string | null;
};

type DeviceTask = {
  id: string;
  app: InkApp;
  deviceId: string;
  deviceName: string;
  status: DeviceTaskStatus;
  nextRunAt: number | null;
  lastRunAt: number | null;
  successCount: number;
  failureCount: number;
  consecutiveFailures: number;
  lastError: string | null;
  lastCanvas?: string;
};

type DragPreview = {
  element: ScreenElementKey;
  x: number;
  y: number;
};

const LOCAL_APPS_KEY = "inkloop-apps-v1";
const WEATHER_CITY_KEY = "inkloop-weather-city-v1";
const CALENDAR_PREFERENCES_KEY = "inkloop-calendar-sources-v1";
const DEFAULT_CALENDAR_PREFERENCES: CalendarPreferences = {
  customUrl: "",
  chinaHolidays: false,
  lunar: false,
};
const GALLERY_PREVIEW_DATE = new Date("2026-08-01T12:34:00+08:00");
const EPAPER_WHITE = "#fafaf8";

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
  "生成本月日历，8号项目发布，18号复盘",
  "生成周一到周五的课程表",
  "横版苹果日历风格，显示未来三天日程",
  "每 15 分钟更新会议室状态",
  "每小时显示本月销售目标进度",
];

const quoteOptions = [
  "把注意力留给真正重要的事",
  "今天也值得认真生活",
  "慢一点，也是在向前",
  "先完成，再完善",
  "愿每一步都有清晰的回响",
  "专注当下，一次只做一件事",
  "保持好奇，保持热爱",
  "去做让自己眼睛发亮的事",
];

const screenElementOptions: Array<{ key: ScreenElementKey; label: string; width: number; height: number }> = [
  { key: "quote", label: "今日名言", width: 400, height: 58 },
  { key: "logo", label: "LOGO", width: 220, height: 48 },
  { key: "date", label: "日期", width: 260, height: 54 },
  { key: "time", label: "时间", width: 170, height: 56 },
  { key: "timeLarge", label: "时间（大）", width: 430, height: 154 },
  { key: "weather", label: "天气（小）", width: 310, height: 64 },
  { key: "weatherLarge", label: "天气（大）", width: 430, height: 230 },
  { key: "qr", label: "二维码", width: 176, height: 176 },
];

const screenFontOptions: Array<{ value: ScreenFont; label: string }> = [
  { value: "sans", label: "思源黑体 · 清晰" },
  { value: "serif", label: "思源宋体 · 优雅" },
  { value: "rounded", label: "M PLUS 圆体 · 亲和" },
  { value: "mono", label: "等宽数字 · 精准" },
  { value: "handwritten", label: "马善政手写 · 醒目" },
];

const renderModeOptions: Array<{
  value: ScreenRenderMode;
  label: string;
  description: string;
}> = [
  { value: "official", label: "Official Skill", description: "完整抖动，适合照片、渐变和丰富细节" },
  { value: "inkloop-text", label: "Inkloop text", description: "低噪点，适合课程表、日历和纯文字画面" },
];

const knownWeatherCities = [
  "上海", "北京", "深圳", "广州", "杭州", "成都", "重庆", "南京", "苏州", "武汉",
  "西安", "天津", "青岛", "厦门", "长沙", "郑州", "昆明", "大连", "宁波", "香港",
  "澳门", "台北", "东京", "大阪", "首尔", "新加坡", "伦敦", "巴黎", "纽约", "洛杉矶",
];

function applyPreferredCityToGeneratedApp(generated: InkApp, sourcePrompt: string, preferredCity: string) {
  const display = displaySettings(generated.spec);
  if (!display.weather && !display.weatherLarge && generated.spec.kind !== "weather") return generated;
  const explicitCity = knownWeatherCities.find((city) => sourcePrompt.includes(city));
  return {
    ...generated,
    spec: {
      ...generated.spec,
      city: explicitCity || preferredCity || generated.spec.city || "上海",
    },
  };
}

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
  const normalizedApp: InkApp = {
    ...savedApp,
    spec: {
      ...savedApp.spec,
      display: displaySettings(savedApp.spec, Boolean(savedApp.localImage)),
    },
  };
  const prompt = savedApp.prompt || "";
  const imageOnly = ["不要任何其他", "不要其他", "不要文字", "只有图片", "只要图片", "纯图片", "纯图"]
    .some((term) => prompt.includes(term));
  const wantsFullscreen = imageOnly && (
    ["全屏", "铺满", "满屏"].some((term) => prompt.includes(term))
    || ["图片", "照片", "海报", "插画"].some((term) => prompt.includes(term))
  );
  if (!wantsFullscreen || !savedApp.spec.artwork) return normalizedApp;
  const hasExplicitSchedule = /每\s*\d+\s*分钟|每(?:个)?小时|每天|每日|早上|上午|下午|晚上/.test(prompt);
  return {
    ...normalizedApp,
    scheduleMode: hasExplicitSchedule ? savedApp.scheduleMode : "once",
    spec: {
      ...normalizedApp.spec,
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

function fitText(
  ctx: CanvasRenderingContext2D,
  text: string,
  maxWidth: number,
  startSize: number,
  family = 'Arial, "PingFang SC", sans-serif',
) {
  let size = startSize;
  while (size > 24) {
    ctx.font = `800 ${size}px ${family}`;
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

function formatExactTime(value: number) {
  return new Date(value).toLocaleString("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
}

function formatRemaining(value: number | null, now: number) {
  if (!value) return "等待安排";
  const total = Math.max(0, Math.ceil((value - now) / 1000));
  const days = Math.floor(total / 86_400);
  const hours = Math.floor((total % 86_400) / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const seconds = total % 60;
  const clock = [hours, minutes, seconds]
    .map((part) => String(part).padStart(2, "0"))
    .join(":");
  return days ? `${days}天 ${clock}` : clock;
}

function retryDelayForFailures(failures: number) {
  return Math.min(300_000, 15_000 * 2 ** Math.max(0, failures - 1));
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
  const displayTime = new Date(now.getTime() + 60_000);
  const pad = (part: number) => String(part).padStart(2, "0");
  const date = `${displayTime.getFullYear()}-${pad(displayTime.getMonth() + 1)}-${pad(displayTime.getDate())}`;
  const weekday = new Intl.DateTimeFormat("zh-CN", { weekday: "short" }).format(displayTime);
  return {
    ...spec,
    eyebrow: replaceTimeVariables(spec.eyebrow, displayTime),
    title: replaceTimeVariables(spec.title, displayTime),
    value: replaceTimeVariables(spec.value, displayTime),
    unit: replaceTimeVariables(spec.unit, displayTime),
    detail: replaceTimeVariables(spec.detail, displayTime),
    footer: replaceTimeVariables(spec.footer, displayTime),
    display: displaySettings(spec),
    dateText: `${weekday} · ${date}`,
    timeText: `${pad(displayTime.getHours())}:${pad(displayTime.getMinutes())}`,
  };
}

type WeatherPayload = {
  available?: boolean;
  city?: string;
  temperature?: number;
  low?: number;
  high?: number;
  rainProbability?: number;
  condition?: string;
};

async function resolveRuntimeScreen(
  currentApp: InkApp,
  now = new Date(),
  calendarPreferences?: CalendarPreferences,
  onCalendarNotice?: (message: string | null) => void,
): Promise<ScreenSpec> {
  const resolved = resolveTimeVariables(currentApp.spec, now);
  if (resolved.table?.type === "agenda") {
    const preferences = calendarPreferences ?? DEFAULT_CALENDAR_PREFERENCES;
    const window = agendaWindow(resolved.table, now);
    let remoteEvents: AgendaEvent[] = [];
    if (preferences.customUrl || preferences.chinaHolidays) {
      try {
        const response = await fetch("/api/calendar", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({
            view: "agenda",
            start: window.start.toISOString(),
            end: window.end.toISOString(),
            timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
            customUrl: preferences.customUrl || undefined,
            presets: preferences.chinaHolidays ? ["china-holidays"] : [],
          }),
          cache: "no-store",
        });
        if (response.ok) {
          const payload = await response.json() as CalendarFeedPayload;
          remoteEvents = Array.isArray(payload.timedEvents) ? payload.timedEvents : [];
          onCalendarNotice?.(payload.warnings?.length ? payload.warnings.join("；") : null);
        } else {
          onCalendarNotice?.("在线日历暂时无法读取");
        }
      } catch {
        remoteEvents = [];
        onCalendarNotice?.("在线日历暂时无法读取");
      }
    } else {
      onCalendarNotice?.(null);
    }
    const events = [...resolved.table.events, ...remoteEvents]
      .filter((event) => {
        const start = Date.parse(event.start);
        const end = Date.parse(event.end);
        return Number.isFinite(start) && Number.isFinite(end) && start < window.end.getTime() && end > window.start.getTime();
      })
      .filter((event, index, values) => values.findIndex((item) => item.uid === event.uid) === index)
      .sort((left, right) => Date.parse(left.start) - Date.parse(right.start))
      .slice(0, 80);
    return {
      ...resolved,
      table: { ...resolved.table, events },
    };
  }
  if (resolved.table?.type === "calendar") {
    const preferences = calendarPreferences ?? DEFAULT_CALENDAR_PREFERENCES;
    let remoteEvents: CalendarEvent[] = [];
    if (preferences.customUrl || preferences.chinaHolidays) {
      try {
        const response = await fetch("/api/calendar", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({
            year: resolved.table.year,
            month: resolved.table.month,
            customUrl: preferences.customUrl || undefined,
            presets: preferences.chinaHolidays ? ["china-holidays"] : [],
          }),
          cache: "no-store",
        });
        if (response.ok) {
          const payload = await response.json() as CalendarFeedPayload;
          remoteEvents = Array.isArray(payload.events) ? payload.events : [];
          onCalendarNotice?.(payload.warnings?.length ? payload.warnings.join("；") : null);
        } else {
          onCalendarNotice?.("在线日历暂时无法读取");
        }
      } catch {
        remoteEvents = [];
        onCalendarNotice?.("在线日历暂时无法读取");
      }
    } else {
      onCalendarNotice?.(null);
    }
    const events = [...resolved.table.events, ...remoteEvents]
      .filter((event, index, values) => values.findIndex((item) => item.day === event.day && item.text === event.text) === index)
      .sort((left, right) => left.day - right.day)
      .slice(0, 24);
    return {
      ...resolved,
      table: {
        ...resolved.table,
        events,
        lunar: Boolean(resolved.table.lunar || preferences.lunar),
      },
    };
  }
  const display = displaySettings(resolved);
  if (
    !display.weather
    && !display.weatherLarge
    || resolved.artwork?.layout === "fullscreen"
  ) return resolved;
  const city = resolved.city || inferWeatherCity(currentApp.prompt);
  try {
    const response = await fetch(`/api/weather?city=${encodeURIComponent(city)}`, { cache: "no-store" });
    if (!response.ok) throw new Error("weather unavailable");
    const weather = (await response.json()) as WeatherPayload;
    if (weather.available === false) {
      const unavailableOverlay = {
        city,
        weatherText: `${city} · 天气暂不可用`,
        weatherValue: "--",
        weatherUnit: "°C",
        weatherDetail: "天气服务暂时不可用",
        weatherAccent: "yellow" as const,
      };
      if (resolved.kind !== "weather" || resolved.clock?.enabled) {
        return { ...resolved, ...unavailableOverlay };
      }
      return {
        ...resolved,
        ...unavailableOverlay,
        eyebrow: `${city} · 今日`,
        title: "今日天气",
        value: "--",
        unit: "°C",
        detail: "天气服务暂时不可用",
        footer: "稍后刷新会自动重试",
      };
    }
    if (
      typeof weather.temperature !== "number"
      || typeof weather.low !== "number"
      || typeof weather.high !== "number"
    ) {
      throw new Error("weather incomplete");
    }
    const rainProbability = typeof weather.rainProbability === "number" ? weather.rainProbability : 0;
    const weekday = new Intl.DateTimeFormat("zh-CN", { weekday: "short" }).format(now);
    const resolvedCity = weather.city || city;
    const temperature = String(Math.round(weather.temperature));
    const detail = `${weather.condition || "天气多变"} · ${Math.round(weather.low)}—${Math.round(weather.high)}°C`;
    const weatherText = `${resolvedCity} · ${temperature}° · ${weather.condition || "天气多变"}`;
    const weatherAccent = rainProbability >= 45 ? "red" as const : "yellow" as const;
    const weatherOverlay = {
      city: resolvedCity,
      weatherText,
      weatherValue: temperature,
      weatherUnit: "°C",
      weatherDetail: detail,
      weatherAccent,
    };
    if (resolved.kind !== "weather" || resolved.clock?.enabled) {
      return {
        ...resolved,
        ...weatherOverlay,
      };
    }
    return {
      ...resolved,
      ...weatherOverlay,
      eyebrow: `${resolvedCity} · ${weekday}`,
      title: "今日天气",
      value: temperature,
      unit: "°C",
      detail,
      footer: rainProbability >= 45
        ? `降雨${Math.round(rainProbability)}% · 带伞 · Open-Meteo`
        : "少雨 · 适合出门 · Open-Meteo",
      accent: weatherAccent,
    };
  } catch {
    const unavailableOverlay = {
      city,
      weatherText: `${city} · 天气暂不可用`,
      weatherValue: "--",
      weatherUnit: "°C",
      weatherDetail: "天气数据暂不可用",
      weatherAccent: "yellow" as const,
    };
    if (resolved.kind !== "weather" || resolved.clock?.enabled) {
      return { ...resolved, ...unavailableOverlay };
    }
    return {
      ...resolved,
      ...unavailableOverlay,
      eyebrow: resolved.eyebrow === "—" ? `${city} · 今日` : resolved.eyebrow,
      title: resolved.title === "—" ? "今日天气" : resolved.title,
      value: resolved.value === "—" ? "--" : resolved.value,
      unit: "°C",
      detail: resolved.detail === "—" ? "天气数据暂不可用" : resolved.detail,
      footer: resolved.footer === "—" ? "稍后刷新重试" : resolved.footer,
    };
  }
}

const screenFonts = {
  sans: '"Noto Sans SC", "PingFang SC", sans-serif',
  serif: '"Noto Serif SC", "Songti SC", serif',
  rounded: '"M PLUS Rounded 1c", "Noto Sans SC", sans-serif',
  mono: '"Geist Mono", "SFMono-Regular", "Noto Sans SC", monospace',
  handwritten: '"Ma Shan Zheng", "Noto Sans SC", cursive',
} as const;

function screenFontFamily(spec: ScreenSpec) {
  return screenFonts[displaySettings(spec).font];
}

function screenElementFontFamily(spec: ScreenSpec, element: ScreenElementKey) {
  const display = displaySettings(spec);
  return screenFonts[display.elementFonts[element] ?? display.font];
}

function screenElementPosition(spec: ScreenSpec, element: ScreenElementKey) {
  return displaySettings(spec).positions[element] ?? DEFAULT_ELEMENT_POSITIONS[element];
}

function screenOrientation(spec: ScreenSpec): ScreenOrientation {
  return spec.orientation === "landscape" ? "landscape" : "portrait";
}

function screenDimensions(spec: ScreenSpec) {
  return screenOrientation(spec) === "landscape"
    ? { width: 792, height: 528 }
    : { width: 528, height: 792 };
}

function screenElementCanvasPosition(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  element: ScreenElementKey,
) {
  const position = screenElementPosition(spec, element);
  return {
    x: position.x / 528 * ctx.canvas.width,
    y: position.y / 792 * ctx.canvas.height,
  };
}

function screenElementFontSize(spec: ScreenSpec, element: ScreenElementKey) {
  const value = displaySettings(spec).elementSizes[element] ?? DEFAULT_ELEMENT_SIZES[element];
  const maximum = element === "timeLarge" ? 180 : element === "weatherLarge" ? 132 : 72;
  return Math.round(Math.min(maximum, Math.max(10, value)));
}

function screenQrSize(spec: ScreenSpec) {
  const value = displaySettings(spec).elementSizes.qr ?? DEFAULT_ELEMENT_SIZES.qr;
  return Math.round(Math.min(260, Math.max(108, value)));
}

function escapeWifiQrValue(value: string) {
  return value.replace(/([\\;,:"])/g, "\\$1");
}

function qrPayload(display: ScreenDisplay) {
  if (display.qrMode !== "wifi") return display.qrText.trim();
  const ssid = display.qrWifiSsid.trim();
  if (!ssid) return "";
  const security = display.qrWifiSecurity;
  const password = security === "nopass" ? "" : display.qrWifiPassword;
  return `WIFI:T:${security};S:${escapeWifiQrValue(ssid)};P:${escapeWifiQrValue(password)};${display.qrWifiHidden ? "H:true;" : ""};`;
}

function clockFontFamily(spec: ScreenSpec) {
  const requested = displaySettings(spec).elementFonts.timeLarge
    ?? spec.display?.font
    ?? spec.clock?.font
    ?? "sans";
  if (requested !== "random") return screenFonts[requested];
  const choices = Object.values(screenFonts);
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
  [250, 250, 248],
  [229, 201, 0],
  [220, 63, 47],
  [39, 86, 199],
  [8, 124, 78],
] as const;

const nativeEPaperPalette = [
  [0, 0, 0],
  [255, 255, 255],
  [255, 255, 0],
  [255, 0, 0],
  [0, 0, 255],
  [0, 255, 0],
] as const;

function artworkUrl(artwork: ArtworkSpec, orientation: ScreenOrientation = "portrait") {
  const params = new URLSearchParams({
    v: "8",
    query: artwork.query,
    style: artwork.style || "editorial high contrast composition",
    seed: String(artwork.seed),
    orientation,
  });
  return `/api/artwork?${params.toString()}`;
}

const artworkCreditCache = new Map<string, ArtworkCredit>();

function decodeArtwork(url: string) {
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

async function loadArtwork(url: string) {
  if (url.startsWith("data:") || url.startsWith("blob:")) return decodeArtwork(url);
  const response = await fetch(url, { cache: "force-cache", signal: AbortSignal.timeout(15_000) });
  if (!response.ok) throw new Error("图片素材加载失败");
  const provider = response.headers.get("X-Inkloop-Image-Source");
  const sourceUrl = response.headers.get("X-Inkloop-Image-Url");
  if (provider && sourceUrl) artworkCreditCache.set(url, { provider, url: sourceUrl });
  const objectUrl = URL.createObjectURL(await response.blob());
  try {
    return await decodeArtwork(objectUrl);
  } finally {
    URL.revokeObjectURL(objectUrl);
  }
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
  context.fillStyle = EPAPER_WHITE;
  context.fillRect(0, 0, canvas.width, canvas.height);
  const scale = Math.max(canvas.width / image.naturalWidth, canvas.height / image.naturalHeight);
  const width = image.naturalWidth * scale;
  const height = image.naturalHeight * scale;
  context.drawImage(image, (canvas.width - width) / 2, (canvas.height - height) / 2, width, height);
  return canvas.toDataURL("image/jpeg", 0.78);
}

function drawImageCover(
  ctx: CanvasRenderingContext2D,
  image: HTMLImageElement,
  x: number,
  y: number,
  width: number,
  height: number,
  renderMode: ScreenRenderMode,
) {
  const scale = Math.max(width / image.naturalWidth, height / image.naturalHeight);
  const drawWidth = image.naturalWidth * scale;
  const drawHeight = image.naturalHeight * scale;
  ctx.save();
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.filter = renderMode === "inkloop-text"
    ? "saturate(0.96) contrast(0.96) brightness(1.02)"
    : "none";
  ctx.drawImage(image, x + (width - drawWidth) / 2, y + (height - drawHeight) / 2, drawWidth, drawHeight);
  ctx.restore();
}

function quantizeNativeRegion(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  protectNeutral: boolean,
) {
  const image = ctx.getImageData(x, y, width, height);
  let currentRed = new Float32Array(width);
  let currentGreen = new Float32Array(width);
  let currentBlue = new Float32Array(width);
  let nextRed = new Float32Array(width);
  let nextGreen = new Float32Array(width);
  let nextBlue = new Float32Array(width);
  const clamp = (value: number, minimum: number, maximum: number) => Math.min(maximum, Math.max(minimum, value));
  const errorStrength = protectNeutral ? 0.46 : 1;

  for (let pixelY = 0; pixelY < height; pixelY += 1) {
    for (let pixelX = 0; pixelX < width; pixelX += 1) {
      const pixel = (pixelY * width + pixelX) * 4;
      const sourceRed = image.data[pixel];
      const sourceGreen = image.data[pixel + 1];
      const sourceBlue = image.data[pixel + 2];
      const sourceMaximum = Math.max(sourceRed, sourceGreen, sourceBlue);
      const sourceChroma = sourceMaximum - Math.min(sourceRed, sourceGreen, sourceBlue);
      const sourceSaturation = sourceChroma / Math.max(1, sourceMaximum);
      const isNeutral = protectNeutral && (sourceChroma < 30 || sourceSaturation < 0.22);
      const red = clamp(sourceRed + currentRed[pixelX], 0, 255);
      const green = clamp(sourceGreen + currentGreen[pixelX], 0, 255);
      const blue = clamp(sourceBlue + currentBlue[pixelX], 0, 255);
      let bestIndex = 0;
      let bestDistance = Number.POSITIVE_INFINITY;
      const availablePalette = isNeutral ? nativeEPaperPalette.slice(0, 2) : nativeEPaperPalette;
      availablePalette.forEach((color, index) => {
        const dr = red - color[0];
        const dg = green - color[1];
        const db = blue - color[2];
        const distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance) {
          bestDistance = distance;
          bestIndex = index;
        }
      });
      const selectedNative = nativeEPaperPalette[bestIndex];
      const selectedPreview = ePaperPalette[bestIndex];
      image.data[pixel] = selectedPreview[0];
      image.data[pixel + 1] = selectedPreview[1];
      image.data[pixel + 2] = selectedPreview[2];
      image.data[pixel + 3] = 255;

      const luminance = red * 0.2126 + green * 0.7152 + blue * 0.0722;
      const neutralError = (luminance - selectedNative[0]) * errorStrength;
      const errors = isNeutral
        ? [neutralError, neutralError, neutralError]
        : [
            (red - selectedNative[0]) * errorStrength,
            (green - selectedNative[1]) * errorStrength,
            (blue - selectedNative[2]) * errorStrength,
          ];
      if (pixelX + 1 < width) {
        currentRed[pixelX + 1] += errors[0] * (7 / 16);
        currentGreen[pixelX + 1] += errors[1] * (7 / 16);
        currentBlue[pixelX + 1] += errors[2] * (7 / 16);
      }
      if (pixelX > 0) {
        nextRed[pixelX - 1] += errors[0] * (3 / 16);
        nextGreen[pixelX - 1] += errors[1] * (3 / 16);
        nextBlue[pixelX - 1] += errors[2] * (3 / 16);
      }
      nextRed[pixelX] += errors[0] * (5 / 16);
      nextGreen[pixelX] += errors[1] * (5 / 16);
      nextBlue[pixelX] += errors[2] * (5 / 16);
      if (pixelX + 1 < width) {
        nextRed[pixelX + 1] += errors[0] * (1 / 16);
        nextGreen[pixelX + 1] += errors[1] * (1 / 16);
        nextBlue[pixelX + 1] += errors[2] * (1 / 16);
      }
    }
    [currentRed, nextRed] = [nextRed, currentRed];
    [currentGreen, nextGreen] = [nextGreen, currentGreen];
    [currentBlue, nextBlue] = [nextBlue, currentBlue];
    nextRed.fill(0);
    nextGreen.fill(0);
    nextBlue.fill(0);
  }
  ctx.putImageData(image, x, y);
}

function quantizeTextRegion(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  const image = ctx.getImageData(x, y, width, height);
  const bayer4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
  let currentRed = new Float32Array(width);
  let currentGreen = new Float32Array(width);
  let currentBlue = new Float32Array(width);
  let nextRed = new Float32Array(width);
  let nextGreen = new Float32Array(width);
  let nextBlue = new Float32Array(width);
  const clamp = (value: number, minimum: number, maximum: number) => Math.min(maximum, Math.max(minimum, value));

  for (let pixelY = 0; pixelY < height; pixelY += 1) {
    for (let pixelX = 0; pixelX < width; pixelX += 1) {
      const pixel = (pixelY * width + pixelX) * 4;
      const orderedBias = (bayer4[(pixelY % 4) * 4 + (pixelX % 4)] - 7.5) * 0.35;
      const sourceRed = image.data[pixel];
      const sourceGreen = image.data[pixel + 1];
      const sourceBlue = image.data[pixel + 2];
      const sourceMaximum = Math.max(sourceRed, sourceGreen, sourceBlue);
      const sourceChroma = sourceMaximum - Math.min(sourceRed, sourceGreen, sourceBlue);
      const sourceSaturation = sourceChroma / Math.max(1, sourceMaximum);
      const isNeutral = sourceChroma < 36 || sourceSaturation < 0.28;
      const neutralError = (currentRed[pixelX] + currentGreen[pixelX] + currentBlue[pixelX]) / 3;
      const red = clamp(sourceRed + (isNeutral ? neutralError : currentRed[pixelX]) + orderedBias, 0, 255);
      const green = clamp(sourceGreen + (isNeutral ? neutralError : currentGreen[pixelX]) + orderedBias, 0, 255);
      const blue = clamp(sourceBlue + (isNeutral ? neutralError : currentBlue[pixelX]) + orderedBias, 0, 255);
      let best: readonly [number, number, number] = ePaperPalette[0];
      let bestDistance = Number.POSITIVE_INFINITY;
      const availablePalette = isNeutral ? ePaperPalette.slice(0, 2) : ePaperPalette;
      for (const color of availablePalette) {
        const dr = red - color[0];
        const dg = green - color[1];
        const db = blue - color[2];
        const redMean = (red + color[0]) / 2;
        const distance = (2 + redMean / 256) * dr * dr
          + 4 * dg * dg
          + (2 + (255 - redMean) / 256) * db * db;
        if (distance < bestDistance) {
          bestDistance = distance;
          best = color;
        }
      }
      image.data[pixel] = best[0];
      image.data[pixel + 1] = best[1];
      image.data[pixel + 2] = best[2];
      image.data[pixel + 3] = 255;

      const errors = [
        clamp((red - best[0]) * 0.26, -18, 18),
        clamp((green - best[1]) * 0.26, -18, 18),
        clamp((blue - best[2]) * 0.26, -18, 18),
      ];
      if (pixelX + 1 < width) {
        currentRed[pixelX + 1] += errors[0] * 0.5;
        currentGreen[pixelX + 1] += errors[1] * 0.5;
        currentBlue[pixelX + 1] += errors[2] * 0.5;
      }
      if (pixelX > 0) {
        nextRed[pixelX - 1] += errors[0] * 0.2;
        nextGreen[pixelX - 1] += errors[1] * 0.2;
        nextBlue[pixelX - 1] += errors[2] * 0.2;
      }
      nextRed[pixelX] += errors[0] * 0.3;
      nextGreen[pixelX] += errors[1] * 0.3;
      nextBlue[pixelX] += errors[2] * 0.3;
    }
    [currentRed, nextRed] = [nextRed, currentRed];
    [currentGreen, nextGreen] = [nextGreen, currentGreen];
    [currentBlue, nextBlue] = [nextBlue, currentBlue];
    nextRed.fill(0);
    nextGreen.fill(0);
    nextBlue.fill(0);
  }
  ctx.putImageData(image, x, y);
}

function quantizeRegion(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  renderMode: ScreenRenderMode,
) {
  if (renderMode === "inkloop-text") {
    quantizeTextRegion(ctx, x, y, width, height);
    return;
  }
  quantizeNativeRegion(ctx, x, y, width, height, false);
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
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(x, y, width, height);

  if (artwork.motif === "rainbow") {
    ctx.fillStyle = "#e5c900";
    ctx.fillRect(x, y, width, height);
    ctx.fillStyle = EPAPER_WHITE;
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
    ctx.fillStyle = EPAPER_WHITE;
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
    ctx.fillStyle = EPAPER_WHITE;
    ctx.fillRect(x, y, width, height);
    const phase = random() * Math.PI * 2;
    const waveColors = ["#2756c7", "#dc3f2f", "#e5c900", "#087c4e"];
    for (let row = 0; row < 5; row += 1) {
      ctx.strokeStyle = waveColors[(row + Math.floor(random() * waveColors.length)) % waveColors.length];
      ctx.lineWidth = Math.max(10, height / 44);
      ctx.lineCap = "round";
      ctx.beginPath();
      for (let px = 0; px <= width; px += 8) {
        const py = y + height * 0.12 + row * (height / 5.2)
          + Math.sin(px / 52 + row * 0.8 + phase) * Math.min(22, height * 0.035);
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

function drawGlowFrame(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
) {
  ctx.save();
  ctx.strokeStyle = "#151816";
  ctx.lineWidth = 4;
  ctx.strokeRect(x, y, width, height);
  ctx.strokeStyle = EPAPER_WHITE;
  ctx.lineWidth = 2;
  ctx.shadowColor = EPAPER_WHITE;
  ctx.shadowBlur = 5;
  ctx.strokeRect(x, y, width, height);
  ctx.restore();
}

function drawGlowText(
  ctx: CanvasRenderingContext2D,
  text: string,
  x: number,
  y: number,
  maxWidth?: number,
) {
  ctx.save();
  ctx.lineJoin = "round";
  ctx.strokeStyle = "#151816";
  ctx.lineWidth = 4;
  ctx.shadowColor = EPAPER_WHITE;
  ctx.shadowBlur = 5;
  if (maxWidth) ctx.strokeText(text, x, y, maxWidth);
  else ctx.strokeText(text, x, y);
  ctx.fillStyle = EPAPER_WHITE;
  if (maxWidth) ctx.fillText(text, x, y, maxWidth);
  else ctx.fillText(text, x, y);
  ctx.restore();
}

function drawWeatherGroup(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  accent: string,
  transparentOverlay: boolean,
  large: boolean,
) {
  const element: ScreenElementKey = large ? "weatherLarge" : "weather";
  const position = screenElementPosition(spec, element);
  const family = screenElementFontFamily(spec, element);
  const fontSize = screenElementFontSize(spec, element);
  const ink = "#151816";
  const summary = spec.weatherText || `${spec.city || "天气"} · ${spec.weatherValue ?? spec.value}${spec.weatherUnit ?? spec.unit}`;
  const weatherDetail = spec.weatherDetail || spec.detail;
  const condition = weatherDetail.split("·")[0]?.trim() || "天气多变";
  const weatherAccent = spec.weatherAccent ? accentColors[spec.weatherAccent] : accent;

  ctx.save();
  ctx.textAlign = "center";
  ctx.fillStyle = weatherAccent;

  if (!large) {
    ctx.beginPath();
    ctx.arc(position.x - 142, position.y - fontSize * 0.3, Math.max(6, fontSize * 0.34), 0, Math.PI * 2);
    ctx.fill();
    ctx.font = `800 ${fontSize}px ${family}`;
    if (transparentOverlay) drawGlowText(ctx, summary.slice(0, 28), position.x + 8, position.y, 280);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(summary.slice(0, 28), position.x + 8, position.y, 280);
    }
    ctx.restore();
    return;
  }

  ctx.beginPath();
  ctx.arc(position.x - 154, position.y - 42, 34, 0, Math.PI * 2);
  ctx.fill();
  ctx.font = `800 ${Math.max(18, Math.round(fontSize * 0.3))}px ${family}`;
  const heading = `${spec.city || "当前城市"} · ${condition}`.slice(0, 24);
  if (transparentOverlay) drawGlowText(ctx, heading, position.x, position.y - 68, 390);
  else {
    ctx.fillStyle = ink;
    ctx.fillText(heading, position.x, position.y - 68, 390);
  }

  const temperature = `${spec.weatherValue ?? spec.value}${spec.weatherUnit ?? spec.unit}`;
  ctx.font = `900 ${fitClockText(ctx, temperature, 390, fontSize, family)}px ${family}`;
  if (transparentOverlay) drawGlowText(ctx, temperature, position.x, position.y + 38, 390);
  else {
    ctx.fillStyle = ink;
    ctx.fillText(temperature, position.x, position.y + 38, 390);
  }

  ctx.font = `700 ${Math.max(16, Math.round(fontSize * 0.24))}px ${family}`;
  if (transparentOverlay) drawGlowText(ctx, weatherDetail.slice(0, 34), position.x, position.y + 90, 400);
  else {
    ctx.fillStyle = ink;
    ctx.fillText(weatherDetail.slice(0, 34), position.x, position.y + 90, 400);
  }
  ctx.restore();
}

function drawEditorialPlate(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  accent: string,
  border: boolean,
) {
  ctx.save();
  if (border) {
    ctx.fillStyle = "#151816";
    ctx.fillRect(x + 7, y + 7, width, height);
  }
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(x, y, width, height);
  if (border) {
    ctx.strokeStyle = "#151816";
    ctx.lineWidth = 2;
    ctx.strokeRect(x, y, width, height);
  }
  ctx.fillStyle = accent;
  ctx.fillRect(x, y, 10, Math.min(height, 48));
  ctx.restore();
}

function drawDisplayMeta(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  accent: string,
  transparentOverlay: boolean,
) {
  const display = displaySettings(spec);
  const ink = "#151816";
  const leftLabel = display.date ? spec.dateText || spec.eyebrow : "";
  ctx.save();
  ctx.textAlign = "center";
  if (leftLabel) {
    const position = screenElementCanvasPosition(ctx, spec, "date");
    ctx.font = `700 ${screenElementFontSize(spec, "date")}px ${screenElementFontFamily(spec, "date")}`;
    if (transparentOverlay) drawGlowText(ctx, leftLabel.slice(0, 32), position.x, position.y, 300);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(leftLabel.slice(0, 32), position.x, position.y, 300);
    }
  }

  if (display.time && spec.timeText) {
    const position = screenElementCanvasPosition(ctx, spec, "time");
    ctx.font = `800 ${screenElementFontSize(spec, "time")}px ${screenElementFontFamily(spec, "time")}`;
    if (transparentOverlay) drawGlowText(ctx, spec.timeText, position.x, position.y, 140);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(spec.timeText, position.x, position.y, 140);
    }
  }
  if (display.weather && spec.weatherText) drawWeatherGroup(ctx, spec, accent, transparentOverlay, false);
  if (display.weatherLarge && spec.weatherText) drawWeatherGroup(ctx, spec, accent, transparentOverlay, true);

  if (display.logo && display.logoText) {
    const position = screenElementCanvasPosition(ctx, spec, "logo");
    ctx.font = `800 ${screenElementFontSize(spec, "logo")}px ${screenElementFontFamily(spec, "logo")}`;
    if (transparentOverlay) drawGlowText(ctx, display.logoText.slice(0, 20), position.x, position.y, 240);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(display.logoText.slice(0, 20), position.x, position.y, 240);
    }
  }

  if (display.quote && spec.footer) {
    const position = screenElementCanvasPosition(ctx, spec, "quote");
    ctx.font = `700 ${screenElementFontSize(spec, "quote")}px ${screenElementFontFamily(spec, "quote")}`;
    if (transparentOverlay) drawGlowText(ctx, spec.footer.slice(0, 30), position.x, position.y, 400);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(spec.footer.slice(0, 30), position.x, position.y, 400);
    }
  }
  ctx.restore();
}

function drawClockCopy(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  accent: string,
  transparentOverlay = false,
) {
  const ink = "#151816";
  const display = displaySettings(spec);
  const family = clockFontFamily(spec);
  const timeValue = spec.timeText || spec.value;
  const maxWidth = Math.min(ctx.canvas.width - 72, screenOrientation(spec) === "landscape" ? 620 : 420);
  const valueSize = fitClockText(
    ctx,
    timeValue,
    maxWidth,
    screenElementFontSize(spec, "timeLarge"),
    family,
  );
  const position = screenElementCanvasPosition(ctx, spec, "timeLarge");

  ctx.textAlign = "center";
  if (display.timeLarge) {
    if (transparentOverlay) {
      ctx.font = `900 ${valueSize}px ${family}`;
      drawGlowText(ctx, timeValue, position.x, position.y, maxWidth);
    } else {
      ctx.font = `900 ${valueSize}px ${family}`;
      ctx.fillStyle = ink;
      ctx.fillText(timeValue, position.x, position.y, maxWidth);
    }
  }
  ctx.textAlign = "left";
}

function drawArtworkCopy(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  accent: string,
  layout: ArtworkSpec["layout"],
  imageBackdrop: boolean,
) {
  const ink = "#151816";
  const paper = EPAPER_WHITE;
  const display = displaySettings(spec);
  const family = screenFontFamily(spec);
  const backgroundLayout = layout === "background";
  const transparentOverlay = backgroundLayout && imageBackdrop;
  drawDisplayMeta(ctx, spec, accent, transparentOverlay);

  if (screenOrientation(spec) === "landscape") {
    if (spec.clock?.enabled) {
      if (display.timeLarge) drawClockCopy(ctx, spec, accent, transparentOverlay);
      return;
    }
    if (spec.kind === "weather" && (display.weather || display.weatherLarge)) return;
    const valueWithUnit = `${spec.value}${spec.unit ? ` ${spec.unit}` : ""}`;
    const copyLeft = layout === "hero" ? 520 : 48;
    const copyWidth = layout === "hero" ? 232 : 690;
    const titleY = layout === "hero" ? 190 : 214;
    const valueY = layout === "hero" ? 280 : 320;
    if (!transparentOverlay) {
      ctx.fillStyle = paper;
      if (layout === "hero") ctx.fillRect(500, 118, 260, 306);
    }
    ctx.textAlign = "left";
    ctx.font = `800 ${fitText(ctx, spec.title, copyWidth, layout === "hero" ? 30 : 42, family)}px ${family}`;
    if (transparentOverlay) drawGlowText(ctx, spec.title, copyLeft, titleY, copyWidth);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(spec.title, copyLeft, titleY, copyWidth);
    }
    const valueSize = fitText(ctx, valueWithUnit, copyWidth, layout === "hero" ? 54 : 92, family);
    ctx.font = `900 ${valueSize}px ${family}`;
    if (transparentOverlay) drawGlowText(ctx, valueWithUnit, copyLeft, valueY, copyWidth);
    else {
      ctx.fillStyle = accent;
      ctx.fillText(valueWithUnit, copyLeft, valueY, copyWidth);
    }
    ctx.font = `700 19px ${family}`;
    if (transparentOverlay) drawGlowText(ctx, spec.detail.slice(0, 42), copyLeft, 456, copyWidth);
    else {
      ctx.fillStyle = ink;
      ctx.fillText(spec.detail.slice(0, 42), copyLeft, layout === "hero" ? 376 : 456, copyWidth);
    }
    if (display.timeLarge) drawClockCopy(ctx, spec, accent, transparentOverlay);
    return;
  }

  if (spec.clock?.enabled) {
    if (display.timeLarge) drawClockCopy(ctx, spec, accent, transparentOverlay);
    return;
  }

  if (spec.kind === "weather" && (display.weather || display.weatherLarge)) return;

  const valueWithUnit = `${spec.value}${spec.unit ? ` ${spec.unit}` : ""}`;
  if (transparentOverlay) {
    ctx.font = `800 ${fitText(ctx, spec.title, 420, 38, family)}px ${family}`;
    drawGlowText(ctx, spec.title, 48, 248, 420);
    const valueSize = fitText(ctx, valueWithUnit, 420, 78, family);
    ctx.font = `900 ${valueSize}px ${family}`;
    drawGlowText(ctx, valueWithUnit, 48, 336, 420);
  } else if (backgroundLayout) {
    drawEditorialPlate(ctx, 48, 216, 432, 226, accent, false);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 368, 34, family)}px ${family}`;
    ctx.fillText(spec.title, 82, 274, 368);
    const valueSize = fitText(ctx, valueWithUnit, 368, 82, family);
    ctx.font = `900 ${valueSize}px ${family}`;
    ctx.fillText(valueWithUnit, 82, 382, 368);
  } else {
    ctx.fillStyle = paper;
    ctx.fillRect(38, 468, 452, 184);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 402, 34, family)}px ${family}`;
    ctx.fillText(spec.title, 60, 516);
    const valueSize = fitText(ctx, spec.value, 402, 64, family);
    ctx.font = `900 ${valueSize}px ${family}`;
    ctx.fillText(spec.value, 60, 592);
    if (spec.unit) {
      ctx.font = `800 26px ${family}`;
      ctx.fillText(spec.unit, 60, 628);
    }
  }

  if (transparentOverlay) {
    ctx.font = `700 18px ${family}`;
    drawGlowText(ctx, spec.detail.slice(0, 30), 48, 672, 432);
  } else if (backgroundLayout) {
    drawEditorialPlate(ctx, 48, 614, 432, 64, accent, false);
    ctx.fillStyle = ink;
    ctx.font = `700 18px ${family}`;
    ctx.fillText(spec.detail.slice(0, 30), 82, 655, 366);
  } else {
    ctx.fillStyle = paper;
    ctx.fillRect(38, 666, 452, 46);
    ctx.fillStyle = ink;
    ctx.font = `700 19px ${family}`;
    ctx.fillText(spec.detail.slice(0, 30), 58, 696);
  }

  if (display.timeLarge) drawClockCopy(ctx, spec, accent, transparentOverlay);

}

function drawOuterScreenBorder(ctx: CanvasRenderingContext2D) {
  ctx.save();
  ctx.strokeStyle = "#151816";
  ctx.lineWidth = 3;
  ctx.strokeRect(22, 22, ctx.canvas.width - 44, ctx.canvas.height - 44);
  ctx.restore();
}

function drawQrElement(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  const display = displaySettings(spec);
  const content = qrPayload(display);
  if (!display.qr || !content) return;

  try {
    const qr = createQrCode(content, { errorCorrectionLevel: "M" });
    const quietModules = 4;
    const moduleCount = qr.modules.size;
    const requestedSize = screenQrSize(spec);
    const scale = Math.max(2, Math.floor(requestedSize / (moduleCount + quietModules * 2)));
    const actualSize = scale * (moduleCount + quietModules * 2);
    const position = screenElementCanvasPosition(ctx, spec, "qr");
    const left = Math.round(position.x - actualSize / 2);
    const top = Math.round(position.y - actualSize / 2);

    ctx.save();
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = EPAPER_WHITE;
    ctx.fillRect(left, top, actualSize, actualSize);
    ctx.fillStyle = "#151816";
    for (let row = 0; row < moduleCount; row += 1) {
      for (let column = 0; column < moduleCount; column += 1) {
        if (!qr.modules.get(row, column)) continue;
        ctx.fillRect(
          left + (column + quietModules) * scale,
          top + (row + quietModules) * scale,
          scale,
          scale,
        );
      }
    }
    ctx.restore();
  } catch {
    // Keep the rest of the poster usable if the entered content is too long.
  }
}

const lunarDayLabels = [
  "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
  "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
  "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
];

function lunarDateLabel(year: number, month: number, day: number) {
  try {
    const parts = new Intl.DateTimeFormat("zh-CN-u-ca-chinese", {
      month: "short",
      day: "numeric",
    }).formatToParts(new Date(year, month - 1, day, 12));
    const lunarMonth = parts.find((part) => part.type === "month")?.value || "";
    const lunarDay = Number(parts.find((part) => part.type === "day")?.value || 0);
    if (lunarDay === 1) return lunarMonth;
    return lunarDayLabels[lunarDay - 1] || "";
  } catch {
    return "";
  }
}

function drawCalendarTable(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  if (spec.table?.type !== "calendar") return;
  const { year, month, weekStartsOn, events, lunar } = spec.table;
  const family = screenFontFamily(spec);
  const ink = "#151816";
  const accent = accentColors[spec.accent];
  const weekdays = weekStartsOn === "sunday"
    ? ["日", "一", "二", "三", "四", "五", "六"]
    : ["一", "二", "三", "四", "五", "六", "日"];
  const firstWeekday = new Date(Date.UTC(year, month - 1, 1)).getUTCDay();
  const offset = weekStartsOn === "sunday" ? firstWeekday : (firstWeekday + 6) % 7;
  const daysInMonth = new Date(Date.UTC(year, month, 0)).getUTCDate();
  const today = new Date();
  const landscape = screenOrientation(spec) === "landscape";
  const eventMap = new Map<number, string[]>();
  events.forEach((event) => {
    const current = eventMap.get(event.day) ?? [];
    if (!current.includes(event.text)) eventMap.set(event.day, [...current, event.text]);
  });
  const left = 24;
  const top = landscape ? 84 : 118;
  const width = ctx.canvas.width - 48;
  const weekdayHeight = landscape ? 30 : 42;
  const rowHeight = Math.floor((ctx.canvas.height - top - weekdayHeight - 18) / 6);
  const columnWidth = width / 7;

  ctx.save();
  ctx.fillStyle = ink;
  ctx.textAlign = "left";
  ctx.font = `900 ${landscape ? 34 : 42}px ${family}`;
  ctx.fillText(`${year} 年 ${month} 月`, left, landscape ? 48 : 68, 360);
  ctx.fillStyle = accent;
  ctx.fillRect(left, landscape ? 62 : 88, 88, 8);
  ctx.textAlign = "center";
  weekdays.forEach((weekday, column) => {
    ctx.fillStyle = column >= 5 ? "#dc3f2f" : ink;
    ctx.font = `800 ${landscape ? 16 : 18}px ${family}`;
    ctx.fillText(weekday, left + columnWidth * (column + 0.5), top + (landscape ? 21 : 27));
  });

  for (let index = 0; index < 42; index += 1) {
    const day = index - offset + 1;
    const column = index % 7;
    const row = Math.floor(index / 7);
    const x = left + column * columnWidth;
    const y = top + weekdayHeight + row * rowHeight;
    const isToday = day >= 1
      && day <= daysInMonth
      && today.getFullYear() === year
      && today.getMonth() + 1 === month
      && today.getDate() === day;
    if (isToday) {
      ctx.fillStyle = "#e5c900";
      ctx.fillRect(x + 2, y + 2, columnWidth - 4, rowHeight - 4);
    }
    ctx.strokeStyle = ink;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + columnWidth, y);
    ctx.stroke();
    if (day < 1 || day > daysInMonth) continue;
    ctx.fillStyle = ink;
    ctx.font = `900 ${landscape ? 19 : 23}px ${family}`;
    ctx.fillText(String(day), x + columnWidth / 2, y + (landscape ? 24 : 30));
    const event = eventMap.get(day)?.join(" · ");
    const lunarLabel = lunar ? lunarDateLabel(year, month, day) : "";
    if (event) {
      const eventFontSize = landscape
        ? event.length <= 4 ? 12 : 11
        : lunarLabel
        ? event.length <= 4 ? 15 : 14
        : event.length <= 4 ? 18 : 16;
      ctx.fillStyle = accent;
      ctx.beginPath();
      ctx.arc(x + columnWidth / 2, y + (landscape ? 34 : 44), 3.5, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = ink;
      ctx.font = `800 ${eventFontSize}px ${family}`;
      ctx.fillText(event.slice(0, landscape ? 8 : 6), x + columnWidth / 2, y + (landscape ? 52 : lunarLabel ? 66 : 70), columnWidth - 7);
      if (!landscape && !lunarLabel && event.length > 6) {
        ctx.font = `800 ${Math.max(14, eventFontSize - 1)}px ${family}`;
        ctx.fillText(event.slice(6, 12), x + columnWidth / 2, y + 90, columnWidth - 7);
      }
    }
    if (lunarLabel) {
      ctx.fillStyle = event ? accent : "#087c4e";
      ctx.font = `800 ${landscape ? 11 : event ? 14 : 17}px ${family}`;
      ctx.fillText(lunarLabel, x + columnWidth / 2, y + (landscape ? rowHeight - 8 : event ? 87 : 70), columnWidth - 7);
    }
  }
  ctx.restore();
}

function tableAccent(text: string) {
  const choices = ["#2756c7", "#087c4e", "#e5c900", "#dc3f2f"];
  let hash = 0;
  for (const character of text) hash = (hash * 31 + character.charCodeAt(0)) >>> 0;
  return choices[hash % choices.length];
}

function drawTimetable(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  if (spec.table?.type !== "timetable") return;
  const { columns, rows } = spec.table;
  const family = screenFontFamily(spec);
  const ink = "#151816";
  const landscape = screenOrientation(spec) === "landscape";
  const left = 24;
  const top = landscape ? 84 : 122;
  const width = ctx.canvas.width - 48;
  const labelWidth = landscape ? 82 : 66;
  const headerHeight = landscape ? 38 : 48;
  const usableRows = rows.slice(0, 8);
  const rowHeight = Math.min(94, Math.floor((ctx.canvas.height - 24 - top - headerHeight) / Math.max(1, usableRows.length)));
  const columnWidth = (width - labelWidth) / Math.max(1, columns.length);

  ctx.save();
  ctx.fillStyle = ink;
  ctx.textAlign = "left";
  ctx.font = `900 ${landscape ? 34 : 42}px ${family}`;
  ctx.fillText(spec.title || "一周课程表", left, landscape ? 48 : 68, ctx.canvas.width - 120);
  ctx.fillStyle = accentColors[spec.accent];
  ctx.fillRect(left, landscape ? 62 : 88, 88, 8);

  ctx.textAlign = "center";
  columns.forEach((column, index) => {
    ctx.fillStyle = ink;
    ctx.font = `800 ${landscape ? 16 : 17}px ${family}`;
    ctx.fillText(column, left + labelWidth + columnWidth * (index + 0.5), top + (landscape ? 25 : 30), columnWidth - 6);
  });

  usableRows.forEach((row, rowIndex) => {
    const y = top + headerHeight + rowIndex * rowHeight;
    ctx.strokeStyle = ink;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + width, y);
    ctx.stroke();
    ctx.fillStyle = ink;
    ctx.font = `800 14px ${family}`;
    ctx.fillText(row.label, left + labelWidth / 2, y + rowHeight / 2 + 5, labelWidth - 8);
    columns.forEach((_, columnIndex) => {
      const text = row.cells[columnIndex] || "";
      const x = left + labelWidth + columnIndex * columnWidth;
      if (text) {
        ctx.fillStyle = tableAccent(text);
        ctx.fillRect(x + 6, y + 10, columnWidth - 12, 5);
      }
      ctx.fillStyle = ink;
      const fontSize = text.length > 5 ? 14 : text.length > 3 ? 16 : 18;
      ctx.font = `800 ${fontSize}px ${family}`;
      ctx.fillText(text, x + columnWidth / 2, y + rowHeight / 2 + 7, columnWidth - 9);
    });
  });
  ctx.restore();
}

function agendaDateKey(value: Date) {
  return `${value.getFullYear()}-${String(value.getMonth() + 1).padStart(2, "0")}-${String(value.getDate()).padStart(2, "0")}`;
}

function agendaTimeLabel(event: AgendaEvent) {
  if (event.allDay) return "全天";
  const start = new Date(event.start);
  const end = new Date(event.end);
  const time = (value: Date) => `${String(value.getHours()).padStart(2, "0")}:${String(value.getMinutes()).padStart(2, "0")}`;
  return `${time(start)}–${time(end)}`;
}

function agendaStartDate(spec: ScreenSpec, events: AgendaEvent[]) {
  if (spec.table?.type !== "agenda") return new Date();
  const custom = spec.table.rangeMode === "custom" && spec.table.customStart
    ? new Date(spec.table.customStart)
    : null;
  const firstEvent = events.length ? new Date(events[0].start) : null;
  const base = custom && Number.isFinite(custom.getTime()) ? custom : firstEvent && Number.isFinite(firstEvent.getTime()) ? firstEvent : new Date();
  const start = new Date(base);
  start.setHours(0, 0, 0, 0);
  if (spec.table.view === "workweek") {
    const weekday = start.getDay() || 7;
    start.setDate(start.getDate() - weekday + 1);
  }
  return start;
}

function drawAgendaTable(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  if (spec.table?.type !== "agenda") return;
  const { view } = spec.table;
  const events = spec.table.events
    .filter((event) => Number.isFinite(Date.parse(event.start)) && Number.isFinite(Date.parse(event.end)))
    .sort((left, right) => Date.parse(left.start) - Date.parse(right.start));
  const family = screenFontFamily(spec);
  const ink = "#151816";
  const paper = EPAPER_WHITE;
  const width = ctx.canvas.width;
  const height = ctx.canvas.height;
  const margin = 24;

  ctx.save();
  ctx.fillStyle = ink;
  ctx.textAlign = "left";
  ctx.font = `900 ${width > height ? 34 : 40}px ${family}`;
  ctx.fillText(spec.title || "智能日程", margin, width > height ? 46 : 62, width - margin * 2 - 150);
  ctx.fillStyle = accentColors[spec.accent];
  ctx.fillRect(margin, width > height ? 60 : 80, 88, 7);
  ctx.fillStyle = ink;
  ctx.textAlign = "right";
  ctx.font = `800 15px ${family}`;
  ctx.fillText(view === "agenda" ? "按时间排序" : view === "three-day" ? "未来三天" : "工作周", width - margin, width > height ? 47 : 62);

  if (view === "agenda") {
    const top = width > height ? 86 : 108;
    const usable = events.slice(0, width > height ? 7 : 9);
    if (!usable.length) {
      ctx.textAlign = "center";
      ctx.font = `800 28px ${family}`;
      ctx.fillText("这段时间没有日程", width / 2, height / 2);
      ctx.font = `700 17px ${family}`;
      ctx.fillText("可以调整时间范围，或读取 iCal", width / 2, height / 2 + 38);
      ctx.restore();
      return;
    }
    const rowHeight = Math.floor((height - top - 20) / usable.length);
    usable.forEach((event, index) => {
      const y = top + index * rowHeight;
      const date = new Date(event.start);
      ctx.fillStyle = index === 0 ? "#e5c900" : paper;
      ctx.fillRect(margin, y + 3, width - margin * 2, rowHeight - 7);
      ctx.fillStyle = tableAccent(event.calendar || event.title);
      ctx.fillRect(margin, y + 3, 8, rowHeight - 7);
      ctx.fillStyle = ink;
      ctx.textAlign = "left";
      ctx.font = `900 ${rowHeight < 54 ? 15 : 18}px ${family}`;
      ctx.fillText(`${date.getMonth() + 1}/${date.getDate()}`, margin + 22, y + rowHeight / 2 - 5, 54);
      ctx.font = `800 ${rowHeight < 54 ? 14 : 17}px ${family}`;
      ctx.fillText(agendaTimeLabel(event), margin + 78, y + rowHeight / 2 - 5, 118);
      ctx.font = `900 ${rowHeight < 54 ? 17 : 21}px ${family}`;
      ctx.fillText(event.title, margin + 210, y + rowHeight / 2 - 5, width - 260);
      if (event.location && rowHeight >= 54) {
        ctx.font = `700 13px ${family}`;
        ctx.fillText(event.location, margin + 210, y + rowHeight / 2 + 18, width - 260);
      }
    });
    if (events.length > usable.length) {
      ctx.textAlign = "right";
      ctx.font = `800 13px ${family}`;
      ctx.fillText(`还有 ${events.length - usable.length} 项`, width - margin, height - 6);
    }
    ctx.restore();
    return;
  }

  const columnCount = view === "workweek" ? 5 : 3;
  const start = agendaStartDate(spec, events);
  const top = width > height ? 90 : 112;
  const gap = 8;
  const columnWidth = (width - margin * 2 - gap * (columnCount - 1)) / columnCount;
  const cardsPerDay = width > height ? (view === "workweek" ? 3 : 4) : 3;
  for (let column = 0; column < columnCount; column += 1) {
    const day = new Date(start);
    day.setDate(start.getDate() + column);
    const key = agendaDateKey(day);
    const dayEvents = events.filter((event) => agendaDateKey(new Date(event.start)) === key);
    const x = margin + column * (columnWidth + gap);
    ctx.fillStyle = column === 0 ? "#e5c900" : "#f0eee4";
    ctx.fillRect(x, top, columnWidth, 48);
    ctx.fillStyle = ink;
    ctx.textAlign = "left";
    ctx.font = `900 ${view === "workweek" ? 16 : 19}px ${family}`;
    const weekday = new Intl.DateTimeFormat("zh-CN", { weekday: "short" }).format(day);
    ctx.fillText(`${day.getMonth() + 1}/${day.getDate()} ${weekday}`, x + 10, top + 31, columnWidth - 18);
    const cardTop = top + 56;
    const cardHeight = Math.floor((height - cardTop - 18 - gap * (cardsPerDay - 1)) / cardsPerDay);
    dayEvents.slice(0, cardsPerDay).forEach((event, index) => {
      const y = cardTop + index * (cardHeight + gap);
      ctx.fillStyle = paper;
      ctx.fillRect(x, y, columnWidth, cardHeight);
      ctx.fillStyle = tableAccent(event.calendar || event.title);
      ctx.fillRect(x, y, 7, cardHeight);
      ctx.fillStyle = ink;
      ctx.font = `800 ${view === "workweek" ? 13 : 15}px ${family}`;
      ctx.fillText(agendaTimeLabel(event), x + 14, y + 21, columnWidth - 22);
      ctx.font = `900 ${view === "workweek" ? 15 : 18}px ${family}`;
      ctx.fillText(event.title, x + 14, y + 45, columnWidth - 22);
      if (event.location && cardHeight >= 76) {
        ctx.font = `700 ${view === "workweek" ? 11 : 13}px ${family}`;
        ctx.fillText(event.location, x + 14, y + 66, columnWidth - 22);
      }
    });
    if (!dayEvents.length) {
      ctx.fillStyle = "#707871";
      ctx.font = `700 14px ${family}`;
      ctx.fillText("无安排", x + 10, cardTop + 28, columnWidth - 18);
    } else if (dayEvents.length > cardsPerDay) {
      ctx.fillStyle = ink;
      ctx.font = `900 13px ${family}`;
      ctx.fillText(`+${dayEvents.length - cardsPerDay} 项`, x + 10, height - 8, columnWidth - 18);
    }
  }
  ctx.restore();
}

function drawStructuredTable(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  if (spec.table?.type === "calendar") drawCalendarTable(ctx, spec);
  if (spec.table?.type === "timetable") drawTimetable(ctx, spec);
  if (spec.table?.type === "agenda") drawAgendaTable(ctx, spec);
}

async function drawScreen(canvas: HTMLCanvasElement, spec: ScreenSpec, localImage?: string) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return false;
  const { width, height } = screenDimensions(spec);
  const ink = "#151816";
  const paper = EPAPER_WHITE;
  const accent = accentColors[spec.accent];
  const display = displaySettings(spec, Boolean(localImage));
  const family = screenFontFamily(spec);

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = paper;
  ctx.fillRect(0, 0, width, height);
  if (spec.table) {
    drawStructuredTable(ctx, spec);
    drawDisplayMeta(ctx, spec, accent, false);
    if (display.border) drawOuterScreenBorder(ctx);
    quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
    drawQrElement(ctx, spec);
    return false;
  }
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
    const imageOnly = !display.quote
      && !display.logo
      && !display.date
      && !display.time
      && !display.timeLarge
      && !display.weather
      && !display.weatherLarge
      && !display.qr
      && !display.border;
    const area = imageOnly || artwork.layout === "fullscreen" || artwork.layout === "background"
      ? { x: 0, y: 0, width, height }
      : artwork.layout === "hero"
      ? screenOrientation(spec) === "landscape"
        ? { x: 34, y: 118, width: 440, height: 306 }
        : { x: 48, y: 132, width: 432, height: 314 }
      : { x: 0, y: 0, width, height };
    try {
      if (localImage || artwork.mode === "web") {
        const image = await loadArtwork(localImage || artworkUrl(artwork, screenOrientation(spec)));
        drawImageCover(ctx, image, area.x, area.y, area.width, area.height, display.renderMode);
        quantizeRegion(ctx, area.x, area.y, area.width, area.height, display.renderMode);
      } else {
        drawGeneratedArtwork(ctx, artwork, area.x, area.y, area.width, area.height);
      }
      if (imageOnly) return true;
      if (artwork.layout === "fullscreen") {
        if (display.border) drawOuterScreenBorder(ctx);
        drawQrElement(ctx, spec);
        return true;
      }
      drawArtworkCopy(ctx, spec, accent, artwork.layout, Boolean(localImage) || artwork.mode === "web");
      if (display.border) drawOuterScreenBorder(ctx);
      drawQrElement(ctx, spec);
      return true;
    } catch {
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = paper;
      ctx.fillRect(0, 0, width, height);
      if (imageOnly || artwork.layout === "fullscreen") return false;
    }
  }

  if (display.border) drawOuterScreenBorder(ctx);
  drawDisplayMeta(ctx, spec, accent, false);

  if (spec.clock?.enabled) {
    if (display.timeLarge) drawClockCopy(ctx, spec, accent, false);
    quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
    drawQrElement(ctx, spec);
    return false;
  }

  if (spec.kind === "weather" && (display.weather || display.weatherLarge)) {
    quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
    drawQrElement(ctx, spec);
    return false;
  }

  if (screenOrientation(spec) === "landscape") {
    const margin = 48;
    const valueWithUnit = `${spec.value}${spec.unit ? ` ${spec.unit}` : ""}`;
    ctx.textAlign = "left";
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, width - margin * 2, 42, family)}px ${family}`;
    ctx.fillText(spec.title, margin, 148, width - margin * 2);
    ctx.fillStyle = accent;
    ctx.fillRect(margin, 186, width - margin * 2, 180);
    ctx.fillStyle = "#ffffff";
    ctx.font = `900 ${fitText(ctx, valueWithUnit, width - margin * 2 - 60, 98, family)}px ${family}`;
    ctx.fillText(valueWithUnit, margin + 30, 304, width - margin * 2 - 60);
    ctx.fillStyle = ink;
    ctx.font = `700 22px ${family}`;
    ctx.fillText(spec.detail.slice(0, 54), margin, 438, width - margin * 2);
    if (display.timeLarge) drawClockCopy(ctx, spec, accent, false);
    quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
    drawQrElement(ctx, spec);
    return false;
  }

  if (spec.kind === "countdown") {
    ctx.fillStyle = ink;
    ctx.font = `700 34px ${family}`;
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.font = `900 238px ${family}`;
    ctx.fillText(spec.value, 34, 438);
    ctx.fillStyle = ink;
    ctx.font = `800 58px ${family}`;
    ctx.fillText(spec.unit, 380, 408);
  } else if (spec.kind === "meeting") {
    ctx.fillStyle = accent;
    ctx.fillRect(48, 140, 432, 112);
    ctx.fillStyle = "#ffffff";
    ctx.font = `800 54px ${family}`;
    ctx.fillText(spec.value, 74, 215);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 430, 70, family)}px ${family}`;
    ctx.fillText(spec.title, 48, 360);
  } else if (spec.kind === "metric") {
    ctx.fillStyle = ink;
    ctx.font = `700 34px ${family}`;
    ctx.fillText(spec.title, 48, 178);
    ctx.font = `900 176px ${family}`;
    ctx.fillText(spec.value, 40, 370);
    ctx.font = `800 62px ${family}`;
    ctx.fillText(spec.unit, 330, 350);
    ctx.fillStyle = "#dedbcf";
    ctx.fillRect(48, 432, 432, 18);
    ctx.fillStyle = accent;
    ctx.fillRect(48, 432, 316, 18);
  } else {
    ctx.fillStyle = ink;
    ctx.font = `700 34px ${family}`;
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.fillRect(48, 226, 432, 206);
    ctx.fillStyle = "#ffffff";
    const size = fitText(ctx, spec.value, 380, 74, family);
    ctx.font = `900 ${size}px ${family}`;
    ctx.fillText(spec.value, 72, 346);
  }

  ctx.fillStyle = ink;
  ctx.font = `700 24px ${family}`;
  ctx.fillText(spec.detail, 48, 552);
  if (display.timeLarge) drawClockCopy(ctx, spec, accent, false);
  quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
  drawQrElement(ctx, spec);

  return false;
}

async function renderScreenToCanvas(canvas: HTMLCanvasElement, spec: ScreenSpec, localImage?: string) {
  const { width, height } = screenDimensions(spec);
  const staging = document.createElement("canvas");
  staging.width = width;
  staging.height = height;
  const usedArtwork = await drawScreen(staging, spec, localImage);
  const context = canvas.getContext("2d");
  if (!context) return usedArtwork;
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  context.clearRect(0, 0, canvas.width, canvas.height);
  context.drawImage(staging, 0, 0);
  return usedArtwork;
}

function canvasForDevice(source: HTMLCanvasElement, orientation: ScreenOrientation) {
  if (orientation === "portrait" && source.width === 528 && source.height === 792) return source;
  const physical = document.createElement("canvas");
  physical.width = 528;
  physical.height = 792;
  const context = physical.getContext("2d");
  if (!context) return source;
  if (orientation === "landscape") {
    context.translate(528, 0);
    context.rotate(Math.PI / 2);
    context.drawImage(source, 0, 0, 792, 528);
  } else {
    context.drawImage(source, 0, 0, 528, 792);
  }
  return physical;
}

function MiniScreen({ app }: { app: InkApp }) {
  const thumbnailRef = useRef<HTMLCanvasElement>(null);
  const dimensions = screenDimensions(app.spec);
  const landscape = screenOrientation(app.spec) === "landscape";

  useEffect(() => {
    const canvas = thumbnailRef.current;
    if (!canvas) return;
    let active = true;
    const staging = document.createElement("canvas");
    staging.width = dimensions.width;
    staging.height = dimensions.height;

    resolveRuntimeScreen(app, GALLERY_PREVIEW_DATE)
      .then((spec) => renderScreenToCanvas(staging, spec, app.localImage))
      .then(() => {
        if (!active) return;
        const context = canvas.getContext("2d");
        if (!context) return;
        context.clearRect(0, 0, canvas.width, canvas.height);
        context.drawImage(staging, 0, 0);
      })
      .catch(() => undefined);

    return () => {
      active = false;
    };
  }, [app, dimensions.height, dimensions.width]);

  return (
    <div className={`mini-screen${landscape ? " landscape" : ""}`} aria-label={`${app.title} 保存时的画布预览`}>
      <canvas ref={thumbnailRef} width={dimensions.width} height={dimensions.height} />
    </div>
  );
}

function publicAppHref(appId: string) {
  return `/?view=explore&app=${encodeURIComponent(appId)}`;
}

function uniquePublicApps(apps: InkApp[]) {
  const seen = new Set<string>();
  return apps.filter((app) => {
    if (seen.has(app.id)) return false;
    seen.add(app.id);
    return true;
  });
}

function AppCard({
  app,
  onUse,
  local,
  onOpen,
}: {
  app: InkApp;
  onUse: () => void;
  local?: boolean;
  onOpen?: () => void;
}) {
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
        <div className="app-card-actions">
          {!local && onOpen && (
            <a
              className="app-card-permalink"
              href={publicAppHref(app.id)}
              onClick={(event) => {
                event.preventDefault();
                onOpen();
              }}
              aria-label={`打开${app.title}的独立链接`}
            >
              独立链接 <span aria-hidden="true">↗</span>
            </a>
          )}
          <button type="button" onClick={onUse} aria-label={`立即使用${app.title}`}>
            <span className="app-card-cta-label"><i>✦</i> 立即使用此应用</span>
            <span className="app-card-cta-arrow" aria-hidden="true">→</span>
          </button>
        </div>
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
  const [selectedPublicAppId, setSelectedPublicAppId] = useState<string | null>(null);
  const [publicLinkStatus, setPublicLinkStatus] = useState<"idle" | "loading" | "missing">("idle");
  const [generating, setGenerating] = useState(false);
  const [generatorStatus, setGeneratorStatus] = useState<GeneratorStatus>("checking");
  const [generatorModel, setGeneratorModel] = useState("auto");
  const [previewStatus, setPreviewStatus] = useState<PreviewStatus>("ready");
  const [previewScale, setPreviewScale] = useState<35 | 50 | 75 | 100>(50);
  const [artworkCredit, setArtworkCredit] = useState<ArtworkCredit | null>(null);
  const [preferredWeatherCity, setPreferredWeatherCity] = useState("上海");
  const [calendarPreferences, setCalendarPreferences] = useState<CalendarPreferences>(DEFAULT_CALENDAR_PREFERENCES);
  const [calendarUrlDraft, setCalendarUrlDraft] = useState("");
  const [calendarNotice, setCalendarNotice] = useState<string | null>(null);
  const [fontTick, setFontTick] = useState(0);
  const [clockTick, setClockTick] = useState(0);
  const [codeOpen, setCodeOpen] = useState(false);
  const [guideOpen, setGuideOpen] = useState(false);
  const [toast, setToast] = useState<Toast>(null);
  const [deviceName, setDeviceName] = useState<string | null>(null);
  const [devices, setDevices] = useState<DeviceProfile[]>([]);
  const [activeDeviceId, setActiveDeviceId] = useState<string | null>(null);
  const [taskPanelDeviceId, setTaskPanelDeviceId] = useState<string | null>(null);
  const [deviceStatus, setDeviceStatus] = useState<"idle" | "ready" | "writing" | "scheduled" | "error">("idle");
  const [progress, setProgress] = useState<TodooProgress | null>(null);
  const [deviceTasks, setDeviceTasks] = useState<DeviceTask[]>([]);
  const [taskPanelOpen, setTaskPanelOpen] = useState(false);
  const [secondTick, setSecondTick] = useState(() => Date.now());
  const [bluetoothSupported, setBluetoothSupported] = useState(false);
  const [dragPreview, setDragPreview] = useState<DragPreview | null>(null);
  const [elementSizeDrafts, setElementSizeDrafts] = useState<Partial<Record<ScreenElementKey, string>>>({});
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const previewVersionRef = useRef(0);
  const currentAppRef = useRef(app);
  const calendarPreferencesRef = useRef(calendarPreferences);
  const driverRef = useRef<TodooCard | null>(null);
  const deviceDriversRef = useRef(new Map<string, TodooCard>());
  const deviceTasksRef = useRef<DeviceTask[]>([]);
  const taskTimersRef = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  const transferLocksRef = useRef(new Set<string>());
  const elementDragRef = useRef<{
    element: ScreenElementKey;
    pointerId: number;
    bounds: DOMRect;
  } | null>(null);

  const showToast = useCallback((message: string, tone: ToastTone = "info") => {
    setToast({ message, tone });
    setTimeout(() => setToast(null), 3400);
  }, []);

  const navigateToTab = useCallback((nextTab: Tab) => {
    setTab(nextTab);
    setSelectedPublicAppId(null);
    setPublicLinkStatus("idle");
    const url = new URL(window.location.href);
    url.searchParams.delete("app");
    if (nextTab === "studio") url.searchParams.delete("view");
    else url.searchParams.set("view", nextTab);
    window.history.pushState(null, "", `${url.pathname}${url.search}${url.hash}`);
  }, []);

  const openPublicApp = useCallback((appId: string) => {
    setTab("explore");
    setSelectedPublicAppId(appId);
    setPublicLinkStatus("loading");
    const url = new URL(window.location.href);
    url.searchParams.set("view", "explore");
    url.searchParams.set("app", appId);
    window.history.pushState(null, "", `${url.pathname}${url.search}${url.hash}`);
    window.scrollTo({ top: 0, behavior: "smooth" });
  }, []);

  const copyPublicAppLink = useCallback(async (appId: string) => {
    const url = new URL(publicAppHref(appId), window.location.origin);
    try {
      await navigator.clipboard.writeText(url.toString());
      showToast("公开应用链接已复制", "success");
    } catch {
      showToast("复制失败，请从浏览器地址栏复制", "error");
    }
  }, [showToast]);

  const updateCalendarPreferences = useCallback((patch: Partial<CalendarPreferences>) => {
    setCalendarPreferences((current) => {
      const next = { ...current, ...patch };
      localStorage.setItem(CALENDAR_PREFERENCES_KEY, JSON.stringify(next));
      return next;
    });
  }, []);

  const commitDeviceTasks = useCallback((updater: (current: DeviceTask[]) => DeviceTask[]) => {
    const next = updater(deviceTasksRef.current);
    deviceTasksRef.current = next;
    setDeviceTasks(next);
  }, []);

  const rememberDevice = useCallback((driver: TodooCard, device: AuthorizedBluetoothDevice) => {
    const profile = { id: device.id, name: device.name ?? "TodooCard" };
    const previousDriver = deviceDriversRef.current.get(profile.id);
    if (previousDriver && previousDriver !== driver) previousDriver.disconnect();
    deviceDriversRef.current.set(profile.id, driver);
    driverRef.current = driver;
    setDevices((current) => [profile, ...current.filter((item) => item.id !== profile.id)]);
    setActiveDeviceId(profile.id);
    setDeviceName(profile.name);
    setDeviceStatus("ready");
    return profile;
  }, []);

  const activateDevice = useCallback((profile: DeviceProfile) => {
    setActiveDeviceId(profile.id);
    setDeviceName(profile.name);
    const driver = deviceDriversRef.current.get(profile.id);
    if (driver) driverRef.current = driver;
  }, []);

  useEffect(() => {
    currentAppRef.current = app;
  }, [app]);

  useEffect(() => {
    calendarPreferencesRef.current = calendarPreferences;
  }, [calendarPreferences]);

  useEffect(() => {
    const syncFromLocation = () => {
      const params = new URLSearchParams(window.location.search);
      const appId = params.get("app")?.trim() || null;
      const requestedView = params.get("view");
      const nextTab = appId
        ? "explore"
        : requestedView === "mine" || requestedView === "explore" || requestedView === "device"
          ? requestedView
          : "studio";
      setSelectedPublicAppId(appId);
      setPublicLinkStatus(appId ? "loading" : "idle");
      setTab(nextTab);
    };
    syncFromLocation();
    window.addEventListener("popstate", syncFromLocation);
    return () => window.removeEventListener("popstate", syncFromLocation);
  }, []);

  useEffect(() => {
    try {
      const stored = JSON.parse(localStorage.getItem(LOCAL_APPS_KEY) ?? "[]") as InkApp[];
      if (Array.isArray(stored)) setLocalApps(stored.map(upgradeLegacyApp));
      const storedCity = localStorage.getItem(WEATHER_CITY_KEY)?.trim();
      if (storedCity) setPreferredWeatherCity(storedCity);
      const storedCalendar = JSON.parse(localStorage.getItem(CALENDAR_PREFERENCES_KEY) ?? "null") as Partial<CalendarPreferences> | null;
      if (storedCalendar && typeof storedCalendar === "object") {
        const preferences: CalendarPreferences = {
          customUrl: typeof storedCalendar.customUrl === "string" ? storedCalendar.customUrl : "",
          chinaHolidays: storedCalendar.chinaHolidays === true,
          lunar: storedCalendar.lunar === true,
        };
        setCalendarPreferences(preferences);
        setCalendarUrlDraft(preferences.customUrl);
      }
    } catch {
      localStorage.removeItem(LOCAL_APPS_KEY);
    }
  }, []);

  useEffect(() => {
    const sample = "今日天气 12:34 专注当下 圆润手写";
    Promise.all(Object.values(screenFonts).map((family) => document.fonts.load(`700 32px ${family}`, sample)))
      .then(() => setFontTick((value) => value + 1))
      .catch(() => undefined);
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
    if (!taskPanelOpen) return;
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") setTaskPanelOpen(false);
    };
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [taskPanelOpen]);

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
        if (data.apps?.length) {
          setPublicApps((current) => uniquePublicApps([
            ...data.apps!.map(upgradeLegacyApp),
            ...current,
            ...featuredApps,
          ]));
        }
      })
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    if (!selectedPublicAppId || publicApps.some((item) => item.id === selectedPublicAppId)) return;
    const controller = new AbortController();
    fetch(`/api/apps?id=${encodeURIComponent(selectedPublicAppId)}`, { signal: controller.signal })
      .then(async (response) => {
        if (!response.ok) throw new Error("not found");
        return (await response.json()) as { app?: InkApp };
      })
      .then((data) => {
        if (!data.app) throw new Error("not found");
        setPublicApps((current) => uniquePublicApps([upgradeLegacyApp(data.app!), ...current]));
        setPublicLinkStatus("idle");
      })
      .catch((error) => {
        if (error instanceof DOMException && error.name === "AbortError") return;
        setPublicLinkStatus("missing");
      });
    return () => controller.abort();
  }, [publicApps, selectedPublicAppId]);

  useEffect(() => {
    setBluetoothSupported(
      Boolean((navigator as Navigator & { bluetooth?: unknown }).bluetooth && globalThis.isSecureContext),
    );
    const driver = new TodooCard(setProgress);
    driverRef.current = driver;
    driver
      .restoreAuthorizedDevice()
      .then((device) => {
        if (device) rememberDevice(driver, device);
      })
      .catch(() => undefined);
    return () => {
      const knownDrivers = new Set(deviceDriversRef.current.values());
      knownDrivers.add(driver);
      knownDrivers.forEach((knownDriver) => knownDriver.disconnect());
      deviceDriversRef.current.clear();
    };
  }, [rememberDevice]);

  useEffect(() => {
    const interval = window.setInterval(() => setSecondTick(Date.now()), 1000);
    return () => window.clearInterval(interval);
  }, []);

  useEffect(() => () => {
    taskTimersRef.current.forEach((timer) => clearTimeout(timer));
    taskTimersRef.current.clear();
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
    const dimensions = screenDimensions(app.spec);
    staging.width = dimensions.width;
    staging.height = dimensions.height;
    const hasArtwork = Boolean(app.spec.artwork || app.localImage);
    setPreviewStatus(hasArtwork ? "loading" : "ready");
    const creditKey = app.spec.artwork?.mode === "web" ? artworkUrl(app.spec.artwork, screenOrientation(app.spec)) : null;
    setArtworkCredit(null);
    resolveRuntimeScreen(app, new Date(), calendarPreferences, setCalendarNotice).then((runtimeSpec) => drawScreen(staging, runtimeSpec, app.localImage)).then((usedArtwork) => {
      if (version !== previewVersionRef.current) return;
      if (canvas.width !== dimensions.width || canvas.height !== dimensions.height) {
        canvas.width = dimensions.width;
        canvas.height = dimensions.height;
      }
      const context = canvas.getContext("2d");
      if (!context) return;
      context.clearRect(0, 0, canvas.width, canvas.height);
      context.drawImage(staging, 0, 0);
      setPreviewStatus(hasArtwork && !usedArtwork ? "fallback" : "ready");
      setArtworkCredit(usedArtwork && creditKey ? artworkCreditCache.get(creditKey) ?? null : null);
    });
  }, [app.spec, app.localImage, app.prompt, clockTick, fontTick, calendarPreferences]);

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
    navigateToTab("studio");
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
      setApp({ ...applyPreferredCityToGeneratedApp(result.app, prompt, preferredWeatherCity), localImage: app.localImage });
      if (result.mode === "llm") {
        setGeneratorStatus("online");
        setGeneratorModel(result.model || "auto");
        showToast(`已由 ${result.model || "在线模型"} 生成应用`, "success");
      } else {
        setGeneratorStatus("local");
        showToast(result.warning || "已使用本地模板生成", "info");
      }
    } catch (error) {
      setApp({ ...applyPreferredCityToGeneratedApp(generateInkApp(prompt), prompt, preferredWeatherCity), localImage: app.localImage });
      setGeneratorStatus("local");
      showToast(error instanceof Error ? `${error.message}，已使用本地模板` : "已使用本地模板", "info");
    } finally {
      setGenerating(false);
    }
  };

  const updateSchedule = (scheduleMode: ScheduleMode) => {
    setApp((current) => ({ ...current, scheduleMode }));
  };

  const applyCalendarUrl = () => {
    const customUrl = calendarUrlDraft.trim();
    if (customUrl && !/^(?:https|webcal):\/\//i.test(customUrl)) {
      showToast("请填写 HTTPS 或 webcal 开头的 iCal 地址", "error");
      return;
    }
    updateCalendarPreferences({ customUrl });
    setCalendarUrlDraft(customUrl);
    setCalendarNotice(null);
    showToast(customUrl ? "iCal 地址已保存，正在读取日程" : "个人 iCal 已移除", "success");
  };

  const updateDisplay = (patch: Partial<ScreenDisplay>) => {
    setApp((current) => {
      const currentDisplay = displaySettings(current.spec, Boolean(current.localImage));
      const nextDisplay = {
        ...currentDisplay,
        ...patch,
        positions: { ...currentDisplay.positions, ...patch.positions },
        elementFonts: { ...currentDisplay.elementFonts, ...patch.elementFonts },
        elementSizes: { ...currentDisplay.elementSizes, ...patch.elementSizes },
      };
      const enablesContentOverlay = screenElementOptions.some(({ key }) => patch[key] === true);
      const artwork = enablesContentOverlay && current.spec.artwork?.layout === "fullscreen"
        ? { ...current.spec.artwork, layout: "background" as const }
        : current.spec.artwork;
      const clock = current.spec.clock
        ? {
            ...current.spec.clock,
            font: patch.font ?? current.spec.clock.font,
          }
        : undefined;
      return {
        ...current,
        spec: {
          ...current.spec,
          artwork,
          clock,
          city: nextDisplay.weather || nextDisplay.weatherLarge
            ? current.spec.city || preferredWeatherCity || inferWeatherCity(current.prompt)
            : current.spec.city,
          footer: nextDisplay.quote && !current.spec.footer ? "今天也要保持好心情" : current.spec.footer,
          display: nextDisplay,
        },
      };
    });
  };

  const updateAgendaTable = (patch: Partial<Extract<NonNullable<ScreenSpec["table"]>, { type: "agenda" }>>) => {
    setApp((current) => {
      if (current.spec.table?.type !== "agenda") return current;
      return {
        ...current,
        spec: {
          ...current.spec,
          table: { ...current.spec.table, ...patch },
        },
      };
    });
  };

  const updateElementPosition = (element: ScreenElementKey, x: number, y: number) => {
    const margin = element === "qr" ? screenQrSize(currentAppRef.current.spec) / 2 : 24;
    updateDisplay({
      positions: {
        ...displaySettings(currentAppRef.current.spec).positions,
        [element]: {
          x: Math.round(Math.min(528 - margin, Math.max(margin, x))),
          y: Math.round(Math.min(792 - margin, Math.max(margin, y))),
        },
      },
    });
  };

  const resetElementPosition = (element: ScreenElementKey) => {
    const position = DEFAULT_ELEMENT_POSITIONS[element];
    updateElementPosition(element, position.x, position.y);
    showToast(`${screenElementOptions.find((item) => item.key === element)?.label ?? "元素"}已复位`, "info");
  };

  const updateElementSize = (element: ScreenElementKey, value: number) => {
    const currentDisplay = displaySettings(currentAppRef.current.spec);
    const minimum = element === "qr" ? 108 : 10;
    const maximum = element === "qr"
      ? 260
      : element === "timeLarge"
        ? 180
        : element === "weatherLarge"
          ? 132
          : 72;
    const nextSize = Math.min(maximum, Math.max(minimum, value));
    const positions = { ...currentDisplay.positions };
    if (element === "qr") {
      const current = positions.qr;
      const margin = nextSize / 2;
      positions.qr = {
        x: Math.round(Math.min(528 - margin, Math.max(margin, current.x))),
        y: Math.round(Math.min(792 - margin, Math.max(margin, current.y))),
      };
    }
    updateDisplay({
      positions,
      elementSizes: {
        ...currentDisplay.elementSizes,
        [element]: nextSize,
      },
    });
    setElementSizeDrafts((current) => {
      const next = { ...current };
      delete next[element];
      return next;
    });
  };

  const commitElementSizeDraft = (element: ScreenElementKey) => {
    const draft = elementSizeDrafts[element];
    if (draft === undefined) return;
    const parsed = Number.parseInt(draft, 10);
    updateElementSize(element, Number.isFinite(parsed) ? parsed : DEFAULT_ELEMENT_SIZES[element]);
  };

  const randomizeQuote = () => {
    setApp((current) => {
      const available = quoteOptions.filter((quote) => quote !== current.spec.footer);
      const footer = available[Math.floor(Math.random() * available.length)] ?? quoteOptions[0];
      return { ...current, spec: { ...current.spec, footer } };
    });
  };

  const handleElementPointerDown = (element: ScreenElementKey, event: ReactPointerEvent<HTMLButtonElement>) => {
    const bounds = event.currentTarget.parentElement?.getBoundingClientRect();
    if (!bounds) return;
    event.currentTarget.setPointerCapture(event.pointerId);
    elementDragRef.current = { element, pointerId: event.pointerId, bounds };
    const current = screenElementPosition(app.spec, element);
    setDragPreview({ element, ...current });
  };

  const handleElementPointerMove = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = elementDragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const x = ((event.clientX - drag.bounds.left) / drag.bounds.width) * 528;
    const y = ((event.clientY - drag.bounds.top) / drag.bounds.height) * 792;
    const margin = drag.element === "qr" ? screenQrSize(currentAppRef.current.spec) / 2 : 24;
    setDragPreview({
      element: drag.element,
      x: Math.min(528 - margin, Math.max(margin, x)),
      y: Math.min(792 - margin, Math.max(margin, y)),
    });
  };

  const finishElementDrag = (event: ReactPointerEvent<HTMLButtonElement>) => {
    const drag = elementDragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const x = ((event.clientX - drag.bounds.left) / drag.bounds.width) * 528;
    const y = ((event.clientY - drag.bounds.top) / drag.bounds.height) * 792;
    elementDragRef.current = null;
    setDragPreview(null);
    updateElementPosition(drag.element, x, y);
  };

  const handleElementKeyDown = (element: ScreenElementKey, event: ReactKeyboardEvent<HTMLButtonElement>) => {
    const offsets: Partial<Record<string, [number, number]>> = {
      ArrowLeft: [-4, 0],
      ArrowRight: [4, 0],
      ArrowUp: [0, -4],
      ArrowDown: [0, 4],
    };
    const offset = offsets[event.key];
    if (!offset) return;
    event.preventDefault();
    const position = screenElementPosition(app.spec, element);
    updateElementPosition(element, position.x + offset[0], position.y + offset[1]);
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
    const currentDisplay = displaySettings(app.spec, Boolean(app.localImage));
    const containsWifiAccess = currentDisplay.qr && (
      currentDisplay.qrMode === "wifi"
      || /^WIFI:/i.test(currentDisplay.qrText.trim())
    );
    const requestedPublic = app.isPublic;
    const saved = {
      ...app,
      id: app.id.startsWith("starter") ? `app-${Date.now()}` : app.id,
      isPublic: requestedPublic && !containsWifiAccess,
    };
    const next = [saved, ...localApps.filter((item) => item.id !== saved.id)].slice(0, 30);
    setApp(saved);
    setLocalApps(next);
    localStorage.setItem(LOCAL_APPS_KEY, JSON.stringify(next));

    if (requestedPublic && containsWifiAccess) {
      showToast("已保存到本机；Wi-Fi 二维码不会公开，避免泄露网络密码", "info");
    } else if (saved.isPublic) {
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

  const copyAppToStudio = (selected: InkApp) => {
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
    setElementSizeDrafts({});
    navigateToTab("studio");
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

  const selectNewDevice = useCallback(async () => {
    const driver = new TodooCard(setProgress);
    if (!driver.supported) {
      showToast("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站", "error");
      return null;
    }
    try {
      const device = await driver.requestDevice();
      const profile = rememberDevice(driver, device);
      showToast(`已添加设备 ${profile.name}，尚未执行写入`, "success");
      return profile;
    } catch (error) {
      driver.disconnect();
      showToast(error instanceof Error ? error.message : "没有选择设备", "error");
      return null;
    }
  }, [rememberDevice, showToast]);

  const stopDeviceTask = useCallback((taskId: string, notify = true) => {
    const timer = taskTimersRef.current.get(taskId);
    if (timer) clearTimeout(timer);
    taskTimersRef.current.delete(taskId);
    commitDeviceTasks((current) => current.filter((task) => task.id !== taskId));
    if (deviceTasksRef.current.length <= 1) setDeviceStatus(deviceName ? "ready" : "idle");
    if (notify) showToast("定时任务已停止", "info");
  }, [commitDeviceTasks, deviceName, showToast]);

  const runTransfer = useCallback(async (
    targetApp: InkApp,
    deviceId: string,
    taskId?: string,
    options: { reusePreview?: boolean; reconnect?: boolean } = {},
  ) => {
    const driver = deviceDriversRef.current.get(deviceId)
      ?? (driverRef.current?.selectedDevice?.id === deviceId ? driverRef.current : null);
    const canvas = canvasRef.current;
    const markTaskFailure = (reason: string, lastCanvas?: string) => {
      if (!taskId) return;
      commitDeviceTasks((current) => current.map((task) => task.id === taskId
        ? {
            ...task,
            status: "error",
            failureCount: task.failureCount + 1,
            consecutiveFailures: task.consecutiveFailures + 1,
            lastError: reason,
            lastCanvas: lastCanvas || task.lastCanvas,
          }
        : task));
    };
    if (!driver) {
      const reason = "找不到这台设备的授权，请重新选择设备";
      markTaskFailure(reason);
      return false;
    }
    const editingThisApp = targetApp.id === currentAppRef.current.id;
    const reuseCurrentPreview = Boolean(options.reusePreview && editingThisApp && canvas);
    const renderInPreview = editingThisApp && !taskId && Boolean(canvas) && !reuseCurrentPreview;
    if (reuseCurrentPreview && previewStatus === "loading") {
      const reason = "图片素材仍在加载，请稍候重试";
      markTaskFailure(reason);
      showToast(reason, "info");
      return false;
    }
    if (transferLocksRef.current.has(deviceId)) {
      const reason = "这台设备正在执行另一个写入任务，将在下一轮重试";
      markTaskFailure(reason);
      showToast(reason, "info");
      return false;
    }
    if (document.visibilityState !== "visible") {
      const reason = "页面在后台，等待重新打开后重试";
      markTaskFailure(reason);
      showToast(reason, "info");
      return false;
    }
    transferLocksRef.current.add(deviceId);
    setDeviceStatus("writing");
    if (taskId) {
      commitDeviceTasks((current) => current.map((task) => task.id === taskId
        ? { ...task, status: "writing", lastRunAt: Date.now(), lastError: null }
        : task));
    }
    let lastCanvas: string | undefined;
    try {
      const nextSeed = !reuseCurrentPreview && targetApp.spec.artwork ? randomArtworkSeed() : null;
      const transferApp = nextSeed === null
        ? targetApp
        : {
            ...targetApp,
            spec: {
              ...targetApp.spec,
              artwork: targetApp.spec.artwork
                ? { ...targetApp.spec.artwork, seed: nextSeed }
                : undefined,
            },
          };
      const outputCanvas = reuseCurrentPreview && canvas
        ? canvas
        : renderInPreview && canvas
          ? canvas
          : document.createElement("canvas");
      if (!reuseCurrentPreview) {
        const dimensions = screenDimensions(transferApp.spec);
        outputCanvas.width = dimensions.width;
        outputCanvas.height = dimensions.height;
        const runtimeSpec = await resolveRuntimeScreen(transferApp, new Date(), calendarPreferencesRef.current);
        const hasArtwork = Boolean(runtimeSpec.artwork || transferApp.localImage);
        if (renderInPreview) setPreviewStatus(hasArtwork ? "loading" : "ready");
        const usedArtwork = await renderScreenToCanvas(outputCanvas, runtimeSpec, transferApp.localImage);
        if (renderInPreview) setPreviewStatus(hasArtwork && !usedArtwork ? "fallback" : "ready");
      }
      lastCanvas = outputCanvas.toDataURL("image/jpeg", 0.76);
      if (options.reconnect) await driver.reconnect();
      await driver.writeCanvas(canvasForDevice(outputCanvas, screenOrientation(transferApp.spec)), true);
      if (nextSeed !== null && renderInPreview) {
        setApp((current) => current.spec.artwork
          ? { ...current, spec: { ...current.spec, artwork: { ...current.spec.artwork, seed: nextSeed } } }
          : current);
      }
      if (taskId) {
        commitDeviceTasks((current) => current.map((task) => task.id === taskId
          ? {
              ...task,
              app: transferApp,
              status: "scheduled",
              successCount: task.successCount + 1,
              consecutiveFailures: 0,
              lastError: null,
              lastCanvas,
            }
          : task));
      }
      setDeviceStatus(taskId ? "scheduled" : "ready");
      showToast("帧已发送，墨水屏可能还会显色几分钟", "success");
      return true;
    } catch (error) {
      const rawMessage = error instanceof Error ? error.message : "写入失败";
      const message = /no longer in range|GATT.*断开|设备.*范围|connection failed/i.test(rawMessage)
        ? "设备已离开蓝牙范围；旧连接已清理，靠近设备后会重新发现服务并重试"
        : rawMessage;
      driver.disconnect();
      markTaskFailure(message, lastCanvas);
      setDeviceStatus("error");
      showToast(message, "error");
      return false;
    } finally {
      transferLocksRef.current.delete(deviceId);
    }
  }, [commitDeviceTasks, previewStatus, showToast]);

  function scheduleDeviceTask(taskId: string, retryDelay?: number) {
    const existing = taskTimersRef.current.get(taskId);
    if (existing) clearTimeout(existing);
    const task = deviceTasksRef.current.find((item) => item.id === taskId);
    if (!task) return;
    const delay = retryDelay ?? calculateNextDelay(task.app);
    if (!delay) return;
    const nextRunAt = Date.now() + delay;
    commitDeviceTasks((current) => current.map((item) => item.id === taskId
      ? { ...item, nextRunAt }
      : item));
    setDeviceStatus(task.status === "error" ? "error" : "scheduled");
    const timer = setTimeout(async () => {
      taskTimersRef.current.delete(taskId);
      const latest = deviceTasksRef.current.find((item) => item.id === taskId);
      if (!latest) return;
      const wrote = await runTransfer(latest.app, latest.deviceId, taskId, {
        reconnect: latest.consecutiveFailures > 0,
      });
      if (!deviceTasksRef.current.some((item) => item.id === taskId)) return;
      const afterRun = deviceTasksRef.current.find((item) => item.id === taskId);
      const retryDelay = !wrote
        ? document.visibilityState !== "visible"
          ? 60_000
          : retryDelayForFailures(afterRun?.consecutiveFailures ?? 1)
        : undefined;
      scheduleDeviceTask(taskId, retryDelay);
    }, delay);
    taskTimersRef.current.set(taskId, timer);
  }

  async function retryDeviceTask(taskId: string) {
    const timer = taskTimersRef.current.get(taskId);
    if (timer) clearTimeout(timer);
    taskTimersRef.current.delete(taskId);
    const task = deviceTasksRef.current.find((item) => item.id === taskId);
    if (!task || task.status === "writing") return;
    commitDeviceTasks((current) => current.map((item) => item.id === taskId
      ? { ...item, status: "writing", nextRunAt: null, lastError: null }
      : item));
    showToast("正在重新连接设备并重试", "info");
    const wrote = await runTransfer(task.app, task.deviceId, taskId, { reconnect: true });
    if (!deviceTasksRef.current.some((item) => item.id === taskId)) return;
    const afterRun = deviceTasksRef.current.find((item) => item.id === taskId);
    scheduleDeviceTask(
      taskId,
      wrote ? undefined : retryDelayForFailures(afterRun?.consecutiveFailures ?? 1),
    );
  }

  const stopSchedule = useCallback(() => {
    const current = deviceTasksRef.current.find((task) => task.app.id === app.id
      && (!activeDeviceId || task.deviceId === activeDeviceId));
    if (current) stopDeviceTask(current.id);
  }, [activeDeviceId, app.id, stopDeviceTask]);

  const start = async () => {
    let driver = activeDeviceId ? deviceDriversRef.current.get(activeDeviceId) ?? null : driverRef.current;
    if (!driver) {
      driver = new TodooCard(setProgress);
      driverRef.current = driver;
    }
    if (!driver?.supported) {
      showToast("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站", "error");
      setDeviceStatus("error");
      return;
    }
    try {
      let device = driver.selectedDevice;
      if (!device) device = await driver.restoreAuthorizedDevice();
      if (!device) device = await driver.requestDevice();
      const profile = rememberDevice(driver, device);
      const resolvedDeviceName = profile.name;
      const resolvedDeviceId = profile.id;
      const existingForApp = deviceTasksRef.current.find((task) => task.app.id === app.id
        && task.deviceId === resolvedDeviceId);
      const highFrequency = app.scheduleMode === "custom" && app.customMinutes < 5;
      const conflicting = highFrequency
        ? deviceTasksRef.current.find((task) => task.id !== existingForApp?.id
          && task.deviceId === resolvedDeviceId
          && task.app.scheduleMode === "custom"
          && task.app.customMinutes < 5)
        : undefined;
      if (conflicting) {
        const replace = window.confirm(
          `${resolvedDeviceName} 已有一个 ${conflicting.app.customMinutes} 分钟高频任务「${conflicting.app.title}」。\n\n五分钟以下的任务同一设备只能运行一个，是否停止原任务并替换？`,
        );
        if (!replace) {
          showToast("已保留原来的高频任务", "info");
          return;
        }
        stopDeviceTask(conflicting.id, false);
      }
      if (existingForApp) {
        if (app.scheduleMode === "once") {
          stopDeviceTask(existingForApp.id, false);
          await runTransfer(app, resolvedDeviceId, undefined, { reusePreview: true });
          return;
        }
        commitDeviceTasks((current) => current.map((task) => task.id === existingForApp.id
          ? { ...task, app, deviceId: resolvedDeviceId, deviceName: resolvedDeviceName }
          : task));
        const wrote = await runTransfer(app, resolvedDeviceId, existingForApp.id, { reusePreview: true });
        const afterRun = deviceTasksRef.current.find((item) => item.id === existingForApp.id);
        scheduleDeviceTask(
          existingForApp.id,
          wrote ? undefined : retryDelayForFailures(afterRun?.consecutiveFailures ?? 1),
        );
        return;
      }
      if (app.scheduleMode === "once") {
        await runTransfer(app, resolvedDeviceId, undefined, { reusePreview: true });
        return;
      }
      const taskId = `task-${crypto.randomUUID()}`;
      const task: DeviceTask = {
        id: taskId,
        app,
        deviceId: resolvedDeviceId,
        deviceName: resolvedDeviceName,
        status: "writing",
        nextRunAt: null,
        lastRunAt: null,
        successCount: 0,
        failureCount: 0,
        consecutiveFailures: 0,
        lastError: null,
      };
      commitDeviceTasks((current) => [task, ...current]);
      const wrote = await runTransfer(app, resolvedDeviceId, taskId, { reusePreview: true });
      const afterRun = deviceTasksRef.current.find((item) => item.id === taskId);
      scheduleDeviceTask(taskId, wrote ? undefined : retryDelayForFailures(afterRun?.consecutiveFailures ?? 1));
      setTaskPanelDeviceId(resolvedDeviceId);
      setTaskPanelOpen(true);
    } catch (error) {
      const message = error instanceof Error ? error.message : "没有选择设备";
      showToast(message, "error");
      setDeviceStatus("error");
    }
  };

  const contentTitle = tab === "mine" ? "我的应用" : tab === "explore" ? "发现灵感" : tab === "device" ? "设备中心" : null;
  const selectedPublicApp = selectedPublicAppId
    ? publicApps.find((item) => item.id === selectedPublicAppId) ?? null
    : null;
  const screenDisplay = displaySettings(app.spec, Boolean(app.localImage));
  const previewDimensions = screenDimensions(app.spec);
  const previewLandscape = screenOrientation(app.spec) === "landscape";
  const previewFrameWidth = previewLandscape ? 486 : 288;
  const previewFrameHeight = previewLandscape ? 288 : 486;
  const activeDevice = devices.find((device) => device.id === activeDeviceId) ?? devices[0] ?? null;
  const deviceSummaries = devices.map((device) => {
    const tasks = deviceTasks.filter((task) => task.deviceId === device.id);
    const hasError = tasks.some((task) => task.status === "error");
    const isWriting = tasks.some((task) => task.status === "writing");
    return {
      ...device,
      tasks,
      hasError,
      status: hasError ? "error" : isWriting ? "writing" : tasks.length ? "scheduled" : "ready",
    };
  });
  const panelDevice = devices.find((device) => device.id === taskPanelDeviceId) ?? activeDevice;
  const panelTasks = panelDevice
    ? deviceTasks.filter((task) => task.deviceId === panelDevice.id)
    : [];
  const currentTask = deviceTasks.find((task) => task.app.id === app.id
    && (!activeDevice?.id || task.deviceId === activeDevice.id));
  const scheduleActive = Boolean(currentTask);
  const nextRun = currentTask?.nextRunAt ?? null;

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <button className="brand" type="button" onClick={() => navigateToTab("studio")} aria-label="返回创作台">
          <span className="brand-mark">I</span>
          <span>Inkloop</span>
        </button>
        <nav aria-label="主导航">
          {navItems.map((item) => (
            <button
              type="button"
              key={item.id}
              className={tab === item.id ? "active" : ""}
              onClick={() => navigateToTab(item.id)}
            >
              <span>{item.glyph}</span>
              {item.label}
            </button>
          ))}
        </nav>
        <div className="sidebar-devices" aria-label="已选择的设备">
          {deviceSummaries.length ? deviceSummaries.map((device) => (
            <button
              type="button"
              key={device.id}
              className={`sidebar-device${device.tasks.length ? " has-tasks" : ""}${device.hasError ? " has-error" : ""}${activeDevice?.id === device.id ? " active" : ""}`}
              onClick={() => {
                activateDevice(device);
                setTaskPanelDeviceId(device.id);
                setTaskPanelOpen((open) => taskPanelDeviceId === device.id ? !open : true);
              }}
              aria-expanded={taskPanelOpen && panelDevice?.id === device.id}
              aria-controls="device-task-panel"
            >
              <span className={`status-dot ${device.status}`} />
              <div className="sidebar-device-copy">
                <strong>{device.name}</strong>
                <small>{device.tasks.length ? `${device.tasks.length} 个定时任务` : "已记住，可自动重连"}</small>
              </div>
              {device.tasks.length > 0 && <span className="task-count" aria-label={`${device.tasks.length} 个任务`}>{device.tasks.length}</span>}
              {device.hasError && <span className="task-alert" aria-label="任务出现错误">!</span>}
              <span className="task-chevron" aria-hidden="true">{taskPanelOpen && panelDevice?.id === device.id ? "‹" : "›"}</span>
            </button>
          )) : (
            <div className="sidebar-device empty">
              <span className="status-dot idle" />
              <div className="sidebar-device-copy"><strong>未连接设备</strong><small>TodooCard · BLE</small></div>
            </div>
          )}
          <button type="button" className="add-device-button" onClick={() => void selectNewDevice()}>
            <span>＋</span> 添加设备
          </button>
        </div>
        <a className="product-link" href="https://p.todoo.tech/?lang=zh" target="_blank" rel="noreferrer">
          产品信息 <span>↗</span>
        </a>
      </aside>

      {taskPanelOpen && (
        <section className="device-task-panel" id="device-task-panel" aria-label="设备定时任务" aria-live="polite">
          <header>
            <div>
              <span>DEVICE SCHEDULES</span>
              <h2>{panelDevice?.name ?? "TodooCard"}</h2>
            </div>
            <button type="button" onClick={() => setTaskPanelOpen(false)} aria-label="关闭任务管理">×</button>
          </header>
          {panelTasks.length ? (
            <div className="device-task-list">
              {panelTasks.map((task) => (
                <article className={`device-task ${task.status}`} key={task.id}>
                  <div className="device-task-heading">
                    <div>
                      <span className="task-status-label">
                        {task.status === "writing" ? "正在写入" : task.status === "error" ? "等待重试" : "运行中"}
                      </span>
                      <h3>{task.app.title}</h3>
                      <p>{scheduleLabel(task.app)}</p>
                    </div>
                    <div className="device-task-heading-actions">
                      <button
                        type="button"
                        className="retry-task-button"
                        onClick={() => void retryDeviceTask(task.id)}
                        disabled={task.status === "writing"}
                      >
                        立即重试
                      </button>
                      <button type="button" onClick={() => stopDeviceTask(task.id)}>停止</button>
                    </div>
                  </div>
                  <div className="task-countdown">
                    <span>距离下次刷新</span>
                    <strong>{formatRemaining(task.nextRunAt, secondTick)}</strong>
                    <small>{task.nextRunAt ? formatExactTime(task.nextRunAt) : "首次写入完成后开始计时"}</small>
                  </div>
                  <dl className="task-stats">
                    <div><dt>成功</dt><dd>{task.successCount}</dd></div>
                    <div><dt>失败</dt><dd>{task.failureCount}</dd></div>
                    <div><dt>最近刷新</dt><dd>{task.lastRunAt ? formatExactTime(task.lastRunAt) : "尚未执行"}</dd></div>
                  </dl>
                  {task.lastError && (
                    <p className="task-error" role="alert"><b>!</b><span>{task.lastError}</span></p>
                  )}
                  {task.lastCanvas ? (
                    <figure className="task-canvas">
                      <img src={task.lastCanvas} alt={`${task.app.title} 最近一次刷新的画面`} />
                      <figcaption>最近一次刷新的 Canvas</figcaption>
                    </figure>
                  ) : (
                    <div className="task-canvas-empty">写入完成后，这里会保留最近画面</div>
                  )}
                </article>
              ))}
            </div>
          ) : (
            <div className="device-task-empty">
              <span>⌁</span>
              <h3>还没有定时任务</h3>
              <p>选择非“单次写入”的刷新计划，再点击开始写入。</p>
            </div>
          )}
          <footer>断联后会清理旧连接，并按 15 秒、30 秒、60 秒逐步重试；设备回到范围内后也可点“立即重试”。</footer>
        </section>
      )}

      <section className="workspace">
        <header className="topbar">
          <div>
            <span className="eyebrow">INKLOOP · TODOO STUDIO</span>
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
                        ? `在线编码已就绪 · ${generatorModel === "auto" ? "自动选择模型" : generatorModel}`
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
                        {previewDimensions.width} × {previewDimensions.height} · {previewStatus === "loading"
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
                    <select
                      className="scale-chip"
                      value={previewScale}
                      onChange={(event) => setPreviewScale(Number(event.target.value) as 35 | 50 | 75 | 100)}
                      aria-label="调整屏幕预览缩放"
                      title={`只调整网页预览大小，不影响 ${previewDimensions.width} × ${previewDimensions.height} 写入画质`}
                    >
                      {[35, 50, 75, 100].map((scale) => (
                        <option value={scale} key={scale}>{scale}%</option>
                      ))}
                    </select>
                  </div>
                </div>
                <div
                  className={`canvas-stage${previewLandscape ? " landscape" : ""}`}
                  style={{ minHeight: `${Math.max(360, previewFrameHeight * (previewScale / 50) + 24)}px` }}
                >
                  <div
                    className="device-preview-scale"
                    style={{
                      width: `${previewFrameWidth * (previewScale / 50)}px`,
                      height: `${previewFrameHeight * (previewScale / 50)}px`,
                    }}
                  >
                    <div
                      className={`device-preview-inner${previewLandscape ? " landscape" : ""}`}
                      style={{ transform: `scale(${previewScale / 50})` }}
                    >
                      <div className="device-shadow" />
                      <div className="device-frame">
                        <div className="device-label">TODOO</div>
                        <div className="screen-canvas-wrap">
                          <canvas ref={canvasRef} width={previewDimensions.width} height={previewDimensions.height} aria-label="电子墨水屏预览" />
                          <div className="screen-drag-layer" aria-label="拖拽画面元素调整位置">
                            {screenElementOptions.filter(({ key }) => screenDisplay[key]).map((element) => {
                              const savedPosition = screenDisplay.positions[element.key];
                              const position = dragPreview?.element === element.key ? dragPreview : savedPosition;
                              const elementWidth = element.key === "qr" ? screenQrSize(app.spec) : element.width;
                              const elementHeight = element.key === "qr" ? screenQrSize(app.spec) : element.height;
                              return (
                                <button
                                  type="button"
                                  className={`canvas-drag-handle canvas-drag-${element.key}${dragPreview?.element === element.key ? " dragging" : ""}`}
                                  key={element.key}
                                  style={{
                                    left: `${(position.x / 528) * 100}%`,
                                    top: `${(position.y / 792) * 100}%`,
                                    width: `${(elementWidth / previewDimensions.width) * 100}%`,
                                    height: `${(elementHeight / previewDimensions.height) * 100}%`,
                                  }}
                                  aria-label={`拖拽调整${element.label}位置，方向键可微调`}
                                  onPointerDown={(event) => handleElementPointerDown(element.key, event)}
                                  onPointerMove={handleElementPointerMove}
                                  onPointerUp={finishElementDrag}
                                  onPointerCancel={finishElementDrag}
                                  onKeyDown={(event) => handleElementKeyDown(element.key, event)}
                                >
                                  <span>{element.label}</span>
                                </button>
                              );
                            })}
                          </div>
                        </div>
                        <div className="device-port" />
                      </div>
                    </div>
                  </div>
                </div>
                <div className="palette-strip" aria-label="屏幕支持六种颜色">
                  {[
                    ["黑", "#111"],
                    ["白", EPAPER_WHITE],
                    ["黄", "#e5c900"],
                    ["红", "#dc3f2f"],
                    ["蓝", "#2756c7"],
                    ["绿", "#087c4e"],
                  ].map(([label, color]) => (
                    <span key={label}><i style={{ background: color }} />{label}</span>
                  ))}
                </div>
                {(app.spec.artwork || app.localImage) && (
                  <div className="preview-source-note">
                    {app.spec.artwork?.mode === "web" && (
                      <p>如果图片主题与要求不符，请点击“重新生成”。</p>
                    )}
                    {app.localImage ? (
                      <p><strong>图片来源</strong> 本机上传（无外部地址）</p>
                    ) : artworkCredit ? (
                      <p>
                        <strong>图片来源</strong> {artworkCredit.provider === "loremflickr"
                          ? "LoremFlickr"
                          : artworkCredit.provider === "wikimedia-commons"
                            ? "Wikimedia Commons"
                            : artworkCredit.provider === "picsum"
                              ? "Picsum"
                              : artworkCredit.provider}
                        <a href={artworkCredit.url} target="_blank" rel="noreferrer">查看真实图片地址 ↗</a>
                      </p>
                    ) : app.spec.artwork?.mode === "generated" ? (
                      <p><strong>图片来源</strong> Inkloop 生成图形</p>
                    ) : null}
                  </div>
                )}
              </section>

              <section className="settings-panel panel">
                <div className="panel-heading compact">
                  <span className="step-number">03</span>
                  <div>
                    <h2>画面、保存与刷新</h2>
                    <p>生成后可继续手动调整画面元素。</p>
                  </div>
                </div>
                <div className="display-editor">
                  <div className="settings-subhead">
                    <strong>画面元素</strong>
                    <small>勾选后可在预览中拖拽</small>
                  </div>
                  <div className="orientation-field">
                    <div>
                      <strong>屏幕方向</strong>
                      <small>LLM 会先建议，你可以随时手动切换</small>
                    </div>
                    <div className="orientation-options" role="group" aria-label="屏幕方向">
                      {([
                        ["portrait", "竖版 528×792"],
                        ["landscape", "横版 792×528"],
                      ] as Array<[ScreenOrientation, string]>).map(([value, label]) => (
                        <button
                          type="button"
                          key={value}
                          className={screenOrientation(app.spec) === value ? "selected" : ""}
                          onClick={() => setApp((current) => ({
                            ...current,
                            spec: { ...current.spec, orientation: value },
                          }))}
                          aria-pressed={screenOrientation(app.spec) === value}
                        >
                          {label}
                        </button>
                      ))}
                    </div>
                  </div>
                  <div className="component-list">
                    {screenElementOptions.map(({ key, label }) => (
                      <div className={`component-row${screenDisplay[key] ? " enabled" : ""}`} key={key}>
                        <div className="component-row-head">
                          <label className="component-toggle">
                            <input
                              type="checkbox"
                              checked={screenDisplay[key]}
                              onChange={(event) => {
                                const checked = event.target.checked;
                                if (key === "weather" && checked) {
                                  updateDisplay({ weather: true, weatherLarge: false });
                                } else if (key === "weatherLarge" && checked) {
                                  updateDisplay({ weather: false, weatherLarge: true });
                                } else {
                                  updateDisplay({ [key]: checked });
                                }
                              }}
                            />
                            <span>{label}</span>
                          </label>
                          <button
                            type="button"
                            onClick={() => resetElementPosition(key)}
                            disabled={!screenDisplay[key]}
                            aria-label={`复位${label}位置`}
                          >
                            位置复位
                          </button>
                        </div>
                        <div className={`component-type-controls${key === "qr" ? " qr-component-controls" : ""}`}>
                          {key !== "qr" && (
                            <label>
                              <span>字体</span>
                              <select
                                value={screenDisplay.elementFonts[key] ?? ""}
                                onChange={(event) => updateDisplay({
                                  elementFonts: {
                                    ...screenDisplay.elementFonts,
                                    [key]: event.target.value || undefined,
                                  } as ScreenDisplay["elementFonts"],
                                })}
                                aria-label={`${label}字体`}
                                disabled={!screenDisplay[key]}
                              >
                                <option value="">跟随默认字体</option>
                                {screenFontOptions.map((font) => (
                                  <option value={font.value} key={font.value}>{font.label}</option>
                                ))}
                              </select>
                            </label>
                          )}
                          <label className={`component-size-control${key === "qr" ? " qr-size-control" : ""}`}>
                            <span>{key === "qr" ? "尺寸" : "字号"}</span>
                            <input
                              type="number"
                              min={key === "qr" ? 108 : 10}
                              max={key === "qr" ? 260 : key === "timeLarge" ? 180 : key === "weatherLarge" ? 132 : 72}
                              step={1}
                              inputMode="numeric"
                              value={elementSizeDrafts[key] ?? String(screenDisplay.elementSizes[key] ?? DEFAULT_ELEMENT_SIZES[key])}
                              onChange={(event) => setElementSizeDrafts((current) => ({
                                ...current,
                                [key]: event.target.value,
                              }))}
                              onBlur={() => commitElementSizeDraft(key)}
                              onKeyDown={(event) => {
                                if (event.key === "Enter") event.currentTarget.blur();
                              }}
                              aria-label={`${label}${key === "qr" ? "尺寸" : "字号"}`}
                              disabled={!screenDisplay[key]}
                            />
                          </label>
                        </div>
                      </div>
                    ))}
                    <div className={`component-row border-control${screenDisplay.border ? " enabled" : ""}`}>
                      <label className="component-toggle">
                        <input
                          type="checkbox"
                          checked={screenDisplay.border}
                          onChange={(event) => updateDisplay({ border: event.target.checked })}
                        />
                        <span>屏幕外框</span>
                      </label>
                      <small>只绘制最外侧细框</small>
                    </div>
                  </div>
                  <div className="display-fields">
                    <div className="render-mode-field">
                      <div className="render-mode-heading">
                        <span>画面渲染</span>
                        <small>{app.spec.artwork || app.localImage
                          ? "图片默认 Official Skill"
                          : "纯文字默认 Inkloop text"}</small>
                      </div>
                      <div className="render-mode-options" role="group" aria-label="画面渲染方式">
                        {renderModeOptions.map((mode) => (
                          <button
                            type="button"
                            key={mode.value}
                            className={screenDisplay.renderMode === mode.value ? "selected" : ""}
                            onClick={() => updateDisplay({ renderMode: mode.value, renderModeExplicit: true })}
                            aria-pressed={screenDisplay.renderMode === mode.value}
                          >
                            {mode.label}
                          </button>
                        ))}
                      </div>
                      <p>{renderModeOptions.find((mode) => mode.value === screenDisplay.renderMode)?.description}</p>
                    </div>
                    <label className="display-field">
                      <span>画面默认字体</span>
                      <select
                        value={screenDisplay.font}
                        onChange={(event) => updateDisplay({ font: event.target.value as ScreenFont })}
                      >
                        {screenFontOptions.map((font) => (
                          <option value={font.value} key={font.value}>{font.label}</option>
                        ))}
                      </select>
                      <span
                        className="font-preview"
                        style={{ fontFamily: screenFonts[screenDisplay.font] }}
                        aria-hidden="true"
                      >
                        今日天气 · 12:34 · 专注当下
                      </span>
                    </label>
                    {screenDisplay.quote && (
                      <label className="display-field">
                        <span>今日名言</span>
                        <div className="quote-input-row">
                          <input
                            value={app.spec.footer}
                            maxLength={40}
                            onChange={(event) => setApp((current) => ({
                              ...current,
                              spec: { ...current.spec, footer: event.target.value },
                            }))}
                            placeholder="输入一句话"
                          />
                          <button type="button" onClick={randomizeQuote}>随机</button>
                        </div>
                      </label>
                    )}
                    {screenDisplay.logo && (
                      <label className="display-field">
                        <span>LOGO 文字</span>
                        <input
                          value={screenDisplay.logoText}
                          maxLength={20}
                          onChange={(event) => updateDisplay({ logoText: event.target.value })}
                          placeholder="例如 INKLOOP"
                        />
                      </label>
                    )}
                    {screenDisplay.qr && (
                      <div className="display-field qr-content-field">
                        <span>二维码内容</span>
                        <div className="qr-mode-options" role="group" aria-label="二维码内容类型">
                          <button
                            type="button"
                            className={screenDisplay.qrMode === "text" ? "selected" : ""}
                            onClick={() => updateDisplay({ qrMode: "text" })}
                          >
                            文字 / 网址
                          </button>
                          <button
                            type="button"
                            className={screenDisplay.qrMode === "wifi" ? "selected" : ""}
                            onClick={() => updateDisplay({ qrMode: "wifi" })}
                          >
                            Wi-Fi / WPA
                          </button>
                        </div>
                        {screenDisplay.qrMode === "wifi" ? (
                          <div className="qr-wifi-fields">
                            <label>
                              <span>Wi-Fi 名称（SSID）</span>
                              <input
                                value={screenDisplay.qrWifiSsid}
                                maxLength={64}
                                onChange={(event) => updateDisplay({ qrWifiSsid: event.target.value })}
                                placeholder="例如 Home WiFi"
                              />
                            </label>
                            <label>
                              <span>安全类型</span>
                              <select
                                value={screenDisplay.qrWifiSecurity}
                                onChange={(event) => updateDisplay({
                                  qrWifiSecurity: event.target.value as ScreenDisplay["qrWifiSecurity"],
                                })}
                              >
                                <option value="WPA">WPA / WPA2 / WPA3</option>
                                <option value="WEP">WEP</option>
                                <option value="nopass">无密码</option>
                              </select>
                            </label>
                            {screenDisplay.qrWifiSecurity !== "nopass" && (
                              <label className="qr-password-field">
                                <span>Wi-Fi 密码</span>
                                <input
                                  type="password"
                                  value={screenDisplay.qrWifiPassword}
                                  maxLength={128}
                                  autoComplete="off"
                                  onChange={(event) => updateDisplay({ qrWifiPassword: event.target.value })}
                                  placeholder="只保存在当前浏览器"
                                />
                              </label>
                            )}
                            <label className="qr-hidden-network">
                              <input
                                type="checkbox"
                                checked={screenDisplay.qrWifiHidden}
                                onChange={(event) => updateDisplay({ qrWifiHidden: event.target.checked })}
                              />
                              <span>这是隐藏网络</span>
                            </label>
                          </div>
                        ) : (
                          <textarea
                            aria-label="二维码文字或网址"
                            value={screenDisplay.qrText}
                            maxLength={512}
                            rows={3}
                            onChange={(event) => updateDisplay({ qrText: event.target.value })}
                            placeholder="输入网址或任意文字"
                          />
                        )}
                        <small>{screenDisplay.qrMode === "wifi"
                          ? "密码仅保存在当前浏览器；包含 Wi-Fi 凭据的应用不能公开到发现页。"
                          : "内容只在本机生成二维码，不会发送到二维码服务；白色留边是扫码所必需。"}</small>
                      </div>
                    )}
                    {(screenDisplay.weather || screenDisplay.weatherLarge) && (
                      <label className="display-field">
                        <span>天气城市</span>
                        <input
                          value={app.spec.city || ""}
                          maxLength={20}
                          onChange={(event) => setApp((current) => ({
                            ...current,
                            spec: { ...current.spec, city: event.target.value },
                          }))}
                          onBlur={(event) => {
                            const city = event.target.value.trim();
                            if (!city) return;
                            setPreferredWeatherCity(city);
                            localStorage.setItem(WEATHER_CITY_KEY, city);
                          }}
                          placeholder="例如 上海"
                        />
                      </label>
                    )}
                  </div>
                </div>
                {app.spec.table?.type === "agenda" && (
                  <section className="agenda-editor" aria-label="智能日程布局与范围">
                    <div className="calendar-source-heading">
                      <strong>智能日程布局</strong>
                      <small>空闲时间会自动压缩</small>
                    </div>
                    <div className="agenda-view-options" role="group" aria-label="日程布局">
                      {([
                        ["agenda", "智能议程"],
                        ["three-day", "三日时间轴"],
                        ["workweek", "工作周"],
                      ] as Array<[AgendaView, string]>).map(([value, label]) => (
                        <button
                          type="button"
                          key={value}
                          className={app.spec.table?.type === "agenda" && app.spec.table.view === value ? "selected" : ""}
                          onClick={() => updateAgendaTable({ view: value })}
                        >
                          {label}
                        </button>
                      ))}
                    </div>
                    <div className="agenda-range-grid">
                      <label>
                        <span>时间范围</span>
                        <select
                          value={app.spec.table.rangeMode === "rolling" ? String(app.spec.table.rangeHours) : app.spec.table.rangeMode}
                          onChange={(event) => {
                            const value = event.target.value;
                            if (value === "today" || value === "custom") updateAgendaTable({ rangeMode: value as AgendaRangeMode });
                            else updateAgendaTable({ rangeMode: "rolling", rangeHours: Number(value) || 72 });
                          }}
                        >
                          <option value="4">接下来 4 小时</option>
                          <option value="8">接下来 8 小时</option>
                          <option value="12">接下来 12 小时</option>
                          <option value="24">接下来 24 小时</option>
                          <option value="72">接下来 3 天</option>
                          <option value="168">接下来 7 天</option>
                          <option value="today">今天</option>
                          <option value="custom">自定义</option>
                        </select>
                      </label>
                      {app.spec.table.rangeMode === "custom" && (
                        <>
                          <label>
                            <span>开始</span>
                            <input
                              type="datetime-local"
                              value={app.spec.table.customStart || ""}
                              onChange={(event) => updateAgendaTable({ customStart: event.target.value })}
                            />
                          </label>
                          <label>
                            <span>结束</span>
                            <input
                              type="datetime-local"
                              value={app.spec.table.customEnd || ""}
                              onChange={(event) => updateAgendaTable({ customEnd: event.target.value })}
                            />
                          </label>
                        </>
                      )}
                    </div>
                  </section>
                )}
                {(app.spec.table?.type === "calendar" || app.spec.table?.type === "agenda") && (
                  <section className="calendar-source-editor" aria-label="在线日历数据">
                    <div className="calendar-source-heading">
                      <strong>在线日历数据</strong>
                      <small>仅保存在当前浏览器</small>
                    </div>
                    <div className="calendar-source-options">
                      {app.spec.table.type === "calendar" && (
                        <label>
                          <input
                            type="checkbox"
                            checked={calendarPreferences.lunar}
                            onChange={(event) => updateCalendarPreferences({ lunar: event.target.checked })}
                          />
                          <span><strong>农历日期</strong><small>内置换算，无需联网</small></span>
                        </label>
                      )}
                      <label>
                        <input
                          type="checkbox"
                          checked={calendarPreferences.chinaHolidays}
                          onChange={(event) => updateCalendarPreferences({ chinaHolidays: event.target.checked })}
                        />
                        <span><strong>中国公众假期</strong><small>读取公开 iCal 日历</small></span>
                      </label>
                    </div>
                    <label className="calendar-url-field" htmlFor="calendar-ical-url">
                      <span>个人 iCal 地址</span>
                      <div className="calendar-url-row">
                        <input
                          id="calendar-ical-url"
                          type="url"
                          value={calendarUrlDraft}
                          onChange={(event) => setCalendarUrlDraft(event.target.value)}
                          onKeyDown={(event) => {
                            if (event.key === "Enter") {
                              event.preventDefault();
                              applyCalendarUrl();
                            }
                          }}
                          placeholder="https://…/basic.ics 或 webcal://…"
                          autoComplete="off"
                          spellCheck={false}
                        />
                        <button type="button" onClick={applyCalendarUrl}>读取</button>
                      </div>
                    </label>
                    <p className={calendarNotice ? "calendar-source-warning" : undefined}>
                      {calendarNotice || "适用于 Google、Apple、Outlook 的只读 iCal；支持 HTTPS 与 webcal。地址不会写入应用或公开发现页。"}
                    </p>
                  </section>
                )}
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
                <div className="run-status-copy">
                  <strong>
                    {deviceStatus === "writing" ? progress?.message ?? "正在写入" : scheduleActive ? "定时任务运行中" : deviceName ?? "准备写入 TodooCard"}
                  </strong>
                  {nextRun ? (
                    <div className="next-run-summary">
                      <span>下次执行</span>
                      <b>{formatExactTime(nextRun)}</b>
                      <em>{formatRemaining(nextRun, secondTick)}</em>
                    </div>
                  ) : (
                    <small>{deviceName ? "已授权设备不会再次弹出选择器" : "首次需要手动选择设备 · 之后自动重连"}</small>
                  )}
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
              <button type="button" onClick={() => navigateToTab("studio")}>＋ 创建新应用</button>
            </div>
            {localApps.length ? (
              <div className="card-grid">
                {localApps.map((item) => <AppCard key={item.id} app={item} local onUse={() => copyAppToStudio(item)} />)}
              </div>
            ) : (
              <div className="empty-state">
                <span>▦</span><h2>还没有保存的应用</h2><p>在创作台生成并保存，第一个应用就会出现在这里。</p>
              </div>
            )}
          </section>
        )}

        {tab === "explore" && selectedPublicAppId && (
          <section className="collection-view public-app-detail">
            <div className="public-app-detail-head">
              <button type="button" onClick={() => navigateToTab("explore")}>← 返回发现</button>
              <div>
                <span className="eyebrow">PUBLIC APP</span>
                <h1>{selectedPublicApp?.title || "公开应用"}</h1>
                <p>这个地址可以直接访问并分享给其他人。</p>
              </div>
              <button
                type="button"
                className="copy-public-link"
                onClick={() => void copyPublicAppLink(selectedPublicAppId)}
              >
                复制链接
              </button>
            </div>
            {selectedPublicApp ? (
              <div className="public-app-detail-card">
                <AppCard app={selectedPublicApp} onUse={() => copyAppToStudio(selectedPublicApp)} />
              </div>
            ) : (
              <div className="empty-state public-link-state">
                <span>{publicLinkStatus === "loading" ? "…" : "◎"}</span>
                <h2>{publicLinkStatus === "loading" ? "正在读取公开应用" : "这个公开应用暂时无法访问"}</h2>
                <p>{publicLinkStatus === "loading" ? "很快就好。" : "它可能已经被移除，或链接不完整。"}</p>
              </div>
            )}
          </section>
        )}

        {tab === "explore" && !selectedPublicAppId && (
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
              {publicApps.map((item) => (
                <AppCard
                  key={item.id}
                  app={item}
                  onUse={() => copyAppToStudio(item)}
                  onOpen={() => openPublicApp(item.id)}
                />
              ))}
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
                <span className={`status-orb ${devices.length ? "connected" : ""}`}>⌁</span>
                <strong>{activeDevice?.name ?? "TodooCard 未连接"}</strong>
                <small>{devices.length
                  ? `已添加 ${devices.length} 台设备 · 写入任务按设备独立管理`
                  : "NEMR99803797 / PICKSMART · 528 × 792"}</small>
                <button type="button" onClick={() => void selectNewDevice()}>{devices.length ? "添加另一台设备" : "选择设备"}</button>
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
              <button type="button" onClick={() => { setGuideOpen(false); navigateToTab("device"); }}>查看设备说明</button>
            </div>

            <p className="guide-footnote">本机图片随应用保存在当前浏览器；公开分享时不会上传你的私人图片，而会保留可复用的版式和主题。</p>
          </section>
        </div>
      )}

      {toast && <div className={`toast ${toast.tone}`} role="status"><span>{toast.tone === "success" ? "✓" : toast.tone === "error" ? "!" : "i"}</span>{toast.message}</div>}
    </main>
  );
}
