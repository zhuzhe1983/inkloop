import { env } from "cloudflare:workers";
import {
  displaySettings,
  generateInkApp,
  inferWeatherCity,
  type ArtworkSpec,
  type AgendaRangeMode,
  type AgendaView,
  type ClockSpec,
  type InkApp,
  type MapLocationMode,
  type MapSpec,
  type MapStyle,
  type ScreenKind,
  type ScreenFont,
  type ScreenDisplay,
  type ScreenSpec,
  type ScreenTable,
} from "../../lib/app-model";

const DEFAULT_BASE_URL = `https://hub.${["tsing", "fly"].join("")}.com/v1`;
const MODEL_PREFERENCES = [
  "Qwen/Qwen3.6-27B",
  "Apsara-Stack/GLM-5.1-W4A8",
  "deepseek-v4-flash",
  "qwen3-32b",
];

const ALLOWED_KINDS = new Set<ScreenKind>(["weather", "focus", "countdown", "meeting", "metric", "calendar", "timetable", "agenda", "map"]);
const ALLOWED_ACCENTS = new Set<ScreenSpec["accent"]>(["red", "blue", "green", "yellow"]);
const ALLOWED_ARTWORK_MODES = new Set<ArtworkSpec["mode"] | "none">(["none", "generated", "web"]);
const ALLOWED_ARTWORK_MOTIFS = new Set<ArtworkSpec["motif"]>([
  "rainbow",
  "sunburst",
  "confetti",
  "waves",
  "grid",
]);
const ALLOWED_ARTWORK_LAYOUTS = new Set<ArtworkSpec["layout"]>(["background", "hero", "fullscreen"]);
const ALLOWED_CLOCK_FONTS = new Set<ClockSpec["font"]>([
  "sans",
  "serif",
  "rounded",
  "mono",
  "handwritten",
  "random",
]);
const ALLOWED_SCREEN_FONTS = new Set<ScreenFont>(["sans", "serif", "rounded", "mono", "handwritten"]);

const EPAPER_DESIGN_GUIDE = `
默认视觉方向是“高级纸张便利贴 + 瑞士编辑排版”，而不是彩色电子屏 UI：
- 六色仅指纸白、墨黑、黄、红、蓝、绿六个真实色值。纸白应占画面 70%—85%，墨黑负责全部正文和主要结构；每页通常只选 1 个主强调色，必要时再加 1 个小面积辅助色。
- 推荐配色：纸白+墨黑+黄用于温暖提醒和待办；纸白+墨黑+蓝用于日程、数据和理性信息；纸白+墨黑+绿用于习惯、完成和自然主题；纸白+墨黑+红只用于截止时间、风险或极少量视觉锚点。不要平均使用六种颜色，不要大面积红绿对撞。
- “插值”不是生成设备不存在的灰色或透明色。需要中间调时，只允许在照片、插画、纸张纹理和大面积装饰中，用相邻两种真实颜色做稀疏有序抖动（建议 12.5%、25% 或 50% 网点密度）模拟层次；禁止对正文、小字号、细线和二维码做抖动或抗锯齿，文字必须保持纯墨黑/纸白的硬边高对比。
- 便利贴方案：40—48px 外边距，8px 基础网格；一张主便签只承载一个核心信息。可用一条窄黄色胶带、一个折角、一个圆点或 6—8px 的纯黑错位投影表达纸张感，四者最多选两个。边框保持 1—2px，禁止多重描边、拟真渐变和大面积阴影。
- 简单设计方案：优先“标签 / 主标题 / 大号值 / 一行补充”四级以内；全页最多 3 个视觉组、最多 5 个文本元素。标题短而粗，正文自然换行，数字使用稳定的等宽感；通过字号、留白和对齐建立层级，不靠堆颜色。
- 可选择四种克制方向：sticky-note（温暖便签）、swiss-editorial（非对称网格与大字）、quiet-checklist（绿色圆点与清单）、urgent-note（红色仅标截止信息）。没有明确主题时优先 sticky-note 或 swiss-editorial。
- 图片背景仍需全屏无边框裁剪；文字只放在低细节区域，以纯白字、细黑描边和轻微白色光晕保证可读，不放大块不透明信息板遮住主体。
- 永远不要添加品牌签名、设备型号、水印、产品脚注、伪按钮或无意义装饰。成品应像安静、耐看的纸品设计，第一眼只看到最重要的信息。`;

