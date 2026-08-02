const WIDTH = 528;
const HEIGHT = 792;
const MAX_IMAGE_BYTES = 5 * 1024 * 1024;

type CommonsResponse = {
  query?: {
    pages?: Array<{
      title?: string;
      index?: number;
      imageinfo?: Array<{
        thumburl?: string;
        url?: string;
        mime?: string;
        width?: number;
        height?: number;
        thumbwidth?: number;
        thumbheight?: number;
      }>;
    }>;
  };
};

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
    headers: {
      Accept: "image/avif,image/webp,image/jpeg,image/png",
      "User-Agent": "Inkloop/1.1 (TodooCard artwork preview)",
    },
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

async function fetchCommonsImage(query: string, seed: number) {
  const ignoredTokens = new Set([
    "photo", "photography", "portrait", "image", "movie", "cinematic", "editorial",
    "high", "contrast", "composition", "background", "superhero",
  ]);
  const subjectTokens = query.toLowerCase().split(/[^a-z0-9-]+/)
    .filter((token) => token.length >= 3 && !ignoredTokens.has(token) && token !== "poster");
  const searchQuery = [
    subjectTokens.slice(0, 4).join(" ") || query,
    query.toLowerCase().includes("poster") ? "poster" : "",
  ].filter(Boolean).join(" ");
  const apiUrl = new URL("https://commons.wikimedia.org/w/api.php");
  apiUrl.search = new URLSearchParams({
    action: "query",
    generator: "search",
    gsrsearch: searchQuery,
    gsrnamespace: "6",
    gsrlimit: "16",
    prop: "imageinfo",
    iiprop: "url|mime|size",
    iiurlwidth: "900",
    format: "json",
    formatversion: "2",
  }).toString();
  const response = await fetch(apiUrl, {
    headers: {
      Accept: "application/json",
      "Api-User-Agent": "Inkloop/1.0 (TodooCard artwork preview)",
    },
    signal: AbortSignal.timeout(8_000),
  });
  if (!response.ok) throw new Error(`Wikimedia Commons ${response.status}`);
  const payload = (await response.json()) as CommonsResponse;
  const targetRatio = WIDTH / HEIGHT;
  const candidates = (payload.query?.pages || [])
    .flatMap((page) => (page.imageinfo || []).map((info) => ({
      ...info,
      title: page.title || "",
      searchIndex: page.index ?? 999,
    })))
    .filter((info) => info.mime?.startsWith("image/") && info.mime !== "image/svg+xml" && (info.thumburl || info.url))
    .map((info) => ({
      ...info,
      relevance: subjectTokens.reduce(
        (score, token) => score + (info.title.toLowerCase().includes(token) ? 1 : 0),
        0,
      ),
    }))
    .sort((left, right) => {
      if (left.relevance !== right.relevance) return right.relevance - left.relevance;
      const leftRatio = (left.thumbwidth || left.width || WIDTH) / (left.thumbheight || left.height || HEIGHT);
      const rightRatio = (right.thumbwidth || right.width || WIDTH) / (right.thumbheight || right.height || HEIGHT);
      const aspectDifference = Math.abs(leftRatio - targetRatio) - Math.abs(rightRatio - targetRatio);
      return Math.abs(aspectDifference) > 0.08 ? aspectDifference : left.searchIndex - right.searchIndex;
    })
    .slice(0, 4);
  if (!candidates.length) return null;
  const selected = candidates[seed % candidates.length];
  return fetchImage(selected.thumburl || selected.url || "");
}

export async function GET(request: Request) {
  const url = new URL(request.url);
  const query = cleanQuery(url.searchParams.get("query"));
  const style = cleanStyle(url.searchParams.get("style"));
  const seed = cleanSeed(url.searchParams.get("seed"));
  const keywords = `${query} ${style}`.split(/[\s,]+/).filter(Boolean).slice(0, 10).join(",");
  const subjectKeywords = query.split(/[\s,]+/)
    .filter((token) => token && !["photo", "photography", "image", "background", "adult"].includes(token.toLowerCase()))
    .slice(0, 6)
    .join(",");
  const failures: string[] = [];

  try {
    const themedUrl = `https://loremflickr.com/${WIDTH}/${HEIGHT}/${encodeURIComponent(subjectKeywords || keywords)}?lock=${seed}`;
    const image = await fetchImage(themedUrl);
    if (image) {
      return new Response(image.body, {
        headers: {
          "Content-Type": image.contentType,
          "Cache-Control": "public, max-age=3600, s-maxage=86400, stale-while-revalidate=3600",
          "X-Content-Type-Options": "nosniff",
          "X-Inkloop-Image-Source": "loremflickr",
        },
      });
    }
    failures.push("loremflickr returned no image");
  } catch (error) {
    failures.push(error instanceof Error ? `loremflickr: ${error.message}` : "loremflickr failed");
  }

  try {
    const image = await fetchCommonsImage(query, seed);
    if (image) {
      return new Response(image.body, {
        headers: {
          "Content-Type": image.contentType,
          "Cache-Control": "public, max-age=3600, s-maxage=86400, stale-while-revalidate=3600",
          "X-Content-Type-Options": "nosniff",
          "X-Inkloop-Image-Source": "wikimedia-commons",
        },
      });
    }
    failures.push("Wikimedia Commons returned no image");
  } catch (error) {
    failures.push(error instanceof Error ? error.message : "Wikimedia Commons failed");
  }

  const genericQuery = /^(colorful editorial illustration|abstract|texture|pattern)/i.test(query);
  const providers: Array<[string, string]> = genericQuery
    ? [["picsum", `https://picsum.photos/seed/${encodeURIComponent(`${keywords}-${seed}`)}/${WIDTH}/${HEIGHT}`]]
    : [];

  for (const [name, provider] of providers) {
    try {
      const image = await fetchImage(provider);
      if (!image) {
        failures.push(`${name} returned no image`);
        continue;
      }
      return new Response(image.body, {
        headers: {
          "Content-Type": image.contentType,
          "Cache-Control": "public, max-age=3600, s-maxage=86400, stale-while-revalidate=3600",
          "X-Content-Type-Options": "nosniff",
          "X-Inkloop-Image-Source": name,
        },
      });
    } catch (error) {
      failures.push(error instanceof Error ? `${name}: ${error.message}` : `${name} failed`);
      // Try the deterministic fallback provider.
    }
  }

  console.warn("artwork-providers-unavailable", { query, failures });
  return Response.json({ error: "暂时无法获取图片素材" }, { status: 502 });
}
