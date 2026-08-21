"use client";

import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ClipboardEvent,
  type KeyboardEvent as ReactKeyboardEvent,
  type PointerEvent as ReactPointerEvent,
  type WheelEvent as ReactWheelEvent,
} from "react";
import { create as createQrCode } from "qrcode";
import { stochasticSixColorDither } from "./lib/six-color-dither";
import { localeTags, localeOptions, useI18n, type Locale } from "./lib/i18n";
import { activeLocaleTag, t as tRuntime } from "./lib/i18n-runtime";
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
  type CardRarity,
  type CardSpec,
  type CalendarEvent,
  type InkApp,
  type MapLocationMode,
  type MapSpec,
  type ScheduleMode,
  type ScreenDisplay,
  type ScreenElementKey,
  type ScreenFont,
  type ScreenRenderMode,
  type ScreenOrientation,
  type ScreenSpec,
} from "./lib/app-model";
import { TodooCard, type TodooProgress } from "./lib/todoo-card";
import { isRecoverableBluetoothError, writeWithBluetoothRecovery } from "./lib/bluetooth-recovery";
import { deviceAdapter, deviceSku, deviceSkusForFamily, deviceManufacturers, filterDeviceSkus, officialProductUrl, resolveDeviceTargetId, type DeviceFamily, type DeviceSkuId } from "./lib/device-catalog";
import {
  paperColorStrategyForScreenMode,
  type PaperColorRenderStrategy,
} from "./lib/papercolor-render";
import {
  claimEsp32Device,
  deleteEsp32Task,
  flashM5PaperColor,
  listEsp32Devices,
  publishEsp32Task,
  type Esp32DeviceRecord,
  type FirmwareDeviceEvent,
  type FirmwareProgress,
} from "./lib/esp32-device";
import {
  CALIBRATION_SWATCHES,
  analyzeCalibrationCapture,
  validCalibration,
  type DeviceColorCalibration,
} from "./lib/device-calibration";

type Tab = "studio" | "mine" | "explore" | "device" | "products";
type Toast = { tone: "success" | "error" | "info"; message: string } | null;
type ToastTone = NonNullable<Toast>["tone"];
type GeneratorStatus = "checking" | "online" | "local";
type PreviewStatus = "ready" | "loading" | "fallback";
type DeviceTaskStatus = "scheduled" | "writing" | "error";
type ArtworkCredit = { provider: string; url: string };
type CalendarSourcePreference = {
  id: string;
  name: string;
  url: string;
  enabled: boolean;
};
type CalendarPreferences = {
  sources: CalendarSourcePreference[];
  chinaHolidays: boolean;
  lunar: boolean;
};
type CalendarFeedPayload = {
  events?: CalendarEvent[];
  timedEvents?: AgendaEvent[];
  warnings?: string[];
};
type MapResolvePayload = {
  configured?: boolean;
  coordinateType?: "bd09ll";
  latitude?: number;
  longitude?: number;
  address?: string;
  city?: string;
  approximate?: boolean;
  source?: MapLocationMode;
  error?: string;
  code?: string;
};
type MapServiceStatus = "idle" | "checking" | "ready" | "missing" | "error";

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
  family: DeviceFamily;
  skuId: DeviceSkuId;
  colorCorrectionEnabled: boolean;
  calibration?: DeviceColorCalibration;
  hardwareId?: string;
  firmwareVersion?: string | null;
  batteryPercent?: number | null;
  lastSeenAt?: string | null;
  online?: boolean;
  desiredRevision?: number;
  appliedRevision?: number;
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
  execution: "browser-bluetooth" | "device-wifi";
  remoteRevision?: number;
  renderStrategy?: PaperColorRenderStrategy;
};

type AddDeviceStep = "family" | "sku" | "method" | "claim" | "flash" | "flash-complete";

type DragPreview = {
  element: ScreenElementKey;
  x: number;
  y: number;
};

const LOCAL_APPS_KEY = "inkloop-apps-v1";
const DEVICE_PROFILES_KEY = "inkloop-device-profiles-v1";
const ACTIVE_DEVICE_KEY = "inkloop-active-device-v1";
const SIDEBAR_COLLAPSED_KEY = "inkloop-sidebar-collapsed-v1";
const WEATHER_CITY_KEY = "inkloop-weather-city-v1";
const CALENDAR_PREFERENCES_KEY = "inkloop-calendar-sources-v1";
const GENERATOR_MODEL_KEY = "inkloop-generator-model-v1";
const DEFAULT_CALENDAR_PREFERENCES: CalendarPreferences = {
  sources: [],
  chinaHolidays: false,
  lunar: false,
};
const GALLERY_PREVIEW_DATE = new Date("2026-08-01T12:34:00+08:00");
const EPAPER_WHITE = "#fafaf8";

function normalizeDeviceProfile(value: unknown): DeviceProfile | null {
  if (!value || typeof value !== "object") return null;
  const candidate = value as Partial<DeviceProfile>;
  if (typeof candidate.id !== "string" || !candidate.id || typeof candidate.name !== "string" || !candidate.name) return null;
  return {
    id: candidate.id,
    name: candidate.name,
    family: candidate.family === "esp32" ? "esp32" : "bluetooth",
    skuId: candidate.skuId === "m5-papercolor-c151" ? "m5-papercolor-c151" : "todoo-card-3.7",
    colorCorrectionEnabled: candidate.colorCorrectionEnabled !== false,
    calibration: validCalibration(candidate.calibration) ? candidate.calibration : undefined,
    hardwareId: typeof candidate.hardwareId === "string" ? candidate.hardwareId : undefined,
    firmwareVersion: typeof candidate.firmwareVersion === "string" ? candidate.firmwareVersion : null,
    batteryPercent: typeof candidate.batteryPercent === "number" ? candidate.batteryPercent : null,
    lastSeenAt: typeof candidate.lastSeenAt === "string" ? candidate.lastSeenAt : null,
    online: candidate.online === true,
    desiredRevision: typeof candidate.desiredRevision === "number" ? candidate.desiredRevision : 0,
    appliedRevision: typeof candidate.appliedRevision === "number" ? candidate.appliedRevision : 0,
  };
}

function profileFromEsp32(record: Esp32DeviceRecord, existing?: DeviceProfile): DeviceProfile {
  return {
    ...existing,
    id: record.id,
    name: record.name,
    family: "esp32",
    skuId: record.skuId,
    colorCorrectionEnabled: true,
    hardwareId: record.hardwareId,
    firmwareVersion: record.firmwareVersion,
    batteryPercent: record.batteryPercent,
    lastSeenAt: record.lastSeenAt,
    online: record.online,
    desiredRevision: record.desiredRevision,
    appliedRevision: record.appliedRevision,
  };
}

function calibrationQualityLabel(profile: DeviceColorCalibration) {
  if (profile.quality === "excellent") return tRuntime("校色质量优秀");
  if (profile.quality === "good") return tRuntime("校色质量良好");
  return tRuntime("色差较大，建议避开反光后重拍");
}

function mapApiParams(map: MapSpec, mode: "resolve" | "image", orientation?: ScreenOrientation) {
  const params = new URLSearchParams({
    mode,
    locationMode: map.locationMode,
    query: map.query,
    coordtype: map.coordinateType,
    zoom: String(map.zoomLevel),
    marker: String(map.marker),
  });
  if (typeof map.latitude === "number") params.set("lat", String(map.latitude));
  if (typeof map.longitude === "number") params.set("lng", String(map.longitude));
  if (map.approximate) params.set("approximate", "true");
  if (orientation) params.set("orientation", orientation);
  return params;
}

async function resolveMapLocation(map: MapSpec) {
  const response = await fetch(`/api/map?${mapApiParams(map, "resolve").toString()}`, {
    cache: "no-store",
  });
  const payload = await response.json() as MapResolvePayload;
  if (!response.ok || typeof payload.latitude !== "number" || typeof payload.longitude !== "number") {
    throw new Error(payload.error || tRuntime("地图位置暂时无法解析"));
  }
  return payload;
}

function mapImageUrl(map: MapSpec, orientation: ScreenOrientation) {
  return `/api/map?${mapApiParams(map, "image", orientation).toString()}`;
}

function panMapPoint(
  map: MapSpec,
  orientation: ScreenOrientation,
  horizontalDeltaRatio: number,
  verticalDeltaRatio: number,
) {
  if (typeof map.longitude !== "number" || typeof map.latitude !== "number") return null;
  const zoom = Math.min(19, Math.max(3, map.zoomLevel));
  const worldSize = 256 * 2 ** zoom;
  const logicalWidth = (orientation === "landscape" ? 792 : 528) / 2;
  const logicalHeight = (orientation === "landscape" ? 528 : 792) / 2;
  const longitudeToX = (longitude: number) => (longitude + 180) / 360 * worldSize;
  const latitudeToY = (latitude: number) => {
    const radians = latitude * Math.PI / 180;
    return (1 - Math.log(Math.tan(radians) + 1 / Math.cos(radians)) / Math.PI) / 2 * worldSize;
  };
  const xToLongitude = (x: number) => x / worldSize * 360 - 180;
  const yToLatitude = (y: number) => {
    const value = Math.PI - 2 * Math.PI * y / worldSize;
    return 180 / Math.PI * Math.atan(Math.sinh(value));
  };
  // The map follows the pointer, so the geographic center moves in the
  // opposite direction of the visual drag.
  const x = longitudeToX(map.longitude) - horizontalDeltaRatio * logicalWidth;
  const y = latitudeToY(map.latitude) - verticalDeltaRatio * logicalHeight;
  return {
    longitude: Math.min(180, Math.max(-180, xToLongitude(x))),
    latitude: Math.min(85, Math.max(-85, yToLatitude(y))),
  };
}

function calendarSourceName(url: string, position: number) {
  try {
    const hostname = new URL(url.replace(/^webcal:/i, "https:")).hostname.toLowerCase();
    const provider = hostname.includes("icloud")
      ? tRuntime("iCloud 日历")
      : hostname.includes("google")
        ? tRuntime("Google 日历")
        : hostname.includes("outlook") || hostname.includes("office365")
          ? tRuntime("Outlook 日历")
          : hostname.replace(/^www\./, "");
    return position === 0 ? provider : `${provider} ${position + 1}`;
  } catch {
    return `${tRuntime("个人日历")} ${position + 1}`;
  }
}

const navItems: Array<{ id: Tab; label: string; glyph: string }> = [
  { id: "studio", label: "创作台", glyph: "✦" },
  { id: "mine", label: "我的模版", glyph: "▦" },
  { id: "explore", label: "模板市场", glyph: "◎" },
  { id: "device", label: "设备中心", glyph: "⌁" },
  { id: "products", label: "产品信息", glyph: "▣" },
];

const samplePrompts = [
  tRuntime("每天 8 点显示上海天气和带伞提醒"),
  tRuntime("显示新品发布倒计时"),
  tRuntime("彩虹背景，中间写一句今天也很棒"),
  tRuntime("美女时钟，每分钟换背景和字体"),
  tRuntime("生成本月日历，8号项目发布，18号复盘"),
  tRuntime("生成周一到周五的课程表"),
  tRuntime("横版苹果日历风格，显示未来三天日程"),
  tRuntime("做一张星穹机械守卫金卡，自动生成等级和攻防"),
  tRuntime("每 15 分钟更新会议室状态"),
  tRuntime("每小时显示本月销售目标进度"),
];

const quoteOptions = [
  tRuntime("把注意力留给真正重要的事"),
  tRuntime("今天也值得认真生活"),
  tRuntime("慢一点，也是在向前"),
  tRuntime("先完成，再完善"),
  tRuntime("愿每一步都有清晰的回响"),
  tRuntime("专注当下，一次只做一件事"),
  tRuntime("保持好奇，保持热爱"),
  tRuntime("去做让自己眼睛发亮的事"),
];

const screenElementOptions: Array<{ key: ScreenElementKey; label: string; width: number; height: number }> = [
  { key: "quote", label: tRuntime("今日名言"), width: 400, height: 58 },
  { key: "logo", label: "LOGO", width: 220, height: 48 },
  { key: "date", label: tRuntime("日期"), width: 260, height: 54 },
  { key: "time", label: tRuntime("时间"), width: 170, height: 56 },
  { key: "timeLarge", label: tRuntime("时间（大）"), width: 430, height: 154 },
  { key: "weather", label: tRuntime("天气（小）"), width: 310, height: 64 },
  { key: "weatherLarge", label: tRuntime("天气（大）"), width: 430, height: 230 },
  { key: "qr", label: tRuntime("二维码"), width: 176, height: 176 },
];

const screenFontOptions: Array<{ value: ScreenFont; label: string }> = [
  { value: "sans", label: tRuntime("思源黑体 · 清晰") },
  { value: "serif", label: tRuntime("思源宋体 · 优雅") },
  { value: "rounded", label: tRuntime("M PLUS 圆体 · 亲和") },
  { value: "mono", label: tRuntime("等宽数字 · 精准") },
  { value: "handwritten", label: tRuntime("马善政手写 · 醒目") },
];

const renderModeOptions: Array<{
  value: ScreenRenderMode;
  label: string;
  description: string;
}> = [
  { value: "official", label: tRuntime("官方画质"), description: tRuntime("由设备的 M5GFX 官方高质量模式完成六色渲染，兼容性最好") },
  { value: "classic-six-color", label: tRuntime("经典六色"), description: tRuntime("RGB Floyd–Steinberg 六色误差扩散，纹理明确且结果稳定") },
  { value: "reflectance-photo", label: tRuntime("反射率照片"), description: tRuntime("使用实测 PaperColor 色彩、Yule–Nielsen 反射率和 Stucki 扩散，适合照片与渐变") },
  { value: "inkloop-text", label: tRuntime("纯色清晰"), description: tRuntime("关闭误差扩散，适合大色块、文字、二维码、表格和信息卡") },
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
    level: tRuntime("入门"),
    number: "01",
    title: tRuntime("贴图片或文字，帮我排版"),
    description: tRuntime("直接粘贴一张图片和要显示的文字，或点“选择图片”。生成器会把内容整理成适合六色墨水屏的版面。"),
    prompt: tRuntime("请用我贴入的图片做背景，把标题「今天也要开心」和小字「一步一步，慢慢来」排成简洁海报。"),
    action: tRuntime("套用排版示例"),
  },
  {
    level: tRuntime("进阶"),
    number: "02",
    title: tRuntime("让 LLM 生成主题随机内容"),
    description: tRuntime("只要说清主题、语气和用途，LLM 会生成文案、业务逻辑与配图关键词；预览不满意可继续换一张。"),
    prompt: tRuntime("生成一张露营主题的每日鼓励卡，每次刷新换一张自然风景，文案简短、有户外杂志感。"),
    action: tRuntime("套用主题示例"),
  },
  {
    level: tRuntime("高级"),
    number: "03",
    title: tRuntime("做会自动更新的屏幕"),
    description: tRuntime("例如美女时钟每分钟换背景和字体，或每天早上更新指定城市的天气，再选择对应的刷新计划。"),
    prompt: tRuntime("美女时钟，每分钟更新日期和时间、换一张时尚人像背景并随机字体，时间放在白色画板里。"),
    action: tRuntime("套用时钟示例"),
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
      map: savedApp.spec.map
        ? {
            ...savedApp.spec.map,
            style: "balanced",
          }
        : undefined,
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

function truncateCanvasText(
  ctx: CanvasRenderingContext2D,
  text: string,
  maxWidth: number,
) {
  if (!text || maxWidth <= 0) return "";
  if (ctx.measureText(text).width <= maxWidth) return text;

  const ellipsis = "…";
  if (ctx.measureText(ellipsis).width > maxWidth) return "";

  const characters = Array.from(text);
  let low = 0;
  let high = characters.length;
  while (low < high) {
    const middle = Math.ceil((low + high) / 2);
    const candidate = `${characters.slice(0, middle).join("")}${ellipsis}`;
    if (ctx.measureText(candidate).width <= maxWidth) low = middle;
    else high = middle - 1;
  }
  return `${characters.slice(0, low).join("")}${ellipsis}`;
}

function randomArtworkSeed() {
  const values = new Uint32Array(1);
  crypto.getRandomValues(values);
  return (values[0] % 999_999) + 1;
}

function formatExactTime(value: number) {
  return new Date(value).toLocaleString(activeLocaleTag(), {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
}

function formatRemaining(value: number | null, now: number) {
  if (!value) return tRuntime("等待安排");
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
    weekday: new Intl.DateTimeFormat(activeLocaleTag(), { weekday: "short" }).format(now),
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
  const weekday = new Intl.DateTimeFormat(activeLocaleTag(), { weekday: "short" }).format(displayTime);
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
  if (resolved.kind === "map" && resolved.map) {
    try {
      const location = await resolveMapLocation(resolved.map);
      return {
        ...resolved,
        map: {
          ...resolved.map,
          latitude: location.latitude,
          longitude: location.longitude,
          coordinateType: "bd09ll",
          address: location.address || location.city || resolved.map.query || tRuntime("已选位置"),
          approximate: location.approximate === true,
          statusMessage: location.approximate
            ? tRuntime("当前为 IP 城市级估算，可在预览上拖拽微调")
            : tRuntime("位置已确认"),
        },
      };
    } catch (error) {
      return {
        ...resolved,
        map: {
          ...resolved.map,
          statusMessage: error instanceof Error ? error.message : tRuntime("地图位置暂时无法解析"),
        },
      };
    }
  }
  if (resolved.table?.type === "agenda") {
    const preferences = calendarPreferences ?? DEFAULT_CALENDAR_PREFERENCES;
    const enabledSources = preferences.sources.filter((source) => source.enabled && source.url.trim());
    const window = agendaWindow(resolved.table, now);
    let remoteEvents: AgendaEvent[] = [];
    if (enabledSources.length || preferences.chinaHolidays) {
      try {
        const response = await fetch("/api/calendar", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({
            view: "agenda",
            start: window.start.toISOString(),
            end: window.end.toISOString(),
            timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
            customUrls: enabledSources.map(({ name, url }) => ({ name, url })),
            presets: preferences.chinaHolidays ? ["china-holidays"] : [],
          }),
          cache: "no-store",
        });
        if (response.ok) {
          const payload = await response.json() as CalendarFeedPayload;
          remoteEvents = Array.isArray(payload.timedEvents) ? payload.timedEvents : [];
          onCalendarNotice?.(payload.warnings?.length ? payload.warnings.join("；") : null);
        } else {
          onCalendarNotice?.(tRuntime("在线日历暂时无法读取"));
        }
      } catch {
        remoteEvents = [];
        onCalendarNotice?.(tRuntime("在线日历暂时无法读取"));
      }
    } else {
      onCalendarNotice?.(null);
    }
    // A personal iCal is the source of truth. Generated sample events are useful
    // before a feed is connected, but must not be mixed into a real calendar.
    const localEvents = enabledSources.length ? [] : resolved.table.events;
    const events = [...localEvents, ...remoteEvents]
      .filter((event) => {
        const start = Date.parse(event.start);
        const end = Date.parse(event.end);
        return Number.isFinite(start) && Number.isFinite(end) && start < window.end.getTime() && end > window.start.getTime();
      })
      .filter((event, index, values) => values.findIndex((item) => item.uid === event.uid && item.calendar === event.calendar) === index)
      .sort((left, right) => Date.parse(left.start) - Date.parse(right.start))
      .slice(0, 80);
    return {
      ...resolved,
      table: { ...resolved.table, events },
    };
  }
  if (resolved.table?.type === "calendar") {
    const preferences = calendarPreferences ?? DEFAULT_CALENDAR_PREFERENCES;
    const enabledSources = preferences.sources.filter((source) => source.enabled && source.url.trim());
    let remoteEvents: CalendarEvent[] = [];
    if (enabledSources.length || preferences.chinaHolidays) {
      try {
        const response = await fetch("/api/calendar", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({
            year: resolved.table.year,
            month: resolved.table.month,
            customUrls: enabledSources.map(({ name, url }) => ({ name, url })),
            presets: preferences.chinaHolidays ? ["china-holidays"] : [],
          }),
          cache: "no-store",
        });
        if (response.ok) {
          const payload = await response.json() as CalendarFeedPayload;
          remoteEvents = Array.isArray(payload.events) ? payload.events : [];
          onCalendarNotice?.(payload.warnings?.length ? payload.warnings.join("；") : null);
        } else {
          onCalendarNotice?.(tRuntime("在线日历暂时无法读取"));
        }
      } catch {
        remoteEvents = [];
        onCalendarNotice?.(tRuntime("在线日历暂时无法读取"));
      }
    } else {
      onCalendarNotice?.(null);
    }
    // Keep generated annotations when only public presets are enabled, but replace
    // sample data once the user connects a personal iCal feed.
    const localEvents = enabledSources.length ? [] : resolved.table.events;
    const events = [...localEvents, ...remoteEvents]
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
        weatherDetail: tRuntime("天气服务暂时不可用"),
        weatherAccent: "yellow" as const,
      };
      if (resolved.kind !== "weather" || resolved.clock?.enabled) {
        return { ...resolved, ...unavailableOverlay };
      }
      return {
        ...resolved,
        ...unavailableOverlay,
        eyebrow: `${city} · 今日`,
        title: tRuntime("今日天气"),
        value: "--",
        unit: "°C",
        detail: tRuntime("天气服务暂时不可用"),
        footer: tRuntime("稍后刷新会自动重试"),
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
    const weekday = new Intl.DateTimeFormat(activeLocaleTag(), { weekday: "short" }).format(now);
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
      title: tRuntime("今日天气"),
      value: temperature,
      unit: "°C",
      detail,
      footer: rainProbability >= 45
        ? `降雨${Math.round(rainProbability)}% · 带伞 · Open-Meteo`
        : tRuntime("少雨 · 适合出门 · Open-Meteo"),
      accent: weatherAccent,
    };
  } catch {
    const unavailableOverlay = {
      city,
      weatherText: `${city} · 天气暂不可用`,
      weatherValue: "--",
      weatherUnit: "°C",
      weatherDetail: tRuntime("天气数据暂不可用"),
      weatherAccent: "yellow" as const,
    };
    if (resolved.kind !== "weather" || resolved.clock?.enabled) {
      return { ...resolved, ...unavailableOverlay };
    }
    return {
      ...resolved,
      ...unavailableOverlay,
      eyebrow: resolved.eyebrow === "—" ? `${city} · 今日` : resolved.eyebrow,
      title: resolved.title === "—" ? tRuntime("今日天气") : resolved.title,
      value: resolved.value === "—" ? "--" : resolved.value,
      unit: "°C",
      detail: resolved.detail === "—" ? tRuntime("天气数据暂不可用") : resolved.detail,
      footer: resolved.footer === "—" ? tRuntime("稍后刷新重试") : resolved.footer,
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
  if (spec.kind === "card") return "portrait";
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
    const timeout = window.setTimeout(() => reject(new Error(tRuntime("图片素材加载超时"))), 15_000);
    image.decoding = "async";
    image.onload = () => {
      window.clearTimeout(timeout);
      resolve(image);
    };
    image.onerror = () => {
      window.clearTimeout(timeout);
      reject(new Error(tRuntime("图片素材加载失败")));
    };
    image.src = url;
  });
}

async function loadArtwork(url: string) {
  if (url.startsWith("data:") || url.startsWith("blob:")) return decodeArtwork(url);
  const response = await fetch(url, { cache: "force-cache", signal: AbortSignal.timeout(15_000) });
  if (!response.ok) throw new Error(tRuntime("图片素材加载失败"));
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
  if (!file.type.startsWith("image/")) throw new Error(tRuntime("请选择图片文件"));
  if (file.size > 15 * 1024 * 1024) throw new Error(tRuntime("图片请控制在 15MB 以内"));
  const source = await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error(tRuntime("读取图片失败")));
    reader.readAsDataURL(file);
  });
  const image = await loadArtwork(source);
  const canvas = document.createElement("canvas");
  canvas.width = 528;
  canvas.height = 792;
  const context = canvas.getContext("2d");
  if (!context) throw new Error(tRuntime("浏览器无法处理这张图片"));
  context.fillStyle = EPAPER_WHITE;
  context.fillRect(0, 0, canvas.width, canvas.height);
  const scale = Math.max(canvas.width / image.naturalWidth, canvas.height / image.naturalHeight);
  const width = image.naturalWidth * scale;
  const height = image.naturalHeight * scale;
  context.drawImage(image, (canvas.width - width) / 2, (canvas.height - height) / 2, width, height);
  return canvas.toDataURL("image/png");
}