const SYSTEM_PROMPT = `你是 Inkloop 的电子墨水屏应用编程助手，同时也是视觉造型师和美术指导。根据用户需求生成一个 TodooCard 应用。

只返回一个 JSON 对象，不要 Markdown，不要解释。JSON 结构必须是：
{
  "title": "应用名，最多20个汉字",
  "description": "一句用途说明",
  "spec": {
    "kind": "weather|focus|countdown|meeting|metric|calendar|timetable|agenda|map",
    "orientation": "portrait|landscape",
    "city": "天气应用填写城市，其他应用省略",
    "eyebrow": "屏幕顶部短标签",
    "title": "屏幕标题",
    "value": "最重要的大号值",
    "unit": "单位，没有则为空字符串",
    "detail": "一行详情",
    "footer": "一行行动建议或补充信息",
    "accent": "red|blue|green|yellow",
    "artwork": {
      "mode": "none|generated|web",
      "motif": "rainbow|sunburst|confetti|waves|grid",
      "query": "用于联网找图的主体英文关键词",
      "style": "用于控制视觉风格的2—5个英文关键词",
      "layout": "background|hero|fullscreen",
      "rotateOnRefresh": false
    },
    "clock": {
      "enabled": false,
      "board": true,
      "font": "sans|serif|rounded|mono|handwritten|random"
    },
    "map": {
      "locationMode": "picker|browser|ip",
      "query": "地点、地址或 POI；不确定时留空，交给用户选点",
      "zoomLevel": 17,
      "style": "eink|balanced|detail",
      "marker": true,
      "showAddress": true,
      "showCoordinates": false
    },
    "display": {
      "quote": false,
      "logo": false,
      "date": false,
      "time": false,
      "timeLarge": false,
      "weather": false,
      "weatherLarge": false,
      "qr": false,
      "border": false,
      "font": "sans|serif|rounded|mono|handwritten",
      "logoText": "INKLOOP",
      "qrMode": "text|wifi",
      "qrText": "二维码包含的网址或文字",
      "qrWifiSsid": "Wi-Fi 名称，未要求则留空",
      "qrWifiPassword": "不要编造密码，默认留空",
      "qrWifiSecurity": "WPA|WEP|nopass",
      "qrWifiHidden": false
    },
    "table": {
      "type": "calendar|timetable|agenda",
      "year": 2026,
      "month": 8,
      "weekStartsOn": "monday|sunday",
      "lunar": false,
      "columns": ["周一", "周二", "周三", "周四", "周五"],
      "rows": [{ "label": "08:00", "cells": ["语文", "数学", "英语", "体育", "美术"] }],
      "view": "agenda|three-day|workweek",
      "rangeMode": "rolling|today|custom",
      "rangeHours": 72,
      "customStart": "2026-08-03T09:00",
      "customEnd": "2026-08-05T22:00",
      "eventWidth": 100,
      "showEndTime": true,
      "showLocation": true,
      "events": [{ "day": 8, "text": "月历事件" }, { "uid": "preview-1", "title": "日程事件", "start": "2026-08-03T10:00:00+08:00", "end": "2026-08-03T11:00:00+08:00", "location": "会议室 A", "allDay": false }]
    }
  },
  "code": "可供用户审阅的 JavaScript 业务逻辑源码字符串",
  "scheduleMode": "once|hourly|daily|custom",
  "customMinutes": 30,
  "dailyTime": "08:00"
}

约束：
1. 代码只用于审阅，不使用 eval，不包含密钥，不直接调用蓝牙。
2. 代码通过 render(ctx) 返回与 spec 对应的数据，外部数据使用 ctx.weather、ctx.calendar 或 ctx.data 等抽象接口。
3. 屏幕竖版为 528×792，横版为 792×528，只支持黑、白、黄、红、蓝、绿六色；内容必须短而清晰。orientation 可由你根据内容建议，月历通常竖版，课程表、三日和工作周日程通常横版；用户明确要求横版或竖版时必须遵循。
4. 只有用户明确提到刷新时间或周期时才设置定时；“随机图片”只表示换图，不代表每小时刷新。没有周期要求时必须使用 once。
5. 不编造真实个人数据；示例值应明显是合理预览。
6. 用户要求图片、照片、背景、插画或明显视觉主题时，artwork.mode 不能是 none。
7. 彩虹、放射、彩纸、波浪、网格等抽象图形使用 generated 并选择最接近的 motif；人物、城市、产品、动物、自然等真实题材使用 web。
8. web 的 query 必须是 2—6 个具体英文关键词，准确概括用户要求的主体和场景；视觉风格单独写入 style。例如 OOTD 的 query 可写为 "outfit of the day"，style 写为 "street style editorial photography"，不要只写 image、random、beautiful 等泛词。
9. 不返回图片 URL；系统会用 query 从主题图库随机取图并缓存素材。
10. 文本字段只允许这些运行时变量：{{date}}、{{year}}、{{month}}、{{day}}、{{weekday}}、{{hour}}、{{minute}}、{{time}}。禁止输出 {{weather.*}}、{{#if}} 或其他模板语法；天气数据由系统根据 city 自动注入，spec 中填写清晰的预览文案。
11. 用户要求时钟时，clock.enabled=true；board 表示是否在画板中显示时间；不同字体或每页换字体使用 font=random。刷新计划用 custom，最小 customMinutes=1。
12. 用户要求每次换背景时，artwork.rotateOnRefresh=true。女性人物主题使用 woman 而不是 girl，禁止生成或搜索未成年人；画板由 clock.board 控制，不要求照片本身带画板。
13. 用户明确要求“全屏图片、不要文字、不要其他内容”时，artwork.layout=fullscreen；此模式不会绘制任何标题、边框、页脚或信息卡。
14. 用户要求“图片做背景”时使用 artwork.layout=background：系统会优先取 1056×1584 的 2 倍竖屏素材，再高质量缩小并居中裁剪到 528×792 全屏无边框显示。只有独立图片区域才使用 hero。
15. 每张背景图都必须填写 artwork.style。优先遵循用户点名的风格；未指定时，像专业 stylist 一样选择适合主题的 editorial、cinematic、minimal、vintage、fashion 或 illustration 风格，并强调主体清晰、高对比、构图简洁，适合六色墨水屏。
16. 只有用户需求明确包含天气、气温、温度、下雨、降雨或通勤信息时，spec.kind 才能使用 weather；海报、图片、时钟等其他需求不得使用 weather。
17. 用户点名漫威、蜘蛛侠、钢铁侠、美国队长、复仇者联盟等明确主题时，query 必须保留对应的英文专名，不能泛化成 colorful illustration、superhero 或 cat 等无关主题。
18. 用户没有明确要求图片、背景、海报、插画或视觉主题时，artwork.mode 必须是 none；信息卡优先使用清晰的纯文字排版。
19. 海报不添加品牌签名、产品型号、水印或“6-COLOR E-PAPER”等脚注。避免无理由使用满屏高饱和蓝色、重复粗线和厚重发光描边；优先留白、清晰层级和至多两种强调色。
20. display 控制可手动编辑的画面组件。time 是顶部小时间，timeLarge 是画面主视觉大时间；weather 是角落的一行小天气摘要，weatherLarge 是把城市、天气、温度和高低温组合成一个整体的大天气组件。天气应用默认使用 weatherLarge，二者不要同时开启。时钟默认使用 timeLarge。qr 是二维码元素，只有用户明确要求二维码时开启；普通内容使用 qrMode=text 并把内容原样放进 qrText。用户明确要求 Wi-Fi/WPA 二维码时使用 qrMode=wifi，填写 qrWifiSsid 和 qrWifiSecurity，但绝不编造 Wi-Fi 密码，qrWifiPassword 默认留空供用户手动填写。border 只控制整张屏幕最外侧的细框，不给文字、画板或其他组件加框；默认且通常必须是 false。logo 只有用户明确要求 LOGO/品牌文字时开启；其他组件只按用户需求开启。
21. 用户要求月历、日历或月度计划时使用 kind=calendar，table.type=calendar，提供 year、month、weekStartsOn 和最多 12 个简短 events；明确要求显示农历时 lunar=true。不需要输出 42 个日期格，系统会按月份计算。
22. 用户要求课程表、课表或周时间表时使用 kind=timetable，table.type=timetable；columns 为 2—7 个列标题，rows 为 1—8 个时间段，每个 cells 长度与 columns 一致，每格最多 8 个汉字。表格数据只表达语义，不包含坐标、HTML、CSS 或绘图代码。
23. 用户要求苹果日历、日程安排、议程、未来几天安排或带起止时间的周日历时使用 kind=agenda，table.type=agenda。默认 orientation=landscape、view=three-day、rangeMode=rolling、rangeHours=72；用户要求五天时使用 view=workweek、rangeHours=120。eventWidth 控制日程块宽度，密集五日视图可设为 70—85；showEndTime 和 showLocation 可按用户希望的简洁程度关闭。只需要生成 3—8 条明显为预览数据的事件，真实事件会由 iCal 在运行时注入。不要把日程当成固定课表，不要为没有事件的小时生成空行。
24. 用户要求地图、附近位置、入口位置或周边导览时使用 kind=map 并填写 map。优先使用 locationMode=picker，让用户在右侧确认准确位置；只有明确要求浏览器定位时用 browser，明确要求 IP 粗定位时用 ip。不要编造经纬度，也不要把密钥或地图 URL 写进 spec。zoomLevel 为 3—19，街区/入口通常 17—19，城市概览通常 10—13。marker、地址、坐标显示按用户要求设置，之后都允许在右侧手动修改。

六色电子纸视觉规范（生成任何应用时都必须遵守）：
${EPAPER_DESIGN_GUIDE}`;

