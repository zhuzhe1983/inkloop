import { t } from "./i18n-runtime";
export type ScreenKind = "weather" | "focus" | "countdown" | "meeting" | "metric" | "calendar" | "timetable" | "agenda" | "map" | "card";

export type ScreenOrientation = "portrait" | "landscape";

export type ScheduleMode = "once" | "hourly" | "daily" | "custom";

export type ArtworkSpec = {
  mode: "generated" | "web";
  motif: "rainbow" | "sunburst" | "confetti" | "waves" | "grid";
  query: string;
  style?: string;
  layout: "background" | "hero" | "fullscreen";
  seed: number;
  rotateOnRefresh?: boolean;
};

export type ClockSpec = {
  enabled: true;
  board: boolean;
  font: "sans" | "serif" | "rounded" | "mono" | "handwritten" | "random";
};

export type ScreenFont = "sans" | "serif" | "rounded" | "mono" | "handwritten";

export type ScreenRenderMode = "official" | "inkloop-text";

export type MapLocationMode = "picker" | "browser" | "ip";
export type MapStyle = "balanced";

export type MapSpec = {
  locationMode: MapLocationMode;
  query: string;
  latitude?: number;
  longitude?: number;
  coordinateType: "bd09ll" | "wgs84ll";
  zoomLevel: number;
  style: MapStyle;
  marker: boolean;
  showAddress: boolean;
  showCoordinates: boolean;
  displayName?: string;
  address?: string;
  approximate?: boolean;
  statusMessage?: string;
};

export type CardRarity = "common" | "silver" | "gold" | "holo";

export type CardSpec = {
  rarity: CardRarity;
  name: string;
  type: string;
  level: number;
  description: string;
  attack: number;
  defense: number;
  cardId: string;
  subjectScale: number;
  subjectX: number;
  subjectY: number;
};

export type ScreenElementKey = "quote" | "logo" | "date" | "time" | "timeLarge" | "weather" | "weatherLarge" | "qr";

export type ScreenElementPosition = {
  x: number;
  y: number;
};

export const DEFAULT_ELEMENT_POSITIONS: Record<ScreenElementKey, ScreenElementPosition> = {
  quote: { x: 264, y: 682 },
  logo: { x: 142, y: 746 },
  date: { x: 160, y: 70 },
  time: { x: 444, y: 70 },
  timeLarge: { x: 264, y: 390 },
  weather: { x: 350, y: 108 },
  weatherLarge: { x: 264, y: 350 },
  qr: { x: 410, y: 660 },
};

export const DEFAULT_ELEMENT_SIZES: Record<ScreenElementKey, number> = {
  quote: 18,
  logo: 15,
  date: 18,
  time: 22,
  timeLarge: 112,
  weather: 18,
  weatherLarge: 88,
  qr: 176,
};

export type ScreenDisplay = {
  quote: boolean;
  logo: boolean;
  date: boolean;
  time: boolean;
  timeLarge: boolean;
  weather: boolean;
  weatherLarge: boolean;
  qr: boolean;
  border: boolean;
  font: ScreenFont;
  renderMode: ScreenRenderMode;
  renderModeExplicit: boolean;
  logoText: string;
  qrMode: "text" | "wifi";
  qrText: string;
  qrWifiSsid: string;
  qrWifiPassword: string;
  qrWifiSecurity: "WPA" | "WEP" | "nopass";
  qrWifiHidden: boolean;
  positions: Record<ScreenElementKey, ScreenElementPosition>;
  elementFonts: Partial<Record<ScreenElementKey, ScreenFont>>;
  elementSizes: Partial<Record<ScreenElementKey, number>>;
};

export type CalendarEvent = {
  day: number;
  text: string;
};

export type AgendaEvent = {
  uid: string;
  title: string;
  start: string;
  end: string;
  allDay?: boolean;
  location?: string;
  calendar?: string;
  category?: string;
};

export type AgendaView = "agenda" | "three-day" | "workweek";
export type AgendaRangeMode = "rolling" | "today" | "custom";

export type TimetableRow = {
  label: string;
  cells: string[];
};

export type ScreenTable =
  | {
      type: "calendar";
      year: number;
      month: number;
      weekStartsOn: "monday" | "sunday";
      events: CalendarEvent[];
      lunar?: boolean;
    }
  | {
      type: "timetable";
      columns: string[];
      rows: TimetableRow[];
    }
  | {
      type: "agenda";
      view: AgendaView;
      rangeMode: AgendaRangeMode;
      rangeHours: number;
      customStart?: string;
      customEnd?: string;
      eventWidth?: number;
      showEndTime?: boolean;
      showLocation?: boolean;
      events: AgendaEvent[];
    };

export type ScreenSpec = {
  kind: ScreenKind;
  orientation?: ScreenOrientation;
  city?: string;
  eyebrow: string;
  title: string;
  value: string;
  unit: string;
  detail: string;
  footer: string;
  accent: "red" | "blue" | "green" | "yellow";
  artwork?: ArtworkSpec;
  clock?: ClockSpec;
  display?: ScreenDisplay;
  dateText?: string;
  timeText?: string;
  weatherText?: string;
  weatherValue?: string;
  weatherUnit?: string;
  weatherDetail?: string;
  weatherAccent?: "red" | "blue" | "green" | "yellow";
  map?: MapSpec;
  card?: CardSpec;
  table?: ScreenTable;
};

export type InkApp = {
  id: string;
  title: string;
  description: string;
  prompt: string;
  spec: ScreenSpec;
  code: string;
  scheduleMode: ScheduleMode;
  customMinutes: number;
  dailyTime: string;
  isPublic: boolean;
  author: string;
  createdAt: string;
  localImage?: string;
};

