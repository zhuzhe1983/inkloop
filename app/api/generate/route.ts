import { env } from "cloudflare:workers";
import {
  displaySettings,
  generateInkApp,
  inferWeatherCity,
  resolveCardSpec,
  type ArtworkSpec,
  type AgendaRangeMode,
  type AgendaView,
  type ClockSpec,
  type CardSpec,
  type InkApp,
  type MapLocationMode,
  type MapSpec,
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
const AUTO_MODEL = "auto";
const MAX_MODEL_OPTIONS = 200;
const MAX_MODEL_ID_LENGTH = 256;

const ALLOWED_KINDS = new Set<ScreenKind>(["weather", "focus", "countdown", "meeting", "metric", "calendar", "timetable", "agenda", "map", "card"]);
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
    "kind": "weather|focus|countdown|meeting|metric|calendar|timetable|agenda|map|card",
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
      "query": "地点、地址或 POI；不确定时留空，交给用户输入或拖拽微调",
      "zoomLevel": 17,
      "style": "balanced",
      "marker": true,
      "showAddress": true,
      "showCoordinates": false
    },
    "card": {
      "rarity": "common|silver|gold|holo",
      "name": "卡名",
      "type": "类型 · 阵营",
      "level": 6,
      "description": "简短、有趣、可读的卡片效果描述",
      "attack": 2600,
      "defense": 2100,
      "cardId": "INK-026",
      "subjectScale": 1,
      "subjectX": 0,
      "subjectY": 0
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
25. 用户要求卡片、卡牌、桌游卡、普卡、银卡、金卡或闪卡时使用 kind=card 并填写 card。你可以根据主题有趣地生成卡名、类型、1—12 星等级、0—9999 的 ATK/DEF 和简短效果描述；稀有度可在 common、silver、gold、holo 中推断。用户明确指定的卡名、稀有度、等级、攻击、防御、类型或描述必须原样优先，不能被你的建议覆盖。默认创造原创角色和机制；artwork 使用 web、layout=hero，query 描述卡片主体而不是整张卡框。四种稀有度共用同一套星盘机械布局，只改变材质和装饰密度，禁止要求模型改变字段坐标。

六色电子纸视觉规范（生成任何应用时都必须遵守）：
${EPAPER_DESIGN_GUIDE}`;

type GatewayModel = { id?: unknown };
type GatewayModels = { data?: GatewayModel[]; models?: GatewayModel[] };
type ChatCompletion = {
  choices?: Array<{
    finish_reason?: unknown;
    message?: { content?: unknown; reasoning_content?: unknown };
  }>;
  output_text?: unknown;
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

function wantsCard(prompt: string) {
  return /卡片|卡牌|桌游卡|收藏卡|对战卡|普卡|银卡|金卡|闪卡|全息卡|游戏王/i.test(prompt);
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
    style: "balanced" as const,
    marker: true,
    showAddress: true,
    showCoordinates: false,
  };
  const requestedMode = candidate.locationMode as MapLocationMode;
  const query = trimText(candidate.query, fallbackMap.query, 80);
  const candidateLatitude = Number(candidate.latitude);
  const candidateLongitude = Number(candidate.longitude);
  const hasApproximateCoordinates = requestedMode !== "browser"
    && requestedMode !== "ip"
    && candidate.latitude !== undefined
    && candidate.latitude !== null
    && candidate.longitude !== undefined
    && candidate.longitude !== null
    && Number.isFinite(candidateLatitude)
    && Number.isFinite(candidateLongitude)
    && candidateLatitude >= -90
    && candidateLatitude <= 90
    && candidateLongitude >= -180
    && candidateLongitude <= 180
    && (Math.abs(candidateLatitude) > 0.0001 || Math.abs(candidateLongitude) > 0.0001);
  const hidesCoordinates = /(?:不|不要|无需|隐藏)(?:显示)?(?:地图)?(?:坐标|经纬度)/.test(prompt);
  return {
    locationMode: requestedMode === "browser" || requestedMode === "ip" ? requestedMode : "picker",
    query,
    latitude: hasApproximateCoordinates ? candidateLatitude : undefined,
    longitude: hasApproximateCoordinates ? candidateLongitude : undefined,
    coordinateType: hasApproximateCoordinates ? "wgs84ll" : "bd09ll",
    zoomLevel: Math.min(19, Math.max(3, Math.round(Number(candidate.zoomLevel) || fallbackMap.zoomLevel))),
    style: "balanced",
    marker: candidate.marker !== false,
    showAddress: candidate.showAddress !== false,
    showCoordinates: !hidesCoordinates && (candidate.showCoordinates === true || /坐标|经纬度/.test(prompt)),
    displayName: trimText(candidate.displayName, fallbackMap.displayName || "", 30) || undefined,
    approximate: hasApproximateCoordinates || undefined,
    statusMessage: hasApproximateCoordinates
      ? "当前为模型估算位置；可在预览上拖拽微调"
      : query
        ? "正在查找地点；可在预览上拖拽微调"
        : "请先输入地点或使用定位",
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

function buildCompactSystemPrompt(prompt: string) {
  const sections: string[] = [];
  const kind = wantsMap(prompt)
    ? "map"
    : wantsCard(prompt)
      ? "card"
      : wantsWeather(prompt)
        ? "weather"
        : wantsAgenda(prompt)
          ? "agenda"
          : wantsCalendar(prompt)
            ? "calendar"
            : wantsTimetable(prompt)
              ? "timetable"
              : "focus|countdown|meeting|metric";

  if (wantsArtwork(prompt) || wantsCard(prompt)) {
    sections.push(`artwork 仅在用户要求图片时返回：
"artwork":{"mode":"generated|web","query":"2—6个准确英文主体词","style":"2—5个英文风格词","layout":"background|hero|fullscreen","rotateOnRefresh":false}
人物、动物、城市和产品用 web；抽象图形用 generated。卡片主体用 hero。全屏纯图用 fullscreen；图片背景用 background。保留 Marvel、Spider-Man、边牧等明确主题，不要泛化。女性只能用成年 woman。`);
  }
  if (wantsWeather(prompt)) {
    sections.push(`天气：kind=weather，填写 city；display.weatherLarge=true、weather=false。天气数据由运行时注入，不要输出 {{weather.*}}。`);
  }
  if (wantsMap(prompt)) {
    sections.push(`地图：
"map":{"locationMode":"picker|browser|ip","query":"地点或POI","latitude":30.2741,"longitude":120.1551,"coordinateType":"wgs84ll","approximate":true,"zoomLevel":17,"style":"balanced","marker":true,"showAddress":true,"showCoordinates":false}
默认 picker。已知城市、区域或著名地标时可以给出常识范围内的大致 WGS84 经纬度，并必须设置 coordinateType=wgs84ll、approximate=true；不确定时省略 latitude/longitude，绝不能声称是精确坐标。zoomLevel 3—19，入口 17—19，城市 10—13。`);
  }
  if (wantsCard(prompt)) {
    sections.push(`卡片：kind=card、orientation=portrait、artwork.layout=hero，并返回：
"card":{"rarity":"common|silver|gold|holo","name":"卡名","type":"类型 · 阵营","level":6,"description":"简短有趣的效果","attack":2600,"defense":2100,"cardId":"INK-026","subjectScale":1,"subjectX":0,"subjectY":0}
可以自动生成有趣数值，但用户明确指定的卡名、稀有度、等级、ATK、DEF、类型和描述必须原样优先。四种稀有度坐标相同，只改变材质。`);
  }
  if (wantsAgenda(prompt)) {
    sections.push(`苹果日历/日程：kind=agenda、orientation=landscape，并返回：
"table":{"type":"agenda","view":"agenda|three-day|workweek","rangeMode":"rolling|today|custom","rangeHours":72,"customStart":"","customEnd":"","eventWidth":85,"showEndTime":true,"showLocation":true,"events":[{"uid":"preview-1","title":"预览日程","start":"ISO时间","end":"ISO时间","location":"","allDay":false}]}
默认未来三天；五天用 workweek、120小时。只给3—8条明显的预览事件，真实事件由 iCal 注入。`);
  } else if (wantsCalendar(prompt)) {
    sections.push(`月历：kind=calendar，并返回：
"table":{"type":"calendar","year":2026,"month":8,"weekStartsOn":"monday|sunday","lunar":false,"events":[{"day":8,"text":"事件"}]}
农历仅在用户要求时为 true；最多12条简短事件。`);
  } else if (wantsTimetable(prompt)) {
    sections.push(`课程表：kind=timetable、orientation=landscape，并返回：
"table":{"type":"timetable","columns":["周一","周二"],"rows":[{"label":"08:00","cells":["语文","数学"]}]}
2—7列、1—8行，每格最多8个汉字。`);
  }
  if (/时钟|时间|日期|二维码|QR|Wi-?Fi|WPA/i.test(prompt)) {
    sections.push(`可选组件放在 display：time 是小时间，timeLarge 是主视觉大时间；border 仅控制全屏外框且默认 false。二维码只有用户明确要求时 qr=true；Wi-Fi 二维码用 qrMode=wifi，不要编造密码。时钟可返回 "clock":{"enabled":true,"board":true,"font":"sans|serif|rounded|mono|handwritten|random"}。`);
  }

  return `你是 Inkloop 六色电子墨水屏应用编程助手。只返回一个 JSON 对象，不要 Markdown、解释或代码围栏。

返回结构：
{"title":"应用名","description":"一句用途","spec":{"kind":"${kind}","orientation":"portrait|landscape","city":"仅天气需要","eyebrow":"短标签","title":"屏幕标题","value":"主值","unit":"单位","detail":"一行详情","footer":"一行补充","accent":"red|blue|green|yellow","display":{"quote":false,"logo":false,"date":false,"time":false,"timeLarge":false,"weather":false,"weatherLarge":false,"qr":false,"border":false,"font":"sans|serif|rounded|mono|handwritten"}},"code":"安全的 JavaScript render(ctx) 业务逻辑源码","scheduleMode":"once|hourly|daily|custom","customMinutes":30,"dailyTime":"08:00"}

通用规则：
- 用户明确要求优先；不编造个人数据、密码或真实日程。地图只能提供明确标为 approximate 的大致 WGS84 坐标，不能伪装成精确定位。
- 没有明确刷新周期时 scheduleMode=once；自定义周期最短1分钟。
- 竖版528×792，横版792×528，只用纸白、墨黑、黄、红、蓝、绿。纸白和墨黑为主，通常只用一个强调色。
- 文本必须短、清晰、高对比；不要品牌签名、设备型号、水印、伪按钮、厚重发光或无意义脚注。
- 运行时变量只允许 {{date}}、{{year}}、{{month}}、{{day}}、{{weekday}}、{{hour}}、{{minute}}、{{time}}。
- code 只用于审阅，不使用 eval、不含密钥、不直接调用蓝牙；外部数据使用 ctx.weather、ctx.calendar 或 ctx.data。
- 未要求的复杂对象可以省略，系统会补默认值。

${sections.join("\n\n")}`;
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
    : wantsCard(prompt)
      ? "card"
    : wantsWeather(prompt)
    ? "weather"
    : wantsAgenda(prompt)
      ? "agenda"
      : wantsCalendar(prompt)
      ? "calendar"
      : wantsTimetable(prompt)
        ? "timetable"
        : ALLOWED_KINDS.has(candidateKind) && !["weather", "calendar", "timetable", "agenda", "map", "card"].includes(candidateKind)
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
      orientation: normalizedKind === "card"
        ? "portrait"
        : /横版|横屏/.test(prompt)
        ? "landscape"
        : /竖版|竖屏/.test(prompt)
          ? "portrait"
          : normalizedKind === "map"
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
      card: normalizedKind === "card"
        ? resolveCardSpec(
            prompt,
            candidateSpec.card && typeof candidateSpec.card === "object" ? candidateSpec.card as Partial<CardSpec> : {},
            fallback.spec.card,
          )
        : undefined,
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

async function listGatewayModels(baseUrl: string, apiKey: string) {
  try {
    const response = await fetch(`${baseUrl}/models`, {
      headers: { Authorization: `Bearer ${apiKey}` },
      signal: AbortSignal.timeout(8_000),
    });
    if (!response.ok) return [];
    const payload = (await response.json()) as GatewayModels;
    return [...new Set((payload.data ?? payload.models ?? [])
      .map((model) => (typeof model.id === "string" ? model.id : ""))
      .map((model) => model.trim())
      .filter((model) => Boolean(model) && model.length <= MAX_MODEL_ID_LENGTH))]
      .slice(0, MAX_MODEL_OPTIONS);
  } catch {
    return [];
  }
}

function preferredModel(models: string[], excluded = "") {
  return MODEL_PREFERENCES.find((model) => model !== excluded && models.includes(model))
    ?? models.find((model) => model !== excluded)
    ?? MODEL_PREFERENCES.find((model) => model !== excluded)
    ?? MODEL_PREFERENCES[0];
}

async function requestCompletion(baseUrl: string, apiKey: string, model: string, prompt: string, timeoutMs: number) {
  const systemPrompt = buildCompactSystemPrompt(prompt);
  const complexRequest = wantsArtwork(prompt)
    || wantsCard(prompt)
    || wantsAgenda(prompt)
    || wantsCalendar(prompt)
    || wantsTimetable(prompt)
    || wantsMap(prompt);
  console.info("Inkloop LLM request", {
    model,
    systemPromptBytes: new TextEncoder().encode(systemPrompt).byteLength,
    userPromptBytes: new TextEncoder().encode(prompt).byteLength,
  });
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
        { role: "system", content: systemPrompt },
        { role: "user", content: prompt },
      ],
      temperature: 0.2,
      max_tokens: complexRequest ? 3000 : 1800,
      response_format: { type: "json_object" },
      ...(model.startsWith("Qwen/")
        ? { chat_template_kwargs: { enable_thinking: false } }
        : {}),
    }),
    signal: AbortSignal.timeout(timeoutMs),
  });
  if (!response.ok) throw new Error(`模型网关返回 ${response.status}`);
  const completion = (await response.json()) as ChatCompletion;
  const choice = completion.choices?.[0];
  const message = choice?.message;
  const asText = (value: unknown) => {
    if (typeof value === "string") return value.trim();
    if (!Array.isArray(value)) return "";
    return value.map((part) => {
      if (typeof part === "string") return part;
      if (!part || typeof part !== "object") return "";
      const candidate = part as { text?: unknown; content?: unknown };
      return typeof candidate.text === "string"
        ? candidate.text
        : typeof candidate.content === "string"
          ? candidate.content
          : "";
    }).join("\n").trim();
  };
  const content = asText(message?.content)
    || asText(completion.output_text)
    || asText(message?.reasoning_content);
  if (!content) {
    throw new Error(`模型响应缺少内容${typeof choice?.finish_reason === "string" ? `（${choice.finish_reason}）` : ""}`);
  }
  return extractJson(content);
}

export async function GET() {
  const apiKey = resolveApiKey();
  const baseUrl = normalizeBaseUrl(env.LLM_BASE_URL);
  const defaultModel = env.LLM_MODEL?.trim() || AUTO_MODEL;
  const gatewayModels = apiKey ? await listGatewayModels(baseUrl, apiKey) : [];
  const models = [...new Set([
    ...(defaultModel === AUTO_MODEL ? [] : [defaultModel]),
    ...gatewayModels,
  ])];
  return Response.json({
    configured: Boolean(apiKey),
    provider: "LLM Gateway",
    endpoint: baseUrl,
    model: defaultModel,
    models,
  });
}

export async function POST(request: Request) {
  let prompt = "";
  try {
    const body = (await request.json()) as { prompt?: unknown; model?: unknown };
    prompt = typeof body.prompt === "string" ? body.prompt.trim().slice(0, 1000) : "";
    const requestedModel = typeof body.model === "string"
      ? body.model.trim().slice(0, MAX_MODEL_ID_LENGTH)
      : AUTO_MODEL;
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
    const modelsPromise = listGatewayModels(baseUrl, apiKey);
    const configuredModel = env.LLM_MODEL?.trim() || "";
    const manuallySelectedModel = requestedModel && requestedModel !== AUTO_MODEL ? requestedModel : "";
    const models = configuredModel && !manuallySelectedModel ? [] : await modelsPromise;
    if (manuallySelectedModel && manuallySelectedModel !== configuredModel) {
      if (!models.length) {
        return Response.json({ error: "暂时无法确认可用模型，请稍后重试" }, { status: 503 });
      }
      if (!models.includes(manuallySelectedModel)) {
        return Response.json({ error: "所选模型已不可用，请重新选择" }, { status: 400 });
      }
    }
    let model = manuallySelectedModel || configuredModel || preferredModel(models);
    const complexRequest = wantsArtwork(prompt)
      || wantsCard(prompt)
      || wantsAgenda(prompt)
      || wantsCalendar(prompt)
      || wantsTimetable(prompt)
      || wantsMap(prompt);
    let generated: Record<string, unknown>;
    try {
      generated = await requestCompletion(baseUrl, apiKey, model, prompt, complexRequest ? 34_000 : 22_000);
    } catch (primaryError) {
      if (manuallySelectedModel) throw primaryError;
      const availableModels = models.length ? models : await modelsPromise;
      const fallbackModel = preferredModel(availableModels, model);
      console.warn("Inkloop primary LLM failed; retrying with fallback", {
        model,
        fallbackModel,
        error: primaryError instanceof Error ? primaryError.message : String(primaryError),
      });
      try {
        generated = await requestCompletion(baseUrl, apiKey, fallbackModel, prompt, complexRequest ? 24_000 : 22_000);
        model = fallbackModel;
      } catch (fallbackError) {
        console.error("Inkloop fallback LLM failed", {
          model: fallbackModel,
          error: fallbackError instanceof Error ? fallbackError.message : String(fallbackError),
        });
        throw fallbackError;
      }
    }
    const app = normalizeApp(generated, prompt);
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