type GatewayModel = { id?: unknown };
type GatewayModels = { data?: GatewayModel[]; models?: GatewayModel[] };
type ChatCompletion = {
  choices?: Array<{ message?: { content?: unknown } }>;
};

function trimText(value: unknown, fallback: string, max: number) {
  return typeof value === "string" && value.trim() ? value.trim().slice(0, max) : fallback;
}

function screenText(value: unknown, fallback: string, max: number) {
  const text = trimText(value, fallback, max);
  const withoutAllowedVariables = text.replace(
    /\{\{(?:date|year|month|day|weekday|hour|minute|time)\}\}/g,
    "",
  );
  return /\{\{|\}\}/.test(withoutAllowedVariables) ? fallback : text;
}

function wantsFullscreenArtwork(prompt: string) {
  const imageOnly = ["不要任何其他", "不要其他", "不要文字", "只有图片", "只要图片", "纯图片", "纯图"]
    .some((term) => prompt.includes(term));
  return imageOnly && (
    ["全屏", "铺满", "满屏"].some((term) => prompt.includes(term))
    || ["图片", "照片", "海报", "插画"].some((term) => prompt.includes(term))
  );
}

function wantsBackgroundArtwork(prompt: string) {
  return ["背景", "背景图", "做背景", "作为背景"].some((term) => prompt.includes(term));
}