const nowIso = () => new Date().toISOString();

export const starterPrompt =
  t("显示今天最重要的一件事和一句简短的专注提醒。");

export const starterApp: InkApp = {
  id: "starter-focus",
  title: t("今日专注卡"),
  description: t("把最重要的一件事留在屏幕上"),
  prompt: starterPrompt,
  spec: {
    kind: "focus",
    eyebrow: "ONE THING · TODAY",
    title: t("今天最重要"),
    value: t("专注完成"),
    unit: "",
    detail: t("先完成，再完善"),
    footer: t("一次只做一件事"),
    accent: "blue",
  },
  code: `export async function render() {
  return {
    title: "今天最重要",
    value: "专注完成",
    detail: "先完成，再完善",
    accent: "blue",
    footer: "一次只做一件事"
  };
}`,
  scheduleMode: "once",
  customMinutes: 30,
  dailyTime: "08:00",
  isPublic: false,
  author: t("我"),
  createdAt: "2026-08-01T00:00:00.000Z",
};

const includesAny = (source: string, terms: string[]) => terms.some((term) => source.includes(term));

export function displaySettings(spec: ScreenSpec, hasLocalImage = false): ScreenDisplay {
  const fallbackFont = spec.clock?.font && spec.clock.font !== "random" ? spec.clock.font : "sans";
  const saved = spec.display as Partial<ScreenDisplay> | undefined;
  const savedRenderMode = saved?.renderMode === "official" || saved?.renderMode === "inkloop-text"
    ? saved.renderMode
    : undefined;
  const renderModeExplicit = saved?.renderModeExplicit === true;
  const renderMode = renderModeExplicit && savedRenderMode
    ? savedRenderMode
    : spec.artwork || hasLocalImage
      ? "official"
      : "inkloop-text";
  return {
    quote: Boolean(spec.footer),
    logo: false,
    date: Boolean(spec.clock?.enabled),
    time: false,
    timeLarge: Boolean(spec.clock?.enabled),
    weather: saved?.weather ?? false,
    weatherLarge: saved?.weatherLarge ?? (!saved && spec.kind === "weather"),
    qr: saved?.qr ?? false,
    border: false,
    font: fallbackFont,
    logoText: "INKLOOP",
    qrMode: "text",
    qrText: "https://p.todoo.tech/?lang=zh",
    qrWifiSsid: "",
    qrWifiPassword: "",
    qrWifiSecurity: "WPA",
    qrWifiHidden: false,
    ...saved,
    renderMode,
    renderModeExplicit,
    positions: {
      ...DEFAULT_ELEMENT_POSITIONS,
      ...saved?.positions,
    },
    elementFonts: {
      ...saved?.elementFonts,
    },
    elementSizes: {
      ...saved?.elementSizes,
    },
  };
}

function promptSeed(source: string) {
  let hash = 2166136261;
  for (let index = 0; index < source.length; index += 1) {
    hash ^= source.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0) % 1_000_000 + 1;
}

export function inferWeatherCity(source: string) {
  const namedCities = [
    t("上海"), t("北京"), t("深圳"), t("广州"), t("杭州"), t("成都"), t("重庆"), t("南京"), t("苏州"), t("武汉"),
    t("西安"), t("天津"), t("青岛"), t("厦门"), t("长沙"), t("郑州"), t("昆明"), t("大连"), t("宁波"), t("香港"),
    t("澳门"), t("台北"), t("东京"), t("大阪"), t("首尔"), t("新加坡"), t("伦敦"), t("巴黎"), t("纽约"), t("洛杉矶"),
  ];
  const named = namedCities.find((city) => source.includes(city));
  if (named) return named;
  const matched = source.match(/([\u4e00-\u9fa5]{2,8})(?:市)?(?:天气|气温|温度)/)?.[1]
    ?.replace(/^(?:显示|查看|更新|刷新|今天|今日)/, "")
    .trim();
  return matched?.slice(-6) || t("上海");
}

function wantsFullscreenArtwork(source: string) {
  const imageOnly = includesAny(source, [t("不要任何其他"), t("不要其他"), t("不要文字"), t("只有图片"), t("只要图片"), t("纯图片"), t("纯图")]);
  return imageOnly && (
    includesAny(source, [t("全屏"), t("铺满"), t("满屏")])
    || includesAny(source, [t("图片"), t("照片"), t("海报"), t("插画")])
  );
}

function wantsBackgroundArtwork(source: string) {
  return includesAny(source, [t("背景"), t("背景图"), t("做背景"), t("作为背景")]);
}

function inferArtworkStyle(source: string) {
  if (includesAny(source, [t("复古"), t("胶片"), t("怀旧")])) return "vintage film editorial";
  if (includesAny(source, [t("日系"), t("清新"), t("治愈")])) return "airy Japanese editorial";
  if (includesAny(source, [t("电影感"), t("光影"), t("戏剧感")])) return "cinematic dramatic lighting";
  if (includesAny(source, [t("极简"), t("简约"), t("留白")])) return "minimal clean composition";
  if (includesAny(source, [t("手绘"), t("水彩"), t("插画")])) return "bold editorial illustration";
  if (includesAny(source, [t("时尚"), t("穿搭"), "OOTD", "ootd"])) return "fashion editorial photography";
  return "editorial high contrast composition";
}

