import http from "node:http";
import { pathToFileURL } from "node:url";

const llmUpstream = new URL(process.env.LLM_PROXY_UPSTREAM || "https://hub.tsingfly.com");
const baiduMapUpstream = new URL(process.env.BAIDU_MAP_UPSTREAM || "https://api.map.baidu.com");
const port = Number.parseInt(process.env.LLM_PROXY_PORT || "8788", 10);
const allowedLlmPaths = new Set(["/v1/models", "/v1/chat/completions"]);
const baiduMapPaths = new Set([
  "/location/ip",
  "/geocoding/v3/",
  "/reverse_geocoding/v3/",
  "/geoconv/v1/",
  "/staticimage/v2",
]);
const allowedOutboundHostSuffixes = [
  "loremflickr.com",
  "staticflickr.com",
  "flickr.com",
  "wikimedia.org",
  "picsum.photos",
  "open-meteo.com",
  "wttr.in",
];
const maxBodyBytes = 4 * 1024 * 1024;
const maxOutboundBytes = 8 * 1024 * 1024;
const maxOutboundRedirects = 5;

function sendJson(response, status, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
    "cache-control": "no-store",
  });
  response.end(body);
}

function isAllowedOutboundHost(hostname) {
  const host = String(hostname || "").toLowerCase().replace(/\.$/u, "");
  return allowedOutboundHostSuffixes.some((suffix) => host === suffix || host.endsWith(`.${suffix}`));
}

function resolveOutboundTarget(rawUrl) {
  if (typeof rawUrl !== "string" || !rawUrl.trim()) return null;
  let parsed;
  try {
    parsed = new URL(rawUrl);
  } catch {
    return null;
  }
  if (parsed.protocol !== "https:") return null;
  if (parsed.username || parsed.password) return null;
  if (parsed.port && parsed.port !== "443") return null;
  const hostname = parsed.hostname.toLowerCase().replace(/\.$/u, "");
  if (!hostname || hostname.includes(":") || !isAllowedOutboundHost(hostname)) return null;
  return parsed;
}

async function readBody(request) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > maxBodyBytes) throw new Error("request_too_large");
    chunks.push(chunk);
  }
  return chunks.length ? Buffer.concat(chunks) : undefined;
}

async function fetchOutbound(initialUrl, requestHeaders) {
  let current = initialUrl;
  for (let redirects = 0; redirects <= maxOutboundRedirects; redirects += 1) {
    const target = resolveOutboundTarget(current);
    if (!target) throw new Error("outbound_forbidden");
    const headers = new Headers();
    for (const name of ["accept", "user-agent", "api-user-agent"]) {
      const value = requestHeaders[name];
      if (typeof value === "string") headers.set(name, value);
    }
    const upstreamResponse = await fetch(target, {
      method: "GET",
      headers,
      redirect: "manual",
      signal: AbortSignal.timeout(20_000),
    });
    if (upstreamResponse.status >= 300 && upstreamResponse.status < 400) {
      const location = upstreamResponse.headers.get("location");
      await upstreamResponse.arrayBuffer().catch(() => undefined);
      if (!location) throw new Error("outbound_redirect_missing");
      current = new URL(location, target).toString();
      continue;
    }
    const declaredSize = Number(upstreamResponse.headers.get("content-length") || 0);
    if (declaredSize > maxOutboundBytes) throw new Error("outbound_too_large");
    const responseBody = Buffer.from(await upstreamResponse.arrayBuffer());
    if (responseBody.length > maxOutboundBytes) throw new Error("outbound_too_large");
    return { response: upstreamResponse, body: responseBody, finalUrl: target.toString() };
  }
  throw new Error("outbound_too_many_redirects");
}

const server = http.createServer(async (request, response) => {
  if (request.url === "/health") {
    return sendJson(response, 200, { status: "ok" });
  }

  const incomingUrl = new URL(request.url || "/", "http://inkloop-llm-proxy");
  const method = request.method || "GET";
  const mapPath = incomingUrl.pathname.startsWith("/baidu/")
    ? incomingUrl.pathname.slice("/baidu".length)
    : "";
  const llmRequest = allowedLlmPaths.has(incomingUrl.pathname) && ["GET", "POST"].includes(method);
  const mapRequest = baiduMapPaths.has(mapPath) && method === "GET";
  const outboundRequest = incomingUrl.pathname.replace(/\/+$/u, "") === "/outbound" && method === "GET";
  if (!llmRequest && !mapRequest && !outboundRequest) {
    return sendJson(response, 404, { error: "not_found" });
  }

  try {
    if (outboundRequest) {
      const target = resolveOutboundTarget(incomingUrl.searchParams.get("url") || "");
      if (!target) return sendJson(response, 400, { error: "invalid_outbound_url" });
      const upstream = await fetchOutbound(target.toString(), request.headers);
      response.writeHead(upstream.response.status, {
        "content-type": upstream.response.headers.get("content-type") || "application/octet-stream",
        "content-length": upstream.body.length,
        "cache-control": "no-store",
        "x-upstream-url": upstream.finalUrl,
      });
      response.end(upstream.body);
      return;
    }

    const target = new URL(
      `${mapRequest ? mapPath : incomingUrl.pathname}${incomingUrl.search}`,
      mapRequest ? baiduMapUpstream : llmUpstream,
    );
    const headers = new Headers();
    for (const name of ["accept", "authorization", "content-type", "user-agent"]) {
      const value = request.headers[name];
      if (typeof value === "string") headers.set(name, value);
    }

    let body = method === "POST" ? await readBody(request) : undefined;
    if (body && incomingUrl.pathname === "/v1/chat/completions") {
      try {
        const payload = JSON.parse(body.toString("utf8"));
        if (typeof payload.model === "string" && payload.model.startsWith("Qwen/")) {
          payload.chat_template_kwargs = {
            ...(payload.chat_template_kwargs || {}),
            enable_thinking: false,
          };
          body = Buffer.from(JSON.stringify(payload));
        }
      } catch {
        // Forward malformed JSON unchanged so the upstream returns its normal error.
      }
    }
    const upstreamResponse = await fetch(target, {
      method,
      headers,
      body,
      redirect: "error",
      signal: AbortSignal.timeout(45_000),
    });
    const responseBody = Buffer.from(await upstreamResponse.arrayBuffer());
    response.writeHead(upstreamResponse.status, {
      "content-type": upstreamResponse.headers.get("content-type") || "application/json; charset=utf-8",
      "content-length": responseBody.length,
      "cache-control": "no-store",
    });
    response.end(responseBody);
  } catch (error) {
    const message = error instanceof Error ? error.message : "";
    const reason = message === "request_too_large" || message === "outbound_too_large"
      ? "request_too_large"
      : message === "outbound_forbidden" || message === "outbound_redirect_missing" || message === "outbound_too_many_redirects"
        ? message
        : "upstream_unavailable";
    const status = reason === "request_too_large" ? 413 : reason.startsWith("outbound_") ? 400 : 502;
    sendJson(response, status, { error: reason });
  }
});

function isMainModule() {
  return Boolean(process.argv[1]) && import.meta.url === pathToFileURL(process.argv[1]).href;
}

if (isMainModule()) {
  server.listen(port, "0.0.0.0", () => {
    console.log(`Inkloop LLM proxy listening on ${port}`);
  });
}

export {
  isAllowedOutboundHost,
  resolveOutboundTarget,
  server,
};