function wantsArtwork(prompt: string) {
  return /图片|照片|摄影|背景|海报|插画|图案|彩虹|彩纸|礼花|波浪|网格|太阳|阳光|漫威|Marvel|蜘蛛侠|钢铁侠|美国队长|复仇者联盟|猫|猫咪|小猫|狗|宠物|边牧|边境牧羊犬|美女|女性|人物|城市|风景|产品图/i.test(prompt);
}

function wantsWeather(prompt: string) {
  return /天气|气温|温度|下雨|降雨|阵雨|通勤/.test(prompt);
}

function wantsCalendar(prompt: string) {
  return /月历|日历|月度计划|月计划/.test(prompt);
}

function wantsAgenda(prompt: string) {
  return /苹果日历|智能日程|周日程|日程安排|未来安排|行程表|议程|时间轴/.test(prompt);
}

function wantsTimetable(prompt: string) {
  return /课程表|课表|排课表|周时间表/.test(prompt);
}

function wantsMap(prompt: string) {
  return /地图|位置图|路线图|导航图|周边图|附近位置|入口位置/.test(prompt);
}

function namedArtworkQuery(prompt: string) {
  if (/边牧|边境牧羊犬/.test(prompt)) return "border collie dog portrait photography";
  if (/美女|女性|女人|女孩|人物时钟/.test(prompt)) return "fashion model woman portrait photography";
  if (/蜘蛛侠/.test(prompt)) return "Spider-Man superhero movie poster";
  if (/钢铁侠/.test(prompt)) return "Iron Man superhero movie poster";
  if (/美国队长/.test(prompt)) return "Captain America superhero movie poster";
  if (/复仇者联盟/.test(prompt)) return "Avengers superhero movie poster";
  if (/漫威|Marvel/i.test(prompt)) return "Marvel superhero movie poster";
  return "";
}

function resolveSchedule(prompt: string, fallback: InkApp, rawMinutes: number, rawTime: string) {
  const minuteMatch = prompt.match(/每\s*(\d{1,5})\s*分钟/);
  if (minuteMatch) {
    return {
      mode: "custom" as const,
      minutes: Math.max(1, Math.min(10080, Number(minuteMatch[1]) || 1)),
      dailyTime: fallback.dailyTime,
    };
  }
  if (/每(?:个)?小时|每小时/.test(prompt)) {
    return { mode: "hourly" as const, minutes: 60, dailyTime: fallback.dailyTime };
  }
  if (/每天|每日|早上|上午|下午|晚上/.test(prompt)) {
    const timeMatch = prompt.match(/(\d{1,2})\s*(?:[:：点时])\s*(\d{1,2})?/);
    const hour = Math.max(0, Math.min(23, Number(timeMatch?.[1]) || 8));
    const minute = Math.max(0, Math.min(59, Number(timeMatch?.[2]) || 0));
    return {
      mode: "daily" as const,
      minutes: Number.isFinite(rawMinutes) ? Math.max(1, Math.min(10080, Math.round(rawMinutes))) : 30,
      dailyTime: `${String(hour).padStart(2, "0")}:${String(minute).padStart(2, "0")}`,
    };
  }
  if (fallback.spec.clock?.enabled) {
    return { mode: "custom" as const, minutes: 1, dailyTime: fallback.dailyTime };
  }
  return {
    mode: "once" as const,
    minutes: Number.isFinite(rawMinutes) ? Math.max(1, Math.min(10080, Math.round(rawMinutes))) : 30,
    dailyTime: /^([01]\d|2[0-3]):[0-5]\d$/.test(rawTime) ? rawTime : fallback.dailyTime,
  };
}

function stableSeed(value: string) {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0) % 1_000_000 + 1;
}

