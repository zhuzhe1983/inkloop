import { env } from "cloudflare:workers";

export async function GET() {
  try {
    if (!env.DB) throw new Error("DB binding is unavailable");
    await env.DB.prepare("SELECT 1 AS ok").first();
    return Response.json({
      status: "ok",
      database: "ready",
      llmConfigured: Boolean(env.LLM_API_KEY),
      mapConfigured: Boolean(env.BAIDU_MAP_AK),
      checkedAt: new Date().toISOString(),
    }, {
      headers: { "Cache-Control": "no-store" },
    });
  } catch (error) {
    return Response.json({
      status: "unhealthy",
      database: "unavailable",
      error: error instanceof Error ? error.message : "Health check failed",
      checkedAt: new Date().toISOString(),
    }, {
      status: 503,
      headers: { "Cache-Control": "no-store" },
    });
  }
}
