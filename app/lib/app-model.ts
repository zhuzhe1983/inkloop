export type ScreenKind = "weather" | "focus" | "countdown" | "meeting" | "metric";

export type ScheduleMode = "once" | "hourly" | "daily" | "custom";

export type ArtworkSpec = {
  mode: "generated" | "web";
  motif: "rainbow" | "sunburst" | "confetti" | "waves" | "grid";
  query: string;
  layout: "background" | "hero" | "fullscreen";
  seed: number;
  rotateOnRefresh?: boolean;
};

export type ClockSpec = {
  enabled: true;
  board: boolean;
  font: "sans" | "serif" | "rounded" | "mono" | "handwritten" | "random";
};

export type ScreenSpec = {
  kind: ScreenKind;
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
  "每天早上 8 点显示上海天气、最高最低温和一句出门建议，雨天用红色提醒。";

export const starterApp: InkApp = {
  id: "starter-weather",
  title: "今日天气卡",
  description: "每天早上更新天气与通勤提醒",
  prompt: starterPrompt,
  spec: {
    kind: "weather",
    city: "上海",
    eyebrow: "上海 · 8月1日 周六",
    title: "今日天气",
    value: "29",
    unit: "°C",
    detail: "阵雨转多云  ·  26—32°C",
    footer: "带伞出门，午后注意防晒",
    accent: "red",
  },
  code: `export async function render(ctx) {
  const weather = await ctx.weather.get({ city: "上海" });
  const isRainy = weather.rainProbability > 45;

  return {
    title: "今日天气",
    value: Math.round(weather.temperature),
    detail: \`${"${weather.summary}"} · ${"${weather.low}"}—${"${weather.high}"}°C\`,
    accent: isRainy ? "red" : "yellow",
    footer: isRainy ? "带伞出门，路面湿滑" : "天气不错，适合步行"
  };
}`,
  scheduleMode: "daily",
  customMinutes: 30,
  dailyTime: "08:00",
  isPublic: false,
  author: "我",
  createdAt: "2026-08-01T00:00:00.000Z",
};

const includesAny = (source: string, terms: string[]) => terms.some((term) => source.includes(term));

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
    "上海", "北京", "深圳", "广州", "杭州", "成都", "重庆", "南京", "苏州", "武汉",
    "西安", "天津", "青岛", "厦门", "长沙", "郑州", "昆明", "大连", "宁波", "香港",
    "澳门", "台北", "东京", "大阪", "首尔", "新加坡", "伦敦", "巴黎", "纽约", "洛杉矶",
  ];
  const named = namedCities.find((city) => source.includes(city));
  if (named) return named;
  const matched = source.match(/([\u4e00-\u9fa5]{2,8})(?:市)?(?:天气|气温|温度)/)?.[1]
    ?.replace(/^(?:显示|查看|更新|刷新|今天|今日)/, "")
    .trim();
  return matched?.slice(-6) || "上海";
}

function wantsFullscreenArtwork(source: string) {
  return includesAny(source, ["全屏", "铺满", "满屏"])
    && includesAny(source, ["不要任何其他", "不要其他", "不要文字", "只有图片", "只要图片", "纯图片"]);
}

