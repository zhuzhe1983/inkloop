import { env } from "cloudflare:workers";
import type { InkApp, ScheduleMode, ScreenSpec } from "../../lib/app-model";

const CREATE_TABLE = `CREATE TABLE IF NOT EXISTS public_apps (
  id TEXT PRIMARY KEY NOT NULL,
  title TEXT NOT NULL,
  description TEXT NOT NULL,
  prompt TEXT NOT NULL,
  spec TEXT NOT NULL,
  code TEXT NOT NULL,
  schedule_mode TEXT NOT NULL,
  custom_minutes INTEGER NOT NULL DEFAULT 30,
  daily_time TEXT NOT NULL DEFAULT '08:00',
  author TEXT NOT NULL DEFAULT '匿名创作者',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
)`;

const CREATE_INDEX = `CREATE INDEX IF NOT EXISTS idx_public_apps_created_at
ON public_apps(created_at DESC)`;

type AppRow = {
  id: string;
  title: string;
  description: string;
  prompt: string;
  spec: string;
  code: string;
  schedule_mode: ScheduleMode;
  custom_minutes: number;
  daily_time: string;
  author: string;
  created_at: string;
};

async function ensureSchema() {
  const db = env.DB;
  if (!db) throw new Error("Public gallery database is unavailable");
  await db.batch([
    db.prepare(CREATE_TABLE),
    db.prepare(CREATE_INDEX),
  ]);
  return db;
}

function cleanText(value: unknown, max: number) {
  return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function rowToApp(row: AppRow): InkApp {
  return {
    id: row.id,
    title: row.title,
    description: row.description,
    prompt: row.prompt,
    spec: JSON.parse(row.spec) as ScreenSpec,
    code: row.code,
    scheduleMode: row.schedule_mode,
    customMinutes: row.custom_minutes,
    dailyTime: row.daily_time,
    isPublic: true,
    author: row.author,
    createdAt: row.created_at,
  };
}

export async function GET() {
  try {
    const db = await ensureSchema();
    const result = await db
      .prepare(`SELECT id, title, description, prompt, spec, code, schedule_mode,
        custom_minutes, daily_time, author, created_at
        FROM public_apps ORDER BY created_at DESC LIMIT 40`)
      .all<AppRow>();
    return Response.json({ apps: result.results.map(rowToApp) });
  } catch (error) {
    return Response.json(
      { error: error instanceof Error ? error.message : "Unable to load public apps" },
      { status: 503 },
    );
  }
}

export async function POST(request: Request) {
  try {
    const payload = (await request.json()) as Partial<InkApp>;
    const title = cleanText(payload.title, 80);
    const description = cleanText(payload.description, 180);
    const prompt = cleanText(payload.prompt, 500);
    const code = cleanText(payload.code, 8000);
    const author = cleanText(payload.author, 40) || "匿名创作者";
    const scheduleMode = (["once", "hourly", "daily", "custom"] as const).includes(
      payload.scheduleMode as ScheduleMode,
    )
      ? (payload.scheduleMode as ScheduleMode)
      : "once";

    if (!title || !description || !prompt || !payload.spec || !code) {
      return Response.json({ error: "应用信息不完整" }, { status: 400 });
    }

    const id = cleanText(payload.id, 90) || crypto.randomUUID();
    const spec = JSON.stringify(payload.spec).slice(0, 5000);
    const customMinutes = Math.max(5, Math.min(10080, Number(payload.customMinutes) || 30));
    const dailyTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(payload.dailyTime ?? "")
      ? payload.dailyTime!
      : "08:00";
    const createdAt = new Date().toISOString();
    const db = await ensureSchema();
    await db
      .prepare(`INSERT INTO public_apps
        (id, title, description, prompt, spec, code, schedule_mode, custom_minutes, daily_time, author, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
          title = excluded.title,
          description = excluded.description,
          prompt = excluded.prompt,
          spec = excluded.spec,
          code = excluded.code,
          schedule_mode = excluded.schedule_mode,
          custom_minutes = excluded.custom_minutes,
          daily_time = excluded.daily_time,
          author = excluded.author`)
      .bind(
        id,
        title,
        description,
        prompt,
        spec,
        code,
        scheduleMode,
        customMinutes,
        dailyTime,
        author,
        createdAt,
      )
      .run();

    return Response.json(
      {
        app: {
          id,
          title,
          description,
          prompt,
          spec: payload.spec,
          code,
          scheduleMode,
          customMinutes,
          dailyTime,
          isPublic: true,
          author,
          createdAt,
        } satisfies InkApp,
      },
      { status: 201 },
    );
  } catch (error) {
    return Response.json(
      { error: error instanceof Error ? error.message : "Unable to publish app" },
      { status: 500 },
    );
  }
}
