export type ScreenKind = "weather" | "focus" | "countdown" | "meeting" | "metric";

export type ScheduleMode = "once" | "hourly" | "daily" | "custom";

export type ScreenSpec = {
  kind: ScreenKind;
  eyebrow: string;
  title: string;
  value: string;
  unit: string;
  detail: string;
  footer: string;
  accent: "red" | "blue" | "green" | "yellow";
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
  createdAt: nowIso(),
};

const includesAny = (source: string, terms: string[]) => terms.some((term) => source.includes(term));

export function generateInkApp(prompt: string): InkApp {
  const source = prompt.trim() || starterPrompt;
  const promptHour = source.match(/(\d{1,2})\s*[点:时]/)?.[1];
  const base = {
    id: `app-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
    prompt: source,
    customMinutes: 30,
    dailyTime: promptHour ? `${promptHour.padStart(2, "0")}:00` : "08:00",
    isPublic: false,
    author: "我",
    createdAt: nowIso(),
  };

  if (includesAny(source, ["天气", "温度", "下雨", "通勤"])) {
    return {
      ...base,
      title: "天气通勤卡",
      description: "天气、温度与出门建议一眼读完",
      scheduleMode: includesAny(source, ["每天", "早上", "点"]) ? "daily" : "hourly",
      spec: {
        kind: "weather",
        eyebrow: source.includes("北京") ? "北京 · 今日" : source.includes("深圳") ? "深圳 · 今日" : "上海 · 今日",
        title: "出门天气",
        value: source.includes("北京") ? "27" : source.includes("深圳") ? "31" : "29",
        unit: "°C",
        detail: "阵雨转多云  ·  26—32°C",
        footer: source.includes("雨") ? "记得带伞 · 避开积水路段" : "午后可能有雨，建议随身带伞",
        accent: "red",
      },
      code: `export async function render(ctx) {
  const city = ${JSON.stringify(source.includes("北京") ? "北京" : source.includes("深圳") ? "深圳" : "上海")};
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
    ...generateInkApp("显示上海天气和下雨提醒"),
    id: "featured-weather",
    title: "通勤天气",
    author: "Han",
    isPublic: true,
  },
  {
    ...generateInkApp("显示产品发布倒计时"),
    id: "featured-countdown",
    title: "发布倒计时",
    author: "Jia",
    isPublic: true,
  },
  {
    ...generateInkApp("显示会议室当前会议"),
    id: "featured-meeting",
    title: "会议室状态",
    author: "Mori Studio",
    isPublic: true,
  },
  {
    ...generateInkApp("显示本月销售目标进度"),
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
