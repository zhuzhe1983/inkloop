const WIDTH = 528;
const HEIGHT = 792;
const MAX_IMAGE_BYTES = 5 * 1024 * 1024;

function cleanQuery(value: string | null) {
  return (value || "colorful editorial illustration")
    .slice(0, 100)
    .replace(/[^a-zA-Z0-9\s,-]/g, " ")
    .replace(/\s+/g, " ")
    .trim() || "colorful editorial illustration";
}

function cleanStyle(value: string | null) {
  return (value || "editorial high contrast composition")
    .slice(0, 80)
    .replace(/[^a-zA-Z0-9\s,-]/g, " ")
    .replace(/\s+/g, " ")
    .trim() || "editorial high contrast composition";
}

function cleanSeed(value: string | null) {
  const seed = Number.parseInt(value || "1", 10);
  return Number.isFinite(seed) ? Math.max(1, Math.min(999_999_999, seed)) : 1;
}

async function fetchImage(url: string) {
  const response = await fetch(url, {
    redirect: "follow",
    headers: { Accept: "image/avif,image/webp,image/jpeg,image/png" },
    signal: AbortSignal.timeout(12_000),
  });
  const contentType = response.headers.get("content-type") || "";
  if (!response.ok || !contentType.startsWith("image/")) return null;
  const declaredSize = Number(response.headers.get("content-length") || 0);
  if (declaredSize > MAX_IMAGE_BYTES) return null;
  const body = await response.arrayBuffer();
  if (!body.byteLength || body.byteLength > MAX_IMAGE_BYTES) return null;
  return { body, contentType };
}

export async function GET(request: Request) {
  const url = new URL(request.url);
  const query = cleanQuery(url.searchParams.get("query"));
  const style = cleanStyle(url.searchParams.get("style"));
  const seed = cleanSeed(url.searchParams.get("seed"));
  const keywords = `${query} ${style}`.split(/[\s,]+/).filter(Boolean).slice(0, 10).join(",");
  const providers = [
    `https://loremflickr.com/${WIDTH}/${HEIGHT}/${encodeURIComponent(keywords)}?lock=${seed}`,
    `https://picsum.photos/seed/${encodeURIComponent(`${keywords}-${seed}`)}/${WIDTH}/${HEIGHT}`,
  ];

  for (const provider of providers) {
    try {
      const image = await fetchImage(provider);
      if (!image) continue;
      return new Response(image.body, {
        headers: {
          "Content-Type": image.contentType,
          "Cache-Control": "public, max-age=86400, s-maxage=604800, immutable",
          "X-Content-Type-Options": "nosniff",
        },
      });
    } catch {
      // Try the deterministic fallback provider.
    }
  }

  return Response.json({ error: "暂时无法获取图片素材" }, { status: 502 });
}
