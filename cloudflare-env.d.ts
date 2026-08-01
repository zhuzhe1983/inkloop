declare namespace Cloudflare {
  interface Env {
    DB: D1Database;
    LLM_API_KEY?: string;
    LLM_BASE_URL?: string;
    LLM_MODEL?: string;
  }
}