function inferArtwork(source: string): ArtworkSpec | undefined {
  const seed = promptSeed(source);
  const fullscreen = wantsFullscreenArtwork(source);
  const background = wantsBackgroundArtwork(source);
  const style = inferArtworkStyle(source);
  const rotateOnRefresh = includesAny(source, [t("随机"), t("每次换"), t("换一张"), t("轮换")]);
  if (includesAny(source, [t("漫威"), "Marvel", "marvel", t("蜘蛛侠"), t("钢铁侠"), t("美国队长"), t("复仇者联盟")])) {
    const query = includesAny(source, [t("蜘蛛侠")])
      ? "Spider-Man superhero movie poster"
      : includesAny(source, [t("钢铁侠")])
        ? "Iron Man superhero movie poster"
        : includesAny(source, [t("美国队长")])
          ? "Captain America superhero movie poster"
          : includesAny(source, [t("复仇者联盟")])
            ? "Avengers superhero movie poster"
            : "Marvel superhero movie poster";
    return {
      mode: "web",
      motif: "grid",
      query,
      style: "cinematic comic book poster high contrast",
      layout: fullscreen ? "fullscreen" : background ? "background" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, [t("边牧"), t("边境牧羊犬")])) {
    return {
      mode: "web",
      motif: "grid",
      query: "border collie dog portrait photography",
      style,
      layout: fullscreen ? "fullscreen" : background ? "background" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, [t("猫"), t("猫猫"), t("猫咪"), t("小猫")])) {
    return {
      mode: "web",
      motif: "grid",
      query: "cute cat portrait photography",
      style,
      layout: fullscreen ? "fullscreen" : background ? "background" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, [t("狗"), t("狗狗"), t("小狗"), t("宠物")])) {
    return {
      mode: "web",
      motif: "grid",
      query: "cute pet portrait photography",
      style,
      layout: fullscreen ? "fullscreen" : background ? "background" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, [t("美女"), t("女性"), t("女孩"), t("人物时钟")])) {
    const holdingBoard = !includesAny(source, [t("没有画板"), t("不要画板"), t("无画板")]);
    return {
      mode: "web",
      motif: "grid",
      query: holdingBoard
        ? "fashion woman portrait"
        : "woman fashion editorial",
      style: includesAny(source, [t("复古"), t("胶片"), t("日系"), t("电影感"), t("极简")])
        ? style
        : "fashion editorial studio lighting",
      layout: fullscreen ? "fullscreen" : "background",
      seed,
      rotateOnRefresh: true,
    };
  }
  if (includesAny(source, [t("彩虹"), t("虹彩"), t("七彩")])) {
    return {
      mode: "generated",
      motif: "rainbow",
      query: "abstract rainbow colorful background",
      style,
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, [t("彩纸"), t("庆祝"), t("礼花")])) {
    return {
      mode: "generated",
      motif: "confetti",
      query: "celebration confetti illustration",
      style,
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, [t("太阳"), t("阳光"), t("放射")])) {
    return {
      mode: "generated",
      motif: "sunburst",
      query: "sunburst graphic background",
      style,
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, [t("图片"), t("照片"), t("摄影"), t("风景"), t("人物"), t("城市"), t("产品图"), t("背景图"), t("插画")])) {
    const query = source.includes(t("城市"))
      ? "modern city architecture"
      : source.includes(t("风景"))
        ? "beautiful nature landscape"
        : source.includes(t("人物"))
          ? "people portrait lifestyle"
          : source.includes(t("产品"))
            ? "minimal product photography"
            : "colorful editorial illustration";
    return {
      mode: "web",
      motif: "grid",
      query,
      style,
      layout: fullscreen ? "fullscreen" : background ? "background" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  return undefined;
}

function requestedCalendarMonth(source: string) {
  const now = new Date();
  const full = source.match(/(20\d{2})\s*年\s*(1[0-2]|0?[1-9])\s*月/);
  const monthOnly = source.match(/(?:^|\D)(1[0-2]|0?[1-9])\s*月/);
  return {
    year: full ? Number(full[1]) : now.getFullYear(),
    month: full ? Number(full[2]) : monthOnly ? Number(monthOnly[1]) : now.getMonth() + 1,
  };
}

function requestedCalendarEvents(source: string): CalendarEvent[] {
  return [...source.matchAll(/(3[01]|[12]?\d)\s*[日号]\s*([^，。,；;\n]{1,12})/g)]
    .map((match) => ({ day: Number(match[1]), text: match[2].trim().slice(0, 8) }))
    .filter((event) => event.day >= 1 && event.day <= 31 && event.text)
    .slice(0, 12);
}

function fallbackTimetable(source: string): TimetableRow[] {
  const knownSubjects = [t("语文"), t("数学"), t("英语"), t("物理"), t("化学"), t("生物"), t("历史"), t("地理"), t("政治"), t("美术"), t("音乐"), t("体育"), t("编程")];
  const subjects = knownSubjects.filter((subject) => source.includes(subject));
  const pool = subjects.length ? subjects : [t("语文"), t("数学"), t("英语"), t("科学"), t("体育")];
  const labels = ["08:00", "09:00", "10:15", "13:30", "14:40", "15:50"];
  return labels.map((label, row) => ({
    label,
    cells: Array.from({ length: 5 }, (_, column) => pool[(row * 2 + column) % pool.length]),
  }));
}

function inferMapQuery(source: string) {
  const quoted = source.match(/[「“\"]([^」”\"]{2,40})[」”\"]/)?.[1]?.trim();
  if (quoted) return quoted;
  const normalized = source
    .replace(/^(?:请|帮我|麻烦)?\s*/, "")
    .replace(/^(?:生成|做一张|做|显示|查看|打开)\s*/, "")
    .replace(/^(?:横版|竖版|横屏|竖屏)\s*/, "")
    .replace(/^(?:生成|做一张|做|显示|查看|打开)\s*/, "");
  const beforeMap = normalized.match(/([^，。；;\n]{2,40}?)(?:附近|周边|入口|位置)?(?:的)?地图/)?.[1]
    ?.replace(/^(?:一个|一张|当前|我的)/, "")
    .trim();
  if (beforeMap && !/^(?:地图|附近|当前位置)$/.test(beforeMap)) return beforeMap.slice(0, 40);
  return "";
}

function inferMapZoomLevel(source: string) {
  const explicit = Number(source.match(/(?:zoomLevel|zoom|缩放(?:级别)?)[\s:=：]*(\d{1,2})/i)?.[1]);
  if (Number.isFinite(explicit) && explicit >= 3 && explicit <= 19) return Math.round(explicit);
  if (includesAny(source, [t("入口"), t("门口"), t("楼栋"), t("停车位")])) return 19;
  if (includesAny(source, [t("城市"), t("城区"), t("全市"), t("概览")])) return 12;
  return 17;
}

function shouldShowMapCoordinates(source: string) {
  if (/(?:不|不要|无需|隐藏)(?:显示)?(?:地图)?(?:坐标|经纬度)/.test(source)) return false;
  return includesAny(source, [t("坐标"), t("经纬度")]);
}

function wantsTradingCard(source: string) {
  return /卡片|卡牌|桌游卡|收藏卡|对战卡|普卡|银卡|金卡|闪卡|全息卡|游戏王/i.test(source);
}

function explicitCardRarity(source: string): CardRarity | undefined {
  if (/闪卡|全息卡|镭射卡|holo/i.test(source)) return "holo";
  if (/金卡|黄金卡|gold\s*card/i.test(source)) return "gold";
  if (/银卡|白银卡|silver\s*card/i.test(source)) return "silver";
  if (/普卡|普通卡|common\s*card/i.test(source)) return "common";
  return undefined;
}

function cardSubjectQuery(source: string) {
  if (/边牧|边境牧羊犬/.test(source)) return "border collie fantasy hero portrait";
  if (/猫|猫咪|小猫/.test(source)) return "cat fantasy guardian portrait";
  if (/狗|宠物/.test(source)) return "dog fantasy guardian portrait";
  if (/龙|巨龙/.test(source)) return "celestial dragon fantasy concept art";
  if (/机甲|机械|机器人/.test(source)) return "celestial mechanical guardian concept art";
  if (/骑士|战士/.test(source)) return "astral knight fantasy concept art";
  if (/魔法|法师|巫师/.test(source)) return "astral mage fantasy concept art";
  if (/女性|美女|女人|女孩/.test(source)) return "woman fantasy hero portrait concept art";
  return "original celestial guardian fantasy concept art";
}

function explicitText(source: string, labels: string[], max: number) {
  const label = labels.map((item) => item.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("|");
  const match = source.match(new RegExp(`(?:${label})\\s*[：:=]\\s*[「“\"]?([^，。；;\\n」”\"]{1,${max}})`, "i"));
  return match?.[1]?.trim();
}

function explicitCardDescription(source: string) {
  const match = source.match(/(?:效果描述|卡片描述|描述|效果)\s*[：:=]\s*/i);
  if (!match || match.index === undefined) return undefined;
  const remainder = source.slice(match.index + match[0].length);
  const nextField = remainder.search(/[，,；;\n]\s*(?:卡名|名称|名字|类型|种族|等级|Lv\.?|ATK|攻击(?:力)?|DEF|防御(?:力)?|卡号|编号|ID)\s*[：:=]/i);
  const value = (nextField >= 0 ? remainder.slice(0, nextField) : remainder)
    .trim()
    .replace(/[。；;\s]+$/, "");
  return value ? value.slice(0, 120) : undefined;
}

function clampInteger(value: unknown, minimum: number, maximum: number, fallback: number) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? Math.min(maximum, Math.max(minimum, Math.round(parsed))) : fallback;
}

function clampCardOffset(value: unknown) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? Math.min(100, Math.max(-100, Math.round(parsed))) : 0;
}

function localCardRarity(source: string, seed: number): CardRarity {
  const explicit = explicitCardRarity(source);
  if (explicit) return explicit;
  if (/神话|传说|限定|终极|宇宙级/.test(source)) return "holo";
  if (/王者|史诗|至尊|稀有/.test(source)) return "gold";
  const roll = seed % 100;
  return roll < 8 ? "holo" : roll < 24 ? "gold" : roll < 48 ? "silver" : "common";
}

function localCardType(source: string) {
  if (/龙|巨龙/.test(source)) return t("龙族 · 星辉");
  if (/机甲|机械|机器人/.test(source)) return t("机械 · 守护");
  if (/骑士|战士/.test(source)) return t("战士 · 星辉");
  if (/魔法|法师|巫师/.test(source)) return t("法术 · 秘仪");
  if (/猫|狗|宠物|边牧/.test(source)) return t("灵兽 · 同伴");
  return t("星辉 · 守护");
}

function localCardName(source: string) {
  const explicit = explicitText(source, [t("卡名"), t("名称"), t("名字")], 20)
    || source.match(/(?:叫做|叫作|名为)\s*[「“\"]?([^，。；;\n」”\"]{1,20})/)?.[1]?.trim()
    || source.match(/[「“]([^」”]{1,20})[」”]/)?.[1]?.trim();
  if (explicit) return explicit;
  if (/边牧|边境牧羊犬/.test(source)) return t("星野边牧");
  if (/猫|猫咪|小猫/.test(source)) return t("月影灵猫");
  if (/龙|巨龙/.test(source)) return t("星穹曜龙");
  if (/机甲|机械|机器人/.test(source)) return t("星穹守望者");
  if (/骑士|战士/.test(source)) return t("暮光誓约骑士");
  if (/魔法|法师|巫师/.test(source)) return t("流光秘术师");
  return t("星穹守望者");
}

export function resolveCardSpec(
  source: string,
  candidate: Partial<CardSpec> = {},
  fallback?: CardSpec,
): CardSpec {
  const seed = promptSeed(source);
  const rarity = explicitCardRarity(source)
    ?? (candidate.rarity === "common" || candidate.rarity === "silver" || candidate.rarity === "gold" || candidate.rarity === "holo"
      ? candidate.rarity
      : fallback?.rarity ?? localCardRarity(source, seed));
  const statBase = rarity === "holo" ? 3000 : rarity === "gold" ? 2400 : rarity === "silver" ? 1700 : 1000;
  const defaultAttack = statBase + seed % (rarity === "common" ? 800 : 1100);
  const defaultDefense = statBase - 200 + Math.floor(seed / 7) % (rarity === "common" ? 800 : 1000);
  const explicitLevel = source.match(/(?:等级\s*[：:=]?\s*|Lv\.?\s*|LV\.?\s*)(\d{1,2})|(?:^|\D)(\d{1,2})\s*(?:星|级)/i);
  const explicitAttack = source.match(/(?:ATK|攻击(?:力)?)\s*[：:=]?\s*(\d{1,5})/i)?.[1];
  const explicitDefense = source.match(/(?:DEF|防御(?:力)?)\s*[：:=]?\s*(\d{1,5})/i)?.[1];
  const explicitDescription = explicitCardDescription(source);
  const explicitType = explicitText(source, [t("卡片类型"), t("类型"), t("种族")], 24);
  const explicitId = explicitText(source, [t("卡号"), t("编号"), "ID"], 18);
  const defaultDescription = /边牧|狗/.test(source)
    ? t("入场时标记最需要守护的目标；每完成一次刷新，获得 1 层默契并提升守护值。")
    : /猫/.test(source)
      ? t("在安静回合中积蓄月光；当画面更新时，有概率复制上一张卡的微光效果。")
      : t("每当设备完成一次刷新，获得 1 层星辉。积满 3 层时清除错误状态，并强化下一次写入。");
  const requestedScale = source.match(/(?:主体|角色|人物|图片)(?:大小|缩放)?\s*[：:=]?\s*(\d{1,3})\s*%/)?.[1];

  return {
    rarity,
    name: (explicitText(source, [t("卡名"), t("名称"), t("名字")], 20) || candidate.name || fallback?.name || localCardName(source)).trim().slice(0, 20),
    type: (explicitType || candidate.type || fallback?.type || localCardType(source)).trim().slice(0, 24),
    level: clampInteger(explicitLevel?.[1] || explicitLevel?.[2] || candidate.level, 1, 12, fallback?.level ?? (3 + seed % 7)),
    description: (explicitDescription || candidate.description || fallback?.description || defaultDescription).trim().slice(0, 120),
    attack: clampInteger(explicitAttack || candidate.attack, 0, 9999, fallback?.attack ?? defaultAttack),
    defense: clampInteger(explicitDefense || candidate.defense, 0, 9999, fallback?.defense ?? defaultDefense),
    cardId: (explicitId || candidate.cardId || fallback?.cardId || `INK-${String(seed % 1000).padStart(3, "0")}`).trim().slice(0, 18),
    subjectScale: Math.min(2.2, Math.max(0.7, Number(requestedScale ? Number(requestedScale) / 100 : candidate.subjectScale ?? fallback?.subjectScale ?? 1) || 1)),
    subjectX: clampCardOffset(candidate.subjectX ?? fallback?.subjectX),
    subjectY: clampCardOffset(candidate.subjectY ?? fallback?.subjectY),
  };
}

export function generateInkApp(prompt: string, stableId?: string): InkApp {
  const source = prompt.trim() || starterPrompt;
  const promptHour = source.match(/(\d{1,2})\s*[点:时]/)?.[1];
  const artwork = inferArtwork(source);
  const base = {
    id: stableId ?? `app-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
    prompt: source,
    customMinutes: 30,
    dailyTime: promptHour ? `${promptHour.padStart(2, "0")}:00` : "08:00",
    isPublic: false,
    author: t("我"),
    createdAt: stableId ? "2026-08-01T00:00:00.000Z" : nowIso(),
  };

  if (includesAny(source, [t("地图"), t("位置图"), t("路线图"), t("导航图"), t("周边图")])) {
    const query = inferMapQuery(source);
    const orientation: ScreenOrientation = includesAny(source, [t("横版"), t("横屏")])
      ? "landscape"
      : "portrait";
    return {
      ...base,
      title: query ? `${query}地图` : t("附近地图"),
      description: t("选择位置并生成适合六色电子纸的静态地图"),
      scheduleMode: "once",
      spec: {
        kind: "map",
        orientation,
        eyebrow: "",
        title: "",
        value: "",
        unit: "",
        detail: "",
        footer: "",
        accent: "blue",
        map: {
          locationMode: "picker",
          query,
          coordinateType: "bd09ll",
          zoomLevel: inferMapZoomLevel(source),
          style: "balanced",
          marker: !includesAny(source, [t("不要标记"), t("无标记"), t("隐藏标记")]),
          showAddress: true,
          showCoordinates: shouldShowMapCoordinates(source),
          statusMessage: query ? t("正在查找地点；可在预览上拖拽微调") : t("请先输入地点或使用定位"),
        },
        display: {
          ...displaySettings({
            kind: "map",
            orientation,
            eyebrow: "",
            title: "",
            value: "",
            unit: "",
            detail: "",
            footer: "",
            accent: "blue",
          }),
          quote: false,
          logo: false,
          date: false,
          time: false,
          timeLarge: false,
          weather: false,
          weatherLarge: false,
          qr: false,
          border: false,
          renderMode: "inkloop-text",
          renderModeExplicit: true,
        },
      },
      code: `export async function render(ctx) {
  const location = await ctx.map.resolve({ mode: "picker", query: ${JSON.stringify(query)} });
  return { type: "map", location, zoomLevel: ${inferMapZoomLevel(source)}, marker: true };
}`,
    };
  }

  if (wantsTradingCard(source)) {
    const card = resolveCardSpec(source);
    const cardArtwork = artwork ?? {
      mode: "web" as const,
      motif: "grid" as const,
      query: cardSubjectQuery(source),
      style: "original astral mechanical trading card character concept art",
      layout: "hero" as const,
      seed: promptSeed(`${source}:card-subject`),
      rotateOnRefresh: false,
    };
    return {
      ...base,
      title: card.name,
      description: `${card.type} · ${card.level} 星 · ATK ${card.attack}`,
      scheduleMode: "once",
      spec: {
        kind: "card",
        orientation: "portrait",
        eyebrow: "",
        title: card.name,
        value: String(card.attack),
        unit: "ATK",
        detail: card.description,
        footer: card.cardId,
        accent: card.rarity === "gold" || card.rarity === "holo" ? "yellow" : card.rarity === "silver" ? "blue" : "green",
        artwork: cardArtwork,
        card,
        display: {
          ...displaySettings({
            kind: "card",
            orientation: "portrait",
            eyebrow: "",
            title: card.name,
            value: String(card.attack),
            unit: "ATK",
            detail: card.description,
            footer: card.cardId,
            accent: "yellow",
          }),
          quote: false,
          logo: false,
          date: false,
          time: false,
          timeLarge: false,
          weather: false,
          weatherLarge: false,
          qr: false,
          border: false,
          renderMode: "official",
          renderModeExplicit: true,
        },
      },
      code: `export function render(ctx) {
  return {
    type: "card",
    name: ${JSON.stringify(card.name)},
    rarity: ${JSON.stringify(card.rarity)},
    level: ${card.level},
    attack: ${card.attack},
    defense: ${card.defense},
    description: ${JSON.stringify(card.description)}
  };
}`,
    };
  }

  if (artwork?.layout === "fullscreen") {
    const subject = includesAny(source, [t("猫"), t("猫猫"), t("猫咪"), t("小猫")])
      ? t("猫咪")
      : includesAny(source, [t("狗"), t("狗狗"), t("小狗"), t("宠物")])
        ? t("宠物")
        : t("主题");
    return {
      ...base,
      title: `随机${subject}全屏`,
      description: t("一张铺满屏幕、没有文字遮挡的随机图片"),
      scheduleMode: "once",
      spec: {
        kind: "focus",
        eyebrow: "",
        title: "",
        value: "",
        unit: "",
        detail: "",
        footer: "",
        accent: "blue",
        artwork,
      },
      code: `export function render() {
  return { artwork: { layout: "fullscreen", query: ${JSON.stringify(artwork.query)} } };
}`,
    };
  }

  if (includesAny(source, [t("苹果日历"), t("周日程"), t("日程安排"), t("智能日程"), t("未来安排"), t("行程表"), t("议程")])) {
    const start = new Date();
    start.setMinutes(Math.ceil(start.getMinutes() / 30) * 30, 0, 0);
    const eventAt = (offsetHours: number, durationHours: number, title: string, location: string) => {
      const eventStart = new Date(start.getTime() + offsetHours * 60 * 60 * 1000);
      const eventEnd = new Date(eventStart.getTime() + durationHours * 60 * 60 * 1000);
      return {
        uid: `preview-${offsetHours}-${title}`,
        title,
        start: eventStart.toISOString(),
        end: eventEnd.toISOString(),
        location,
      };
    };
    const events = [
      eventAt(1, 1, t("项目同步"), t("线上会议")),
      eventAt(4, 1.5, t("方案评审"), t("会议室 A")),
      eventAt(24, 1, t("客户沟通"), t("视频会议")),
      eventAt(29, 2, t("专注工作"), t("工作室")),
      eventAt(50, 1, t("周计划复盘"), t("办公室")),
    ];
    return {
      ...base,
      title: t("智能日程"),
      description: t("从现在开始，只显示真正需要关注的日程"),
      scheduleMode: "custom",
      customMinutes: 15,
      spec: {
        kind: "agenda",
        orientation: "landscape",
        eyebrow: "SMART CALENDAR",
        title: t("接下来三天"),
        value: "",
        unit: "",
        detail: t("按时间自动压缩空闲区间"),
        footer: "",
        accent: "blue",
        table: {
          type: "agenda",
          view: "three-day",
          rangeMode: "rolling",
          rangeHours: 72,
          eventWidth: 100,
          showEndTime: true,
          showLocation: true,
          events,
        },
        display: {
          ...displaySettings({
            kind: "agenda",
            orientation: "landscape",
            eyebrow: "",
            title: "",
            value: "",
            unit: "",
            detail: "",
            footer: "",
            accent: "blue",
          }),
          quote: false,
          date: false,
          time: false,
          weather: false,
          weatherLarge: false,
        },
      },
      code: `export async function render(ctx) {
  const events = await ctx.calendar.range({ hours: 72 });
  return { type: "agenda", view: "three-day", events };
}`,
    };
  }

  if (includesAny(source, [t("月历"), t("日历"), t("月度计划"), t("月计划")])) {
    const { year, month } = requestedCalendarMonth(source);
    const events = requestedCalendarEvents(source);
    return {
      ...base,
      title: `${year} 年 ${month} 月月历`,
      description: t("完整六周月历，可在日期中显示简短事项"),
      scheduleMode: includesAny(source, [t("每天"), t("每日")]) ? "daily" : "once",
      dailyTime: "00:05",
      spec: {
        kind: "calendar",
        orientation: "portrait",
        eyebrow: "MONTHLY OVERVIEW",
        title: `${year} 年 ${month} 月`,
        value: "",
        unit: "",
        detail: events.length ? `${events.length} 个日程已标记` : t("本月安排一览"),
        footer: "",
        accent: "blue",
        table: { type: "calendar", year, month, weekStartsOn: "monday", events, lunar: source.includes(t("农历")) },
        display: {
          ...displaySettings({
            kind: "calendar",
            eyebrow: "",
            title: "",
            value: "",
            unit: "",
            detail: "",
            footer: "",
            accent: "blue",
          }),
          quote: false,
          date: false,
          weather: false,
          weatherLarge: false,
        },
      },
      code: `export function render() {
  return ${JSON.stringify({ type: "calendar", year, month, weekStartsOn: "monday", events }, null, 2)};
}`,
    };
  }

  if (includesAny(source, [t("课程表"), t("课表"), t("排课表"), t("时间表")])) {
    const columns = [t("周一"), t("周二"), t("周三"), t("周四"), t("周五")];
    const rows = fallbackTimetable(source);
    return {
      ...base,
      title: t("一周课程表"),
      description: t("按星期和时段整理的一页课程安排"),
      scheduleMode: "once",
      spec: {
        kind: "timetable",
        orientation: "landscape",
        eyebrow: "WEEKLY SCHEDULE",
        title: t("一周课程表"),
        value: "",
        unit: "",
        detail: "",
        footer: "",
        accent: "green",
        table: { type: "timetable", columns, rows },
        display: {
          ...displaySettings({
            kind: "timetable",
            eyebrow: "",
            title: "",
            value: "",
            unit: "",
            detail: "",
            footer: "",
            accent: "green",
          }),
          quote: false,
          date: false,
          weather: false,
          weatherLarge: false,
        },
      },
      code: `export function render() {
  return ${JSON.stringify({ type: "timetable", columns, rows }, null, 2)};
}`,
    };
  }

  if (includesAny(source, [t("时钟"), t("时间"), t("几点"), t("钟表")])) {
    const board = !includesAny(source, [t("没有画板"), t("不要画板"), t("无画板")]);
    return {
      ...base,
      title: includesAny(source, [t("美女"), t("女性"), t("女孩")]) ? t("美女时钟") : t("主题时钟"),
      description: t("每分钟更新日期与时间，并可轮换主题背景"),
      scheduleMode: "custom",
      customMinutes: 1,
      spec: {
        kind: "focus",
        eyebrow: "{{weekday}} · {{date}}",
        title: t("现在时间"),
        value: "{{time}}",
        unit: "",
        detail: t("{{year}}年{{month}}月{{day}}日"),
        footer: t("愿今天的每一分钟都值得"),
        accent: "red",
        artwork: artwork ?? {
          mode: "generated",
          motif: "sunburst",
          query: "modern clock graphic background",
          style: "minimal high contrast graphic poster",
          layout: "background",
          seed: promptSeed(`${source}:clock`),
          rotateOnRefresh: false,
        },
        clock: {
          enabled: true,
          board,
          font: "random",
        },
      },
      code: `export function render(ctx) {
  const now = ctx.now;
  return {
    eyebrow: "{{weekday}} · {{date}}",
    value: "{{time}}",
    detail: "{{year}}年{{month}}月{{day}}日"
  };
}`,
    };
  }

  if (includesAny(source, [t("鼓励"), t("加油"), t("你很棒"), t("你超棒"), t("彩虹")])) {
    return {
      ...base,
      title: t("彩虹鼓励卡"),
      description: t("用醒目的图形和一句话给自己打气"),
      scheduleMode: "once",
      spec: {
        kind: "focus",
        eyebrow: "A LITTLE BOOST · TODAY",
        title: t("你超棒"),
        value: t("继续加油"),
        unit: "",
        detail: t("每一天都是新的开始"),
        footer: t("相信自己，你可以的！"),
        accent: "yellow",
        artwork: artwork ?? {
          mode: "generated",
          motif: "confetti",
          query: "colorful encouragement celebration",
          style: "bold editorial poster",
          layout: "background",
          seed: promptSeed(source),
        },
      },
      code: `export function render() {
  return {
    title: "你超棒",
    value: "继续加油",
    detail: "每一天都是新的开始",
    footer: "相信自己，你可以的！"
  };
}`,
    };
  }

  if (includesAny(source, [t("天气"), t("温度"), t("下雨"), t("通勤")])) {
    const city = inferWeatherCity(source);
    const explicitDaily = includesAny(source, [t("每天"), t("早上"), t("上午"), t("下午"), t("晚上")]);
    const explicitHourly = includesAny(source, [t("每小时"), t("每个小时")]);
    return {
      ...base,
      title: t("天气通勤卡"),
      description: t("天气、温度与出门建议一眼读完"),
      scheduleMode: explicitDaily ? "daily" : explicitHourly ? "hourly" : "once",
      spec: {
        kind: "weather",
        city,
        eyebrow: `${city} · 今日`,
        title: t("出门天气"),
        value: "--",
        unit: "°C",
        detail: t("正在获取最新天气"),
        footer: t("数据会在预览和写入前自动更新"),
        accent: "red",
        artwork,
      },
      code: `export async function render(ctx) {
  const city = ${JSON.stringify(city)};
  const weather = await ctx.weather.get({ city });
  return {
    eyebrow: \`${"${city}"} · ${"${ctx.date.weekday}"}\`,
    value: Math.round(weather.temperature),
    detail: \`${"${weather.summary}"} · ${"${weather.low}"}—${"${weather.high}"}°C\`,
    accent: weather.rainProbability > 45 ? "red" : "yellow",
    footer: weather.rainProbability > 45 ? "记得带伞 · 避开积水路段" : "适合步行出门"
  };
}`,
    };
  }

  if (includesAny(source, [t("倒计时"), t("还有几天"), t("纪念日"), t("生日")])) {
    const event = source.includes(t("生日")) ? t("生日") : source.includes(t("发布")) ? t("新品发布") : t("重要日子");
    return {
      ...base,
      title: `${event}倒计时`,
      description: t("让期待的日子每天更近一点"),
      scheduleMode: "daily",
      spec: {
        kind: "countdown",
        eyebrow: "COUNTDOWN · 2026",
        title: event,
        value: "24",
        unit: t("天"),
        detail: t("目标日 · 8月25日"),
        footer: t("今天也向目标前进一步"),
        accent: "blue",
        artwork,
      },
      code: `export function render(ctx) {
  const target = new Date("2026-08-25T00:00:00+08:00");
  const days = Math.max(0, Math.ceil((target - ctx.now) / 86400000));
  return { value: days, unit: "天", detail: "目标日 · 8月25日" };
}`,
    };
  }

  if (includesAny(source, [t("会议"), t("会议室"), t("访客"), t("门牌")])) {
    return {
      ...base,
      title: t("会议室门牌"),
      description: t("清晰展示当前会议与下一个空闲时段"),
      scheduleMode: "custom",
      customMinutes: 15,
      spec: {
        kind: "meeting",
        eyebrow: "MEETING ROOM · 03",
        title: t("产品周会"),
        value: t("进行中"),
        unit: "",
        detail: t("10:00—11:00 · 6 人"),
        footer: t("下一空闲时段  11:00—13:30"),
        accent: "green",
        artwork,
      },
      code: `export async function render(ctx) {
  const room = await ctx.calendar.room("M03");
  const current = room.currentEvent;
  return current
    ? { title: current.title, value: "进行中", detail: current.timeRange }
    : { title: "会议室 M03", value: "空闲", detail: room.nextEvent?.timeRange };
}`,
    };
  }

  if (includesAny(source, [t("销售"), t("数据"), t("目标"), t("营收"), t("进度")])) {
    return {
      ...base,
      title: t("目标进度卡"),
      description: t("关键指标无需点亮另一块屏幕"),
      scheduleMode: "hourly",
      spec: {
        kind: "metric",
        eyebrow: "THIS MONTH · AUG",
        title: t("销售目标"),
        value: "76",
        unit: "%",
        detail: "¥ 380,240 / ¥ 500,000",
        footer: t("较昨日 +3.8% · 保持节奏"),
        accent: "yellow",
        artwork,
      },
      code: `export async function render(ctx) {
  const sales = await ctx.data.metric("monthly_sales");
  const progress = Math.round(sales.actual / sales.target * 100);
  return { value: progress, unit: "%", detail: ctx.money(sales.actual) };
}`,
    };
  }

  return {
    ...base,
    title: t("今日专注卡"),
    description: t("把最重要的一件事留在视线里"),
    scheduleMode: "once",
    spec: {
      kind: "focus",
      eyebrow: "ONE THING · TODAY",
      title: t("今日专注"),
      value: t("深度工作"),
      unit: "",
      detail: t("09:30—11:30 · 请勿打扰"),
      footer: source.slice(0, 28) || t("完成比完美更重要"),
      accent: "blue",
      artwork,
    },
    code: `export function render(ctx) {
  return {
    eyebrow: "ONE THING · TODAY",
    title: "今日专注",
    value: "深度工作",
    detail: "09:30—11:30 · 请勿打扰",
    footer: ${JSON.stringify(source.slice(0, 42))}
  };
}`,
  };
}

export const featuredApps: InkApp[] = [
  {
    ...generateInkApp(t("显示上海天气和下雨提醒"), "featured-weather"),
    id: "featured-weather",
    title: t("通勤天气"),
    author: "Han",
    isPublic: true,
  },
  {
    ...generateInkApp(t("显示产品发布倒计时"), "featured-countdown"),
    id: "featured-countdown",
    title: t("发布倒计时"),
    author: "Jia",
    isPublic: true,
  },
  {
    ...generateInkApp(t("显示会议室当前会议"), "featured-meeting"),
    id: "featured-meeting",
    title: t("会议室状态"),
    author: "Mori Studio",
    isPublic: true,
  },
  {
    ...generateInkApp(t("显示本月销售目标进度"), "featured-metric"),
    id: "featured-metric",
    title: t("目标进度"),
    author: "Lemon",
    isPublic: true,
  },
];

export function intervalFor(app: InkApp): number | null {
  if (app.scheduleMode === "once") return null;
  if (app.scheduleMode === "hourly") return 60 * 60 * 1000;
  if (app.scheduleMode === "custom") return Math.max(1, app.customMinutes) * 60 * 1000;
  return 24 * 60 * 60 * 1000;
}

export function scheduleLabel(app: InkApp) {
  if (app.scheduleMode === "once") return t("仅写入一次");
  if (app.scheduleMode === "hourly") return t("每小时刷新");
  if (app.scheduleMode === "daily") return `每天 ${app.dailyTime}`;
  return `每 ${app.customMinutes} 分钟`;
}