async function analyzeCalibrationPhoto(file: File) {
  if (!file.type.startsWith("image/")) throw new Error(tRuntime("请选择拍摄的图片文件"));
  if (file.size > 20 * 1024 * 1024) throw new Error(tRuntime("照片请控制在 20MB 以内"));
  const source = await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error(tRuntime("读取照片失败")));
    reader.readAsDataURL(file);
  });
  const image = await loadArtwork(source);
  const analysisCanvas = document.createElement("canvas");
  const analysisScale = Math.min(1, 900 / Math.max(image.naturalWidth, image.naturalHeight));
  analysisCanvas.width = Math.max(120, Math.round(image.naturalWidth * analysisScale));
  analysisCanvas.height = Math.max(120, Math.round(image.naturalHeight * analysisScale));
  const analysisContext = analysisCanvas.getContext("2d", { willReadFrequently: true });
  if (!analysisContext) throw new Error(tRuntime("浏览器无法分析这张照片"));
  analysisContext.drawImage(image, 0, 0, analysisCanvas.width, analysisCanvas.height);
  const imageData = analysisContext.getImageData(0, 0, analysisCanvas.width, analysisCanvas.height);
  const analyzed = analyzeCalibrationCapture(imageData.data, imageData.width, imageData.height);
  const { bounds, rotation } = analyzed.detection;
  const sourceX = bounds.x * image.naturalWidth;
  const sourceY = bounds.y * image.naturalHeight;
  const sourceWidth = bounds.width * image.naturalWidth;
  const sourceHeight = bounds.height * image.naturalHeight;
  const previewCanvas = document.createElement("canvas");
  previewCanvas.width = 528;
  previewCanvas.height = 792;
  const previewContext = previewCanvas.getContext("2d");
  if (!previewContext) throw new Error(tRuntime("浏览器无法生成校色预览"));
  previewContext.fillStyle = "#f7f4e8";
  previewContext.fillRect(0, 0, previewCanvas.width, previewCanvas.height);
  previewContext.save();
  if (rotation === 90) {
    previewContext.translate(previewCanvas.width, 0);
    previewContext.rotate(Math.PI / 2);
    previewContext.drawImage(image, sourceX, sourceY, sourceWidth, sourceHeight, 0, 0, previewCanvas.height, previewCanvas.width);
  } else if (rotation === 270) {
    previewContext.translate(0, previewCanvas.height);
    previewContext.rotate(-Math.PI / 2);
    previewContext.drawImage(image, sourceX, sourceY, sourceWidth, sourceHeight, 0, 0, previewCanvas.height, previewCanvas.width);
  } else if (rotation === 180) {
    previewContext.translate(previewCanvas.width, previewCanvas.height);
    previewContext.rotate(Math.PI);
    previewContext.drawImage(image, sourceX, sourceY, sourceWidth, sourceHeight, 0, 0, previewCanvas.width, previewCanvas.height);
  } else {
    previewContext.drawImage(image, sourceX, sourceY, sourceWidth, sourceHeight, 0, 0, previewCanvas.width, previewCanvas.height);
  }
  previewContext.restore();
  return {
    profile: analyzed.profile,
    preview: previewCanvas.toDataURL("image/jpeg", 0.84),
  };
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
  ctx.save();
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
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
  image.data.set(stochasticSixColorDither(image.data, width, height, { protectNeutral }));
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
  ctx.strokeStyle = EPAPER_WHITE;
  ctx.lineWidth = 6;
  ctx.shadowColor = "rgba(250, 250, 248, 0.9)";
  ctx.shadowBlur = 8;
  if (maxWidth) ctx.strokeText(text, x, y, maxWidth);
  else ctx.strokeText(text, x, y);
  ctx.shadowBlur = 0;
  ctx.fillStyle = "#151816";
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
  const condition = weatherDetail.split("·")[0]?.trim() || tRuntime("天气多变");
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
  tRuntime("初一"), tRuntime("初二"), tRuntime("初三"), tRuntime("初四"), tRuntime("初五"), tRuntime("初六"), tRuntime("初七"), tRuntime("初八"), tRuntime("初九"), tRuntime("初十"),
  tRuntime("十一"), tRuntime("十二"), tRuntime("十三"), tRuntime("十四"), tRuntime("十五"), tRuntime("十六"), tRuntime("十七"), tRuntime("十八"), tRuntime("十九"), tRuntime("二十"),
  tRuntime("廿一"), tRuntime("廿二"), tRuntime("廿三"), tRuntime("廿四"), tRuntime("廿五"), tRuntime("廿六"), tRuntime("廿七"), tRuntime("廿八"), tRuntime("廿九"), tRuntime("三十"),
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
    ? [tRuntime("日"), tRuntime("一"), tRuntime("二"), tRuntime("三"), tRuntime("四"), tRuntime("五"), tRuntime("六")]
    : [tRuntime("一"), tRuntime("二"), tRuntime("三"), tRuntime("四"), tRuntime("五"), tRuntime("六"), tRuntime("日")];
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

function agendaEventAccent(event: AgendaEvent) {
  return tableAccent(event.category || event.calendar || event.title);
}