function normalizeArtwork(
  value: unknown,
  fallback: ArtworkSpec | undefined,
  prompt: string,
): ArtworkSpec | undefined {
  // Pure information requests should stay typographic. The model must not
  // invent a decorative background that competes with the content.
  if (!fallback && !wantsArtwork(prompt)) return undefined;
  if (!value || typeof value !== "object") return fallback;
  const candidate = value as Record<string, unknown>;
  const mode = candidate.mode as ArtworkSpec["mode"] | "none";
  if (!ALLOWED_ARTWORK_MODES.has(mode) || mode === "none") return fallback;
  const rawQuery = trimText(candidate.query, fallback?.query || "colorful editorial illustration", 100);
  const query = rawQuery.replace(/[^a-zA-Z0-9\s,-]/g, " ").replace(/\s+/g, " ").trim();
  const rawStyle = trimText(
    candidate.style,
    fallback?.style || "editorial high contrast composition",
    80,
  );
  const style = rawStyle.replace(/[^a-zA-Z0-9\s,-]/g, " ").replace(/\s+/g, " ").trim()
    || "editorial high contrast composition";
  const requestedMotif = candidate.motif as ArtworkSpec["motif"];
  const motif = ALLOWED_ARTWORK_MOTIFS.has(requestedMotif)
    ? requestedMotif
    : query.toLowerCase().includes("rainbow")
      ? "rainbow"
      : "grid";
  const requestedLayout = candidate.layout as ArtworkSpec["layout"];
  const promptRequestsCat = /猫|猫猫|猫咪|小猫/.test(prompt);
  const namedQuery = namedArtworkQuery(prompt);
  const normalizedQuery = promptRequestsCat
    ? "cute cat portrait photography"
    : namedQuery || query || "colorful editorial illustration";
  return {
    mode: promptRequestsCat ? "web" : mode,
    motif,
    query: normalizedQuery,
    style,
    layout: wantsFullscreenArtwork(prompt)
      ? "fullscreen"
      : wantsBackgroundArtwork(prompt)
        ? "background"
        : ALLOWED_ARTWORK_LAYOUTS.has(requestedLayout)
          ? requestedLayout
          : fallback?.layout ?? "background",
    seed: stableSeed(`${prompt}:${normalizedQuery}:${style}:${motif}:${crypto.randomUUID()}`),
    rotateOnRefresh: candidate.rotateOnRefresh === true
      || fallback?.rotateOnRefresh === true
      || /随机|每次换|换一张|轮换/.test(prompt),
  };
}

function normalizeClock(value: unknown, fallback: ClockSpec | undefined): ClockSpec | undefined {
  if (!value || typeof value !== "object") return fallback;
  const candidate = value as Record<string, unknown>;
  if (candidate.enabled !== true) return fallback;
  const font = candidate.font as ClockSpec["font"];
  return {
    enabled: true,
    board: candidate.board !== false,
    font: ALLOWED_CLOCK_FONTS.has(font) ? font : "sans",
  };
}

function normalizeMap(value: unknown, fallback: MapSpec | undefined, prompt: string): MapSpec | undefined {
  if (!wantsMap(prompt) && !fallback) return undefined;
  const candidate = value && typeof value === "object" ? value as Record<string, unknown> : {};
  const fallbackMap = fallback ?? {
    locationMode: "picker" as const,
    query: "",
    coordinateType: "bd09ll" as const,
    zoomLevel: 17,
    style: "eink" as const,
    marker: true,
    showAddress: true,
    showCoordinates: false,
  };
  const requestedMode = candidate.locationMode as MapLocationMode;
  const requestedStyle = candidate.style as MapStyle;
  const query = trimText(candidate.query, fallbackMap.query, 80);
  const hidesCoordinates = /(?:不|不要|无需|隐藏)(?:显示)?(?:地图)?(?:坐标|经纬度)/.test(prompt);
  return {
    locationMode: requestedMode === "browser" || requestedMode === "ip" ? requestedMode : "picker",
    query,
    coordinateType: "bd09ll",
    zoomLevel: Math.min(19, Math.max(3, Math.round(Number(candidate.zoomLevel) || fallbackMap.zoomLevel))),
    style: requestedStyle === "balanced" || requestedStyle === "detail" ? requestedStyle : "eink",
    marker: candidate.marker !== false,
    showAddress: candidate.showAddress !== false,
    showCoordinates: !hidesCoordinates && (candidate.showCoordinates === true || /坐标|经纬度/.test(prompt)),
    statusMessage: query ? "正在查找地点；可在右侧重新选点" : "请在右侧地图中选点",
  };
}

