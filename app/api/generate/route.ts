import { env } from "cloudflare:workers";
import {
  generateInkApp,
  type ArtworkSpec,
  type InkApp,
  type ScreenKind,
  type ScreenSpec,
} from "../../lib/app-model";

const DEFAULT_BASE_URL = "https://hub.tsingfly.com/v1";
const MODEL_PREFERENCES = [
  "Qwen/Qwen3.6-27B",
  "Apsara-Stack/GLM-5.1-W4A8",
  "deepseek-v4-flash",
  "qwen3-32b",
];

const ALLOWED_KINDS = new Set<ScreenKind>(["weather", "focus", "countdown", "meeting", "metric"]);
const ALLOWED_ACCENTS = new Set<ScreenSpec["accent"]>(["red", "blue", "green", "yellow"]);
const ALLOWED_SCHEDULES = new Set<InkApp["scheduleMode"]>(["once", "hourly", "daily", "custom"]);
const ALLOWED_ARTWORK_MODES = new Set<ArtworkSpec["mode"] | "none">(["none", "generated", "web"]);
const ALLOWED_ARTWORK_MOTIFS = new Set<ArtworkSpec["motif"]>([
  "rainbow",
  "sunburst",
  "confetti",
  "waves",
  "grid",
]);
const ALLOWED_ARTWORK_LAYOUTS = new Set<ArtworkSpec["layout"]>(["background", "hero"]);

const SYSTEM_PROMPT = `你是 Inkloop 的电子墨水屏应用编程助手。根据用户需求生成一个 TodooCard 应用。

只返回一个 JSON 对象，不要 Markdown，不要解释。JSON 结构必须是：
{
  "title": "应用名，最多20个汉字",
  "description": "一句用途说明",
  "spec": {
    "kind": "weather|focus|countdown|meeting|metric",
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
      "query": "用于联网找图的简短英文关键词",
      "layout": "background|hero"
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
3. 屏幕为 528×792 竖屏，只支持黑、白、黄、红、蓝、绿六色；内容必须短而清晰。
4. 如果用户提到刷新时间或周期，准确设置 scheduleMode、dailyTime 或 customMinutes。
5. 不编造真实个人数据；示例值应明显是合理预览。
6. 用户要求图片、照片、背景、插画或明显视觉主题时，artwork.mode 不能是 none。
7. 彩虹、放射、彩纸、波浪、网格等抽象图形使用 generated 并选择最接近的 motif；人物、城市、产品、动物、自然等真实题材使用 web。
8. web 的 query 必须是 2—6 个具体英文关键词，准确概括用户要求的主体、场景和风格。例如 OOTD 可写为 "outfit of the day street style"，不要只写 image、random、beautiful 等泛词。
9. 不返回图片 URL；系统会用 query 从主题图库随机取图并缓存素材。`;

type GatewayModel = { id?: unknown };
type GatewayModels = { data?: GatewayModel[]; models?: GatewayModel[] };
type ChatCompletion = {
  choices?: Array<{ message?: { content?: unknown } }>;
};

function trimText(value: unknown, fallback: string, max: number) {
  return typeof value === "string" && value.trim() ? value.trim().slice(0, max) : fallback;
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
  if (!value || typeof value !== "object") return fallback;
  const candidate = value as Record<string, unknown>;
  const mode = candidate.mode as ArtworkSpec["mode"] | "none";
  if (!ALLOWED_ARTWORK_MODES.has(mode) || mode === "none") return fallback;
  const rawQuery = trimText(candidate.query, fallback?.query || "colorful editorial illustration", 100);
  const query = rawQuery.replace(/[^a-zA-Z0-9\s,-]/g, " ").replace(/\s+/g, " ").trim();
  const requestedMotif = candidate.motif as ArtworkSpec["motif"];
  const motif = ALLOWED_ARTWORK_MOTIFS.has(requestedMotif)
    ? requestedMotif
    : query.toLowerCase().includes("rainbow")
      ? "rainbow"
      : "grid";
  const requestedLayout = candidate.layout as ArtworkSpec["layout"];
  return {
    mode,
    motif,
    query: query || "colorful editorial illustration",
    layout: mode === "web"
      ? "hero"
      : ALLOWED_ARTWORK_LAYOUTS.has(requestedLayout)
        ? requestedLayout
        : "background",
    seed: stableSeed(`${prompt}:${query}:${motif}:${crypto.randomUUID()}`),
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
  const candidateSchedule = value.scheduleMode as InkApp["scheduleMode"];
  const rawMinutes = Number(value.customMinutes);
  const rawTime = typeof value.dailyTime === "string" ? value.dailyTime : "";

  return {
    ...fallback,
    id: `app-${Date.now()}-${crypto.randomUUID().slice(0, 6)}`,
    title: trimText(value.title, fallback.title, 40),
    description: trimText(value.description, fallback.description, 120),
    prompt,
    spec: {
      kind: ALLOWED_KINDS.has(candidateKind) ? candidateKind : fallback.spec.kind,
      eyebrow: trimText(candidateSpec.eyebrow, fallback.spec.eyebrow, 40),
      title: trimText(candidateSpec.title, fallback.spec.title, 28),
      value: trimText(candidateSpec.value, fallback.spec.value, 20),
      unit: typeof candidateSpec.unit === "string" ? candidateSpec.unit.trim().slice(0, 8) : fallback.spec.unit,
      detail: trimText(candidateSpec.detail, fallback.spec.detail, 48),
      footer: trimText(candidateSpec.footer, fallback.spec.footer, 48),
      accent: ALLOWED_ACCENTS.has(candidateAccent) ? candidateAccent : fallback.spec.accent,
      artwork: normalizeArtwork(candidateSpec.artwork, fallback.spec.artwork, prompt),
    },
    code: trimText(value.code, fallback.code, 8000),
    scheduleMode: ALLOWED_SCHEDULES.has(candidateSchedule) ? candidateSchedule : fallback.scheduleMode,
    customMinutes: Number.isFinite(rawMinutes) ? Math.max(5, Math.min(10080, Math.round(rawMinutes))) : fallback.customMinutes,
    dailyTime: /^([01]\d|2[0-3]):[0-5]\d$/.test(rawTime) ? rawTime : fallback.dailyTime,
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
    provider: "Tsingfly Token Hub",
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
