declare namespace Cloudflare {
  interface Env {
    DB: D1Database;
    TSINGFLY_API_KEY?: string;
    TOKENHUB_API_KEY?: string;
    APP_KEY?: string;
    TSINGFLY_BASE_URL?: string;
    TSINGFLY_MODEL?: string;
  }
}