function normalizeDisplay(value: unknown, fallback: InkApp, prompt: string): ScreenDisplay {
  const defaults = displaySettings(fallback.spec);
  const candidate = value && typeof value === "object" ? value as Record<string, unknown> : {};
  const requestedFont = candidate.font as ScreenFont;
  const forbidsBorder = /不要(?:任何|所有|这些|UI|ui|组件|卡片|画板)?边框|无边框|取消边框|去掉边框|所有.{0,8}(?:不要|不需要|没有)边框/.test(prompt);
  const explicitBorder = !forbidsBorder
    && /(?:显示|保留|添加|开启|要|有)(?:一个|外部|卡片|画板)?边框|描边框|带边框|有画板/.test(prompt);
  const explicitLogo = /logo|标志|品牌字样/i.test(prompt);
  const explicitQuote = /名言|金句|格言|一句话|鼓励|提醒/.test(prompt);
  const explicitDate = /日期|年月日|星期|周几/.test(prompt) || Boolean(fallback.spec.clock?.enabled);
  const explicitTime = /时间|时钟|几点|钟表/.test(prompt);
  const explicitLargeTime = /大时间|大号时间|超大时间|时间\s*[（(]?大[）)]?|时钟/.test(prompt)
    || Boolean(fallback.spec.clock?.enabled);
  const explicitWeather = wantsWeather(prompt);
  const explicitQr = /二维码|QR\s*码|QR\s*code|qrcode/i.test(prompt);
  return {
    quote: candidate.quote === true || (defaults.quote && explicitQuote),
    logo: explicitLogo && candidate.logo !== false,
    date: candidate.date === true || explicitDate,
    time: candidate.time === true || (explicitTime && !explicitLargeTime),
    timeLarge: candidate.timeLarge === true || explicitLargeTime,
    weather: candidate.weather === true,
    weatherLarge: candidate.weatherLarge === true || (explicitWeather && candidate.weather !== true),
    qr: explicitQr && candidate.qr !== false,
    border: explicitBorder && candidate.border !== false,
    font: ALLOWED_SCREEN_FONTS.has(requestedFont) ? requestedFont : defaults.font,
    renderMode: defaults.renderMode,
    renderModeExplicit: false,
    logoText: trimText(candidate.logoText, defaults.logoText, 20),
    qrMode: candidate.qrMode === "wifi" ? "wifi" as const : "text" as const,
    qrText: trimText(candidate.qrText, defaults.qrText, 512),
    qrWifiSsid: trimText(candidate.qrWifiSsid, "", 64),
    qrWifiPassword: trimText(candidate.qrWifiPassword, "", 128),
    qrWifiSecurity: candidate.qrWifiSecurity === "WEP" || candidate.qrWifiSecurity === "nopass"
      ? candidate.qrWifiSecurity
      : "WPA" as const,
    qrWifiHidden: candidate.qrWifiHidden === true,
    positions: defaults.positions,
    elementFonts: defaults.elementFonts,
    elementSizes: defaults.elementSizes,
  };
}

