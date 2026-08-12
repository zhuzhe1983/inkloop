import http from "node:http";

const upstream = new URL(process.env.LLM_PROXY_UPSTREAM || "https://hub.tsingfly.com");
const port = Number.parseInt(process.env.LLM_PROXY_PORT || "8788", 10);
const allowedPaths = new Set(["/v1/models", "/v1/chat/completions"]);
const maxBodyBytes = 4 * 1024 * 1024;

function sendJson(response, status, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
    "cache-control": "no-store",
  });
  response.end(body);
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

const server = http.createServer(async (request, response) => {
  if (request.url === "/health") {
    return sendJson(response, 200, { status: "ok" });
  }

  const incomingUrl = new URL(request.url || "/", "http://inkloop-llm-proxy");
  const method = request.method || "GET";
  if (!allowedPaths.has(incomingUrl.pathname) || !["GET", "POST"].includes(method)) {
    return sendJson(response, 404, { error: "not_found" });
  }

  try {
    const target = new URL(incomingUrl.pathname + incomingUrl.search, upstream);
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
    const reason = error instanceof Error && error.message === "request_too_large"
      ? "request_too_large"
      : "upstream_unavailable";
    sendJson(response, reason === "request_too_large" ? 413 : 502, { error: reason });
  }
});

server.listen(port, "0.0.0.0", () => {
  console.log(`Inkloop LLM proxy listening on ${port}`);
});