function fillAgendaTint(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  accent: string,
) {
  if (width <= 0 || height <= 0) return;
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(x, y, width, height);

  // The panel has no pastel pigments. A sparse, ordered one-pixel screen
  // keeps the card visibly tinted without introducing photographic noise.
  const spacing = 5;
  ctx.fillStyle = accent;
  const firstRow = Math.ceil(y / spacing);
  const lastRow = Math.floor((y + height - 1) / spacing);
  for (let row = firstRow; row <= lastRow; row += 1) {
    const py = row * spacing;
    const offset = row % 2 === 0 ? 0 : Math.floor(spacing / 2);
    const firstColumn = Math.ceil((x - offset) / spacing);
    const lastColumn = Math.floor((x + width - 1 - offset) / spacing);
    for (let column = firstColumn; column <= lastColumn; column += 1) {
      ctx.fillRect(column * spacing + offset, py, 1, 1);
    }
  }
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
  ctx.fillText(spec.title || tRuntime("一周课程表"), left, landscape ? 48 : 68, ctx.canvas.width - 120);
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

function agendaTimeLabel(event: AgendaEvent, showEndTime = true) {
  if (event.allDay) return tRuntime("全天");
  const start = new Date(event.start);
  const end = new Date(event.end);
  const time = (value: Date) => `${String(value.getHours()).padStart(2, "0")}:${String(value.getMinutes()).padStart(2, "0")}`;
  return showEndTime ? `${time(start)}–${time(end)}` : time(start);
}

type AgendaLaneItem = {
  event: AgendaEvent;
  lane: number;
  laneCount: number;
};

function layoutAgendaLanes(events: AgendaEvent[], minimumVisualMinutes: number): AgendaLaneItem[] {
  const minuteOfDay = (value: Date) => value.getHours() * 60 + value.getMinutes();
  const items = events
    .map((event) => {
      const start = minuteOfDay(new Date(event.start));
      const naturalEnd = minuteOfDay(new Date(event.end));
      const end = Math.max(start + 15, naturalEnd, start + minimumVisualMinutes);
      return { event, start, end };
    })
    .sort((left, right) => left.start - right.start || right.end - left.end);
  const clusters: typeof items[] = [];
  let cluster: typeof items = [];
  let clusterEnd = -Infinity;
  items.forEach((item) => {
    if (cluster.length && item.start >= clusterEnd) {
      clusters.push(cluster);
      cluster = [];
      clusterEnd = -Infinity;
    }
    cluster.push(item);
    clusterEnd = Math.max(clusterEnd, item.end);
  });
  if (cluster.length) clusters.push(cluster);

  return clusters.flatMap((group) => {
    const laneEnds: number[] = [];
    const assigned = group.map((item) => {
      let lane = laneEnds.findIndex((end) => end <= item.start);
      if (lane < 0) lane = laneEnds.length;
      laneEnds[lane] = item.end;
      return { event: item.event, lane };
    });
    return assigned.map((item) => ({ ...item, laneCount: laneEnds.length }));
  });
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
  const table = spec.table;
  const { view } = table;
  const events = table.events
    .filter((event) => Number.isFinite(Date.parse(event.start)) && Number.isFinite(Date.parse(event.end)))
    .sort((left, right) => Date.parse(left.start) - Date.parse(right.start));
  const family = screenFontFamily(spec);
  const ink = "#151816";
  const width = ctx.canvas.width;
  const height = ctx.canvas.height;
  const margin = 24;

  ctx.save();
  if (view === "agenda") {
    ctx.fillStyle = ink;
    ctx.textAlign = "left";
    ctx.font = `900 ${width > height ? 34 : 40}px ${family}`;
    ctx.fillText(spec.title || tRuntime("智能日程"), margin, width > height ? 46 : 62, width - margin * 2 - 150);
    ctx.fillStyle = accentColors[spec.accent];
    ctx.fillRect(margin, width > height ? 60 : 80, 88, 7);
    ctx.fillStyle = ink;
    ctx.textAlign = "right";
    ctx.font = `800 15px ${family}`;
    ctx.fillText(tRuntime("按时间排序"), width - margin, width > height ? 47 : 62);
    const top = width > height ? 86 : 108;
    const usable = events.slice(0, width > height ? 7 : 9);
    if (!usable.length) {
      ctx.textAlign = "center";
      ctx.font = `800 28px ${family}`;
      ctx.fillText(tRuntime("这段时间没有日程"), width / 2, height / 2);
      ctx.font = `700 17px ${family}`;
      ctx.fillText(tRuntime("可以调整时间范围，或读取 iCal"), width / 2, height / 2 + 38);
      ctx.restore();
      return;
    }
    const rowHeight = Math.floor((height - top - 20) / usable.length);
    usable.forEach((event, index) => {
      const y = top + index * rowHeight;
      const date = new Date(event.start);
      const accent = agendaEventAccent(event);
      fillAgendaTint(ctx, margin, y + 3, width - margin * 2, rowHeight - 7, accent);
      ctx.fillStyle = accent;
      ctx.fillRect(margin, y + 3, 8, rowHeight - 7);
      ctx.fillStyle = ink;
      ctx.textAlign = "left";
      ctx.font = `900 ${rowHeight < 54 ? 15 : 18}px ${family}`;
      ctx.fillText(
        truncateCanvasText(ctx, `${date.getMonth() + 1}/${date.getDate()}`, 54),
        margin + 22,
        y + rowHeight / 2 - 5,
      );
      ctx.font = `800 ${rowHeight < 54 ? 14 : 17}px ${family}`;
      ctx.fillText(
        truncateCanvasText(ctx, agendaTimeLabel(event, table.showEndTime !== false), 118),
        margin + 78,
        y + rowHeight / 2 - 5,
      );
      ctx.font = `900 ${rowHeight < 54 ? 17 : 21}px ${family}`;
      ctx.fillText(
        truncateCanvasText(ctx, event.title, width - 260),
        margin + 210,
        y + rowHeight / 2 - 5,
      );
      if (table.showLocation !== false && event.location && rowHeight >= 54) {
        ctx.fillStyle = ink;
        ctx.font = `700 13px ${family}`;
        ctx.fillText(
          truncateCanvasText(ctx, event.location, width - 260),
          margin + 210,
          y + rowHeight / 2 + 18,
        );
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
  const top = width > height ? 16 : 20;
  const gap = 8;
  const headerHeight = 48;
  const timeRailWidth = width > height ? 44 : 38;
  const eventWidthRatio = Math.min(100, Math.max(45, table.eventWidth ?? 100)) / 100;
  const gridLeft = margin + timeRailWidth;
  const gridRight = width - margin;
  const columnWidth = (gridRight - gridLeft - gap * (columnCount - 1)) / columnCount;
  const days = Array.from({ length: columnCount }, (_, column) => {
    const day = new Date(start);
    day.setDate(start.getDate() + column);
    const key = agendaDateKey(day);
    const dayEvents = events.filter((event) => agendaDateKey(new Date(event.start)) === key);
    return {
      day,
      allDay: dayEvents.filter((event) => event.allDay),
      timed: dayEvents.filter((event) => !event.allDay),
    };
  });
  const allDayRows = Math.min(2, Math.max(0, ...days.map((day) => day.allDay.length)));
  const allDayRowHeight = 32;
  const allDayTop = top + headerHeight + 8;
  const allDayHeight = allDayRows ? allDayRows * allDayRowHeight + 4 : 0;
  const timelineTop = allDayTop + allDayHeight + (allDayRows ? 8 : 0);
  const timelineBottom = height - 18;
  const visibleTimed = days.flatMap((day) => day.timed);
  const minuteOfDay = (value: Date) => value.getHours() * 60 + value.getMinutes();
  const timelineHeight = Math.max(80, timelineBottom - timelineTop);
  const activeSegments = visibleTimed
    .map((event) => {
      const start = minuteOfDay(new Date(event.start));
      const end = Math.min(24 * 60, Math.max(start + 15, minuteOfDay(new Date(event.end))));
      return { start, end };
    })
    .sort((left, right) => left.start - right.start)
    .reduce<Array<{ start: number; end: number }>>((segments, segment) => {
      const previous = segments.at(-1);
      if (previous && segment.start - previous.end <= 30) {
        previous.end = Math.max(previous.end, segment.end);
      } else {
        segments.push({ ...segment });
      }
      return segments;
    }, []);
  if (!activeSegments.length) activeSegments.push({ start: 8 * 60, end: 18 * 60 });

  // The vertical timeline is intentionally discontinuous: periods with no event
  // on any visible day collapse into a small visual gap instead of consuming space.
  const segmentGap = activeSegments.length > 1 ? Math.min(14, timelineHeight / (activeSegments.length * 5)) : 0;
  const totalGapHeight = segmentGap * Math.max(0, activeSegments.length - 1);
  const activeMinuteSpan = activeSegments.reduce((total, segment) => total + segment.end - segment.start, 0);
  const desiredActiveHeight = Math.max(activeSegments.length * 56, activeMinuteSpan * 0.78);
  const activeHeight = Math.min(timelineHeight - totalGapHeight, desiredActiveHeight);
  const minuteScale = activeHeight / Math.max(1, activeMinuteSpan);
  let segmentCursor = timelineTop;
  const segmentLayouts = activeSegments.map((segment) => {
    const pixelStart = segmentCursor;
    const pixelEnd = pixelStart + (segment.end - segment.start) * minuteScale;
    segmentCursor = pixelEnd + segmentGap;
    return { ...segment, pixelStart, pixelEnd };
  });
  const timelineStart = activeSegments[0].start;
  const timelineEnd = activeSegments.at(-1)?.end ?? timelineStart + 60;
  const yForMinute = (minute: number) => {
    const layout = segmentLayouts.find((segment) => minute >= segment.start && minute <= segment.end)
      ?? (minute < timelineStart ? segmentLayouts[0] : segmentLayouts.at(-1));
    if (!layout) return timelineTop;
    const clamped = Math.min(layout.end, Math.max(layout.start, minute));
    return layout.pixelStart + (clamped - layout.start) * minuteScale;
  };

  days.forEach(({ day }, column) => {
    const x = gridLeft + column * (columnWidth + gap);
    ctx.fillStyle = column === 0 ? "#e5c900" : "#f0eee4";
    ctx.fillRect(x, top, columnWidth, headerHeight);
    ctx.fillStyle = ink;
    ctx.textAlign = "left";
    ctx.font = `900 ${view === "workweek" ? 16 : 19}px ${family}`;
    const weekday = new Intl.DateTimeFormat(activeLocaleTag(), { weekday: "short" }).format(day);
    ctx.fillText(
      truncateCanvasText(ctx, `${day.getMonth() + 1}/${day.getDate()} ${weekday}`, columnWidth - 18),
      x + 10,
      top + 31,
    );
  });

  if (allDayRows) {
    ctx.fillStyle = ink;
    ctx.textAlign = "right";
    ctx.font = `800 11px ${family}`;
    ctx.fillText(tRuntime("全天"), gridLeft - 8, allDayTop + 20, timeRailWidth - 4);
    days.forEach(({ allDay }, column) => {
      const x = gridLeft + column * (columnWidth + gap);
      const cardWidth = columnWidth * eventWidthRatio;
      allDay.slice(0, allDayRows).forEach((event, index) => {
        const y = allDayTop + index * allDayRowHeight;
        const accent = agendaEventAccent(event);
        fillAgendaTint(ctx, x, y, cardWidth, allDayRowHeight - 4, accent);
        ctx.fillStyle = accent;
        ctx.fillRect(x, y, 6, allDayRowHeight - 4);
        ctx.fillStyle = ink;
        ctx.textAlign = "left";
        ctx.font = `900 ${view === "workweek" ? 12 : 15}px ${family}`;
        ctx.fillText(truncateCanvasText(ctx, event.title, cardWidth - 20), x + 12, y + 20);
      });
    });
  }

  segmentLayouts.forEach((segment) => {
    ctx.fillStyle = ink;
    ctx.textAlign = "right";
    ctx.font = `700 10px ${family}`;
    ctx.fillText(
      `${String(Math.floor(segment.start / 60)).padStart(2, "0")}:${String(segment.start % 60).padStart(2, "0")}`,
      gridLeft - 8,
      segment.pixelStart + 10,
    );
  });

  segmentLayouts.slice(0, -1).forEach((segment, index) => {
    const next = segmentLayouts[index + 1];
    const gapY = Math.round((segment.pixelEnd + next.pixelStart) / 2);
    const markerCenter = Math.round(gridLeft - timeRailWidth / 2);
    ctx.fillStyle = ink;
    [-6, 0, 6].forEach((offset) => ctx.fillRect(markerCenter + offset, gapY, 2, 2));
  });

  days.forEach(({ timed }, column) => {
    const x = gridLeft + column * (columnWidth + gap);
    const eventLimit = view === "workweek" ? 8 : 10;
    const minimumVisualMinutes = Math.max(15, Math.ceil(44 / Math.max(0.01, minuteScale)));
    const laneItems = layoutAgendaLanes(timed.slice(0, eventLimit), minimumVisualMinutes);
    laneItems.forEach(({ event, lane, laneCount }) => {
      const totalCardWidth = columnWidth * eventWidthRatio;
      const laneGap = laneCount > 1 ? 3 : 0;
      const cardWidth = Math.max(12, (totalCardWidth - laneGap * (laneCount - 1)) / laneCount);
      const cardX = x + lane * (cardWidth + laneGap);
      const startMinute = Math.max(timelineStart, minuteOfDay(new Date(event.start)));
      const endMinute = Math.min(timelineEnd, Math.max(startMinute + 15, minuteOfDay(new Date(event.end))));
      const naturalTop = yForMinute(startMinute);
      const naturalHeight = yForMinute(endMinute) - naturalTop - 2;
      const cardHeight = Math.max(42, naturalHeight);
      const containingSegment = segmentLayouts.find((segment) => startMinute >= segment.start && startMinute <= segment.end);
      const segmentBottom = containingSegment?.pixelEnd ?? timelineBottom;
      const y = Math.min(naturalTop, Math.max(timelineTop, segmentBottom - cardHeight));
      const accent = agendaEventAccent(event);
      fillAgendaTint(ctx, cardX, y, cardWidth, cardHeight, accent);
      const stripeWidth = laneCount > 2 ? 4 : 6;
      ctx.fillStyle = accent;
      ctx.fillRect(cardX, y, stripeWidth, cardHeight);
      ctx.fillStyle = ink;
      ctx.textAlign = "left";
      const dense = laneCount > 1;
      const textInset = stripeWidth + (dense ? 3 : 5);
      ctx.font = `800 ${dense ? 8 : view === "workweek" ? 10 : 12}px ${family}`;
      ctx.fillText(
        truncateCanvasText(ctx, agendaTimeLabel(event, table.showEndTime !== false && cardWidth >= 76), cardWidth - textInset - 5),
        cardX + textInset,
        y + 14,
      );
      ctx.font = `900 ${dense ? 10 : view === "workweek" ? 13 : 16}px ${family}`;
      ctx.fillText(truncateCanvasText(ctx, event.title, cardWidth - textInset - 5), cardX + textInset, y + 34);
      if (table.showLocation !== false && event.location && cardHeight >= 64 && cardWidth >= 68) {
        ctx.font = `700 ${dense ? 8 : view === "workweek" ? 10 : 11}px ${family}`;
        ctx.fillText(truncateCanvasText(ctx, event.location, cardWidth - textInset - 5), cardX + textInset, y + 51);
      }
    });
    if (!timed.length && !days[column].allDay.length) {
      ctx.fillStyle = "#707871";
      ctx.textAlign = "left";
      ctx.font = `700 12px ${family}`;
      ctx.fillText(tRuntime("无安排"), x + 10, timelineTop + 22, columnWidth - 18);
    } else if (timed.length > (view === "workweek" ? 8 : 10)) {
      ctx.fillStyle = ink;
      ctx.font = `900 13px ${family}`;
      ctx.fillText(`+${timed.length - (view === "workweek" ? 8 : 10)} 项`, x + 10, height - 8, columnWidth - 18);
    }
  });
  ctx.restore();
}

function drawStructuredTable(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  if (spec.table?.type === "calendar") drawCalendarTable(ctx, spec);
  if (spec.table?.type === "timetable") drawTimetable(ctx, spec);
  if (spec.table?.type === "agenda") drawAgendaTable(ctx, spec);
}

function drawMapMessage(ctx: CanvasRenderingContext2D, spec: ScreenSpec, message: string) {
  const family = screenFontFamily(spec);
  const centerX = ctx.canvas.width / 2;
  const centerY = ctx.canvas.height / 2;
  ctx.save();
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
  ctx.fillStyle = accentColors.yellow;
  ctx.beginPath();
  ctx.arc(centerX, centerY - 74, 28, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = "#151816";
  ctx.textAlign = "center";
  ctx.font = `900 34px ${family}`;
  ctx.fillText(tRuntime("地图等待位置"), centerX, centerY, ctx.canvas.width - 80);
  ctx.font = `700 19px ${family}`;
  ctx.fillText(truncateCanvasText(ctx, message, ctx.canvas.width - 96), centerX, centerY + 50);
  ctx.font = `700 16px ${family}`;
  ctx.fillText(tRuntime("请在右侧地图设置中修正后重试"), centerX, centerY + 88, ctx.canvas.width - 96);
  ctx.restore();
}

function drawMapInformation(ctx: CanvasRenderingContext2D, spec: ScreenSpec) {
  const map = spec.map;
  if (!map || (!map.showAddress && !map.showCoordinates)) return;
  const family = screenFontFamily(spec);
  const landscape = screenOrientation(spec) === "landscape";
  const innerPadding = landscape ? 30 : 28;
  const address = map.displayName?.trim()
    || map.address
    || map.query
    || (map.approximate ? tRuntime("IP 城市级估算") : tRuntime("已选位置"));
  const coordinateText = typeof map.latitude === "number" && typeof map.longitude === "number"
    ? `${map.longitude.toFixed(5)}, ${map.latitude.toFixed(5)}`
    : "";
  const lineCount = Number(map.showAddress) + Number(map.showCoordinates && coordinateText);
  const panelHeight = lineCount > 1 ? (landscape ? 94 : 112) : (landscape ? 64 : 76);
  const top = ctx.canvas.height - panelHeight;
  const width = ctx.canvas.width;
  ctx.save();
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(0, top, width, panelHeight);
  ctx.fillStyle = map.approximate ? accentColors.yellow : accentColors.blue;
  ctx.fillRect(0, top, landscape ? 10 : 12, panelHeight);
  ctx.fillStyle = "#151816";
  ctx.textAlign = "left";
  if (map.showAddress) {
    ctx.font = `900 ${landscape ? 22 : 23}px ${family}`;
    const prefix = map.approximate ? tRuntime("约 ") : "";
    ctx.fillText(
      truncateCanvasText(ctx, `${prefix}${address}`, width - innerPadding * 2),
      innerPadding,
      top + (landscape ? 35 : 42),
    );
  }
  if (map.showCoordinates && coordinateText) {
    ctx.font = `700 ${landscape ? 16 : 18}px ${screenFonts.mono}`;
    ctx.fillText(
      truncateCanvasText(ctx, coordinateText, width - innerPadding * 2),
      innerPadding,
      top + (map.showAddress ? (landscape ? 72 : 84) : (landscape ? 40 : 48)),
    );
  }
  ctx.restore();
}

async function drawMapScreen(ctx: CanvasRenderingContext2D, spec: ScreenSpec, quantize = true) {
  const map = spec.map;
  if (!map || typeof map.latitude !== "number" || typeof map.longitude !== "number") {
    drawMapMessage(ctx, spec, map?.statusMessage || tRuntime("请先选择地图位置"));
    return false;
  }
  try {
    const image = await loadArtwork(mapImageUrl(map, screenOrientation(spec)));
    ctx.save();
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(image, 0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.restore();
    if (quantize) {
      quantizeRegion(
        ctx,
        0,
        0,
        ctx.canvas.width,
        ctx.canvas.height,
        "official",
      );
    }
    drawMapInformation(ctx, spec);
    return true;
  } catch {
    drawMapMessage(ctx, spec, tRuntime("静态地图暂时无法获取；请检查服务端配置或稍后重试"));
    return false;
  }
}

const cardMaterialAssets: Record<CardRarity, string> = {
  common: "/card-templates/common-paper.webp",
  silver: "/card-templates/silver-foil.webp",
  gold: "/card-templates/gold-foil.webp",
  holo: "/card-templates/holo-prism.webp",
};

const cardRarityLabels: Record<CardRarity, string> = {
  common: "STANDARD",
  silver: "SILVER",
  gold: "GOLD",
  holo: "HOLOGRAPHIC",
};

function chamferedRectPath(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  width: number,
  height: number,
  corner = 12,
) {
  ctx.beginPath();
  ctx.moveTo(x + corner, y);
  ctx.lineTo(x + width - corner, y);
  ctx.lineTo(x + width, y + corner);
  ctx.lineTo(x + width, y + height - corner);
  ctx.lineTo(x + width - corner, y + height);
  ctx.lineTo(x + corner, y + height);
  ctx.lineTo(x, y + height - corner);
  ctx.lineTo(x, y + corner);
  ctx.closePath();
}

function cardTextLines(ctx: CanvasRenderingContext2D, text: string, maxWidth: number, maximumLines: number) {
  const characters = Array.from(text.trim());
  const lines: string[] = [];
  let line = "";
  characters.forEach((character) => {
    const candidate = `${line}${character}`;
    if (line && ctx.measureText(candidate).width > maxWidth) {
      lines.push(line);
      line = character;
    } else {
      line = candidate;
    }
  });
  if (line) lines.push(line);
  if (lines.length <= maximumLines) return lines;
  const visible = lines.slice(0, maximumLines);
  visible[maximumLines - 1] = truncateCanvasText(ctx, `${visible[maximumLines - 1]}${lines.slice(maximumLines).join("")}`, maxWidth);
  return visible;
}

function drawCardStar(ctx: CanvasRenderingContext2D, x: number, y: number, radius: number, color: string) {
  ctx.save();
  ctx.translate(x, y);
  ctx.fillStyle = color;
  ctx.beginPath();
  for (let index = 0; index < 8; index += 1) {
    const angle = -Math.PI / 2 + index * Math.PI / 4;
    const distance = index % 2 === 0 ? radius : radius * 0.28;
    const px = Math.cos(angle) * distance;
    const py = Math.sin(angle) * distance;
    if (index === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  }
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

async function drawCardScreen(
  ctx: CanvasRenderingContext2D,
  spec: ScreenSpec,
  localImage?: string,
  quantize = true,
) {
  const card = spec.card;
  if (!card) return false;
  const family = screenFonts.sans;
  const material = card.rarity;
  const darkMaterial = material === "gold";
  const ink = darkMaterial ? EPAPER_WHITE : "#151816";
  const accent = material === "common"
    ? accentColors.green
    : material === "silver"
      ? accentColors.blue
      : material === "gold"
        ? accentColors.yellow
        : accentColors.red;
  // Every generated material master uses this exact 528×792 content grid.
  const header = { x: 58, y: 112, width: 412, height: 66 };
  const art = { x: 61, y: 187, width: 406, height: 366 };
  const copy = { x: 58, y: 562, width: 412, height: 112 };

  ctx.save();
  ctx.fillStyle = EPAPER_WHITE;
  ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
  try {
    const template = await loadArtwork(cardMaterialAssets[material]);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(template, 0, 0, ctx.canvas.width, ctx.canvas.height);
  } catch {
    ctx.fillStyle = darkMaterial ? "#151816" : EPAPER_WHITE;
    ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
  }

  // The master already contains the frame. Artwork is clipped inside its fixed window.
  ctx.save();
  chamferedRectPath(ctx, art.x, art.y, art.width, art.height, 9);
  ctx.clip();
  ctx.fillStyle = darkMaterial ? "#151816" : "#dfe4df";
  ctx.fillRect(art.x, art.y, art.width, art.height);
  let usedArtwork = false;
  try {
    const artwork = spec.artwork;
    const source = localImage || (artwork ? artworkUrl(artwork, "portrait") : "");
    if (source) {
      const image = await loadArtwork(source);
      const baseScale = Math.max(art.width / image.naturalWidth, art.height / image.naturalHeight);
      const scale = baseScale * card.subjectScale;
      const drawWidth = image.naturalWidth * scale;
      const drawHeight = image.naturalHeight * scale;
      const offsetX = card.subjectX / 100 * art.width * 0.42;
      const offsetY = card.subjectY / 100 * art.height * 0.42;
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(
        image,
        art.x + (art.width - drawWidth) / 2 + offsetX,
        art.y + (art.height - drawHeight) / 2 + offsetY,
        drawWidth,
        drawHeight,
      );
      usedArtwork = true;
    }
  } catch {
    usedArtwork = false;
  }
  if (!usedArtwork) {
    ctx.fillStyle = accent;
    ctx.fillRect(art.x, art.y, art.width, art.height);
    drawCardStar(ctx, art.x + art.width / 2, art.y + art.height / 2, 112, darkMaterial ? EPAPER_WHITE : "#151816");
  }
  ctx.restore();

  // Fine decorative lines in the generated masters can disappear after the
  // six-colour conversion. Reinforce only the shared structural grid so every
  // material keeps the same readable hierarchy on the physical display.
  ctx.save();
  ctx.strokeStyle = ink;
  ctx.lineWidth = 2;
  chamferedRectPath(ctx, 18, 18, 492, 756, 18);
  ctx.stroke();
  ctx.strokeStyle = accent;
  ctx.lineWidth = 3;
  chamferedRectPath(ctx, header.x, header.y, header.width, header.height, 10);
  ctx.stroke();
  chamferedRectPath(ctx, art.x - 3, art.y - 3, art.width + 6, art.height + 6, 12);
  ctx.stroke();
  chamferedRectPath(ctx, copy.x, copy.y, copy.width, copy.height, 10);
  ctx.stroke();
  drawCardStar(ctx, 264, 70, 14, accent);
  drawCardStar(ctx, 264, 738, 12, accent);
  ctx.restore();

  ctx.textAlign = "left";
  ctx.fillStyle = ink;
  ctx.font = `900 ${fitText(ctx, card.name, 294, 30, family)}px ${family}`;
  ctx.fillText(card.name, header.x + 28, header.y + 31, 294);
  ctx.font = `800 13px ${family}`;
  ctx.fillText(`${card.type}  ·  ${cardRarityLabels[material]}`, header.x + 28, header.y + 53, 308);
  ctx.textAlign = "right";
  ctx.font = `900 18px ${screenFonts.mono}`;
  ctx.fillText(`LV ${card.level}`, header.x + header.width - 26, header.y + 37);

  ctx.textAlign = "left";
  ctx.fillStyle = ink;
  ctx.font = `900 13px ${family}`;
  ctx.fillText("CARD EFFECT", copy.x + 24, copy.y + 25);
  ctx.fillStyle = accent;
  ctx.fillRect(copy.x + 24, copy.y + 34, 66, 4);
  ctx.fillStyle = ink;
  ctx.font = `700 16px ${family}`;
  cardTextLines(ctx, card.description, copy.width - 48, 3).forEach((line, index) => {
    ctx.fillText(line, copy.x + 24, copy.y + 61 + index * 20, copy.width - 48);
  });

  ctx.textAlign = "center";
  ctx.fillStyle = ink;
  ctx.font = `800 11px ${screenFonts.mono}`;
  ctx.fillText(card.cardId, 264, 704, 132);

  ctx.textAlign = "left";
  ctx.fillStyle = accent;
  ctx.font = `900 16px ${family}`;
  ctx.fillText("ATK", 62, 754);
  ctx.fillStyle = ink;
  ctx.font = `900 25px ${screenFonts.mono}`;
  ctx.fillText(String(card.attack), 105, 758, 112);
  ctx.textAlign = "right";
  ctx.fillStyle = accent;
  ctx.font = `900 16px ${family}`;
  ctx.fillText("DEF", 466, 754);
  ctx.fillStyle = ink;
  ctx.font = `900 25px ${screenFonts.mono}`;
  ctx.fillText(String(card.defense), 424, 758, 112);
  ctx.restore();
  if (quantize) quantizeRegion(ctx, 0, 0, ctx.canvas.width, ctx.canvas.height, "official");
  return usedArtwork;
}

async function drawScreen(
  canvas: HTMLCanvasElement,
  spec: ScreenSpec,
  localImage?: string,
  quantize = true,
) {
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
  if (spec.kind === "map") {
    return drawMapScreen(ctx, spec, quantize);
  }
  if (spec.kind === "card") {
    return drawCardScreen(ctx, spec, localImage, quantize);
  }
  if (spec.table) {
    drawStructuredTable(ctx, spec);
    drawDisplayMeta(ctx, spec, accent, false);
    if (display.border) drawOuterScreenBorder(ctx);
    if (quantize) quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
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
        drawImageCover(ctx, image, area.x, area.y, area.width, area.height);
        if (quantize) quantizeRegion(ctx, area.x, area.y, area.width, area.height, display.renderMode);
      } else {
        drawGeneratedArtwork(ctx, artwork, area.x, area.y, area.width, area.height);
      }
      if (imageOnly) return true;
      if (artwork.layout === "fullscreen") {
        // Text must stay crisp above the photo: quantize the picture first,
        // then draw overlay copy and only dither the copy as final layer.
        drawArtworkCopy(ctx, spec, accent, artwork.layout, Boolean(localImage) || artwork.mode === "web");
        if (quantize && display.renderMode === "official") quantizeTextRegion(ctx, 0, 0, width, height);
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
    if (quantize) quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
    drawQrElement(ctx, spec);
    return false;
  }

  if (spec.kind === "weather" && (display.weather || display.weatherLarge)) {
    if (quantize) quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
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
    if (quantize) quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
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
  if (quantize) quantizeRegion(ctx, 0, 0, width, height, display.renderMode);
  drawQrElement(ctx, spec);

  return false;
}

async function renderScreenToCanvas(canvas: HTMLCanvasElement, spec: ScreenSpec, localImage?: string) {
  const { width, height } = screenDimensions(spec);
  const staging = document.createElement("canvas");
  staging.width = width;
  staging.height = height;
  const usedArtwork = await drawScreen(staging, spec, localImage, false);
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

function canvasForM5PaperColor(source: HTMLCanvasElement, orientation: ScreenOrientation) {
  const target = deviceAdapter("m5-papercolor-c151").renderTarget(orientation);
  const output = document.createElement("canvas");
  output.width = target.width;
  output.height = target.height;
  const context = output.getContext("2d");
  if (!context) throw new Error(tRuntime("无法生成 M5 PaperColor 画面"));
  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = "high";
  context.fillStyle = "#fffefa";
  context.fillRect(0, 0, output.width, output.height);
  context.drawImage(source, 0, 0, output.width, output.height);
  return output;
}

function canvasPng(canvas: HTMLCanvasElement) {
  return new Promise<Blob>((resolve, reject) => {
    canvas.toBlob((blob) => blob ? resolve(blob) : reject(new Error(tRuntime("无法导出设备画面"))), "image/png");
  });
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
    <div className={`mini-screen${landscape ? " landscape" : ""}`} aria-label={`${app.title} ${tRuntime("保存时的画布预览")}`}>
      <canvas ref={thumbnailRef} width={dimensions.width} height={dimensions.height} />
    </div>
  );
}

function templateStudioHref(templateId: string) {
  return `/?template=${encodeURIComponent(templateId)}`;
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
  onShare,
}: {
  app: InkApp;
  onUse: () => void;
  local?: boolean;
  onShare?: () => void;
}) {
  return (
    <article className="app-card">
      <MiniScreen app={app} />
      <div className="app-card-copy">
        <div className="app-card-meta">
          <span>{local ? tRuntime("本机") : `by ${app.author}`}</span>
          <span>{scheduleLabel(app)}</span>
        </div>
        <h3>{app.title}</h3>
        <p>{app.description}</p>
        <div className="app-card-actions">
          {!local && onShare && (
            <button type="button" className="app-card-share" onClick={onShare} aria-label={`${tRuntime("分享模版")}${app.title}`}>
              <span aria-hidden="true">↗</span> {tRuntime("分享模版")}
            </button>
          )}
          <button type="button" className="app-card-use" onClick={onUse} aria-label={`${tRuntime("立即使用")}${app.title}`}>
            <span className="app-card-cta-label"><i>✦</i> {tRuntime("立即使用模版")}</span>
            <span className="app-card-cta-arrow" aria-hidden="true">→</span>
          </button>
        </div>
      </div>
    </article>
  );
}

function DeviceTaskCard({
  task,
  now,
  onRetry,
  onStop,
}: {
  task: DeviceTask;
  now: number;
  onRetry: (taskId: string) => void | Promise<void>;
  onStop: (taskId: string) => void | Promise<void>;
}) {
  const runsOnDevice = task.execution === "device-wifi";
  return (
    <article className={`device-task ${task.status}`}>
      <div className="device-task-heading">
        <div>
          <span className="task-status-label">
            {task.status === "writing" ? tRuntime("正在下发") : task.status === "error" ? tRuntime("等待重试") : runsOnDevice ? tRuntime("设备端运行") : tRuntime("运行中")}
          </span>
          <h3>{task.app.title}</h3>
          <p>{scheduleLabel(task.app)}</p>
        </div>
        <div className="device-task-heading-actions">
          <button
            type="button"
            className="retry-task-button"
            onClick={() => void onRetry(task.id)}
            disabled={task.status === "writing"}
          >
            {tRuntime("立即重试")}
          </button>
          <button type="button" onClick={() => void onStop(task.id)}>{tRuntime("停止")}</button>
        </div>
      </div>
      <div className="task-countdown">
        <span>{runsOnDevice ? tRuntime("任务调度位置") : tRuntime("距离下次刷新")}</span>
        <strong>{runsOnDevice ? tRuntime("设备端") : formatRemaining(task.nextRunAt, now)}</strong>
        <small>{runsOnDevice
          ? tRuntime("设备开机联网后从服务器同步；无需保持浏览器打开")
          : task.nextRunAt ? formatExactTime(task.nextRunAt) : tRuntime("首次写入完成后开始计时")}</small>
      </div>
      <dl className="task-stats">
        <div><dt>{tRuntime("成功")}</dt><dd>{task.successCount}</dd></div>
        <div><dt>{tRuntime("失败")}</dt><dd>{task.failureCount}</dd></div>
        <div><dt>{tRuntime("最近刷新")}</dt><dd>{task.lastRunAt ? formatExactTime(task.lastRunAt) : tRuntime("尚未执行")}</dd></div>
      </dl>
      {task.lastError && (
        <p className="task-error" role="alert"><b>!</b><span>{task.lastError}</span></p>
      )}
      {task.lastCanvas ? (
        <figure className="task-canvas">
          <img src={task.lastCanvas} alt={`${task.app.title} ${tRuntime("最近一次刷新的画面")}`} />
          <figcaption>{tRuntime("最近一次刷新的 Canvas")}</figcaption>
        </figure>
      ) : (
        <div className="task-canvas-empty">{tRuntime("写入完成后，这里会保留最近画面")}</div>
      )}
    </article>
  );
}

const localeShortLabels: Record<Locale, string> = {
  zh: "中",
  en: "EN",
  ja: "日",
};

function LocaleSwitch({
  locale,
  setLocale,
  t,
  compact = false,
  className,
}: {
  locale: Locale;
  setLocale: (next: Locale) => void;
  t: (zh: string) => string;
  compact?: boolean;
  className: string;
}) {
  return (
    <div className={className} role="group" aria-label={t("界面语言")}>
      {localeOptions.map((option) => (
        <button
          key={option.value}
          type="button"
          className={locale === option.value ? "active" : ""}
          onClick={() => setLocale(option.value)}
          aria-pressed={locale === option.value}
          title={option.label}
        >
          {compact ? localeShortLabels[option.value] : option.label}
        </button>
      ))}
    </div>
  );
}

export default function InkStudio() {
  const { locale, setLocale, t } = useI18n();
  const [tab, setTab] = useState<Tab>("studio");
  const [prompt, setPrompt] = useState(starterPrompt);
  const [app, setApp] = useState<InkApp>(starterApp);
  const [localApps, setLocalApps] = useState<InkApp[]>([]);
  const [publicApps, setPublicApps] = useState<InkApp[]>(featuredApps);
  const [generating, setGenerating] = useState(false);
  const [generatorStatus, setGeneratorStatus] = useState<GeneratorStatus>("checking");
  const [generatorModel, setGeneratorModel] = useState("auto");
  const [generatorModels, setGeneratorModels] = useState<string[]>([]);
  const [previewStatus, setPreviewStatus] = useState<PreviewStatus>("ready");
  const [previewScale, setPreviewScale] = useState<35 | 50 | 75 | 100>(50);
  const [artworkCredit, setArtworkCredit] = useState<ArtworkCredit | null>(null);
  const [preferredWeatherCity, setPreferredWeatherCity] = useState(tRuntime("上海"));
  const [calendarPreferences, setCalendarPreferences] = useState<CalendarPreferences>(DEFAULT_CALENDAR_PREFERENCES);
  const [calendarUrlDraft, setCalendarUrlDraft] = useState("");
  const [calendarNotice, setCalendarNotice] = useState<string | null>(null);
  const [mapServiceStatus, setMapServiceStatus] = useState<MapServiceStatus>("idle");
  const [mapServiceMessage, setMapServiceMessage] = useState<string | null>(null);
  const [mapPanPreview, setMapPanPreview] = useState<{ x: number; y: number } | null>(null);
  const [mapZoomPreview, setMapZoomPreview] = useState<number | null>(null);
  const [fontTick, setFontTick] = useState(0);
  const [clockTick, setClockTick] = useState(0);
  const [codeOpen, setCodeOpen] = useState(false);
  const [saveMenuOpen, setSaveMenuOpen] = useState(false);
  const [guideOpen, setGuideOpen] = useState(false);
  const [toast, setToast] = useState<Toast>(null);
  const [deviceName, setDeviceName] = useState<string | null>(null);
  const [devices, setDevices] = useState<DeviceProfile[]>([]);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(false);
  const [expandedDeviceIds, setExpandedDeviceIds] = useState<Set<string>>(() => new Set());
  const [activeDeviceId, setActiveDeviceId] = useState<string | null>(null);
  const [deviceStatus, setDeviceStatus] = useState<"idle" | "ready" | "writing" | "scheduled" | "error">("idle");
  const [progress, setProgress] = useState<TodooProgress | null>(null);
  const [deviceTasks, setDeviceTasks] = useState<DeviceTask[]>([]);
  const [addDeviceOpen, setAddDeviceOpen] = useState(false);
  const [addDeviceStep, setAddDeviceStep] = useState<AddDeviceStep>("family");
  const [selectedDeviceSkuId, setSelectedDeviceSkuId] = useState<DeviceSkuId | null>(null);
  const [productFamily, setProductFamily] = useState<"all" | DeviceFamily>("all");
  const [productBrand, setProductBrand] = useState<"all" | string>("all");
  const [deviceCode, setDeviceCode] = useState("");
  const [deviceFlowBusy, setDeviceFlowBusy] = useState(false);
  const [deviceFlowError, setDeviceFlowError] = useState<string | null>(null);
  const [firmwareProgress, setFirmwareProgress] = useState<FirmwareProgress | null>(null);
  const [firmwareLogs, setFirmwareLogs] = useState<string[]>([]);
  const [firmwareAccessPoint, setFirmwareAccessPoint] = useState<string | null>(null);
  const [firmwareMonitoring, setFirmwareMonitoring] = useState(false);
  const [calibrationDeviceId, setCalibrationDeviceId] = useState<string | null>(null);
  const [calibrationStep, setCalibrationStep] = useState<1 | 2 | 3>(1);
  const [calibrationBusy, setCalibrationBusy] = useState(false);
  const [calibrationError, setCalibrationError] = useState<string | null>(null);
  const [calibrationDraft, setCalibrationDraft] = useState<DeviceColorCalibration | null>(null);
  const [calibrationPhoto, setCalibrationPhoto] = useState<string | null>(null);
  const [secondTick, setSecondTick] = useState(() => Date.now());
  const [bluetoothSupported, setBluetoothSupported] = useState(false);
  const [dragPreview, setDragPreview] = useState<DragPreview | null>(null);
  const [elementSizeDrafts, setElementSizeDrafts] = useState<Partial<Record<ScreenElementKey, string>>>({});
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const calibrationFileInputRef = useRef<HTMLInputElement>(null);
  const previewVersionRef = useRef(0);
  const currentAppRef = useRef(app);
  const deviceProfilesRef = useRef<DeviceProfile[]>([]);
  const activeDeviceIdRef = useRef<string | null>(null);
  const calendarPreferencesRef = useRef(calendarPreferences);
  const driverRef = useRef<TodooCard | null>(null);
  const deviceDriversRef = useRef(new Map<string, TodooCard>());
  const deviceTasksRef = useRef<DeviceTask[]>([]);
  const taskTimersRef = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  const transferLocksRef = useRef(new Set<string>());
  const saveMenuRef = useRef<HTMLDivElement>(null);
  const sharedTemplateRef = useRef<{ sourceId: string; shareId: string } | null>(null);
  const elementDragRef = useRef<{
    element: ScreenElementKey;
    pointerId: number;
    bounds: DOMRect;
  } | null>(null);
  const mapDragRef = useRef<{
    pointerId: number;
    startX: number;
    startY: number;
    bounds: DOMRect;
    map: MapSpec;
  } | null>(null);
  const mapWheelZoomRef = useRef<number | null>(null);
  const mapWheelTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const firmwareStopRef = useRef<(() => void) | null>(null);
  const activeDevice = devices.find((device) => device.id === activeDeviceId) ?? null;
  const previewSourceDimensions = screenDimensions(app.spec);
  const previewDimensions = activeDevice?.skuId === "m5-papercolor-c151"
    ? deviceAdapter(activeDevice.skuId).renderTarget(screenOrientation(app.spec))
    : previewSourceDimensions;

  const showToast = useCallback((message: string, tone: ToastTone = "info") => {
    setToast({ message, tone });
    setTimeout(() => setToast(null), 3400);
  }, []);

  const navigateToTab = useCallback((nextTab: Tab) => {
    setTab(nextTab);
    const url = new URL(window.location.href);
    url.searchParams.delete("app");
    url.searchParams.delete("template");
    if (nextTab === "studio") url.searchParams.delete("view");
    else url.searchParams.set("view", nextTab);
    window.history.pushState(null, "", `${url.pathname}${url.search}${url.hash}`);
  }, []);

  const copyTemplateLink = useCallback(async (templateId: string) => {
    const url = new URL(templateStudioHref(templateId), window.location.origin);
    try {
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(url.toString());
      } else {
        const textarea = document.createElement("textarea");
        textarea.value = url.toString();
        textarea.style.position = "fixed";
        textarea.style.opacity = "0";
        document.body.appendChild(textarea);
        textarea.select();
        const copied = document.execCommand("copy");
        textarea.remove();
        if (!copied) throw new Error("copy unavailable");
      }
      showToast(tRuntime("创作台模版链接已复制"), "success");
    } catch {
      showToast(tRuntime("复制失败，请从浏览器地址栏复制"), "error");
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

  const selectActiveDevice = useCallback((profile: DeviceProfile | null) => {
    const nextId = profile?.id ?? null;
    activeDeviceIdRef.current = nextId;
    setActiveDeviceId(nextId);
    setDeviceName(profile?.name ?? null);
    try {
      if (nextId) localStorage.setItem(ACTIVE_DEVICE_KEY, nextId);
      else localStorage.removeItem(ACTIVE_DEVICE_KEY);
    } catch {
      // The current tab still keeps the explicit target when storage is unavailable.
    }
    if (profile) {
      const driver = deviceDriversRef.current.get(profile.id);
      if (driver) driverRef.current = driver;
    }
  }, []);

  const commitDeviceProfiles = useCallback((updater: (current: DeviceProfile[]) => DeviceProfile[]) => {
    const next = updater(deviceProfilesRef.current);
    deviceProfilesRef.current = next;
    setDevices(next);
    try {
      localStorage.setItem(DEVICE_PROFILES_KEY, JSON.stringify(next));
    } catch {
      // Calibration remains active in this tab if browser storage is unavailable.
    }
    let storedTarget: string | null = null;
    try {
      storedTarget = localStorage.getItem(ACTIVE_DEVICE_KEY);
    } catch {
      // Resolve against the in-memory selection only.
    }
    const resolvedId = resolveDeviceTargetId(next, activeDeviceIdRef.current, storedTarget);
    if (resolvedId !== activeDeviceIdRef.current) {
      selectActiveDevice(next.find((profile) => profile.id === resolvedId) ?? null);
    }
  }, [selectActiveDevice]);

  const applyEsp32Records = useCallback((records: Esp32DeviceRecord[]) => {
    const existingById = new Map(deviceProfilesRef.current.map((profile) => [profile.id, profile]));
    const remoteProfiles = records.map((record) => profileFromEsp32(record, existingById.get(record.id)));
    commitDeviceProfiles((current) => [
      ...current.filter((profile) => profile.family !== "esp32"),
      ...remoteProfiles,
    ].slice(0, 12));
    commitDeviceTasks((current) => {
      const bluetoothTasks = current.filter((task) => task.execution !== "device-wifi");
      const wifiTasks = records.flatMap((record) => record.tasks.map((task): DeviceTask => ({
        id: task.id,
        app: upgradeLegacyApp(task.app),
        deviceId: record.id,
        deviceName: record.name,
        execution: "device-wifi",
        remoteRevision: task.revision,
        renderStrategy: task.renderStrategy,
        status: "scheduled",
        nextRunAt: null,
        lastRunAt: record.lastSeenAt ? new Date(record.lastSeenAt).getTime() : null,
        successCount: record.appliedRevision >= task.revision ? 1 : 0,
        failureCount: 0,
        consecutiveFailures: 0,
        lastError: null,
      })));
      return [...wifiTasks, ...bluetoothTasks];
    });
    return remoteProfiles;
  }, [commitDeviceProfiles, commitDeviceTasks]);

  const refreshEsp32Devices = useCallback(async (notify = false) => {
    try {
      const records = await listEsp32Devices();
      return applyEsp32Records(records);
    } catch (error) {
      if (notify) showToast(error instanceof Error ? error.message : tRuntime("无法刷新 ESP32 设备"), "error");
      return null;
    }
  }, [applyEsp32Records, showToast]);

  const openAddDevice = useCallback(() => {
    firmwareStopRef.current?.();
    firmwareStopRef.current = null;
    setAddDeviceStep("family");
    setSelectedDeviceSkuId(null);
    setDeviceCode("");
    setDeviceFlowBusy(false);
    setDeviceFlowError(null);
    setFirmwareProgress(null);
    setFirmwareLogs([]);
    setFirmwareAccessPoint(null);
    setFirmwareMonitoring(false);
    setAddDeviceOpen(true);
  }, []);

  const closeAddDevice = useCallback(() => {
    if (deviceFlowBusy) return;
    firmwareStopRef.current?.();
    firmwareStopRef.current = null;
    setFirmwareMonitoring(false);
    setAddDeviceOpen(false);
    setDeviceFlowError(null);
  }, [deviceFlowBusy]);

  const rememberDevice = useCallback((driver: TodooCard, device: AuthorizedBluetoothDevice) => {
    const existing = deviceProfilesRef.current.find((item) => item.id === device.id);
    const profile: DeviceProfile = {
      ...existing,
      id: device.id,
      name: device.name ?? existing?.name ?? "TodooCard",
      family: "bluetooth",
      skuId: "todoo-card-3.7",
      colorCorrectionEnabled: existing?.colorCorrectionEnabled !== false,
    };
    const previousDriver = deviceDriversRef.current.get(profile.id);
    if (previousDriver && previousDriver !== driver) previousDriver.disconnect();
    deviceDriversRef.current.set(profile.id, driver);
    driverRef.current = driver;
    commitDeviceProfiles((current) => [profile, ...current.filter((item) => item.id !== profile.id)]);
    selectActiveDevice(profile);
    setDeviceStatus("ready");
    return profile;
  }, [commitDeviceProfiles, selectActiveDevice]);

  const activateDevice = useCallback((profile: DeviceProfile) => {
    selectActiveDevice(profile);
  }, [selectActiveDevice]);

  const openDeviceCenter = useCallback((profile: DeviceProfile) => {
    setExpandedDeviceIds((current) => {
      if (current.has(profile.id)) return current;
      const next = new Set(current);
      next.add(profile.id);
      return next;
    });
    navigateToTab("device");
    window.setTimeout(() => {
      const deviceCard = document.getElementById(`device-card-${profile.id}`);
      deviceCard?.scrollIntoView({ behavior: "smooth", block: "start" });
      deviceCard?.querySelector<HTMLButtonElement>(".device-registry-summary")?.focus({ preventScroll: true });
    }, 0);
  }, [navigateToTab]);

  const toggleSidebar = useCallback(() => {
    setSidebarCollapsed((current) => {
      const next = !current;
      try {
        localStorage.setItem(SIDEBAR_COLLAPSED_KEY, String(next));
      } catch {
        // Continue without persisting the display preference.
      }
      return next;
    });
  }, []);

  const toggleDeviceTasks = useCallback((deviceId: string) => {
    setExpandedDeviceIds((current) => {
      const next = new Set(current);
      if (next.has(deviceId)) next.delete(deviceId);
      else next.add(deviceId);
      return next;
    });
  }, []);

  useEffect(() => {
    currentAppRef.current = app;
  }, [app]);

  useEffect(() => {
    deviceProfilesRef.current = devices;
  }, [devices]);

  useEffect(() => {
    calendarPreferencesRef.current = calendarPreferences;
  }, [calendarPreferences]);

  useEffect(() => {
    let controller: AbortController | null = null;
    const openTemplateInStudio = (selected: InkApp) => {
      const upgraded = upgradeLegacyApp(selected);
      const cloned: InkApp = {
        ...upgraded,
        id: `app-${Date.now()}`,
        author: tRuntime("我"),
        isPublic: false,
        createdAt: new Date().toISOString(),
      };
      setApp(cloned);
      setPrompt(cloned.prompt);
      setElementSizeDrafts({});
      setTab("studio");
      showToast(tRuntime("分享模版已载入创作台"), "success");
    };
    const syncFromLocation = () => {
      controller?.abort();
      const params = new URLSearchParams(window.location.search);
      const legacyAppId = params.get("app")?.trim() || "";
      const templateId = params.get("template")?.trim() || legacyAppId;
      const requestedView = params.get("view");
      if (!templateId) {
        setTab(requestedView === "mine" || requestedView === "explore" || requestedView === "device" || requestedView === "products"
          ? requestedView
          : "studio");
        return;
      }

      setTab("studio");
      if (legacyAppId) {
        const normalizedUrl = new URL(window.location.href);
        normalizedUrl.searchParams.delete("app");
        normalizedUrl.searchParams.delete("view");
        normalizedUrl.searchParams.set("template", legacyAppId);
        window.history.replaceState(null, "", `${normalizedUrl.pathname}${normalizedUrl.search}${normalizedUrl.hash}`);
      }
      controller = new AbortController();
      fetch(`/api/apps?id=${encodeURIComponent(templateId)}`, { signal: controller.signal })
        .then(async (response) => {
          if (!response.ok) throw new Error("not found");
          return (await response.json()) as { app?: InkApp };
        })
        .then((data) => {
          if (!data.app) throw new Error("not found");
          openTemplateInStudio(data.app);
        })
        .catch((error) => {
          if (error instanceof DOMException && error.name === "AbortError") return;
          showToast(tRuntime("这个分享模版暂时无法访问"), "error");
        });
    };
    syncFromLocation();
    window.addEventListener("popstate", syncFromLocation);
    return () => {
      controller?.abort();
      window.removeEventListener("popstate", syncFromLocation);
    };
  }, [showToast]);

  useEffect(() => {
    if (!saveMenuOpen) return;
    const closeMenu = (event: PointerEvent) => {
      if (!saveMenuRef.current?.contains(event.target as Node)) setSaveMenuOpen(false);
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") setSaveMenuOpen(false);
    };
    document.addEventListener("pointerdown", closeMenu);
    window.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeMenu);
      window.removeEventListener("keydown", closeOnEscape);
    };
  }, [saveMenuOpen]);

  useEffect(() => {
    try {
      const stored = JSON.parse(localStorage.getItem(LOCAL_APPS_KEY) ?? "[]") as InkApp[];
      if (Array.isArray(stored)) setLocalApps(stored.map(upgradeLegacyApp));
      const storedDevices = JSON.parse(localStorage.getItem(DEVICE_PROFILES_KEY) ?? "[]") as DeviceProfile[];
      if (Array.isArray(storedDevices)) {
        const normalizedDevices = storedDevices
          .map(normalizeDeviceProfile)
          .filter((device): device is DeviceProfile => Boolean(device))
          .slice(0, 12);
        deviceProfilesRef.current = normalizedDevices;
        setDevices(normalizedDevices);
        const storedTarget = localStorage.getItem(ACTIVE_DEVICE_KEY);
        const resolvedTarget = resolveDeviceTargetId(normalizedDevices, null, storedTarget);
        selectActiveDevice(normalizedDevices.find((device) => device.id === resolvedTarget) ?? null);
      }
      setSidebarCollapsed(localStorage.getItem(SIDEBAR_COLLAPSED_KEY) === "true");
      const storedCity = localStorage.getItem(WEATHER_CITY_KEY)?.trim();
      if (storedCity) setPreferredWeatherCity(storedCity);
      const storedCalendar = JSON.parse(localStorage.getItem(CALENDAR_PREFERENCES_KEY) ?? "null") as (Partial<CalendarPreferences> & { customUrl?: unknown }) | null;
      if (storedCalendar && typeof storedCalendar === "object") {
        const storedSources = Array.isArray(storedCalendar.sources)
          ? storedCalendar.sources
              .filter((source): source is CalendarSourcePreference => Boolean(source && typeof source === "object" && typeof source.url === "string" && source.url.trim()))
              .slice(0, 5)
              .map((source, index) => ({
                id: typeof source.id === "string" && source.id ? source.id : `ical-${index + 1}`,
                name: typeof source.name === "string" && source.name.trim()
                  ? source.name.trim().slice(0, 24)
                  : calendarSourceName(source.url, index),
                url: source.url.trim(),
                enabled: source.enabled !== false,
              }))
          : [];
        const legacyUrl = typeof storedCalendar.customUrl === "string" ? storedCalendar.customUrl.trim() : "";
        const preferences: CalendarPreferences = {
          sources: storedSources.length
            ? storedSources
            : legacyUrl
              ? [{ id: "ical-legacy", name: calendarSourceName(legacyUrl, 0), url: legacyUrl, enabled: true }]
              : [],
          chinaHolidays: storedCalendar.chinaHolidays === true,
          lunar: storedCalendar.lunar === true,
        };
        setCalendarPreferences(preferences);
        localStorage.setItem(CALENDAR_PREFERENCES_KEY, JSON.stringify(preferences));
      }
    } catch {
      localStorage.removeItem(LOCAL_APPS_KEY);
    }
  }, [selectActiveDevice]);

  useEffect(() => {
    const sample = tRuntime("今日天气 12:34 专注当下 圆润手写");
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
    if (!calibrationDeviceId) return;
    const previousOverflow = document.body.style.overflow;
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !calibrationBusy) setCalibrationDeviceId(null);
    };
    document.body.style.overflow = "hidden";
    window.addEventListener("keydown", closeOnEscape);
    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener("keydown", closeOnEscape);
    };
  }, [calibrationBusy, calibrationDeviceId]);

  useEffect(() => {
    if (!addDeviceOpen) return;
    const previousOverflow = document.body.style.overflow;
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !deviceFlowBusy) setAddDeviceOpen(false);
    };
    document.body.style.overflow = "hidden";
    window.addEventListener("keydown", closeOnEscape);
    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener("keydown", closeOnEscape);
    };
  }, [addDeviceOpen, deviceFlowBusy]);

  useEffect(() => {
    void refreshEsp32Devices();
    const interval = window.setInterval(() => void refreshEsp32Devices(), 15_000);
    return () => window.clearInterval(interval);
  }, [refreshEsp32Devices]);

  useEffect(() => {
    if (app.spec.kind !== "map") return;
    const controller = new AbortController();
    const checkMapService = async () => {
      await Promise.resolve();
      setMapServiceStatus("checking");
      try {
        const response = await fetch("/api/map?mode=status", { signal: controller.signal, cache: "no-store" });
        const payload = await response.json() as MapResolvePayload;
        if (!response.ok || payload.configured !== true) throw new Error(payload.error || tRuntime("地图服务未配置"));
        setMapServiceStatus("ready");
        setMapServiceMessage(null);
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        const message = error instanceof Error ? error.message : tRuntime("地图服务暂时不可用");
        setMapServiceStatus(message.includes("BAIDU_MAP_AK") || message.includes(tRuntime("未配置")) ? "missing" : "error");
        setMapServiceMessage(message);
      }
    };
    void checkMapService();
    return () => controller.abort();
  }, [app.spec.kind]);

  useEffect(() => () => {
    if (mapWheelTimerRef.current) clearTimeout(mapWheelTimerRef.current);
  }, []);

  useEffect(() => {
    fetch("/api/generate")
      .then(async (response) => {
        if (!response.ok) throw new Error("generator unavailable");
        return (await response.json()) as { configured?: boolean; model?: string; models?: string[] };
      })
      .then((data) => {
        setGeneratorStatus(data.configured ? "online" : "local");
        const models = Array.isArray(data.models)
          ? [...new Set(data.models.filter((model) => typeof model === "string" && model.trim()).map((model) => model.trim()))]
          : [];
        const defaultModel = data.model?.trim() || "auto";
        const availableModels = defaultModel === "auto" || models.includes(defaultModel)
          ? models
          : [defaultModel, ...models];
        setGeneratorModels(availableModels);
        const storedModel = localStorage.getItem(GENERATOR_MODEL_KEY)?.trim() || "";
        setGeneratorModel(storedModel === "auto" || availableModels.includes(storedModel) ? storedModel : defaultModel);
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
    setBluetoothSupported(
      Boolean((navigator as Navigator & { bluetooth?: unknown }).bluetooth && globalThis.isSecureContext),
    );
    const driver = new TodooCard(setProgress);
    driverRef.current = driver;
    driver
      .listAuthorizedDevices()
      .then((authorizedDevices) => {
        if (!authorizedDevices.length) return;
        const profiles = authorizedDevices.map((device, index) => {
          const deviceDriver = index === 0 ? driver : new TodooCard(setProgress);
          deviceDriver.useAuthorizedDevice(device);
          deviceDriversRef.current.set(device.id, deviceDriver);
          const existing = deviceProfilesRef.current.find((profile) => profile.id === device.id);
          return {
            ...existing,
            id: device.id,
            name: device.name ?? existing?.name ?? "TodooCard",
            family: "bluetooth",
            skuId: "todoo-card-3.7",
            colorCorrectionEnabled: existing?.colorCorrectionEnabled !== false,
          } satisfies DeviceProfile;
        });
        driverRef.current = deviceDriversRef.current.get(profiles[0].id) ?? driver;
        commitDeviceProfiles((current) => {
          const authorizedIds = new Set(profiles.map((profile) => profile.id));
          return [...profiles, ...current.filter((profile) => !authorizedIds.has(profile.id))].slice(0, 12);
        });
        setDeviceStatus("ready");
      })
      .catch(() => undefined);
    return () => {
      const knownDrivers = new Set(deviceDriversRef.current.values());
      knownDrivers.add(driver);
      knownDrivers.forEach((knownDriver) => knownDriver.disconnect());
      deviceDriversRef.current.clear();
    };
  }, [commitDeviceProfiles]);

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
    const sourceDimensions = screenDimensions(app.spec);
    staging.width = sourceDimensions.width;
    staging.height = sourceDimensions.height;
    const hasArtwork = Boolean(app.spec.artwork || app.localImage || app.spec.kind === "map");
    setPreviewStatus(hasArtwork ? "loading" : "ready");
    const creditKey = app.spec.artwork?.mode === "web" ? artworkUrl(app.spec.artwork, screenOrientation(app.spec)) : null;
    setArtworkCredit(null);
    resolveRuntimeScreen(app, new Date(), calendarPreferences, setCalendarNotice).then((runtimeSpec) => drawScreen(staging, runtimeSpec, app.localImage, false)).then((usedArtwork) => {
      if (version !== previewVersionRef.current) return;
      const rendered = activeDevice?.skuId === "m5-papercolor-c151"
        ? canvasForM5PaperColor(staging, screenOrientation(runtimeSpec))
        : staging;
      if (canvas.width !== previewDimensions.width || canvas.height !== previewDimensions.height) {
        canvas.width = previewDimensions.width;
        canvas.height = previewDimensions.height;
      }
      const context = canvas.getContext("2d");
      if (!context) return;
      context.clearRect(0, 0, canvas.width, canvas.height);
      context.drawImage(rendered, 0, 0, canvas.width, canvas.height);
      setPreviewStatus(hasArtwork && !usedArtwork ? "fallback" : "ready");
      setArtworkCredit(usedArtwork && creditKey ? artworkCreditCache.get(creditKey) ?? null : null);
    });
  }, [activeDevice?.skuId, app.spec, app.localImage, app.prompt, clockTick, fontTick, calendarPreferences, previewDimensions.height, previewDimensions.width]);

  const attachLocalImage = async (file?: File) => {
    if (!file) return;
    try {
      const localImage = await prepareLocalImage(file);
      setApp((current) => ({ ...current, localImage }));
      showToast(tRuntime("图片已贴入，生成后会自动参与排版"), "success");
    } catch (error) {
      showToast(error instanceof Error ? error.message : tRuntime("图片处理失败"), "error");
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
      showToast(tRuntime("先描述你想让屏幕显示什么"), "error");
      return;
    }
    setGenerating(true);
    try {
      const response = await fetch("/api/generate", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ prompt, model: generatorModel }),
      });
      const result = (await response.json()) as {
        app?: InkApp;
        mode?: "llm" | "local";
        model?: string | null;
        warning?: string;
        error?: string;
      };
      if (!response.ok) throw new Error(result.error || tRuntime("生成服务暂时不可用"));
      if (!result.app) throw new Error(tRuntime("生成结果不完整"));
      setApp({ ...applyPreferredCityToGeneratedApp(result.app, prompt, preferredWeatherCity), localImage: app.localImage });
      if (result.mode === "llm") {
        setGeneratorStatus("online");
        showToast(`已由 ${result.model || "在线模型"} 生成应用`, "success");
      } else {
        setGeneratorStatus(generatorModels.length ? "online" : "local");
        showToast(result.warning || tRuntime("已使用本地模板生成"), "info");
      }
    } catch (error) {
      setApp({ ...applyPreferredCityToGeneratedApp(generateInkApp(prompt), prompt, preferredWeatherCity), localImage: app.localImage });
      setGeneratorStatus(generatorModels.length ? "online" : "local");
      showToast(error instanceof Error ? `${error.message}，${t("已使用本地模板")}` : tRuntime("已使用本地模板"), "info");
    } finally {
      setGenerating(false);
    }
  };

  const updateSchedule = (scheduleMode: ScheduleMode) => {
    setApp((current) => ({ ...current, scheduleMode }));
  };

  const addCalendarSource = () => {
    const url = calendarUrlDraft.trim();
    if (!url) {
      showToast(tRuntime("先填写 iCal 地址"), "error");
      return;
    }
    if (!/^(?:https|webcal):\/\//i.test(url)) {
      showToast(tRuntime("请填写 HTTPS 或 webcal 开头的 iCal 地址"), "error");
      return;
    }
    const normalizedUrl = url.replace(/^webcal:/i, "https:");
    if (calendarPreferences.sources.some((source) => source.url.replace(/^webcal:/i, "https:") === normalizedUrl)) {
      showToast(tRuntime("这个日历已经添加过了"), "info");
      return;
    }
    if (calendarPreferences.sources.length >= 5) {
      showToast(tRuntime("最多可添加 5 个个人日历"), "error");
      return;
    }
    const source: CalendarSourcePreference = {
      id: `ical-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
      name: calendarSourceName(url, calendarPreferences.sources.length),
      url,
      enabled: true,
    };
    updateCalendarPreferences({ sources: [...calendarPreferences.sources, source] });
    setCalendarUrlDraft("");
    setCalendarNotice(null);
    showToast(tRuntime("日历已添加，正在合并日程"), "success");
  };

  const updateCalendarSource = (id: string, patch: Partial<CalendarSourcePreference>) => {
    updateCalendarPreferences({
      sources: calendarPreferences.sources.map((source) => source.id === id ? { ...source, ...patch } : source),
    });
    setCalendarNotice(null);
  };

  const removeCalendarSource = (id: string) => {
    const source = calendarPreferences.sources.find((item) => item.id === id);
    updateCalendarPreferences({ sources: calendarPreferences.sources.filter((item) => item.id !== id) });
    setCalendarNotice(null);
    showToast(`${source?.name || t("个人日历")}${t("已移除")}`, "success");
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
          footer: nextDisplay.quote && !current.spec.footer ? tRuntime("今天也要保持好心情") : current.spec.footer,
          display: nextDisplay,
        },
      };
    });
  };

  const updateMap = (patch: Partial<MapSpec>) => {
    setApp((current) => {
      if (current.spec.kind !== "map" || !current.spec.map) return current;
      const nextMap = {
        ...current.spec.map,
        ...patch,
        zoomLevel: patch.zoomLevel === undefined
          ? current.spec.map.zoomLevel
          : Math.min(19, Math.max(3, Math.round(patch.zoomLevel))),
      };
      return {
        ...current,
        spec: { ...current.spec, map: nextMap },
      };
    });
  };

  const updateCard = (patch: Partial<CardSpec>) => {
    setApp((current) => {
      if (current.spec.kind !== "card" || !current.spec.card) return current;
      const card = {
        ...current.spec.card,
        ...patch,
        level: patch.level === undefined ? current.spec.card.level : Math.min(12, Math.max(1, Math.round(patch.level))),
        attack: patch.attack === undefined ? current.spec.card.attack : Math.min(9999, Math.max(0, Math.round(patch.attack))),
        defense: patch.defense === undefined ? current.spec.card.defense : Math.min(9999, Math.max(0, Math.round(patch.defense))),
        subjectScale: patch.subjectScale === undefined ? current.spec.card.subjectScale : Math.min(2.2, Math.max(0.7, patch.subjectScale)),
        subjectX: patch.subjectX === undefined ? current.spec.card.subjectX : Math.min(100, Math.max(-100, Math.round(patch.subjectX))),
        subjectY: patch.subjectY === undefined ? current.spec.card.subjectY : Math.min(100, Math.max(-100, Math.round(patch.subjectY))),
      };
      const accent = card.rarity === "gold" || card.rarity === "holo"
        ? "yellow" as const
        : card.rarity === "silver"
          ? "blue" as const
          : "green" as const;
      return {
        ...current,
        title: card.name,
        description: `${card.type} · ${card.level} ${t("星")} · ATK ${card.attack}`,
        spec: {
          ...current.spec,
          orientation: "portrait",
          title: card.name,
          value: String(card.attack),
          detail: card.description,
          footer: card.cardId,
          accent,
          card,
        },
      };
    });
  };

  const commitMapPan = (map: MapSpec, horizontalDeltaRatio: number, verticalDeltaRatio: number) => {
    const point = panMapPoint(map, screenOrientation(currentAppRef.current.spec), horizontalDeltaRatio, verticalDeltaRatio);
    if (!point) return;
    updateMap({
      ...point,
      locationMode: "picker",
      address: tRuntime("已调整位置"),
      approximate: false,
      statusMessage: tRuntime("已通过预览拖拽调整位置"),
    });
  };

  const handleMapPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    const map = currentAppRef.current.spec.map;
    if (!map || typeof map.latitude !== "number" || typeof map.longitude !== "number") {
      showToast(tRuntime("先输入地点或使用浏览器定位，再拖动地图"), "info");
      return;
    }
    event.currentTarget.setPointerCapture(event.pointerId);
    mapDragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      bounds: event.currentTarget.getBoundingClientRect(),
      map,
    };
    setMapPanPreview({ x: 0, y: 0 });
  };

  const handleMapPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = mapDragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const previewRatio = previewScale / 50;
    setMapPanPreview({
      x: (event.clientX - drag.startX) / previewRatio,
      y: (event.clientY - drag.startY) / previewRatio,
    });
  };

  const finishMapDrag = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = mapDragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const deltaX = event.clientX - drag.startX;
    const deltaY = event.clientY - drag.startY;
    mapDragRef.current = null;
    setMapPanPreview(null);
    if (Math.hypot(deltaX, deltaY) < 4) return;
    commitMapPan(drag.map, deltaX / drag.bounds.width, deltaY / drag.bounds.height);
  };

  const handleMapWheel = (event: ReactWheelEvent<HTMLDivElement>) => {
    event.preventDefault();
    const map = currentAppRef.current.spec.map;
    if (!map) return;
    const current = mapWheelZoomRef.current ?? map.zoomLevel;
    const next = Math.min(19, Math.max(3, current + (event.deltaY < 0 ? 1 : -1)));
    mapWheelZoomRef.current = next;
    setMapZoomPreview(next);
    if (mapWheelTimerRef.current) clearTimeout(mapWheelTimerRef.current);
    mapWheelTimerRef.current = setTimeout(() => {
      updateMap({ zoomLevel: mapWheelZoomRef.current ?? next });
      mapWheelZoomRef.current = null;
      setMapZoomPreview(null);
      mapWheelTimerRef.current = null;
    }, 180);
  };

  const handleMapKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    const map = currentAppRef.current.spec.map;
    if (!map) return;
    const panStep = event.shiftKey ? 0.18 : 0.08;
    const direction = {
      ArrowLeft: [panStep, 0],
      ArrowRight: [-panStep, 0],
      ArrowUp: [0, panStep],
      ArrowDown: [0, -panStep],
    }[event.key] as [number, number] | undefined;
    if (direction) {
      event.preventDefault();
      commitMapPan(map, direction[0], direction[1]);
      return;
    }
    if (["+", "=", "-", "_"].includes(event.key)) {
      event.preventDefault();
      updateMap({ zoomLevel: map.zoomLevel + (["+", "="].includes(event.key) ? 1 : -1) });
    }
  };

  const locateWithBrowser = () => {
    if (!navigator.geolocation) {
      showToast(tRuntime("当前浏览器不支持位置授权"), "error");
      return;
    }
    showToast(tRuntime("正在请求浏览器位置授权"), "info");
    navigator.geolocation.getCurrentPosition((position) => {
      updateMap({
        locationMode: "browser",
        latitude: position.coords.latitude,
        longitude: position.coords.longitude,
        coordinateType: "wgs84ll",
        query: "",
        address: undefined,
        approximate: false,
        statusMessage: tRuntime("浏览器位置已获取，正在转换地图坐标"),
      });
      showToast(tRuntime("已获取位置，地图正在刷新"), "success");
    }, (error) => {
      const message = error.code === error.PERMISSION_DENIED
        ? tRuntime("位置权限被拒绝，可输入地点或坐标")
        : tRuntime("暂时无法获取浏览器位置，可输入地点或使用 IP 粗定位");
      showToast(message, "error");
    }, {
      enableHighAccuracy: true,
      timeout: 12_000,
      maximumAge: 60_000,
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
    showToast(tRuntime("正在按原主题重新生成图片素材"), "info");
  };

  const hasWifiCredentials = (candidate: InkApp) => {
    const currentDisplay = displaySettings(candidate.spec, Boolean(candidate.localImage));
    return currentDisplay.qr && (
      currentDisplay.qrMode === "wifi"
      || /^WIFI:/i.test(currentDisplay.qrText.trim())
    );
  };

  const publishTemplateRecord = async (candidate: InkApp, listed: boolean) => {
    const response = await fetch("/api/apps", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ ...candidate, localImage: undefined, listed }),
    });
    if (!response.ok) throw new Error("publish failed");
    const data = (await response.json()) as { app: InkApp };
    return upgradeLegacyApp(data.app);
  };

  const saveApp = async (publishToMarket: boolean) => {
    const containsWifiAccess = hasWifiCredentials(app);
    const saved = {
      ...app,
      id: app.id.startsWith("starter") ? `app-${Date.now()}` : app.id,
      isPublic: publishToMarket && !containsWifiAccess,
    };
    const next = [saved, ...localApps.filter((item) => item.id !== saved.id)].slice(0, 30);
    setApp(saved);
    setLocalApps(next);
    localStorage.setItem(LOCAL_APPS_KEY, JSON.stringify(next));

    if (publishToMarket && containsWifiAccess) {
      showToast(tRuntime("已保存到本机；Wi-Fi 二维码不会公开，避免泄露网络密码"), "info");
    } else if (publishToMarket) {
      try {
        const published = await publishTemplateRecord(saved, true);
        setPublicApps((items) => [published, ...items.filter((item) => item.id !== published.id)]);
        showToast(tRuntime("已保存到本机，并发布到模板市场"), "success");
      } catch {
        showToast(tRuntime("已保存到本机；公开发布暂时不可用"), "info");
      }
    } else {
      showToast(tRuntime("模版已保存在这台设备上"), "success");
    }
  };

  const shareCurrentTemplate = async () => {
    if (hasWifiCredentials(app)) {
      showToast(tRuntime("请先移除 Wi-Fi 凭据，再分享模版链接"), "error");
      return;
    }
    try {
      const existingShare = sharedTemplateRef.current;
      const shareId = existingShare?.sourceId === app.id
        ? existingShare.shareId
        : `share-${crypto.randomUUID()}`;
      sharedTemplateRef.current = { sourceId: app.id, shareId };
      const shared = await publishTemplateRecord({ ...app, id: shareId, isPublic: false }, false);
      await copyTemplateLink(shared.id);
    } catch {
      showToast(tRuntime("分享链接生成失败，请稍后重试"), "error");
    }
  };

  const shareMarketTemplate = async (selected: InkApp) => {
    try {
      const existing = await fetch(`/api/apps?id=${encodeURIComponent(selected.id)}`, { cache: "no-store" });
      if (existing.status === 404) await publishTemplateRecord({ ...selected, isPublic: false }, false);
      else if (!existing.ok) throw new Error("template unavailable");
      await copyTemplateLink(selected.id);
    } catch {
      showToast(tRuntime("分享链接生成失败，请稍后重试"), "error");
    }
  };

  const copyAppToStudio = (selected: InkApp) => {
    const upgraded = upgradeLegacyApp(selected);
    const cloned = {
      ...upgraded,
      id: `app-${Date.now()}`,
      author: tRuntime("我"),
      isPublic: false,
      createdAt: new Date().toISOString(),
    };
    setApp(cloned);
    setPrompt(cloned.prompt);
    setElementSizeDrafts({});
    navigateToTab("studio");
    showToast(tRuntime("已复制到创作台，可以继续调整"), "success");
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

  const selectNewBluetoothDevice = useCallback(async () => {
    const driver = new TodooCard(setProgress);
    if (!driver.supported) {
      showToast(tRuntime("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站"), "error");
      return null;
    }
    try {
      const device = await driver.requestDevice();
      const profile = rememberDevice(driver, device);
      showToast(`${t("已添加设备")} ${profile.name}${t("，尚未执行写入")}`, "success");
      setAddDeviceOpen(false);
      return profile;
    } catch (error) {
      driver.disconnect();
      showToast(error instanceof Error ? error.message : tRuntime("没有选择设备"), "error");
      return null;
    }
  }, [rememberDevice, showToast]);

  const bindEsp32Device = useCallback(async () => {
    if (!/^\d{6}$/.test(deviceCode)) {
      setDeviceFlowError(tRuntime("请输入设备屏幕上的六位设备码"));
      return;
    }
    setDeviceFlowBusy(true);
    setDeviceFlowError(null);
    try {
      const claimed = await claimEsp32Device(deviceCode);
      const profiles = await refreshEsp32Devices();
      const profile = profiles?.find((item) => item.id === claimed.id) ?? profileFromEsp32(claimed);
      activateDevice(profile);
      setDeviceStatus(profile.online ? "ready" : "idle");
      setExpandedDeviceIds((current) => new Set(current).add(profile.id));
      firmwareStopRef.current?.();
      firmwareStopRef.current = null;
      setFirmwareMonitoring(false);
      setAddDeviceOpen(false);
      showToast(`已绑定 ${profile.name}，设备端计划会自动同步`, "success");
    } catch (error) {
      setDeviceFlowError(error instanceof Error ? error.message : tRuntime("设备绑定失败"));
    } finally {
      setDeviceFlowBusy(false);
    }
  }, [activateDevice, deviceCode, refreshEsp32Devices, showToast]);

  const flashEsp32Device = useCallback(async () => {
    firmwareStopRef.current?.();
    firmwareStopRef.current = null;
    setDeviceFlowBusy(true);
    setDeviceFlowError(null);
    setFirmwareProgress(null);
    setFirmwareLogs([]);
    setFirmwareAccessPoint(null);
    setFirmwareMonitoring(false);
    setDeviceCode("");
    const onDeviceEvent = (event: FirmwareDeviceEvent) => {
      setFirmwareLogs((current) => [...current, event.message].slice(-120));
      if (event.accessPoint) setFirmwareAccessPoint(event.accessPoint);
      if (event.pairingCode) setDeviceCode(event.pairingCode);
    };
    try {
      const session = await flashM5PaperColor(setFirmwareProgress, onDeviceEvent);
      firmwareStopRef.current = session.stopMonitoring;
      setFirmwareMonitoring(true);
      setAddDeviceStep("flash-complete");
      showToast(tRuntime("M5 PaperColor 瘦客户端已写入"), "success");
      void session.monitor.then((result) => {
        if (result.accessPoint) setFirmwareAccessPoint(result.accessPoint);
        if (result.pairingCode) setDeviceCode(result.pairingCode);
      }).catch((error) => {
        setFirmwareLogs((current) => [
          ...current,
          error instanceof Error ? error.message : tRuntime("设备调试串口意外断开"),
        ].slice(-120));
      }).finally(() => {
        setFirmwareMonitoring(false);
        firmwareStopRef.current = null;
      });
    } catch (error) {
      setDeviceFlowError(error instanceof Error ? error.message : tRuntime("设备刷机失败"));
    } finally {
      setDeviceFlowBusy(false);
    }
  }, [showToast]);

  const openDeviceCalibration = useCallback((deviceId: string) => {
    setCalibrationDeviceId(deviceId);
    setCalibrationStep(1);
    setCalibrationBusy(false);
    setCalibrationError(null);
    setCalibrationDraft(null);
    setCalibrationPhoto(null);
  }, []);

  const closeDeviceCalibration = useCallback(() => {
    if (calibrationBusy) return;
    setCalibrationDeviceId(null);
    setCalibrationError(null);
    setCalibrationDraft(null);
    setCalibrationPhoto(null);
  }, [calibrationBusy]);

  const calibrationDriverFor = useCallback(async (device: DeviceProfile) => {
    const existing = deviceDriversRef.current.get(device.id);
    if (existing) return existing;
    const driver = new TodooCard(setProgress);
    if (!driver.supported) throw new Error(tRuntime("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站"));
    const authorized = await driver.listAuthorizedDevices();
    const restored = authorized.find((item) => item.id === device.id);
    if (restored) {
      driver.useAuthorizedDevice(restored);
      rememberDevice(driver, restored);
      return driver;
    }
    const selected = await driver.requestDevice();
    if (selected.id !== device.id) {
      driver.disconnect();
      throw new Error(`${t("请选择设备")}“${device.name}”；${t("刚才选择的是另一台设备")}`);
    }
    rememberDevice(driver, selected);
    return driver;
  }, [rememberDevice]);

  const writeCalibrationCard = useCallback(async () => {
    const device = deviceProfilesRef.current.find((item) => item.id === calibrationDeviceId);
    if (!device) return;
    if (transferLocksRef.current.has(device.id)) {
      setCalibrationError(tRuntime("这台设备正在写入，请等待当前任务完成后再校色"));
      return;
    }
    setCalibrationBusy(true);
    setCalibrationError(null);
    transferLocksRef.current.add(device.id);
    setDeviceStatus("writing");
    try {
      const driver = await calibrationDriverFor(device);
      await driver.writeCalibration(true);
      setCalibrationStep(2);
      setDeviceStatus(deviceTasksRef.current.some((task) => task.deviceId === device.id) ? "scheduled" : "ready");
      showToast(tRuntime("标准六色色卡已写入；颜色稳定后请拍照"), "success");
    } catch (error) {
      const message = error instanceof Error ? error.message : tRuntime("标准色卡写入失败");
      setCalibrationError(message);
      setDeviceStatus("error");
    } finally {
      transferLocksRef.current.delete(device.id);
      setCalibrationBusy(false);
    }
  }, [calibrationDeviceId, calibrationDriverFor, showToast]);

  const handleCalibrationPhoto = useCallback(async (file: File | undefined) => {
    if (!file) return;
    setCalibrationBusy(true);
    setCalibrationError(null);
    try {
      const analyzed = await analyzeCalibrationPhoto(file);
      setCalibrationDraft(analyzed.profile);
      setCalibrationPhoto(analyzed.preview);
      setCalibrationStep(3);
    } catch (error) {
      setCalibrationError(error instanceof Error ? error.message : tRuntime("照片分析失败，请重新拍摄"));
    } finally {
      setCalibrationBusy(false);
      if (calibrationFileInputRef.current) calibrationFileInputRef.current.value = "";
    }
  }, []);

  const saveDeviceCalibration = useCallback(() => {
    if (!calibrationDeviceId || !calibrationDraft) return;
    commitDeviceProfiles((current) => current.map((device) => device.id === calibrationDeviceId
      ? { ...device, colorCorrectionEnabled: true, calibration: calibrationDraft }
      : device));
    setCalibrationDeviceId(null);
    setCalibrationDraft(null);
    setCalibrationPhoto(null);
    showToast(tRuntime("设备校色 Profile 已保存并启用"), "success");
  }, [calibrationDeviceId, calibrationDraft, commitDeviceProfiles, showToast]);

  const toggleDeviceColorCorrection = useCallback((deviceId: string, enabled: boolean) => {
    commitDeviceProfiles((current) => current.map((device) => device.id === deviceId
      ? { ...device, colorCorrectionEnabled: enabled }
      : device));
    showToast(enabled ? tRuntime("已开启设备色差纠正") : tRuntime("已关闭设备色差纠正"), "info");
  }, [commitDeviceProfiles, showToast]);

  const stopDeviceTask = useCallback(async (taskId: string, notify = true) => {
    const task = deviceTasksRef.current.find((item) => item.id === taskId);
    if (task?.execution === "device-wifi") {
      commitDeviceTasks((current) => current.map((item) => item.id === taskId
        ? { ...item, status: "writing", lastError: null }
        : item));
      try {
        await deleteEsp32Task(task.deviceId, task.id);
        await refreshEsp32Devices();
        if (notify) showToast(tRuntime("任务已从服务器删除；在线设备会在下一次同步时移除"), "success");
      } catch (error) {
        commitDeviceTasks((current) => current.map((item) => item.id === taskId
          ? { ...item, status: "error", lastError: error instanceof Error ? error.message : tRuntime("删除任务失败") }
          : item));
        if (notify) showToast(error instanceof Error ? error.message : tRuntime("删除任务失败"), "error");
      }
      return;
    }
    const timer = taskTimersRef.current.get(taskId);
    if (timer) clearTimeout(timer);
    taskTimersRef.current.delete(taskId);
    commitDeviceTasks((current) => current.filter((task) => task.id !== taskId));
    if (deviceTasksRef.current.length <= 1) setDeviceStatus(deviceName ? "ready" : "idle");
    if (notify) showToast(tRuntime("定时任务已停止"), "info");
  }, [commitDeviceTasks, deviceName, refreshEsp32Devices, showToast]);

  const prepareEsp32Frame = useCallback(async (targetApp: InkApp) => {
    const editingCurrent = targetApp.id === currentAppRef.current.id;
    const currentCanvas = editingCurrent ? canvasRef.current : null;
    if (editingCurrent && previewStatus === "loading") throw new Error(tRuntime("图片素材仍在加载，请稍候重试"));
    const outputCanvas = currentCanvas ?? document.createElement("canvas");
    if (!currentCanvas) {
      const dimensions = screenDimensions(targetApp.spec);
      outputCanvas.width = dimensions.width;
      outputCanvas.height = dimensions.height;
      const runtimeSpec = await resolveRuntimeScreen(targetApp, new Date(), calendarPreferencesRef.current);
      await renderScreenToCanvas(outputCanvas, runtimeSpec, targetApp.localImage);
    }
    const deviceCanvas = canvasForM5PaperColor(outputCanvas, screenOrientation(targetApp.spec));
    return {
      blob: await canvasPng(deviceCanvas),
      preview: deviceCanvas.toDataURL("image/png"),
    };
  }, [previewStatus]);

  const publishTaskToEsp32 = useCallback(async (profile: DeviceProfile, targetApp: InkApp) => {
    setDeviceStatus("writing");
    const frame = await prepareEsp32Frame(targetApp);
    const renderStrategy = paperColorStrategyForScreenMode(
      displaySettings(targetApp.spec, Boolean(targetApp.localImage)).renderMode,
    );
    await publishEsp32Task(profile.id, targetApp, frame.blob, renderStrategy);
    const profiles = await refreshEsp32Devices();
    const refreshed = profiles?.find((item) => item.id === profile.id) ?? profile;
    setDeviceStatus("scheduled");
    activateDevice(profile);
    return { frame, refreshed };
  }, [activateDevice, prepareEsp32Frame, refreshEsp32Devices]);

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
      const reason = tRuntime("找不到这台设备的授权，请重新选择设备");
      markTaskFailure(reason);
      return false;
    }
    const editingThisApp = targetApp.id === currentAppRef.current.id;
    const sourceDimensions = screenDimensions(targetApp.spec);
    const reuseCurrentPreview = Boolean(
      options.reusePreview && editingThisApp && canvas &&
      canvas.width === sourceDimensions.width && canvas.height === sourceDimensions.height,
    );
    const renderInPreview = editingThisApp && !taskId && Boolean(canvas) && !reuseCurrentPreview;
    if (reuseCurrentPreview && previewStatus === "loading") {
      const reason = tRuntime("图片素材仍在加载，请稍候重试");
      markTaskFailure(reason);
      showToast(reason, "info");
      return false;
    }
    if (transferLocksRef.current.has(deviceId)) {
      const reason = tRuntime("这台设备正在执行另一个写入任务，将在下一轮重试");
      markTaskFailure(reason);
      showToast(reason, "info");
      return false;
    }
    if (document.visibilityState !== "visible") {
      const reason = tRuntime("页面在后台，等待重新打开后重试");
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
      const renderMode = displaySettings(transferApp.spec, Boolean(transferApp.localImage)).renderMode;
      const deviceCanvas = canvasForDevice(outputCanvas, screenOrientation(transferApp.spec));
      await writeWithBluetoothRecovery({
        forceReconnect: options.reconnect,
        reconnect: () => driver.reconnect(),
        write: () => driver.writeCanvas(deviceCanvas, true, renderMode === "inkloop-text"
          ? { renderProfile: "verified", dither: false }
          : { renderProfile: "skillT3", dither: true }),
        onRecovering: () => {
          if (!taskId) setProgress({ phase: "connecting", percent: 2, message: tRuntime("连接中断，正在自动重连…") });
          if (taskId) {
            commitDeviceTasks((current) => current.map((task) => task.id === taskId
              ? { ...task, status: "writing", lastError: tRuntime("连接中断，正在自动重连…") }
              : task));
          }
        },
      });
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
      if (!taskId) showToast(tRuntime("帧已发送，墨水屏可能还会显色几分钟"), "success");
      return true;
    } catch (error) {
      const rawMessage = error instanceof Error ? error.message : tRuntime("写入失败");
      const recoverable = isRecoverableBluetoothError(error);
      const message = recoverable
        ? taskId
          ? tRuntime("设备暂时离线，自动重连未成功；稍后将继续后台重试")
          : tRuntime("设备暂时离线，自动重连未成功；请靠近设备后重试")
        : rawMessage;
      driver.disconnect();
      markTaskFailure(message, lastCanvas);
      setDeviceStatus("error");
      if (!taskId) showToast(message, "error");
      return false;
    } finally {
      transferLocksRef.current.delete(deviceId);
    }
  }, [commitDeviceTasks, previewStatus, showToast]);

  function scheduleDeviceTask(taskId: string, retryDelay?: number) {
    const existing = taskTimersRef.current.get(taskId);
    if (existing) clearTimeout(existing);
    const task = deviceTasksRef.current.find((item) => item.id === taskId);
    if (!task || task.execution === "device-wifi") return;
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
    if (task.execution === "device-wifi") {
      const profile = deviceProfilesRef.current.find((item) => item.id === task.deviceId);
      if (!profile) return;
      commitDeviceTasks((current) => current.map((item) => item.id === taskId
        ? { ...item, status: "writing", lastError: null }
        : item));
      showToast(tRuntime("正在更新服务器任务，设备联网后会自动同步"), "info");
      try {
        await publishTaskToEsp32(profile, task.app);
        showToast(profile.online ? tRuntime("任务已更新，在线设备即将同步") : tRuntime("任务已更新，设备开机后同步"), "success");
      } catch (error) {
        commitDeviceTasks((current) => current.map((item) => item.id === taskId
          ? { ...item, status: "error", lastError: error instanceof Error ? error.message : tRuntime("任务更新失败") }
          : item));
        showToast(error instanceof Error ? error.message : tRuntime("任务更新失败"), "error");
        setDeviceStatus("error");
      }
      return;
    }
    commitDeviceTasks((current) => current.map((item) => item.id === taskId
      ? { ...item, status: "writing", nextRunAt: null, lastError: null }
      : item));
    showToast(tRuntime("正在重新连接设备并重试"), "info");
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
    if (current) void stopDeviceTask(current.id);
  }, [activeDeviceId, app.id, stopDeviceTask]);

  const start = async () => {
    const selectedProfile = deviceProfilesRef.current.find((item) => item.id === activeDeviceId)
      ?? null;
    if (selectedProfile?.family === "esp32") {
      try {
        await publishTaskToEsp32(selectedProfile, app);
        showToast(
          selectedProfile.online
            ? tRuntime("任务已保存，M5 PaperColor 将在下一次同步时拉取")
            : tRuntime("任务已保存；设备开机联网后会自动拉取"),
          "success",
        );
      } catch (error) {
        setDeviceStatus("error");
        showToast(error instanceof Error ? error.message : tRuntime("ESP32 任务下发失败"), "error");
      }
      return;
    }
    let driver = activeDeviceId ? deviceDriversRef.current.get(activeDeviceId) ?? null : driverRef.current;
    if (!driver) {
      driver = new TodooCard(setProgress);
      driverRef.current = driver;
    }
    if (!driver?.supported) {
      showToast(tRuntime("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站"), "error");
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
          `${resolvedDeviceName} ${t("已有一个")} ${conflicting.app.customMinutes} ${t("分钟高频任务")}「${conflicting.app.title}」。\n\n${t("五分钟以下的任务同一设备只能运行一个，是否停止原任务并替换？")}`,
        );
        if (!replace) {
          showToast(tRuntime("已保留原来的高频任务"), "info");
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
        execution: "browser-bluetooth",
      };
      commitDeviceTasks((current) => [task, ...current]);
      const wrote = await runTransfer(app, resolvedDeviceId, taskId, { reusePreview: true });
      const afterRun = deviceTasksRef.current.find((item) => item.id === taskId);
      scheduleDeviceTask(taskId, wrote ? undefined : retryDelayForFailures(afterRun?.consecutiveFailures ?? 1));
    } catch (error) {
      const message = error instanceof Error ? error.message : tRuntime("没有选择设备");
      showToast(message, "error");
      setDeviceStatus("error");
    }
  };

  const contentTitle = tab === "mine" ? t("我的模版") : tab === "explore" ? t("模板市场") : tab === "device" ? t("设备中心") : tab === "products" ? t("产品信息") : null;
  const visibleProductSkus = filterDeviceSkus({ family: productFamily, manufacturer: productBrand });
  const screenDisplay = displaySettings(app.spec, Boolean(app.localImage));
  const previewLandscape = screenOrientation(app.spec) === "landscape";
  const previewFrameWidth = previewLandscape ? 486 : 288;
  const previewFrameHeight = previewLandscape ? 288 : 486;
  const activeDeviceSku = deviceSku(activeDevice?.skuId);
  const activeRenderStrategies = deviceSku(activeDevice?.skuId)?.render.supportedStrategies;
  const supportedRenderModeOptions = renderModeOptions.filter((mode) =>
    !activeRenderStrategies || activeRenderStrategies.includes(
      paperColorStrategyForScreenMode(mode.value),
    ));
  const calibrationDevice = devices.find((device) => device.id === calibrationDeviceId) ?? null;
  const selectedDeviceSku = deviceSku(selectedDeviceSkuId);
  const deviceSummaries = devices.map((device) => {
    const tasks = deviceTasks.filter((task) => task.deviceId === device.id);
    const hasError = tasks.some((task) => task.status === "error");
    const isWriting = tasks.some((task) => task.status === "writing");
    const adapter = deviceAdapter(device.skuId);
    const authorized = !adapter.requiresBrowserDriver || deviceDriversRef.current.has(device.id);
    return {
      ...device,
      tasks,
      hasError,
      authorized,
      status: hasError
        ? "error"
        : isWriting
          ? "writing"
          : device.family === "esp32"
            ? device.online ? (tasks.length ? "scheduled" : "ready") : "idle"
            : tasks.length ? "scheduled" : authorized ? "ready" : "idle",
    };
  });
  const currentTask = deviceTasks.find((task) => task.app.id === app.id
    && (!activeDevice?.id || task.deviceId === activeDevice.id));
  const scheduleActive = Boolean(currentTask);
  const nextRun = currentTask?.nextRunAt ?? null;

  return (
    <main className={`app-shell${sidebarCollapsed ? " sidebar-collapsed" : ""}`}>
      <aside className="sidebar">
        <div className="sidebar-head">
          <button className="brand" type="button" onClick={() => navigateToTab("studio")} aria-label={tRuntime("返回创作台")}>
            <span className="brand-mark">I</span>
            <span className="brand-name">Inkloop</span>
          </button>
          <button
            type="button"
            className="sidebar-toggle"
            onClick={toggleSidebar}
            aria-label={sidebarCollapsed ? tRuntime("展开左侧边栏") : tRuntime("收起左侧边栏")}
            aria-expanded={!sidebarCollapsed}
            title={sidebarCollapsed ? tRuntime("展开边栏") : tRuntime("收起边栏")}
          >
            <span aria-hidden="true">{sidebarCollapsed ? "›" : "‹"}</span>
          </button>
        </div>
        <nav aria-label={tRuntime("主导航")}>
          {navItems.map((item) => (
            <button
              type="button"
              key={item.id}
              className={tab === item.id ? "active" : ""}
              onClick={() => navigateToTab(item.id)}
              title={sidebarCollapsed ? t(item.label) : undefined}
            >
              <span className="nav-glyph" aria-hidden="true">{item.glyph}</span>
              <span className="nav-label">{t(item.label)}</span>
            </button>
          ))}
        </nav>
        <div className="sidebar-devices" aria-label={tRuntime("已选择的设备")}>
          {deviceSummaries.length ? deviceSummaries.map((device) => (
            <button
              type="button"
              key={device.id}
              className={`sidebar-device${device.tasks.length ? " has-tasks" : ""}${device.hasError ? " has-error" : ""}${activeDevice?.id === device.id ? " write-target" : ""}`}
              onClick={() => openDeviceCenter(device)}
              aria-expanded={tab === "device" && expandedDeviceIds.has(device.id)}
              aria-controls={`device-card-${device.id}`}
              title={sidebarCollapsed
                ? `${device.name} · ${device.tasks.length} ${t("个刷新任务")}${activeDevice?.id === device.id ? ` · ${tRuntime("当前写入设备")}` : ""}`
                : tRuntime("查看设备详情，不切换写入设备")}
            >
              <span className={`status-dot ${device.status}`} />
              <div className="sidebar-device-copy">
                <strong>{device.name}</strong>
                <small>{device.tasks.length
                  ? `${device.tasks.length} ${t("个定时任务")}`
                  : device.family === "esp32" ? (device.online ? tRuntime("Wi‑Fi 在线") : tRuntime("离线 · 开机后同步")) : tRuntime("已记住，可自动重连")}</small>
              </div>
              {device.tasks.length > 0 && <span className="task-count" aria-label={`${device.tasks.length} ${t("个任务")}`}>{device.tasks.length}</span>}
              {device.hasError && <span className="task-alert" aria-label={tRuntime("任务出现错误")}>!</span>}
              <span className="task-chevron" aria-hidden="true">›</span>
            </button>
          )) : (
            <div className="sidebar-device empty">
              <span className="status-dot idle" />
              <div className="sidebar-device-copy"><strong>{t("未连接设备")}</strong><small>TodooCard · BLE</small></div>
            </div>
          )}
          <button type="button" className="add-device-button" onClick={openAddDevice}>
            <span aria-hidden="true">＋</span><span className="add-device-label">{t("添加设备")}</span>
          </button>
        </div>
        <LocaleSwitch className="sidebar-locale" locale={locale} setLocale={setLocale} t={t} />
      </aside>

      <section className="workspace">
        <header className="topbar">
          <div>
            <span className="eyebrow">INKLOOP · TODOO STUDIO</span>
            <strong>{contentTitle ?? app.title}</strong>
          </div>
          <div className="topbar-actions">
            <LocaleSwitch className="topbar-locale" locale={locale} setLocale={setLocale} t={t} compact />
            <button type="button" className="guide-button" onClick={() => setGuideOpen(true)}>
              <span>?</span> {t("使用说明")}
            </button>
            {tab === "studio" && (
              <div className="template-top-actions">
                <button type="button" className="share-template-button" onClick={() => void shareCurrentTemplate()}>
                  <span aria-hidden="true">↗</span> {t("分享模版")}
                </button>
                <div className="save-split" ref={saveMenuRef}>
                  <button type="button" className="save-button" onClick={() => void saveApp(false)}>
                    {t("保存模版")}
                  </button>
                  <button
                    type="button"
                    className="save-menu-toggle"
                    aria-label={tRuntime("打开保存选项")}
                    aria-haspopup="menu"
                    aria-expanded={saveMenuOpen}
                    onClick={() => setSaveMenuOpen((open) => !open)}
                  >
                    <span aria-hidden="true">⌄</span>
                  </button>
                  {saveMenuOpen && (
                    <div className="save-menu" role="menu">
                      <button
                        type="button"
                        role="menuitem"
                        onClick={() => {
                          setSaveMenuOpen(false);
                          void saveApp(true);
                        }}
                      >
                        <strong>{t("保存并发布到市场")}</strong>
                        <small>{t("保存到本机，同时出现在模板市场")}</small>
                      </button>
                    </div>
                  )}
                </div>
              </div>
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
                    <h2>{t("描述你想看到的内容")}</h2>
                    <p>{t("说人话就好，生成器会补全数据与排版逻辑。")}</p>
                  </div>
                </div>
                <label htmlFor="app-prompt">{t("应用需求")}</label>
                <div className="prompt-box">
                  <textarea
                    id="app-prompt"
                    value={prompt}
                    onChange={(event) => setPrompt(event.target.value)}
                    onPaste={handlePromptPaste}
                    placeholder={tRuntime("例如：每天早上 8 点显示上海天气…")}
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
                        aria-label={t("已贴入的图片缩略图")}
                        style={{ backgroundImage: `url(${app.localImage})` }}
                      />
                      <span className="attached-image-copy"><strong>{t("图片已贴入")}</strong><small>{t("将转换成六色参与排版")}</small></span>
                      <button type="button" onClick={() => setApp((current) => ({ ...current, localImage: undefined }))}>{t("移除")}</button>
                    </div>
                  ) : (
                    <button type="button" className="attach-button" onClick={() => fileInputRef.current?.click()}>
                      <span>＋</span> {t("选择图片")} <small>{t("也可直接粘贴")}</small>
                    </button>
                  )}
                </div>
                <div className="suggestions">
                  <span>{t("试试这些")}</span>
                  {samplePrompts.map((sample) => (
                    <button type="button" key={sample} onClick={() => setPrompt(sample)}>
                      {sample.replace(tRuntime("每天 8 点"), tRuntime("天气")).replace(tRuntime("显示"), "").slice(0, 10)}
                    </button>
                  ))}
                </div>
                <button className="generate-button" type="button" onClick={generate} disabled={generating}>
                  <span>{generating ? (generatorStatus === "online" ? tRuntime("模型编码中") : tRuntime("生成中")) : tRuntime("✦ 生成应用")}</span>
                  <i>{generating ? "•••" : "→"}</i>
                </button>
                <div className="generator-note">
                  <span className={generatorStatus === "online" ? "online" : ""}>LLM</span>
                  {generatorStatus === "online" ? (
                    <p className="generator-ready">
                      <span>{t("在线编码已就绪 ·")}</span>
                      <select
                        className="generator-model-select"
                        value={generatorModel}
                        onChange={(event) => {
                          const nextModel = event.target.value;
                          setGeneratorModel(nextModel);
                          localStorage.setItem(GENERATOR_MODEL_KEY, nextModel);
                        }}
                        disabled={generating}
                        aria-label={tRuntime("选择在线编码模型")}
                        title={generatorModel === "auto" ? tRuntime("由服务自动选择模型") : generatorModel}
                      >
                        <option value="auto">{t("自动选择模型")}</option>
                        {generatorModels.map((model) => (
                          <option value={model} key={model}>{model}</option>
                        ))}
                      </select>
                    </p>
                  ) : (
                    <p>{generatorStatus === "checking" ? tRuntime("正在检查在线编码服务…") : tRuntime("等待配置 LLM_API_KEY · 当前自动使用本地模板")}</p>
                  )}
                </div>
              </section>

              <section className="preview-panel panel">
                <div className="preview-toolbar">
                  <div>
                    <span className="step-number">02</span>
                    <div>
                      <h2>{t("屏幕预览")}</h2>
                      <p>
                        {previewDimensions.width} × {previewDimensions.height}{activeDeviceSku ? ` · ${activeDeviceSku.displayName}` : ""} · {previewStatus === "loading"
                          ? app.spec.kind === "map" ? tRuntime("正在获取并转换静态地图") : tRuntime("正在获取并转换图片素材")
                          : previewStatus === "fallback"
                            ? app.spec.kind === "map"
                              ? tRuntime("地图暂不可用，请查看右侧设置")
                              : app.spec.artwork?.layout === "fullscreen"
                              ? tRuntime("图片暂不可用，已保持纯图片模式")
                              : tRuntime("素材暂不可用，已使用图形排版")
                            : app.localImage
                              ? tRuntime("本机图片原图预览，写入时转换为六色")
                              : app.spec.kind === "map"
                                ? tRuntime("百度静态地图原图预览，写入时转换为六色")
                              : app.spec.artwork
                                ? tRuntime("图片原图预览，写入时转换为六色")
                              : tRuntime("实际六色色板")}
                      </p>
                    </div>
                  </div>
                  <div className="preview-actions">
                    {app.spec.kind !== "map" && (
                      <button
                        className="regenerate-preview"
                        type="button"
                        onClick={regeneratePreviewArtwork}
                        disabled={!app.spec.artwork || Boolean(app.localImage) || previewStatus === "loading"}
                        title={app.localImage ? tRuntime("当前使用的是你贴入的图片") : app.spec.artwork ? `${t("保持主题：")}${app.spec.artwork.query}` : tRuntime("当前应用没有图片素材")}
                      >
                        ↻ {t("重新生成")}
                      </button>
                    )}
                    <select
                      className="scale-chip"
                      value={previewScale}
                      onChange={(event) => setPreviewScale(Number(event.target.value) as 35 | 50 | 75 | 100)}
                      aria-label={tRuntime("调整屏幕预览缩放")}
                      title={`${t("只调整网页预览大小，不影响")} ${previewDimensions.width} × ${previewDimensions.height} ${t("写入画质")}`}
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
                        <div className="device-label">{activeDeviceSku?.displayName ?? "INKLOOP"}</div>
                        <div className="screen-canvas-wrap">
                          <canvas
                            ref={canvasRef}
                            width={previewDimensions.width}
                            height={previewDimensions.height}
                            aria-label={tRuntime("电子墨水屏预览")}
                          />
                          {app.spec.kind !== "map" && <div className="screen-drag-layer" aria-label={tRuntime("拖拽画面元素调整位置")}>
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
                                    width: `${(elementWidth / previewSourceDimensions.width) * 100}%`,
                                    height: `${(elementHeight / previewSourceDimensions.height) * 100}%`,
                                  }}
                                  aria-label={`${t("拖拽调整")}${element.label}${t("位置，方向键可微调")}`}
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
                          </div>}
                          {app.spec.kind === "map" && app.spec.map && (
                            <div
                              className={`map-preview-interaction${mapPanPreview ? " dragging" : ""}`}
                              role="application"
                              tabIndex={0}
                              aria-label={tRuntime("地图预览：按住拖动并在松开后更新位置，滚轮缩放；方向键移动，加减键缩放")}
                              onPointerDown={handleMapPointerDown}
                              onPointerMove={handleMapPointerMove}
                              onPointerUp={finishMapDrag}
                              onPointerCancel={finishMapDrag}
                              onWheel={handleMapWheel}
                              onKeyDown={handleMapKeyDown}
                            >
                              <span>{mapPanPreview ? tRuntime("松开后更新地图") : tRuntime("按住拖动 · 滚轮缩放")}{mapZoomPreview ? ` · ${mapZoomPreview} ${t("级")}` : ""}</span>
                            </div>
                          )}
                        </div>
                        <div className="device-port" />
                      </div>
                    </div>
                  </div>
                </div>
                <div className="palette-strip" aria-label={tRuntime("屏幕支持六种颜色")}>
                  {[
                    [tRuntime("黑"), "#111"],
                    [tRuntime("白"), EPAPER_WHITE],
                    [tRuntime("黄"), "#e5c900"],
                    [tRuntime("红"), "#dc3f2f"],
                    [tRuntime("蓝"), "#2756c7"],
                    [tRuntime("绿"), "#087c4e"],
                  ].map(([label, color]) => (
                    <span key={label}><i style={{ background: color }} />{label}</span>
                  ))}
                </div>
                {app.spec.kind === "map" && (
                  <div className="preview-source-note map-source-note">
                    <p><strong>{t("地图来源")}</strong> {t("百度静态地图 · 服务端代理")}</p>
                    <p>{t("浏览器精确定位需你授权；IP 定位只用于城市级估算。")}</p>
                  </div>
                )}
                {(app.spec.artwork || app.localImage) && (
                  <div className="preview-source-note">
                    {app.spec.artwork?.mode === "web" && (
                      <p>{t("如果图片主题与要求不符，请点击“重新生成”。")}</p>
                    )}
                    {app.localImage ? (
                      <p><strong>{t("图片来源")}</strong> {t("本机上传（无外部地址）")}</p>
                    ) : artworkCredit ? (
                      <p>
                        <strong>{t("图片来源")}</strong> {artworkCredit.provider === "loremflickr"
                          ? "LoremFlickr"
                          : artworkCredit.provider === "wikimedia-commons"
                            ? "Wikimedia Commons"
                            : artworkCredit.provider === "picsum"
                              ? "Picsum"
                              : artworkCredit.provider}
                        <a href={artworkCredit.url} target="_blank" rel="noreferrer">{t("查看真实图片地址")} ↗</a>
                      </p>
                    ) : app.spec.artwork?.mode === "generated" ? (
                      <p><strong>{t("图片来源")}</strong> {t("Inkloop 生成图形")}</p>
                    ) : null}
                  </div>
                )}
              </section>

              <section className="settings-panel panel">
                <div className="panel-heading compact">
                  <span className="step-number">03</span>
                  <div>
                    <h2>{t("画面、保存与刷新")}</h2>
                    <p>{t("生成后可继续手动调整画面元素。")}</p>
                  </div>
                </div>
                <div className="display-editor">
                  <div className="settings-subhead">
                    <strong>{app.spec.kind === "map" ? tRuntime("地图画面") : app.spec.kind === "card" ? tRuntime("卡片设计") : tRuntime("画面元素")}</strong>
                    <small>{app.spec.kind === "map"
                      ? tRuntime("位置和样式都可在这里继续调整")
                      : app.spec.kind === "card"
                        ? tRuntime("LLM 先生成数值与文案，你可以继续修改")
                        : tRuntime("勾选后可在预览中拖拽")}</small>
                  </div>
                  {app.spec.kind === "card" ? (
                    <div className="card-orientation-note">
                      <strong>{t("固定竖版 528×792")}</strong>
                      <small>{t("四种材质共用同一坐标网格，切换稀有度不会改变内容位置")}</small>
                    </div>
                  ) : <div className="orientation-field">
                    <div>
                      <strong>{t("屏幕方向")}</strong>
                      <small>{t("LLM 会先建议，你可以随时手动切换")}</small>
                    </div>
                    <div className="orientation-options" role="group" aria-label={tRuntime("屏幕方向")}>
                      {([
                        ["portrait", tRuntime("竖版 528×792")],
                        ["landscape", tRuntime("横版 792×528")],
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
                  </div>}
                  {app.spec.kind === "map" && app.spec.map && (
                    <div className="map-editor" aria-label={tRuntime("地图设置")}>
                      <div className={`map-service-state ${mapServiceStatus}`} role="status">
                        <i />
                        <span>{mapServiceStatus === "checking"
                          ? tRuntime("正在检查地图服务")
                          : mapServiceStatus === "ready"
                            ? tRuntime("百度静态地图服务已就绪")
                            : mapServiceMessage || tRuntime("地图服务尚未配置")}</span>
                      </div>

                      <div className="map-editor-group">
                        <div className="map-editor-label">
                          <strong>{t("定位方式")}</strong>
                          <small>{t("按住预览拖动，松开后更新位置；IP 仅为城市级估算")}</small>
                        </div>
                        <div className="map-location-options" role="group" aria-label={tRuntime("地图定位方式")}>
                          <button
                            type="button"
                            className={app.spec.map.locationMode === "browser" ? "selected" : ""}
                            onClick={locateWithBrowser}
                          >
                            <strong>{t("浏览器定位")}</strong><small>{t("需要位置授权")}</small>
                          </button>
                          <button
                            type="button"
                            className={app.spec.map.locationMode === "ip" ? "selected" : ""}
                            onClick={() => {
                              updateMap({
                                locationMode: "ip",
                                query: "",
                                latitude: undefined,
                                longitude: undefined,
                                coordinateType: "bd09ll",
                                address: undefined,
                                approximate: true,
                                statusMessage: tRuntime("正在按网络 IP 估算所在城市"),
                              });
                            }}
                          >
                            <strong>{t("IP 粗定位")}</strong><small>{t("城市级兜底")}</small>
                          </button>
                        </div>
                      </div>

                      <div className="map-editor-group">
                        <div className="map-editor-label">
                          <strong>{t("地点与坐标")}</strong>
                          <small>{app.spec.map.coordinateType === "wgs84ll" ? tRuntime("浏览器 WGS84 · 预览时服务端转换") : tRuntime("百度 BD-09 坐标")}</small>
                        </div>
                        <label className="map-place-field">
                          <span>{t("地点、地址或 POI")}</span>
                          <input
                            key={`${app.id}:${app.spec.map.query}`}
                            defaultValue={app.spec.map.query}
                            maxLength={80}
                            onBlur={(event) => {
                              const query = event.currentTarget.value.trim();
                              if (query === app.spec.map?.query) return;
                              updateMap({
                                locationMode: "picker",
                                query,
                                latitude: undefined,
                                longitude: undefined,
                                coordinateType: "bd09ll",
                                address: undefined,
                                approximate: false,
                                statusMessage: query ? tRuntime("正在查找地点") : tRuntime("请先输入地点或使用定位"),
                              });
                            }}
                            onKeyDown={(event) => {
                              if (event.key === "Enter") event.currentTarget.blur();
                            }}
                            placeholder={tRuntime("例如：上海市杨浦区公司北门")}
                          />
                        </label>
                        <div className="map-coordinate-grid">
                          <label>
                            <span>{t("经度")}</span>
                            <input
                              type="number"
                              min={-180}
                              max={180}
                              step="0.000001"
                              value={app.spec.map.longitude ?? ""}
                              onChange={(event) => updateMap({
                                longitude: event.target.value === "" ? undefined : Number(event.target.value),
                                locationMode: "picker",
                                coordinateType: "bd09ll",
                                approximate: false,
                              })}
                            />
                          </label>
                          <label>
                            <span>{t("纬度")}</span>
                            <input
                              type="number"
                              min={-90}
                              max={90}
                              step="0.000001"
                              value={app.spec.map.latitude ?? ""}
                              onChange={(event) => updateMap({
                                latitude: event.target.value === "" ? undefined : Number(event.target.value),
                                locationMode: "picker",
                                coordinateType: "bd09ll",
                                approximate: false,
                              })}
                            />
                          </label>
                        </div>
                        {app.spec.map.address && <p className="map-current-address">{app.spec.map.address}</p>}
                        <label className="map-place-field">
                          <span>{t("自定义显示名称（可选）")}</span>
                          <input
                            key={`${app.id}:${app.spec.map.displayName || "display-name"}`}
                            defaultValue={app.spec.map.displayName || ""}
                            maxLength={30}
                            onBlur={(event) => updateMap({ displayName: event.currentTarget.value.trim() || undefined })}
                            onKeyDown={(event) => {
                              if (event.key === "Enter") event.currentTarget.blur();
                            }}
                            placeholder={app.spec.map.address || app.spec.map.query || tRuntime("例如：集合点")}
                          />
                        </label>
                      </div>

                      <div className="map-editor-group map-zoom-group">
                        <div className="map-editor-label">
                          <strong>{t("地图缩放")}</strong>
                          <small>{t("拖动滑杆，或在预览上滚动鼠标 · 3—19")}</small>
                        </div>
                        <div className="map-zoom-control">
                          <button type="button" onClick={() => updateMap({ zoomLevel: app.spec.map!.zoomLevel - 1 })} aria-label={tRuntime("缩小地图")}>−</button>
                          <input
                            type="range"
                            min={3}
                            max={19}
                            step={1}
                            value={app.spec.map.zoomLevel}
                            onChange={(event) => updateMap({ zoomLevel: Number(event.target.value) })}
                            aria-label={tRuntime("地图缩放级别")}
                          />
                          <input
                            type="number"
                            min={3}
                            max={19}
                            step={1}
                            value={app.spec.map.zoomLevel}
                            onChange={(event) => updateMap({ zoomLevel: Number(event.target.value) || 3 })}
                            aria-label={tRuntime("手动输入地图缩放级别")}
                          />
                          <button type="button" onClick={() => updateMap({ zoomLevel: app.spec.map!.zoomLevel + 1 })} aria-label={tRuntime("放大地图")}>＋</button>
                        </div>
                      </div>

                      <div className="map-toggle-list">
                        {([
                          ["marker", tRuntime("显示位置标记"), tRuntime("在地图中心标出目标位置")],
                          ["showAddress", tRuntime("显示名称"), tRuntime("优先显示自定义名称，否则使用解析地址")],
                          ["showCoordinates", tRuntime("显示坐标"), tRuntime("显示 BD-09 经纬度")],
                        ] as Array<["marker" | "showAddress" | "showCoordinates", string, string]>).map(([key, label, detail]) => (
                          <label key={key}>
                            <span><strong>{label}</strong><small>{detail}</small></span>
                            <input
                              type="checkbox"
                              checked={app.spec.map?.[key] ?? false}
                              onChange={(event) => updateMap({ [key]: event.target.checked })}
                            />
                          </label>
                        ))}
                      </div>
                      <p className={`map-location-note${app.spec.map.approximate ? " approximate" : ""}`}>
                        {app.spec.map.statusMessage || (app.spec.map.approximate
                          ? tRuntime("当前只是城市级估算，写入前建议在预览上拖动并松开以微调。")
                          : tRuntime("在屏幕预览上按住拖动，松开后更新地图；滚轮调整缩放。"))}
                      </p>
                    </div>
                  )}
                  {app.spec.kind === "card" && app.spec.card && (
                    <div className="card-editor" aria-label={tRuntime("卡片设计设置")}>
                      <div className="card-rarity-options" role="group" aria-label={tRuntime("卡片稀有度")}>
                        {([
                          ["common", tRuntime("普卡"), tRuntime("纸张")],
                          ["silver", tRuntime("银卡"), tRuntime("银箔")],
                          ["gold", tRuntime("金卡"), tRuntime("鎏金")],
                          ["holo", tRuntime("闪卡"), tRuntime("闪膜")],
                        ] as Array<[CardRarity, string, string]>).map(([value, label, detail]) => (
                          <button
                            type="button"
                            key={value}
                            className={app.spec.card?.rarity === value ? `selected ${value}` : value}
                            onClick={() => updateCard({ rarity: value })}
                          >
                            <strong>{label}</strong><small>{detail}</small>
                          </button>
                        ))}
                      </div>

                      <div className="card-fields-grid">
                        <label className="card-field card-name-field">
                          <span>{t("卡名")}</span>
                          <input value={app.spec.card.name} maxLength={20} onChange={(event) => updateCard({ name: event.target.value })} />
                        </label>
                        <label className="card-field">
                          <span>{t("类型")}</span>
                          <input value={app.spec.card.type} maxLength={24} onChange={(event) => updateCard({ type: event.target.value })} />
                        </label>
                        <label className="card-field compact">
                          <span>{t("等级")}</span>
                          <input type="number" min={1} max={12} value={app.spec.card.level} onChange={(event) => updateCard({ level: Number(event.target.value) || 1 })} />
                        </label>
                        <label className="card-field compact">
                          <span>ATK</span>
                          <input type="number" min={0} max={9999} value={app.spec.card.attack} onChange={(event) => updateCard({ attack: Number(event.target.value) || 0 })} />
                        </label>
                        <label className="card-field compact">
                          <span>DEF</span>
                          <input type="number" min={0} max={9999} value={app.spec.card.defense} onChange={(event) => updateCard({ defense: Number(event.target.value) || 0 })} />
                        </label>
                        <label className="card-field compact">
                          <span>{t("编号")}</span>
                          <input value={app.spec.card.cardId} maxLength={18} onChange={(event) => updateCard({ cardId: event.target.value })} />
                        </label>
                        <label className="card-field card-description-field">
                          <span>{t("效果描述")}</span>
                          <textarea value={app.spec.card.description} maxLength={120} rows={3} onChange={(event) => updateCard({ description: event.target.value })} />
                        </label>
                      </div>

                      <div className="card-subject-editor">
                        <div className="card-editor-heading">
                          <div><strong>{t("主角画面")}</strong><small>{t("保持卡框不动，只调整中间主角")}</small></div>
                          <div className="card-subject-actions">
                            <label>
                              {t("上传图片")}
                              <input type="file" accept="image/*" onChange={(event) => void attachLocalImage(event.target.files?.[0])} />
                            </label>
                            <button type="button" onClick={regeneratePreviewArtwork} disabled={!app.spec.artwork || Boolean(app.localImage)}>{t("随机主角")}</button>
                            {app.localImage && <button type="button" onClick={() => setApp((current) => ({ ...current, localImage: undefined }))}>{t("恢复网络图")}</button>}
                          </div>
                        </div>
                        <label className="card-range-field">
                          <span>{t("主体大小")}</span>
                          <input type="range" min={0.7} max={2.2} step={0.05} value={app.spec.card.subjectScale} onChange={(event) => updateCard({ subjectScale: Number(event.target.value) })} />
                          <input type="number" min={70} max={220} value={Math.round(app.spec.card.subjectScale * 100)} onChange={(event) => updateCard({ subjectScale: (Number(event.target.value) || 100) / 100 })} />
                          <em>%</em>
                        </label>
                        <label className="card-range-field">
                          <span>{t("水平位置")}</span>
                          <input type="range" min={-100} max={100} value={app.spec.card.subjectX} onChange={(event) => updateCard({ subjectX: Number(event.target.value) })} />
                          <input type="number" min={-100} max={100} value={app.spec.card.subjectX} onChange={(event) => updateCard({ subjectX: Number(event.target.value) || 0 })} />
                        </label>
                        <label className="card-range-field">
                          <span>{t("垂直位置")}</span>
                          <input type="range" min={-100} max={100} value={app.spec.card.subjectY} onChange={(event) => updateCard({ subjectY: Number(event.target.value) })} />
                          <input type="number" min={-100} max={100} value={app.spec.card.subjectY} onChange={(event) => updateCard({ subjectY: Number(event.target.value) || 0 })} />
                        </label>
                        <button type="button" className="card-subject-reset" onClick={() => updateCard({ subjectScale: 1, subjectX: 0, subjectY: 0 })}>{t("主角位置复位")}</button>
                      </div>
                    </div>
                  )}
                  {app.spec.kind !== "map" && app.spec.kind !== "card" && (<>
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
                            aria-label={`${t("复位")}${label}${t("位置")}`}
                          >
                            {t("位置复位")}
                          </button>
                        </div>
                        <div className={`component-type-controls${key === "qr" ? " qr-component-controls" : ""}`}>
                          {key !== "qr" && (
                            <label>
                              <span>{t("字体")}</span>
                              <select
                                value={screenDisplay.elementFonts[key] ?? ""}
                                onChange={(event) => updateDisplay({
                                  elementFonts: {
                                    ...screenDisplay.elementFonts,
                                    [key]: event.target.value || undefined,
                                  } as ScreenDisplay["elementFonts"],
                                })}
                                aria-label={`${label}${t("字体")}`}
                                disabled={!screenDisplay[key]}
                              >
                                <option value="">{t("跟随默认字体")}</option>
                                {screenFontOptions.map((font) => (
                                  <option value={font.value} key={font.value}>{font.label}</option>
                                ))}
                              </select>
                            </label>
                          )}
                          <label className={`component-size-control${key === "qr" ? " qr-size-control" : ""}`}>
                            <span>{key === "qr" ? tRuntime("尺寸") : tRuntime("字号")}</span>
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
                              aria-label={`${label}${key === "qr" ? tRuntime("尺寸") : tRuntime("字号")}`}
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
                        <span>{t("屏幕外框")}</span>
                      </label>
                      <small>{t("只绘制最外侧细框")}</small>
                    </div>
                  </div>
                  <div className="display-fields">
                    <div className="render-mode-field">
                      <div className="render-mode-heading">
                        <span>{t("画面渲染")}</span>
                        <small>{app.spec.artwork || app.localImage
                          ? tRuntime("图片默认 Official Skill")
                          : tRuntime("纯文字默认 Inkloop text")}</small>
                      </div>
                      <div className="render-mode-options" role="group" aria-label={tRuntime("画面渲染方式")}>
                        {supportedRenderModeOptions.map((mode) => (
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
                      <span>{t("画面默认字体")}</span>
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
                        {t("今日天气 · 12:34 · 专注当下")}
                      </span>
                    </label>
                    {screenDisplay.quote && (
                      <label className="display-field">
                        <span>{t("今日名言")}</span>
                        <div className="quote-input-row">
                          <input
                            value={app.spec.footer}
                            maxLength={40}
                            onChange={(event) => setApp((current) => ({
                              ...current,
                              spec: { ...current.spec, footer: event.target.value },
                            }))}
                            placeholder={tRuntime("输入一句话")}
                          />
                          <button type="button" onClick={randomizeQuote}>{t("随机")}</button>
                        </div>
                      </label>
                    )}
                    {screenDisplay.logo && (
                      <label className="display-field">
                        <span>{t("LOGO 文字")}</span>
                        <input
                          value={screenDisplay.logoText}
                          maxLength={20}
                          onChange={(event) => updateDisplay({ logoText: event.target.value })}
                          placeholder={tRuntime("例如 INKLOOP")}
                        />
                      </label>
                    )}
                    {screenDisplay.qr && (
                      <div className="display-field qr-content-field">
                        <span>{t("二维码内容")}</span>
                        <div className="qr-mode-options" role="group" aria-label={tRuntime("二维码内容类型")}>
                          <button
                            type="button"
                            className={screenDisplay.qrMode === "text" ? "selected" : ""}
                            onClick={() => updateDisplay({ qrMode: "text" })}
                          >
                            {t("文字 / 网址")}
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
                              <span>{t("Wi-Fi 名称（SSID）")}</span>
                              <input
                                value={screenDisplay.qrWifiSsid}
                                maxLength={64}
                                onChange={(event) => updateDisplay({ qrWifiSsid: event.target.value })}
                                placeholder={tRuntime("例如 Home WiFi")}
                              />
                            </label>
                            <label>
                              <span>{t("安全类型")}</span>
                              <select
                                value={screenDisplay.qrWifiSecurity}
                                onChange={(event) => updateDisplay({
                                  qrWifiSecurity: event.target.value as ScreenDisplay["qrWifiSecurity"],
                                })}
                              >
                                <option value="WPA">WPA / WPA2 / WPA3</option>
                                <option value="WEP">WEP</option>
                                <option value="nopass">{t("无密码")}</option>
                              </select>
                            </label>
                            {screenDisplay.qrWifiSecurity !== "nopass" && (
                              <label className="qr-password-field">
                                <span>{t("Wi-Fi 密码")}</span>
                                <input
                                  type="password"
                                  value={screenDisplay.qrWifiPassword}
                                  maxLength={128}
                                  autoComplete="off"
                                  onChange={(event) => updateDisplay({ qrWifiPassword: event.target.value })}
                                  placeholder={tRuntime("只保存在当前浏览器")}
                                />
                              </label>
                            )}
                            <label className="qr-hidden-network">
                              <input
                                type="checkbox"
                                checked={screenDisplay.qrWifiHidden}
                                onChange={(event) => updateDisplay({ qrWifiHidden: event.target.checked })}
                              />
                              <span>{t("这是隐藏网络")}</span>
                            </label>
                          </div>
                        ) : (
                          <textarea
                            aria-label={tRuntime("二维码文字或网址")}
                            value={screenDisplay.qrText}
                            maxLength={512}
                            rows={3}
                            onChange={(event) => updateDisplay({ qrText: event.target.value })}
                            placeholder={tRuntime("输入网址或任意文字")}
                          />
                        )}
                        <small>{screenDisplay.qrMode === "wifi"
                          ? tRuntime("密码仅保存在当前浏览器；包含 Wi-Fi 凭据的模版不能发布到模板市场。")
                          : tRuntime("内容只在本机生成二维码，不会发送到二维码服务；白色留边是扫码所必需。")}</small>
                      </div>
                    )}
                    {(screenDisplay.weather || screenDisplay.weatherLarge) && (
                      <label className="display-field">
                        <span>{t("天气城市")}</span>
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
                          placeholder={tRuntime("例如 上海")}
                        />
                      </label>
                    )}
                  </div>
                  </>)}
                </div>
                {app.spec.table?.type === "agenda" && (
                  <section className="agenda-editor" aria-label={tRuntime("智能日程布局与范围")}>
                    <div className="calendar-source-heading">
                      <strong>{t("智能日程布局")}</strong>
                      <small>{t("空闲时间会自动压缩")}</small>
                    </div>
                    <div className="agenda-view-options" role="group" aria-label={tRuntime("日程布局")}>
                      {([
                        ["agenda", tRuntime("智能议程")],
                        ["three-day", tRuntime("三日时间轴")],
                        ["workweek", tRuntime("五日时间轴")],
                      ] as Array<[AgendaView, string]>).map(([value, label]) => (
                        <button
                          type="button"
                          key={value}
                          className={app.spec.table?.type === "agenda" && app.spec.table.view === value ? "selected" : ""}
                          onClick={() => updateAgendaTable(value === "workweek"
                            ? { view: value, rangeMode: "rolling", rangeHours: 120 }
                            : { view: value })}
                        >
                          {label}
                        </button>
                      ))}
                    </div>
                    <div className="agenda-range-grid">
                      <label>
                        <span>{t("时间范围")}</span>
                        <select
                          value={app.spec.table.rangeMode === "rolling" ? String(app.spec.table.rangeHours) : app.spec.table.rangeMode}
                          onChange={(event) => {
                            const value = event.target.value;
                            if (value === "today" || value === "custom") updateAgendaTable({ rangeMode: value as AgendaRangeMode });
                            else updateAgendaTable({ rangeMode: "rolling", rangeHours: Number(value) || 72 });
                          }}
                        >
                          <option value="4">{t("接下来 4 小时")}</option>
                          <option value="8">{t("接下来 8 小时")}</option>
                          <option value="12">{t("接下来 12 小时")}</option>
                          <option value="24">{t("接下来 24 小时")}</option>
                          <option value="72">{t("接下来 3 天")}</option>
                          <option value="120">{t("接下来 5 天")}</option>
                          <option value="168">{t("接下来 7 天")}</option>
                          <option value="today">{t("今天")}</option>
                          <option value="custom">{t("自定义")}</option>
                        </select>
                      </label>
                      {app.spec.table.rangeMode === "custom" && (
                        <>
                          <label>
                            <span>{t("开始")}</span>
                            <input
                              type="datetime-local"
                              value={app.spec.table.customStart || ""}
                              onChange={(event) => updateAgendaTable({ customStart: event.target.value })}
                            />
                          </label>
                          <label>
                            <span>{t("结束")}</span>
                            <input
                              type="datetime-local"
                              value={app.spec.table.customEnd || ""}
                              onChange={(event) => updateAgendaTable({ customEnd: event.target.value })}
                            />
                          </label>
                        </>
                      )}
                    </div>
                    <div className="agenda-density-controls">
                      <label className="agenda-width-control">
                        <span>
                          <strong>{t("日程块宽度")}</strong>
                          <output>{Math.round(app.spec.table.eventWidth ?? 100)}%</output>
                        </span>
                        <input
                          type="range"
                          min={45}
                          max={100}
                          step={5}
                          value={app.spec.table.eventWidth ?? 100}
                          aria-label={tRuntime("日程块宽度")}
                          onChange={(event) => updateAgendaTable({ eventWidth: Number(event.target.value) })}
                        />
                      </label>
                      <div className="agenda-content-toggles" role="group" aria-label={tRuntime("日程显示内容")}>
                        <label>
                          <input
                            type="checkbox"
                            checked={app.spec.table.showEndTime !== false}
                            onChange={(event) => updateAgendaTable({ showEndTime: event.target.checked })}
                          />
                          <span>{t("结束时间")}</span>
                        </label>
                        <label>
                          <input
                            type="checkbox"
                            checked={app.spec.table.showLocation !== false}
                            onChange={(event) => updateAgendaTable({ showLocation: event.target.checked })}
                          />
                          <span>{t("地点")}</span>
                        </label>
                      </div>
                    </div>
                  </section>
                )}
                {(app.spec.table?.type === "calendar" || app.spec.table?.type === "agenda") && (
                  <section className="calendar-source-editor" aria-label={tRuntime("在线日历数据")}>
                    <div className="calendar-source-heading">
                      <strong>{t("在线日历数据")}</strong>
                      <small>{calendarPreferences.sources.length} {t("个个人来源 · 仅保存在当前浏览器")}</small>
                    </div>
                    <div className="calendar-source-options">
                      {app.spec.table.type === "calendar" && (
                        <label>
                          <input
                            type="checkbox"
                            checked={calendarPreferences.lunar}
                            onChange={(event) => updateCalendarPreferences({ lunar: event.target.checked })}
                          />
                          <span><strong>{t("农历日期")}</strong><small>{t("内置换算，无需联网")}</small></span>
                        </label>
                      )}
                      <label>
                        <input
                          type="checkbox"
                          checked={calendarPreferences.chinaHolidays}
                          onChange={(event) => updateCalendarPreferences({ chinaHolidays: event.target.checked })}
                        />
                        <span><strong>{t("中国公众假期")}</strong><small>{t("读取公开 iCal 日历")}</small></span>
                      </label>
                    </div>
                    {calendarPreferences.sources.length > 0 && (
                      <div className="calendar-source-list" role="list" aria-label={tRuntime("已添加的个人日历")}>
                        {calendarPreferences.sources.map((source, index) => (
                          <div
                            key={source.id}
                            className={`calendar-source-item${source.enabled ? "" : " disabled"}`}
                            role="listitem"
                          >
                            <label className="calendar-source-toggle" title={source.enabled ? tRuntime("暂停读取此日历") : tRuntime("启用此日历")}>
                              <input
                                type="checkbox"
                                checked={source.enabled}
                                onChange={(event) => updateCalendarSource(source.id, { enabled: event.target.checked })}
                                aria-label={`${source.enabled ? tRuntime("停用") : tRuntime("启用")}${source.name}`}
                              />
                              <span
                                className="calendar-source-swatch"
                                style={{ background: tableAccent(source.name) }}
                                aria-hidden="true"
                              />
                            </label>
                            <div className="calendar-source-meta">
                              <input
                                defaultValue={source.name}
                                onBlur={(event) => {
                                  const name = event.target.value.trim().slice(0, 24) || calendarSourceName(source.url, index);
                                  event.target.value = name;
                                  if (name !== source.name) updateCalendarSource(source.id, { name });
                                }}
                                aria-label={`${t("日历")} ${index + 1} ${t("名称")}`}
                                maxLength={24}
                              />
                              <small title={source.url}>{source.url.replace(/^webcal:\/\//i, "")}</small>
                            </div>
                            <button
                              type="button"
                              onClick={() => removeCalendarSource(source.id)}
                              aria-label={`${t("移除")}${source.name}`}
                            >
                              {t("移除")}
                            </button>
                          </div>
                        ))}
                      </div>
                    )}
                    <label className="calendar-url-field" htmlFor="calendar-ical-url">
                      <span>{t("添加 iCal 地址")}</span>
                      <div className="calendar-url-row">
                        <input
                          id="calendar-ical-url"
                          type="text"
                          inputMode="url"
                          value={calendarUrlDraft}
                          onChange={(event) => setCalendarUrlDraft(event.target.value)}
                          onKeyDown={(event) => {
                            if (event.key === "Enter") {
                              event.preventDefault();
                              addCalendarSource();
                            }
                          }}
                          placeholder={tRuntime("https://…/basic.ics 或 webcal://…")}
                          autoComplete="off"
                          spellCheck={false}
                        />
                        <button type="button" onClick={addCalendarSource}>{t("添加")}</button>
                      </div>
                    </label>
                    <p className={calendarNotice ? "calendar-source-warning" : undefined}>
                      {calendarNotice || tRuntime("最多 5 个 Google、Apple 或 Outlook 只读日历；可分别暂停与改名。地址不会写入模版或发布到模板市场。")}
                    </p>
                  </section>
                )}
                <label>{t("刷新计划")}</label>
                <div className="schedule-options">
                  {[
                    ["once", tRuntime("单次写入"), tRuntime("立即执行一次")],
                    ["hourly", tRuntime("每小时"), tRuntime("整点后循环")],
                    ["daily", tRuntime("每天"), app.dailyTime],
                    ["custom", tRuntime("自定义"), `${app.customMinutes} ${t("分钟")}`],
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
                    <label htmlFor="daily-time">{t("每天执行时间")}</label>
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
                    <label htmlFor="custom-minutes">{t("间隔分钟（最短 1 分钟）")}</label>
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
                <button type="button" className="code-toggle" onClick={() => setCodeOpen((open) => !open)}>
                  <span><i>&lt;/&gt;</i> {t("查看生成逻辑")}</span><b>{codeOpen ? "−" : "+"}</b>
                </button>
                {codeOpen && <pre className="code-preview"><code>{app.code}</code></pre>}
              </section>
            </div>

            <div className="run-dock">
              <div className="run-status">
                <span className={`run-icon ${deviceStatus}`}>{deviceStatus === "writing" ? "↻" : "⌁"}</span>
                <div className="run-status-copy">
                  <div className="run-device-target">
                    <span>{tRuntime("准备写入设备")}</span>
                    {deviceSummaries.length ? (
                      <select
                        value={activeDevice?.id ?? ""}
                        disabled={deviceStatus === "writing"}
                        aria-label={tRuntime("准备写入设备")}
                        onChange={(event) => {
                          const profile = deviceProfilesRef.current.find((device) => device.id === event.target.value);
                          if (!profile) return;
                          activateDevice(profile);
                          const target = profile.skuId === "m5-papercolor-c151"
                            ? deviceAdapter(profile.skuId).renderTarget(screenOrientation(app.spec))
                            : screenDimensions(app.spec);
                          setDeviceStatus(profile.family === "esp32"
                            ? profile.online ? "ready" : "idle"
                            : deviceDriversRef.current.has(profile.id) ? "ready" : "idle");
                          showToast(`${tRuntime("已切换写入设备")} · ${profile.name} · ${target.width} × ${target.height}`, "info");
                        }}
                      >
                        {deviceSummaries.map((device) => {
                          const sku = deviceSku(device.skuId);
                          return (
                            <option value={device.id} key={device.id}>
                              {device.name} · {sku?.screen.width ?? "—"} × {sku?.screen.height ?? "—"}
                            </option>
                          );
                        })}
                      </select>
                    ) : (
                      <button type="button" onClick={openAddDevice}>{tRuntime("选择或添加设备")}</button>
                    )}
                  </div>
                  {deviceStatus === "writing" ? (
                    <small>{activeDevice?.family === "esp32"
                      ? tRuntime("正在保存设备任务")
                      : progress?.message ?? tRuntime("正在写入")}</small>
                  ) : nextRun ? (
                    <div className="next-run-summary">
                      <span>{t("下次执行")}</span>
                      <b>{formatExactTime(nextRun)}</b>
                      <em>{formatRemaining(nextRun, secondTick)}</em>
                    </div>
                  ) : (
                    <small>{activeDevice?.family === "esp32"
                      ? activeDevice.online ? tRuntime("Wi‑Fi 在线 · 计划由设备端执行") : tRuntime("设备离线 · 任务会在开机后同步")
                      : activeDevice ? tRuntime("已授权设备不会再次弹出选择器") : tRuntime("首次需要手动选择设备 · 之后自动重连")}</small>
                  )}
                </div>
              </div>
              {deviceStatus === "writing" && (
                <div className="transfer-progress"><i style={{ width: `${progress?.percent ?? 0}%` }} /></div>
              )}
              <div className="run-actions">
                <button
                  type="button"
                  className={`bluetooth-status-button ${activeDevice?.family === "esp32" ? activeDevice.online ? "ok" : "warn" : bluetoothSupported ? "ok" : "warn"}`}
                  onClick={openAddDevice}
                  title={tRuntime("选择或添加设备")}
                >
                  <i aria-hidden="true" />
                  {activeDevice?.family === "esp32"
                    ? activeDevice.online ? tRuntime("Wi‑Fi 在线") : tRuntime("Wi‑Fi 离线")
                    : bluetoothSupported ? tRuntime("蓝牙可用") : tRuntime("请使用 Chromium")}
                </button>
                {scheduleActive && <button type="button" className="stop-button" onClick={stopSchedule}>{t("停止任务")}</button>}
                <button type="button" className="start-button" onClick={start} disabled={deviceStatus === "writing"}>
                  <span>{deviceStatus === "writing"
                    ? activeDevice?.family === "esp32" ? tRuntime("正在保存") : tRuntime("正在写入")
                    : activeDevice?.family === "esp32" ? scheduleActive ? tRuntime("更新设备任务") : tRuntime("下发到设备") : scheduleActive ? tRuntime("立即再写一次") : tRuntime("开始写入")}</span>
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
              <h1>{t("我的模版")}</h1>
              <p>{t("这些模版保存在浏览器本机，不上传个人数据。清理浏览器数据会一并删除。")}</p>
              <button type="button" onClick={() => navigateToTab("studio")}>＋ {t("创建新模版")}</button>
            </div>
            {localApps.length ? (
              <div className="card-grid">
                {localApps.map((item) => <AppCard key={item.id} app={item} local onUse={() => copyAppToStudio(item)} />)}
              </div>
            ) : (
              <div className="empty-state">
                <span>▦</span><h2>{t("还没有保存的模版")}</h2><p>{t("在创作台生成并保存，第一个模版就会出现在这里。")}</p>
              </div>
            )}
          </section>
        )}

        {tab === "explore" && (
          <section className="collection-view explore-view">
            <div className="collection-hero split">
              <div>
                <span className="eyebrow">PUBLIC GALLERY</span>
                <h1>{t("模板市场")}</h1>
                <p>{t("所有模板都能一键复制到创作台，再按自己的数据与频率修改。")}</p>
              </div>
              <div className="gallery-stat"><b>{publicApps.length}</b><span>{t("公开模板")}</span></div>
            </div>
            <div className="filter-row"><button className="active">{t("精选")}</button><button>{t("生活")}</button><button>{t("效率")}</button><button>{t("数据")}</button></div>
            <div className="card-grid">
              {publicApps.map((item) => (
                <AppCard
                  key={item.id}
                  app={item}
                  onUse={() => copyAppToStudio(item)}
                  onShare={() => void shareMarketTemplate(item)}
                />
              ))}
            </div>
          </section>
        )}

        {tab === "products" && (
          <section className="collection-view products-view">
            <div className="collection-hero">
              <span className="eyebrow">SUPPORTED DISPLAYS</span>
              <h1>{t("已支持的墨水屏")}</h1>
              <p>{t("按连接方式和品牌筛选，查看不同类型支持的墨水屏。")}</p>
            </div>
            <div className="filter-row" role="group" aria-label={t("连接方式")}>
              {([
                { id: "all" as const, label: "全部类型" },
                { id: "bluetooth" as const, label: "蓝牙" },
                { id: "esp32" as const, label: "ESP32" },
              ]).map((item) => (
                <button
                  key={item.id}
                  type="button"
                  className={productFamily === item.id ? "active" : ""}
                  onClick={() => setProductFamily(item.id)}
                >
                  {t(item.label)}
                </button>
              ))}
            </div>
            <div className="filter-row" role="group" aria-label={t("品牌")}>
              <button
                type="button"
                className={productBrand === "all" ? "active" : ""}
                onClick={() => setProductBrand("all")}
              >
                {t("全部品牌")}
              </button>
              {deviceManufacturers().map((brand) => (
                <button
                  key={brand}
                  type="button"
                  className={productBrand === brand ? "active" : ""}
                  onClick={() => setProductBrand(brand)}
                >
                  {brand}
                </button>
              ))}
            </div>
            {visibleProductSkus.length ? (
              <div className="card-grid product-grid">
                {visibleProductSkus.map((sku) => (
                  <a
                    key={sku.id}
                    className="product-card"
                    href={officialProductUrl(sku, locale)}
                    target="_blank"
                    rel="noreferrer"
                  >
                    <div
                      className="product-screen"
                      style={{ aspectRatio: `${sku.screen.width} / ${sku.screen.height}` }}
                      aria-hidden="true"
                    >
                      <small>{sku.manufacturer}</small>
                      <b>{sku.screen.width}×{sku.screen.height}</b>
                      <em>{sku.sizeInches} {t("英寸")}</em>
                      <span className="product-screen-swatches">
                        <i style={{ background: "#151a17" }} />
                        <i style={{ background: "#f7f4e8" }} />
                        <i style={{ background: "#e5c900" }} />
                        <i style={{ background: "#dc3f2f" }} />
                        <i style={{ background: "#2756c7" }} />
                        <i style={{ background: "#087c4e" }} />
                      </span>
                    </div>
                    <div className="product-card-copy">
                      <small>{sku.manufacturer} · {sku.family === "bluetooth" ? t("蓝牙") : t("ESP32")}</small>
                      <strong>{sku.displayName}</strong>
                      <p>{t(sku.description)}</p>
                      <dl>
                        <div><dt>{t("连接方式")}</dt><dd>{sku.family === "bluetooth" ? t("蓝牙") : t("ESP32")}</dd></div>
                        <div><dt>{t("品牌")}</dt><dd>{sku.manufacturer}</dd></div>
                        <div><dt>{t("支持的产品")}</dt><dd>{sku.model}</dd></div>
                      </dl>
                      <span className="product-card-cta">{t("查看官方介绍")} ↗</span>
                    </div>
                  </a>
                ))}
              </div>
            ) : (
              <div className="empty-state">
                <span>▣</span>
                <h2>{t("没有符合筛选的产品")}</h2>
                <p>{t("试试切换类型或品牌。")}</p>
              </div>
            )}
          </section>
        )}

        {tab === "device" && (
          <section className="device-view">
            <div className="device-hero">
              <div>
                <span className="eyebrow">BLUETOOTH + ESP32 · ONE DEVICE CENTER</span>
                <h1>{t("同一个创作台，两种无痛写入方式。")}</h1>
                <p>{t("TodooCard 延续浏览器蓝牙写入；M5 PaperColor 在设备端保存计划并主动联网拉取，浏览器关闭也不会丢失任务。")}</p>
              </div>
              <div className="connection-card">
                <span className={`status-orb ${devices.length ? "connected" : ""}`}>⌁</span>
                <strong>{activeDevice?.name ?? tRuntime("尚未添加设备")}</strong>
                <small>{devices.length
                  ? `${t("已添加")} ${devices.length} ${t("台设备 · 写入任务按设备独立管理")}`
                  : tRuntime("支持 TodooCard 与 M5 PaperColor")}</small>
                <button type="button" onClick={openAddDevice}>{devices.length ? tRuntime("添加另一台设备") : tRuntime("添加设备")}</button>
              </div>
            </div>
            <section className="device-registry" aria-labelledby="device-registry-title">
              <header>
                <div>
                  <span className="eyebrow">AUTHORIZED DEVICES</span>
                  <h2 id="device-registry-title">{t("连接过的设备")}</h2>
                  <p>{t("蓝牙任务在当前页面调度；ESP32 任务持久化在服务器与设备上。点开设备即可查看其执行方式与同步状态。")}</p>
                </div>
                <button type="button" onClick={openAddDevice}>{t("添加设备")}</button>
              </header>
              {deviceSummaries.length ? (
                <div className="device-registry-list">
                  {deviceSummaries.map((device) => {
                    const expanded = expandedDeviceIds.has(device.id);
                    const statusLabel = device.hasError
                      ? tRuntime("任务异常")
                      : device.status === "writing"
                        ? tRuntime("正在写入")
                        : device.family === "esp32"
                          ? device.online ? tRuntime("Wi‑Fi 在线") : tRuntime("离线 · 等待开机")
                        : device.tasks.length
                          ? tRuntime("定时刷新中")
                          : device.authorized
                            ? tRuntime("浏览器已授权")
                            : tRuntime("历史设备");
                    return (
                      <article
                        className={`device-registry-item ${device.status}`}
                        id={`device-card-${device.id}`}
                        key={device.id}
                      >
                        <button
                          type="button"
                          className="device-registry-summary"
                          onClick={() => {
                            activateDevice(device);
                            toggleDeviceTasks(device.id);
                          }}
                          aria-expanded={expanded}
                          aria-controls={`device-history-${device.id}`}
                        >
                          <span className={`status-dot ${device.status}`} aria-hidden="true" />
                          <span className="device-registry-copy">
                            <strong>{device.name}</strong>
                            <small>{statusLabel}{device.family === "bluetooth" && !device.authorized ? tRuntime(" · 写入时可能需要重新选择") : ""}</small>
                          </span>
                          <span className="device-registry-meta">
                            <b>{device.tasks.length}</b>
                            <small>{t("刷新任务")}</small>
                          </span>
                          {device.hasError && <span className="device-registry-alert" aria-label={tRuntime("任务出现错误")}>!</span>}
                          <span className="device-registry-chevron" aria-hidden="true">{expanded ? "−" : "+"}</span>
                        </button>
                        {expanded && (
                          <div className="device-registry-tasks" id={`device-history-${device.id}`}>
                            {deviceAdapter(device.skuId).supportsCalibration ? <section className={`device-calibration-panel${device.calibration ? " calibrated" : ""}`} aria-label={`${device.name} ${t("设备校色")}`}>
                              <div className="device-calibration-copy">
                                <span className="device-calibration-mark" aria-hidden="true">C</span>
                                <div>
                                  <span className="device-calibration-title">
                                    <strong>{t("设备色差纠正")}</strong>
                                    {device.calibration
                                      ? <b>ΔE {device.calibration.averageDeltaE}</b>
                                      : <b>{t("待校色")}</b>}
                                  </span>
                                  <small>{device.calibration
                                    ? `${calibrationQualityLabel(device.calibration)} · ${new Date(device.calibration.createdAt).toLocaleDateString(activeLocaleTag())}`
                                    : tRuntime("写入标准六色色卡并上传照片，建立这台设备专属的颜色 Profile。")}</small>
                                </div>
                              </div>
                              {device.calibration && (
                                <div className="device-calibration-swatches" aria-label={tRuntime("校色后测得的六色色板")}>
                                  {device.calibration.samples.map((sample) => (
                                    <span
                                      key={sample.key}
                                      title={`${sample.label} · ΔE ${sample.deltaE}`}
                                      style={{ background: `rgb(${sample.measured.join(",")})` }}
                                    ><i>{sample.label}</i></span>
                                  ))}
                                </div>
                              )}
                              <div className="device-calibration-actions">
                                <button
                                  type="button"
                                  role="switch"
                                  aria-checked={device.colorCorrectionEnabled}
                                  className={`switch ${device.colorCorrectionEnabled ? "on" : ""}`}
                                  onClick={() => toggleDeviceColorCorrection(device.id, !device.colorCorrectionEnabled)}
                                  title={device.colorCorrectionEnabled ? tRuntime("关闭设备色差纠正") : tRuntime("开启设备色差纠正")}
                                ><span /></button>
                                <button type="button" onClick={() => openDeviceCalibration(device.id)}>
                                  {device.calibration ? tRuntime("重新校色") : tRuntime("开始校色")}
                                </button>
                              </div>
                            </section> : (
                              <section className="device-adapter-panel" aria-label={`${device.name} ${t("设备信息")}`}>
                                <div>
                                  <span className="device-calibration-mark" aria-hidden="true">W</span>
                                  <div><strong>{deviceSku(device.skuId)?.displayName ?? device.name}</strong><small>{deviceSku(device.skuId) ? t(deviceSku(device.skuId)!.description) : ""}</small></div>
                                </div>
                                <dl>
                                  <div><dt>{t("适配器")}</dt><dd>{deviceAdapter(device.skuId).id}</dd></div>
                                  <div><dt>{t("渲染")}</dt><dd>{t(deviceAdapter(device.skuId).taskStatusCopy)}</dd></div>
                                  <div><dt>{t("写入")}</dt><dd>{t("设备 HTTPS 主动拉取")}</dd></div>
                                  <div><dt>{t("同步")}</dt><dd>{device.appliedRevision ?? 0} / {device.desiredRevision ?? 0}</dd></div>
                                  <div><dt>{t("固件")}</dt><dd>{device.firmwareVersion ?? tRuntime("待上报")}</dd></div>
                                </dl>
                              </section>
                            )}
                            {device.tasks.length ? (
                              <div className="device-history-task-list">
                                {device.tasks.map((task) => (
                                  <DeviceTaskCard
                                    key={task.id}
                                    task={task}
                                    now={secondTick}
                                    onRetry={retryDeviceTask}
                                    onStop={stopDeviceTask}
                                  />
                                ))}
                              </div>
                            ) : (
                              <div className="device-history-empty">
                                <strong>{t("暂无运行中的刷新任务")}</strong>
                                <p>{t("在创作台选择每小时、每天或自定义计划并开始写入后，任务会显示在这里。")}</p>
                              </div>
                            )}
                          </div>
                        )}
                      </article>
                    );
                  })}
                </div>
              ) : (
                <div className="device-registry-empty">
                  <span className="status-dot idle" aria-hidden="true" />
                  <div><strong>{t("还没有连接过设备")}</strong><p>{t("点击“添加设备”，授权后会保留在这个列表中。")}</p></div>
                  <button type="button" onClick={openAddDevice}>{t("添加设备")}</button>
                </div>
              )}
            </section>
            <div className="feasibility-grid">
              <article className="verdict-card yes">
                <span>{t("可以做到")}</span>
                <h2>{t("同一会话自动重连")}</h2>
                <p>{t("保留 BluetoothDevice，定时到点后连接 GATT、写入、断开。支持 getDevices() 时，下次访问也可找回已授权设备。")}</p>
              </article>
              <article className="verdict-card caution">
                <span>{t("ESP32 模式")}</span>
                <h2>{t("设备主动同步")}</h2>
                <p>{t("计划保存在设备端；开机联网后每 15 秒同步变更，按本机时钟执行并拉取最新画面。")}</p>
              </article>
              <article className="verdict-card no">
                <span>{t("无法保证")}</span>
                <h2>{t("蓝牙仍依赖浏览器")}</h2>
                <p>{t("TodooCard 的定时任务仍受后台节流、系统休眠与设备唤醒影响；已有操作习惯保持不变。")}</p>
              </article>
            </div>
            <div className="protocol-table">
              <div><span>{t("传输协议")}</span><strong>BLE GATT</strong></div>
              <div><span>{t("设备服务")}</span><strong>FEF0 · FEF1 · FEF2</strong></div>
              <div><span>{t("单次数据")}</span><strong>{t("219,120 bytes · 913 包")}</strong></div>
              <div><span>{t("显色时间")}</span><strong>{t("复杂画面可能约 3 分钟")}</strong></div>
            </div>
            <div className="reliability-note">
              <span>{t("长期无人值守建议")}</span>
              <p>{t("M5 PaperColor 已采用设备侧调度；离线期间不会抓取，重新开机联网后会同步服务器上的新增、更新与删除。")}</p>
            </div>
          </section>
        )}
      </section>

      {addDeviceOpen && (
        <div className="calibration-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) closeAddDevice();
        }}>
          <section className="add-device-dialog" role="dialog" aria-modal="true" aria-labelledby="add-device-title">
            <header className="calibration-header">
              <div>
                <span className="eyebrow">ADD DEVICE · ADAPTER READY</span>
                <h2 id="add-device-title">
                  {addDeviceStep === "family" ? tRuntime("添加设备") : selectedDeviceSku?.displayName ?? tRuntime("添加 ESP32 设备")}
                </h2>
                <p>{t("现有蓝牙流程保持不变；Wi‑Fi 设备绑定后由硬件端保存计划并主动同步。")}</p>
              </div>
              <button type="button" onClick={closeAddDevice} disabled={deviceFlowBusy} aria-label={tRuntime("关闭添加设备")}>×</button>
            </header>

            {addDeviceStep === "family" && (
              <div className="device-family-grid">
                <button type="button" onClick={() => void selectNewBluetoothDevice()}>
                  <span className="device-choice-icon bluetooth">⌁</span>
                  <strong>{t("蓝牙设备")}</strong>
                  <p>{t("TodooCard · 浏览器直接写入，原有使用方式不变。")}</p>
                  <small>{bluetoothSupported ? tRuntime("当前浏览器蓝牙可用") : tRuntime("需要桌面 Chrome / Edge 或 Android Chromium")}</small>
                  <i>{t("选择设备")} →</i>
                </button>
                <button type="button" onClick={() => setAddDeviceStep("sku")}>
                  <span className="device-choice-icon wifi">W</span>
                  <strong>{t("ESP32 设备")}</strong>
                  <p>{t("通过 Wi‑Fi 拉取任务，关掉创作台后仍由设备按计划运行。")}</p>
                  <small>{t("支持设备码绑定与 USB 在线刷机")}</small>
                  <i>{t("选择型号")} →</i>
                </button>
              </div>
            )}

            {addDeviceStep === "sku" && (
              <div className="device-flow-panel">
                <button type="button" className="device-flow-back" onClick={() => setAddDeviceStep("family")}>← {t("返回设备类型")}</button>
                <h3>{t("选择具体型号")}</h3>
                <div className="device-sku-grid">
                  {deviceSkusForFamily("esp32").map((sku) => (
                    <button type="button" key={sku.id} onClick={() => {
                      setSelectedDeviceSkuId(sku.id as DeviceSkuId);
                      setAddDeviceStep("method");
                    }}>
                      <span className="sku-screen"><b>M5</b><i>INKLOOP</i></span>
                      <div><strong>{sku.displayName}</strong><p>{t(sku.description)}</p><small>{sku.screen.width} × {sku.screen.height} · {sku.write.strategy === "https-image-pull" ? tRuntime("Wi‑Fi 拉取") : sku.write.strategy}</small></div>
                      <em>→</em>
                    </button>
                  ))}
                </div>
              </div>
            )}

            {addDeviceStep === "method" && selectedDeviceSku && (
              <div className="device-flow-panel">
                <button type="button" className="device-flow-back" onClick={() => setAddDeviceStep("sku")}>← {t("返回型号")}</button>
                <h3>{t("这台设备准备好了吗？")}</h3>
                <div className="device-method-grid">
                  <button type="button" onClick={() => setAddDeviceStep("claim")}>
                    <b>01</b><strong>{t("输入六位设备码")}</strong><p>{t("瘦客户端已运行，屏幕上正在显示绑定码。")}</p><i>{t("立即绑定")} →</i>
                  </button>
                  <button type="button" onClick={() => setAddDeviceStep("flash")}>
                    <b>02</b><strong>{t("给设备刷机")}</strong><p>{t("通过 USB 写入 Inkloop 瘦客户端，首次启动后再绑定。")}</p><i>{t("开始刷机")} →</i>
                  </button>
                </div>
              </div>
            )}

            {addDeviceStep === "claim" && (
              <div className="device-flow-panel device-code-panel">
                <button type="button" className="device-flow-back" onClick={() => setAddDeviceStep("method")}>← {t("返回连接方式")}</button>
                <span className="device-code-mark">6</span>
                <h3>{t("输入屏幕上的六位设备码")}</h3>
                <p>{t("保持 M5 PaperColor 开机并联网。绑定成功后，任务会存储到设备端。")}</p>
                <input
                  type="text"
                  inputMode="numeric"
                  autoComplete="one-time-code"
                  autoFocus
                  maxLength={6}
                  value={deviceCode}
                  onChange={(event) => {
                    setDeviceCode(event.target.value.replace(/\D/g, "").slice(0, 6));
                    setDeviceFlowError(null);
                  }}
                  onKeyDown={(event) => {
                    if (event.key === "Enter") void bindEsp32Device();
                  }}
                  aria-label={tRuntime("六位设备码")}
                  placeholder="000000"
                />
                {deviceFlowError && <p className="device-flow-error" role="alert">{deviceFlowError}</p>}
                <button type="button" className="device-flow-primary" onClick={() => void bindEsp32Device()} disabled={deviceFlowBusy || deviceCode.length !== 6}>
                  {deviceFlowBusy ? tRuntime("正在绑定…") : tRuntime("绑定设备")}
                </button>
              </div>
            )}

            {addDeviceStep === "flash" && (
              <div className="device-flow-panel device-flash-panel">
                <button type="button" className="device-flow-back" onClick={() => setAddDeviceStep("method")} disabled={deviceFlowBusy}>← {t("返回连接方式")}</button>
                <h3>{t("插入 M5 PaperColor")}</h3>
                <p>{t("使用可传输数据的 USB 线连接设备和电脑。浏览器会在下一步让你选择串口，写入过程中不要拔线。")}</p>
                <ol>
                  <li><b>1</b><span><strong>{t("连接设备")}</strong><small>{t("请使用桌面版 Chrome 或 Edge")}</small></span></li>
                  <li><b>2</b><span><strong>{t("写入瘦客户端")}</strong><small>{t("不会读取电脑上的其他文件")}</small></span></li>
                  <li><b>3</b><span><strong>{t("配置 Wi‑Fi 并绑定")}</strong><small>{t("首次启动屏幕会给出引导和六位码")}</small></span></li>
                </ol>
                {firmwareProgress && (
                  <div className="firmware-progress" aria-live="polite">
                    <span><i style={{ width: `${firmwareProgress.percent}%` }} /></span>
                    <div><strong>{firmwareProgress.message}</strong><b>{firmwareProgress.percent}%</b></div>
                  </div>
                )}
                {deviceFlowError && <p className="device-flow-error" role="alert">{deviceFlowError}</p>}
                <button type="button" className="device-flow-primary" onClick={() => void flashEsp32Device()} disabled={deviceFlowBusy}>
                  {deviceFlowBusy ? tRuntime("正在写入，请勿拔线…") : tRuntime("选择串口并开始刷机")}
                </button>
              </div>
            )}

            {addDeviceStep === "flash-complete" && (
              <div className="device-flow-panel device-flash-complete">
                <span>{deviceCode ? "✓" : firmwareMonitoring ? "…" : "!"}</span>
                <h3>{deviceCode ? t("发现可绑定的设备") : t("固件已写入并自动重启")}</h3>
                <p>{deviceCode
                  ? `${t("串口检测到六位设备码")} ${deviceCode}。${t("是否将这台设备绑定到当前浏览器？")}`
                  : firmwareMonitoring
                    ? t("正在监听启动日志。请按屏幕提示完成 Wi‑Fi 配置，六位码出现后即可直接绑定。")
                    : t("串口监听已结束；你仍可输入设备屏幕上的六位码完成绑定。")}</p>
                {firmwareAccessPoint && (
                  <div className="firmware-access-point">
                    <small>{t("等待 Wi‑Fi 配置")}</small>
                    <strong>{firmwareAccessPoint}</strong>
                  </div>
                )}
                {deviceCode && <strong className="firmware-pairing-code">{deviceCode}</strong>}
                {firmwareLogs.length > 0 && (
                  <details className="firmware-console" open={!deviceCode}>
                    <summary>{t("串口调试日志")} · {firmwareLogs.length}</summary>
                    <pre>{firmwareLogs.join("\n")}</pre>
                    <button type="button" onClick={() => {
                      void navigator.clipboard.writeText(firmwareLogs.join("\n"));
                      showToast(tRuntime("调试日志已复制"), "success");
                    }}>{t("复制日志给 AI")}</button>
                  </details>
                )}
                {deviceFlowError && <p className="device-flow-error" role="alert">{deviceFlowError}</p>}
                {deviceCode ? (
                  <button type="button" className="device-flow-primary" onClick={() => void bindEsp32Device()} disabled={deviceFlowBusy}>
                    {deviceFlowBusy ? tRuntime("正在绑定…") : tRuntime("绑定到当前浏览器")}
                  </button>
                ) : (
                  <button type="button" className="device-flow-primary" onClick={() => {
                    firmwareStopRef.current?.();
                    firmwareStopRef.current = null;
                    setFirmwareMonitoring(false);
                    setDeviceFlowError(null);
                    setAddDeviceStep("claim");
                  }}>{t("手动输入六位设备码")}</button>
                )}
                <button type="button" className="device-flow-secondary" onClick={closeAddDevice}>{t("稍后再绑定")}</button>
              </div>
            )}
          </section>
        </div>
      )}

      {calibrationDevice && (
        <div className="calibration-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) closeDeviceCalibration();
        }}>
          <section className="calibration-dialog" role="dialog" aria-modal="true" aria-labelledby="calibration-title">
            <header className="calibration-header">
              <div>
                <span className="eyebrow">DEVICE COLOR PROFILE</span>
                <h2 id="calibration-title">{t("校准")} {calibrationDevice.name}</h2>
                <p>{t("用一次标准色卡测量这台屏幕，再为后续写入自动修正六色量化。")}</p>
              </div>
              <button type="button" onClick={closeDeviceCalibration} disabled={calibrationBusy} aria-label={tRuntime("关闭设备校色")}>×</button>
            </header>

            <ol className="calibration-progress" aria-label={`${t("校色进度：第")} ${calibrationStep} ${t("步，共 3 步")}`}>
              {[tRuntime("写入色卡"), tRuntime("拍照上传"), tRuntime("确认 Profile")].map((label, index) => {
                const step = (index + 1) as 1 | 2 | 3;
                return <li key={label} className={calibrationStep === step ? "current" : calibrationStep > step ? "complete" : ""}>
                  <b>{String(step).padStart(2, "0")}</b><span>{label}</span>
                </li>;
              })}
            </ol>

            {calibrationStep === 1 && (
              <div className="calibration-step calibration-write-step">
                <div className="calibration-card-preview" aria-label={tRuntime("标准六色色卡预览")}>
                  {CALIBRATION_SWATCHES.map((swatch) => (
                    <div
                      key={swatch.key}
                      className={swatch.key === "black" || swatch.key === "blue" ? "dark" : ""}
                      style={{ background: `rgb(${swatch.expected.join(",")})` }}
                    ><strong>{swatch.label}</strong><small>{swatch.key.toUpperCase()}</small></div>
                  ))}
                </div>
                <div className="calibration-instructions">
                  <span className="calibration-step-number">{t("步骤 1")}</span>
                  <h3>{t("先把标准色卡写入屏幕")}</h3>
                  <p>{t("保持设备在电脑附近。写入完成后，等待屏幕颜色完全稳定，再进入拍照步骤。")}</p>
                  <ul>
                    <li>{t("使用协议原生六色色带，不受当前应用与校色 Profile 影响。")}</li>
                    <li>{t("写入约需 1 分钟，电子纸继续显色可能再等几分钟。")}</li>
                  </ul>
                  <div className="calibration-step-actions">
                    <button type="button" className="calibration-primary" onClick={() => void writeCalibrationCard()} disabled={calibrationBusy}>
                      {calibrationBusy ? progress?.message || tRuntime("正在写入标准色卡") : tRuntime("写入标准色卡")}
                    </button>
                    <button
                      type="button"
                      className="calibration-secondary"
                      aria-label={tRuntime("色卡已写入，跳过写入并进入拍照步骤")}
                      onClick={() => {
                        setCalibrationError(null);
                        setCalibrationStep(2);
                      }}
                      disabled={calibrationBusy}
                    >{t("已写入")}</button>
                  </div>
                </div>
              </div>
            )}

            {calibrationStep === 2 && (
              <div className="calibration-step calibration-photo-step">
                <div className="calibration-camera-guide" aria-hidden="true">
                  <div className="calibration-camera-frame">
                    {CALIBRATION_SWATCHES.map((swatch) => (
                      <i key={swatch.key} style={{ background: `rgb(${swatch.expected.join(",")})` }} />
                    ))}
                  </div>
                  <span>{t("让屏幕边缘贴近取景框")}</span>
                </div>
                <div className="calibration-instructions">
                  <span className="calibration-step-number">{t("步骤 2")}</span>
                  <h3>{t("正对屏幕拍一张照片")}</h3>
                  <p>{t("让屏幕完整出现在画面中央，避免灯光反射；横拍、竖拍都可以，系统会自动找边缘并旋转。")}</p>
                  <ul>
                    <li>{t("自动裁掉屏幕外区域，并把检测到的画面缩放到标准尺寸。")}</li>
                    <li>{t("系统会用黑、白色带抵消曝光和相机白平衡。")}</li>
                    <li>{t("照片只在当前浏览器分析，不上传也不保存。")}</li>
                  </ul>
                  <input
                    ref={calibrationFileInputRef}
                    className="visually-hidden"
                    type="file"
                    accept="image/*"
                    capture="environment"
                    onChange={(event) => void handleCalibrationPhoto(event.target.files?.[0])}
                  />
                  <button
                    type="button"
                    className="calibration-primary"
                    onClick={() => calibrationFileInputRef.current?.click()}
                    disabled={calibrationBusy}
                  >{calibrationBusy ? t("正在分析照片") : t("拍照或选择照片")}</button>
                  <button type="button" className="calibration-secondary" onClick={() => setCalibrationStep(1)} disabled={calibrationBusy}>{t("重新写入色卡")}</button>
                </div>
              </div>
            )}

            {calibrationStep === 3 && calibrationDraft && (
              <div className="calibration-step calibration-result-step">
                <figure className="calibration-photo-preview">
                  {calibrationPhoto && <img src={calibrationPhoto} alt={tRuntime("自动检测边缘、裁切和旋转后的设备校色色卡")} />}
                  <figcaption>
                    {t("已自动检测边缘")} · {calibrationDraft.capture?.axis === "vertical" ? tRuntime("纵向色带") : tRuntime("横向色带")}
                    {calibrationDraft.capture?.rotation ? ` · ${t("已旋转")} ${calibrationDraft.capture.rotation}°` : ""}
                    {calibrationDraft.capture?.confidence ? ` · ${t("置信度")} ${calibrationDraft.capture.confidence}%` : ""}
                  </figcaption>
                </figure>
                <div className="calibration-result">
                  <span className="calibration-step-number">{t("步骤 3")}</span>
                  <div className="calibration-score">
                    <div><small>{t("平均色差")}</small><strong>ΔE {calibrationDraft.averageDeltaE}</strong></div>
                    <b className={calibrationDraft.quality}>{calibrationQualityLabel(calibrationDraft)}</b>
                  </div>
                  <div className="calibration-comparison" aria-label={tRuntime("标准颜色与照片测量颜色对比")}>
                    {calibrationDraft.samples.map((sample) => (
                      <div key={sample.key}>
                        <span>
                          <i style={{ background: `rgb(${sample.expected.join(",")})` }} title={`${sample.label}${t("标准色")}`} />
                          <i style={{ background: `rgb(${sample.measured.join(",")})` }} title={`${sample.label}${t("测量色")}`} />
                        </span>
                        <strong>{sample.label}</strong>
                        <small>ΔE {sample.deltaE}</small>
                      </div>
                    ))}
                  </div>
                  <p>{t("保存后，这台设备的图片会按照实测六色重新分配颜色；纯色颜料本身不会被软件改变。")}</p>
                  <div className="calibration-result-actions">
                    <button type="button" className="calibration-secondary" onClick={() => {
                      setCalibrationStep(2);
                      setCalibrationDraft(null);
                      setCalibrationPhoto(null);
                    }}>{t("重新拍照")}</button>
                    <button type="button" className="calibration-primary" onClick={saveDeviceCalibration}>{t("保存并启用")}</button>
                  </div>
                </div>
              </div>
            )}

            {calibrationError && <p className="calibration-error" role="alert"><b>!</b><span>{calibrationError}</span></p>}
          </section>
        </div>
      )}

      {guideOpen && (
        <div className="guide-backdrop" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) setGuideOpen(false);
        }}>
          <section className="guide-dialog" role="dialog" aria-modal="true" aria-labelledby="guide-title">
            <div className="guide-header">
              <div>
                <span className="eyebrow">FROM FIRST SCREEN TO AUTOMATION</span>
                <h2 id="guide-title">{t("三步用好 Inkloop")}</h2>
                <p>{t("先做一张满意的画面，再保存应用、选择刷新时间，最后写入屏幕。")}</p>
              </div>
              <button type="button" className="guide-close" onClick={() => setGuideOpen(false)} aria-label={tRuntime("关闭使用说明")}>×</button>
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
                <strong>{t("写入前，把设备放在电脑附近")}</strong>
                <p>{t("建议保持在 1–3 米蓝牙范围内并唤醒屏幕。首次点击“开始写入”后选择 PICKSMART；之后保持这个页面打开、电脑不要休眠，就能按计划自动重连。")}</p>
              </div>
              <button type="button" onClick={() => { setGuideOpen(false); navigateToTab("device"); }}>{t("查看设备说明")}</button>
            </div>

            <p className="guide-footnote">{t("本机图片随应用保存在当前浏览器；公开分享时不会上传你的私人图片，而会保留可复用的版式和主题。")}</p>
          </section>
        </div>
      )}

      {toast && <div className={`toast ${toast.tone}`} role="status"><span>{toast.tone === "success" ? "✓" : toast.tone === "error" ? "!" : "i"}</span>{toast.message}</div>}
    </main>
  );
}