function normalizeTable(value: unknown, fallback: ScreenTable | undefined, kind: ScreenKind) {
  if (kind !== "calendar" && kind !== "timetable" && kind !== "agenda") return undefined;
  const candidate = value && typeof value === "object" ? value as Record<string, unknown> : {};

  if (kind === "calendar") {
    const fallbackCalendar = fallback?.type === "calendar" ? fallback : undefined;
    const year = Math.round(Number(candidate.year) || fallbackCalendar?.year || new Date().getFullYear());
    const month = Math.round(Number(candidate.month) || fallbackCalendar?.month || new Date().getMonth() + 1);
    const rawEvents = Array.isArray(candidate.events) ? candidate.events : fallbackCalendar?.events ?? [];
    const events = rawEvents.flatMap((entry) => {
      if (!entry || typeof entry !== "object") return [];
      const event = entry as Record<string, unknown>;
      const day = Math.round(Number(event.day));
      const text = typeof event.text === "string" ? event.text.trim().slice(0, 8) : "";
      return day >= 1 && day <= 31 && text ? [{ day, text }] : [];
    }).slice(0, 12);
    return {
      type: "calendar" as const,
      year: Math.min(2100, Math.max(2020, year)),
      month: Math.min(12, Math.max(1, month)),
      weekStartsOn: candidate.weekStartsOn === "sunday" ? "sunday" as const : "monday" as const,
      events,
      lunar: candidate.lunar === true || fallbackCalendar?.lunar === true,
    };
  }

  if (kind === "agenda") {
    const fallbackAgenda = fallback?.type === "agenda" ? fallback : undefined;
    const rawEvents = Array.isArray(candidate.events) ? candidate.events : fallbackAgenda?.events ?? [];
    const events = rawEvents.flatMap((entry, index) => {
      if (!entry || typeof entry !== "object") return [];
      const event = entry as Record<string, unknown>;
      const title = typeof event.title === "string" ? event.title.trim().slice(0, 32) : "";
      const start = typeof event.start === "string" && Number.isFinite(Date.parse(event.start)) ? event.start : "";
      const end = typeof event.end === "string" && Number.isFinite(Date.parse(event.end)) ? event.end : start;
      if (!title || !start) return [];
      return [{
        uid: typeof event.uid === "string" ? event.uid.slice(0, 120) : `preview-${index}-${start}`,
        title,
        start,
        end,
        allDay: event.allDay === true,
        location: typeof event.location === "string" ? event.location.trim().slice(0, 40) : undefined,
        calendar: typeof event.calendar === "string" ? event.calendar.trim().slice(0, 24) : undefined,
      }];
    }).slice(0, 12);
    const view: AgendaView = candidate.view === "agenda" || candidate.view === "workweek" ? candidate.view : "three-day";
    const rangeMode: AgendaRangeMode = candidate.rangeMode === "today" || candidate.rangeMode === "custom" ? candidate.rangeMode : "rolling";
    return {
      type: "agenda" as const,
      view,
      rangeMode,
      rangeHours: Math.min(168, Math.max(4, Math.round(Number(candidate.rangeHours) || fallbackAgenda?.rangeHours || (view === "workweek" ? 120 : 72)))),
      customStart: typeof candidate.customStart === "string" ? candidate.customStart.slice(0, 32) : fallbackAgenda?.customStart,
      customEnd: typeof candidate.customEnd === "string" ? candidate.customEnd.slice(0, 32) : fallbackAgenda?.customEnd,
      eventWidth: Math.min(100, Math.max(45, Math.round(Number(candidate.eventWidth) || fallbackAgenda?.eventWidth || 100))),
      showEndTime: typeof candidate.showEndTime === "boolean" ? candidate.showEndTime : fallbackAgenda?.showEndTime ?? true,
      showLocation: typeof candidate.showLocation === "boolean" ? candidate.showLocation : fallbackAgenda?.showLocation ?? true,
      events: events.length ? events : fallbackAgenda?.events ?? [],
    };
  }

  const fallbackTimetable = fallback?.type === "timetable" ? fallback : undefined;
  const rawColumns = Array.isArray(candidate.columns) ? candidate.columns : fallbackTimetable?.columns ?? [];
  const columns = rawColumns
    .filter((column): column is string => typeof column === "string")
    .map((column) => column.trim().slice(0, 6))
    .filter(Boolean)
    .slice(0, 7);
  const safeColumns = columns.length >= 2 ? columns : ["周一", "周二", "周三", "周四", "周五"];
  const rawRows = Array.isArray(candidate.rows) ? candidate.rows : fallbackTimetable?.rows ?? [];
  const rows = rawRows.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];
    const row = entry as Record<string, unknown>;
    const label = typeof row.label === "string" ? row.label.trim().slice(0, 8) : "";
    const rawCells = Array.isArray(row.cells) ? row.cells : [];
    const cells = Array.from({ length: safeColumns.length }, (_, index) => {
      const cell = rawCells[index];
      return typeof cell === "string" ? cell.trim().slice(0, 8) : "";
    });
    return label ? [{ label, cells }] : [];
  }).slice(0, 8);
  return {
    type: "timetable" as const,
    columns: safeColumns,
    rows: rows.length ? rows : fallbackTimetable?.rows ?? [],
  };
}

function normalizeBaseUrl(value?: string) {
  const raw = value?.trim() || DEFAULT_BASE_URL;
  return raw.replace(/\/+$/, "");
}

function resolveApiKey() {
  return env.LLM_API_KEY?.trim() || "";
}

function extractJson(content: string) {
  const cleaned = content.replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/i, "").trim();
  const start = cleaned.indexOf("{");
  const end = cleaned.lastIndexOf("}");
  if (start < 0 || end <= start) throw new Error("模型没有返回有效 JSON");
  return JSON.parse(cleaned.slice(start, end + 1)) as Record<string, unknown>;
}

