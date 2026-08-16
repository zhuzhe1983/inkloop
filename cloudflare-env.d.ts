declare namespace Cloudflare {
  interface Env {
    DB: D1Database;
    ASSETS: R2Bucket;
    LLM_API_KEY?: string;
    LLM_BASE_URL?: string;
    LLM_MODEL?: string;
    BAIDU_MAP_AK?: string;
    BAIDU_MAP_BASE_URL?: string;
    OUTBOUND_PROXY_BASE_URL?: string;
  }
}