function inferArtwork(source: string): ArtworkSpec | undefined {
  const seed = promptSeed(source);
  const fullscreen = wantsFullscreenArtwork(source);
  const rotateOnRefresh = includesAny(source, ["随机", "每次换", "换一张", "轮换"]);
  if (includesAny(source, ["猫", "猫猫", "猫咪", "小猫"])) {
    return {
      mode: "web",
      motif: "grid",
      query: "cute cat portrait photography",
      layout: fullscreen ? "fullscreen" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, ["狗", "狗狗", "小狗", "宠物"])) {
    return {
      mode: "web",
      motif: "grid",
      query: "cute pet portrait photography",
      layout: fullscreen ? "fullscreen" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  if (includesAny(source, ["美女", "女性", "女孩", "人物时钟"])) {
    const holdingBoard = !includesAny(source, ["没有画板", "不要画板", "无画板"]);
    return {
      mode: "web",
      motif: "grid",
      query: holdingBoard
        ? "fashion woman portrait"
        : "woman fashion editorial",
      layout: "background",
      seed,
      rotateOnRefresh: true,
    };
  }
  if (includesAny(source, ["彩虹", "虹彩", "七彩"])) {
    return {
      mode: "generated",
      motif: "rainbow",
      query: "abstract rainbow colorful background",
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, ["彩纸", "庆祝", "礼花"])) {
    return {
      mode: "generated",
      motif: "confetti",
      query: "celebration confetti illustration",
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, ["太阳", "阳光", "放射"])) {
    return {
      mode: "generated",
      motif: "sunburst",
      query: "sunburst graphic background",
      layout: "background",
      seed,
    };
  }
  if (includesAny(source, ["图片", "照片", "摄影", "风景", "人物", "城市", "产品图", "背景图", "插画"])) {
    const query = source.includes("城市")
      ? "modern city architecture"
      : source.includes("风景")
        ? "beautiful nature landscape"
        : source.includes("人物")
          ? "people portrait lifestyle"
          : source.includes("产品")
            ? "minimal product photography"
            : "colorful editorial illustration";
    return {
      mode: "web",
      motif: "grid",
      query,
      layout: fullscreen ? "fullscreen" : "hero",
      seed,
      rotateOnRefresh,
    };
  }
  return undefined;
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
    author: "我",
    createdAt: stableId ? "2026-08-01T00:00:00.000Z" : nowIso(),
  };

  if (artwork?.layout === "fullscreen") {
    const subject = includesAny(source, ["猫", "猫猫", "猫咪", "小猫"])
      ? "猫咪"
      : includesAny(source, ["狗", "狗狗", "小狗", "宠物"])
        ? "宠物"
        : "主题";
    return {
      ...base,
      title: `随机${subject}全屏`,
      description: "一张铺满屏幕、没有文字遮挡的随机图片",
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

  if (includesAny(source, ["时钟", "时间", "几点", "钟表"])) {
    const board = !includesAny(source, ["没有画板", "不要画板", "无画板"]);
    return {
      ...base,
      title: includesAny(source, ["美女", "女性", "女孩"]) ? "美女时钟" : "主题时钟",
      description: "每分钟更新日期与时间，并可轮换主题背景",
      scheduleMode: "custom",
      customMinutes: 1,
      spec: {
        kind: "focus",
        eyebrow: "{{weekday}} · {{date}}",
        title: "现在时间",
        value: "{{time}}",
        unit: "",
        detail: "{{year}}年{{month}}月{{day}}日",
        footer: "愿今天的每一分钟都值得",
        accent: "red",
        artwork: artwork ?? {
          mode: "generated",
          motif: "sunburst",
          query: "modern clock graphic background",
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

  if (includesAny(source, ["鼓励", "加油", "你很棒", "你超棒", "彩虹"])) {
    return {
      ...base,
      title: "彩虹鼓励卡",
      description: "用醒目的图形和一句话给自己打气",
      scheduleMode: "once",
      spec: {
        kind: "focus",
        eyebrow: "A LITTLE BOOST · TODAY",
        title: "你超棒",
        value: "继续加油",
        unit: "",
        detail: "每一天都是新的开始",
        footer: "相信自己，你可以的！",
        accent: "yellow",
        artwork: artwork ?? {
          mode: "generated",
          motif: "confetti",
          query: "colorful encouragement celebration",
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

  if (includesAny(source, ["天气", "温度", "下雨", "通勤"])) {
    const city = inferWeatherCity(source);
    const explicitDaily = includesAny(source, ["每天", "早上", "上午", "下午", "晚上"]);
    const explicitHourly = includesAny(source, ["每小时", "每个小时"]);
    return {
      ...base,
      title: "天气通勤卡",
      description: "天气、温度与出门建议一眼读完",
      scheduleMode: explicitDaily ? "daily" : explicitHourly ? "hourly" : "once",
      spec: {
        kind: "weather",
        city,
        eyebrow: `${city} · 今日`,
        title: "出门天气",
        value: "--",
        unit: "°C",
        detail: "正在获取最新天气",
        footer: "数据会在预览和写入前自动更新",
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

  if (includesAny(source, ["倒计时", "还有几天", "纪念日", "生日"])) {
    const event = source.includes("生日") ? "生日" : source.includes("发布") ? "新品发布" : "重要日子";
    return {
      ...base,
      title: `${event}倒计时`,
      description: "让期待的日子每天更近一点",
      scheduleMode: "daily",
      spec: {
        kind: "countdown",
        eyebrow: "COUNTDOWN · 2026",
        title: event,
        value: "24",
        unit: "天",
        detail: "目标日 · 8月25日",
        footer: "今天也向目标前进一步",
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

  if (includesAny(source, ["会议", "会议室", "访客", "门牌"])) {
    return {
      ...base,
      title: "会议室门牌",
      description: "清晰展示当前会议与下一个空闲时段",
      scheduleMode: "custom",
      customMinutes: 15,
      spec: {
        kind: "meeting",
        eyebrow: "MEETING ROOM · 03",
        title: "产品周会",
        value: "进行中",
        unit: "",
        detail: "10:00—11:00 · 6 人",
        footer: "下一空闲时段  11:00—13:30",
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

  if (includesAny(source, ["销售", "数据", "目标", "营收", "进度"])) {
    return {
      ...base,
      title: "目标进度卡",
      description: "关键指标无需点亮另一块屏幕",
      scheduleMode: "hourly",
      spec: {
        kind: "metric",
        eyebrow: "THIS MONTH · AUG",
        title: "销售目标",
        value: "76",
        unit: "%",
        detail: "¥ 380,240 / ¥ 500,000",
        footer: "较昨日 +3.8% · 保持节奏",
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
    title: "今日专注卡",
    description: "把最重要的一件事留在视线里",
    scheduleMode: "once",
    spec: {
      kind: "focus",
      eyebrow: "ONE THING · TODAY",
      title: "今日专注",
      value: "深度工作",
      unit: "",
      detail: "09:30—11:30 · 请勿打扰",
      footer: source.slice(0, 28) || "完成比完美更重要",
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
    ...generateInkApp("显示上海天气和下雨提醒", "featured-weather"),
    id: "featured-weather",
    title: "通勤天气",
    author: "Han",
    isPublic: true,
  },
  {
    ...generateInkApp("显示产品发布倒计时", "featured-countdown"),
    id: "featured-countdown",
    title: "发布倒计时",
    author: "Jia",
    isPublic: true,
  },
  {
    ...generateInkApp("显示会议室当前会议", "featured-meeting"),
    id: "featured-meeting",
    title: "会议室状态",
    author: "Mori Studio",
    isPublic: true,
  },
  {
    ...generateInkApp("显示本月销售目标进度", "featured-metric"),
    id: "featured-metric",
    title: "目标进度",
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
  if (app.scheduleMode === "once") return "仅写入一次";
  if (app.scheduleMode === "hourly") return "每小时刷新";
  if (app.scheduleMode === "daily") return `每天 ${app.dailyTime}`;
  return `每 ${app.customMinutes} 分钟`;
}