function normalizeApp(value: Record<string, unknown>, prompt: string): InkApp {
  const fallback = generateInkApp(prompt);
  const candidateSpec = value.spec && typeof value.spec === "object"
    ? (value.spec as Record<string, unknown>)
    : {};
  const candidateKind = candidateSpec.kind as ScreenKind;
  const candidateAccent = candidateSpec.accent as ScreenSpec["accent"];
  const rawMinutes = Number(value.customMinutes);
  const rawTime = typeof value.dailyTime === "string" ? value.dailyTime : "";
  const schedule = resolveSchedule(prompt, fallback, rawMinutes, rawTime);
  const normalizedKind = wantsMap(prompt)
    ? "map"
    : wantsWeather(prompt)
    ? "weather"
    : wantsAgenda(prompt)
      ? "agenda"
      : wantsCalendar(prompt)
      ? "calendar"
      : wantsTimetable(prompt)
        ? "timetable"
        : ALLOWED_KINDS.has(candidateKind) && !["weather", "calendar", "timetable", "agenda", "map"].includes(candidateKind)
          ? candidateKind
          : fallback.spec.kind;

  return {
    ...fallback,
    id: `app-${Date.now()}-${crypto.randomUUID().slice(0, 6)}`,
    title: trimText(value.title, fallback.title, 40),
    description: trimText(value.description, fallback.description, 120),
    prompt,
    spec: {
      kind: normalizedKind,
      orientation: /横版|横屏/.test(prompt)
        ? "landscape"
        : /竖版|竖屏/.test(prompt)
          ? "portrait"
          : candidateSpec.orientation === "landscape" || candidateSpec.orientation === "portrait"
            ? candidateSpec.orientation
            : fallback.spec.orientation || (normalizedKind === "agenda" || normalizedKind === "timetable" ? "landscape" : "portrait"),
      city: normalizedKind === "weather"
        ? screenText(candidateSpec.city, fallback.spec.city || inferWeatherCity(prompt), 30)
        : undefined,
      eyebrow: screenText(candidateSpec.eyebrow, fallback.spec.eyebrow, 40),
      title: screenText(candidateSpec.title, fallback.spec.title, 28),
      value: screenText(candidateSpec.value, fallback.spec.value, 20),
      unit: screenText(candidateSpec.unit, fallback.spec.unit, 8),
      detail: screenText(candidateSpec.detail, fallback.spec.detail, 48),
      footer: screenText(candidateSpec.footer, fallback.spec.footer, 48),
      accent: ALLOWED_ACCENTS.has(candidateAccent) ? candidateAccent : fallback.spec.accent,
      artwork: normalizeArtwork(candidateSpec.artwork, fallback.spec.artwork, prompt),
      clock: normalizeClock(candidateSpec.clock, fallback.spec.clock),
      display: normalizeDisplay(candidateSpec.display, fallback, prompt),
      map: normalizedKind === "map" ? normalizeMap(candidateSpec.map, fallback.spec.map, prompt) : undefined,
      table: normalizeTable(candidateSpec.table, fallback.spec.table, normalizedKind),
    },
    code: trimText(value.code, fallback.code, 8000),
    scheduleMode: schedule.mode,
    customMinutes: schedule.minutes,
    dailyTime: schedule.dailyTime,
    isPublic: false,
    author: "我",
    createdAt: new Date().toISOString(),
  };
}

async function chooseModel(baseUrl: string, apiKey: string) {
  if (env.LLM_MODEL?.trim()) return env.LLM_MODEL.trim();
  const response = await fetch(`${baseUrl}/models`, {
    headers: { Authorization: `Bearer ${apiKey}` },
    signal: AbortSignal.timeout(10_000),
  });
  if (!response.ok) return MODEL_PREFERENCES[0];
  const payload = (await response.json()) as GatewayModels;
  const models = (payload.data ?? payload.models ?? [])
    .map((model) => (typeof model.id === "string" ? model.id : ""))
    .filter(Boolean);
  return MODEL_PREFERENCES.find((model) => models.includes(model)) ?? models[0] ?? MODEL_PREFERENCES[0];
}

export function GET() {
  return Response.json({
    configured: Boolean(resolveApiKey()),
    provider: "LLM Gateway",
    endpoint: normalizeBaseUrl(env.LLM_BASE_URL),
    model: env.LLM_MODEL?.trim() || "auto",
  });
}

export async function POST(request: Request) {
  let prompt = "";
  try {
    const body = (await request.json()) as { prompt?: unknown };
    prompt = typeof body.prompt === "string" ? body.prompt.trim().slice(0, 1000) : "";
    if (!prompt) return Response.json({ error: "请先描述应用需求" }, { status: 400 });

    const apiKey = resolveApiKey();
    if (!apiKey) {
      return Response.json({
        app: generateInkApp(prompt),
        mode: "local",
        model: null,
        warning: "尚未配置 LLM_API_KEY，已使用本地模板引擎。",
      });
    }

    const baseUrl = normalizeBaseUrl(env.LLM_BASE_URL);
    const model = await chooseModel(baseUrl, apiKey);
    const response = await fetch(`${baseUrl}/chat/completions`, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${apiKey}`,
        "Content-Type": "application/json",
        "X-Client-Name": "Inkloop",
      },
      body: JSON.stringify({
        model,
        messages: [
          { role: "system", content: SYSTEM_PROMPT },
          { role: "user", content: prompt },
        ],
        temperature: 0.2,
        max_tokens: 2400,
      }),
      signal: AbortSignal.timeout(50_000),
    });
    if (!response.ok) {
      throw new Error(`模型网关返回 ${response.status}`);
    }
    const completion = (await response.json()) as ChatCompletion;
    const content = completion.choices?.[0]?.message?.content;
    if (typeof content !== "string") throw new Error("模型响应缺少内容");
    const app = normalizeApp(extractJson(content), prompt);
    return Response.json({ app, mode: "llm", model });
  } catch (error) {
    if (!prompt) {
      return Response.json({ error: error instanceof Error ? error.message : "请求格式错误" }, { status: 400 });
    }
    return Response.json({
      app: generateInkApp(prompt),
      mode: "local",
      model: null,
      warning: `在线生成暂时不可用，已切换到本地模板。${error instanceof Error ? ` ${error.message}` : ""}`,
    });
  }
}
