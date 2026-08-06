import { env } from "cloudflare:workers";

const BAIDU_IP_LOCATION_ENDPOINT = "https://api.map.baidu.com/location/ip";
const BAIDU_GEOCODING_ENDPOINT = "https://api.map.baidu.com/geocoding/v3/";
const BAIDU_REVERSE_GEOCODING_ENDPOINT = "https://api.map.baidu.com/reverse_geocoding/v3/";
const BAIDU_COORD_CONVERT_ENDPOINT = "https://api.map.baidu.com/geoconv/v1/";
const BAIDU_STATIC_MAP_ENDPOINT = "https://api.map.baidu.com/staticimage/v2";
const REQUEST_TIMEOUT_MS = 8_000;
const MAX_JSON_BYTES = 256 * 1024;
const MAX_IMAGE_BYTES = 8 * 1024 * 1024;

type MapPoint = {
  latitude: number;
  longitude: number;
  address: string;
  city?: string;
  approximate: boolean;
  source: "picker" | "browser" | "ip";
};

class MapServiceError extends Error {
  constructor(
    readonly code: string,
    readonly status: number,
    message: string,
    readonly recoverable = true,
  ) {
    super(message);
  }
}

function mapAk() {
  const value = env.BAIDU_MAP_AK?.trim() || "";
  if (!value || !/^[A-Za-z0-9_-]{8,256}$/u.test(value)) {
    throw new MapServiceError(
      "MAP_NOT_CONFIGURED",
      503,
      "地图服务尚未配置。请在服务端设置 BAIDU_MAP_AK 后重新加载预览。",
      true,
    );
  }
  return value;
}

function validNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function coordinate(value: string | null, minimum: number, maximum: number) {
  if (value === null || !value.trim()) return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= minimum && parsed <= maximum ? parsed : undefined;
}

function cleanQuery(value: string | null) {
  return (value || "").replace(/[\u0000-\u001f\u007f]/g, " ").replace(/\s+/g, " ").trim().slice(0, 80);
}

async function fetchJson(url: URL, label: string) {
  let response: Response;
  try {
    response = await fetch(url, {
      headers: { Accept: "application/json" },
      redirect: "error",
      cache: "no-store",
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
  } catch {
    throw new MapServiceError("MAP_UPSTREAM_UNAVAILABLE", 502, `${label}暂时不可用，请稍后重试。`);
  }
  if (!response.ok) {
    throw new MapServiceError("MAP_UPSTREAM_UNAVAILABLE", 502, `${label}暂时不可用，请稍后重试。`);
  }
  const announcedSize = Number(response.headers.get("content-length") || 0);
  if (announcedSize > MAX_JSON_BYTES) {
    throw new MapServiceError("MAP_UPSTREAM_INVALID", 502, `${label}返回了异常数据。`);
  }
  const text = await response.text();
  if (new TextEncoder().encode(text).byteLength > MAX_JSON_BYTES) {
    throw new MapServiceError("MAP_UPSTREAM_INVALID", 502, `${label}返回了异常数据。`);
  }
  try {
    return JSON.parse(text) as Record<string, unknown>;
  } catch {
    throw new MapServiceError("MAP_UPSTREAM_INVALID", 502, `${label}返回了无效数据。`);
  }
}

function baiduStatus(payload: Record<string, unknown>, label: string) {
  const status = Number(payload.status);
  if (status !== 0) {
    const upstreamMessage = typeof payload.message === "string" ? payload.message.trim().slice(0, 160) : "";
    throw new MapServiceError(
      "MAP_UPSTREAM_REJECTED",
      502,
      `${label}没有返回可用结果${upstreamMessage ? `：${upstreamMessage}` : ""}。`,
    );
  }
}

async function locateByIp(request: Request, ak: string): Promise<MapPoint> {
  const url = new URL(BAIDU_IP_LOCATION_ENDPOINT);
  url.searchParams.set("ak", ak);
  url.searchParams.set("coor", "bd09ll");
  const forwardedIp = request.headers.get("CF-Connecting-IP")?.trim();
  if (forwardedIp && /^[0-9a-f:.]{3,64}$/i.test(forwardedIp)) url.searchParams.set("ip", forwardedIp);
  const payload = await fetchJson(url, "IP 粗定位");
  baiduStatus(payload, "IP 粗定位");
  const content = payload.content && typeof payload.content === "object"
    ? payload.content as Record<string, unknown>
    : {};
  const point = content.point && typeof content.point === "object"
    ? content.point as Record<string, unknown>
    : {};
  const longitude = Number(point.x);
  const latitude = Number(point.y);
  if (!validNumber(longitude) || !validNumber(latitude)) {
    throw new MapServiceError("MAP_LOCATION_UNAVAILABLE", 502, "无法通过当前网络估算所在城市。");
  }
  const details = content.address_detail && typeof content.address_detail === "object"
    ? content.address_detail as Record<string, unknown>
    : {};
  const city = typeof details.city === "string" ? details.city.trim() : "";
  const address = typeof content.address === "string" && content.address.trim()
    ? content.address.trim()
    : city || "当前网络所在城市";
  return { latitude, longitude, address, city: city || undefined, approximate: true, source: "ip" };
}

async function geocode(query: string, ak: string): Promise<MapPoint> {
  const url = new URL(BAIDU_GEOCODING_ENDPOINT);
  url.searchParams.set("ak", ak);
  url.searchParams.set("address", query);
  url.searchParams.set("output", "json");
  url.searchParams.set("ret_coordtype", "bd09ll");
  const payload = await fetchJson(url, "地点搜索");
  baiduStatus(payload, "地点搜索");
  const result = payload.result && typeof payload.result === "object"
    ? payload.result as Record<string, unknown>
    : {};
  const location = result.location && typeof result.location === "object"
    ? result.location as Record<string, unknown>
    : {};
  const longitude = Number(location.lng);
  const latitude = Number(location.lat);
  if (!validNumber(longitude) || !validNumber(latitude)) {
    throw new MapServiceError("MAP_LOCATION_UNAVAILABLE", 404, `没有找到“${query}”，请换个完整地址或在地图上选点。`);
  }
  return { latitude, longitude, address: query, approximate: false, source: "picker" };
}

async function convertWgs84(longitude: number, latitude: number, ak: string) {
  const url = new URL(BAIDU_COORD_CONVERT_ENDPOINT);
  url.searchParams.set("ak", ak);
  url.searchParams.set("coords", `${longitude.toFixed(7)},${latitude.toFixed(7)}`);
  url.searchParams.set("from", "1");
  url.searchParams.set("to", "5");
  const payload = await fetchJson(url, "坐标转换");
  baiduStatus(payload, "坐标转换");
  const result = Array.isArray(payload.result) ? payload.result[0] : undefined;
  const point = result && typeof result === "object" ? result as Record<string, unknown> : {};
  const convertedLongitude = Number(point.x);
  const convertedLatitude = Number(point.y);
  if (!validNumber(convertedLongitude) || !validNumber(convertedLatitude)) {
    throw new MapServiceError("MAP_LOCATION_UNAVAILABLE", 502, "浏览器位置暂时无法转换为地图坐标。");
  }
  return { longitude: convertedLongitude, latitude: convertedLatitude };
}

async function reverseGeocode(longitude: number, latitude: number, ak: string) {
  const url = new URL(BAIDU_REVERSE_GEOCODING_ENDPOINT);
  url.searchParams.set("ak", ak);
  url.searchParams.set("location", `${latitude.toFixed(7)},${longitude.toFixed(7)}`);
  url.searchParams.set("coordtype", "bd09ll");
  url.searchParams.set("ret_coordtype", "bd09ll");
  url.searchParams.set("output", "json");
  const payload = await fetchJson(url, "地址解析");
  baiduStatus(payload, "地址解析");
  const result = payload.result && typeof payload.result === "object"
    ? payload.result as Record<string, unknown>
    : {};
  const component = result.addressComponent && typeof result.addressComponent === "object"
    ? result.addressComponent as Record<string, unknown>
    : {};
  const address = typeof result.formatted_address === "string" ? result.formatted_address.trim() : "";
  const semantic = typeof result.sematic_description === "string" ? result.sematic_description.trim() : "";
  const city = typeof component.city === "string" ? component.city.trim() : "";
  return {
    address: [address, semantic].filter(Boolean).join(" · ") || city || "已选位置",
    city: city || undefined,
  };
}

async function resolveLocation(request: Request, url: URL, ak: string, includeAddress = true): Promise<MapPoint> {
  const mode = url.searchParams.get("locationMode");
  const latitude = coordinate(url.searchParams.get("lat"), -90, 90);
  const longitude = coordinate(url.searchParams.get("lng"), -180, 180);
  if (latitude !== undefined && longitude !== undefined) {
    const converted = url.searchParams.get("coordtype") === "wgs84ll"
      ? await convertWgs84(longitude, latitude, ak)
      : { longitude, latitude };
    const address = includeAddress
      ? await reverseGeocode(converted.longitude, converted.latitude, ak).catch(() => ({
          address: "已选位置",
          city: undefined,
        }))
      : { address: "已选位置", city: undefined };
    return {
      ...converted,
      ...address,
      approximate: false,
      source: mode === "browser" ? "browser" : "picker",
    };
  }

  if (mode === "ip") return locateByIp(request, ak);

  const query = cleanQuery(url.searchParams.get("query"));
  if (query) return geocode(query, ak);

  const fallback = await locateByIp(request, ak);
  return {
    ...fallback,
    address: `${fallback.address}（尚未选点）`,
  };
}

function staticMapUrl(ak: string, url: URL, point: MapPoint) {
  const landscape = url.searchParams.get("orientation") === "landscape";
  const logicalWidth = landscape ? 792 : 528;
  const logicalHeight = landscape ? 528 : 792;
  const zoom = Math.min(19, Math.max(3, Math.round(Number(url.searchParams.get("zoom")) || 17)));
  const marker = url.searchParams.get("marker") !== "false";
  const center = `${point.longitude.toFixed(7)},${point.latitude.toFixed(7)}`;
  const imageUrl = new URL(BAIDU_STATIC_MAP_ENDPOINT);
  imageUrl.searchParams.set("ak", ak);
  imageUrl.searchParams.set("width", String(logicalWidth / 2));
  imageUrl.searchParams.set("height", String(logicalHeight / 2));
  imageUrl.searchParams.set("scale", "2");
  imageUrl.searchParams.set("center", center);
  imageUrl.searchParams.set("zoom", String(zoom));
  imageUrl.searchParams.set("coordtype", "bd09ll");
  imageUrl.searchParams.set("copyright", "1");
  if (marker) {
    imageUrl.searchParams.set("markers", center);
    imageUrl.searchParams.set("markerStyles", "l,P,0x151816");
  }
  return imageUrl;
}

function errorResponse(error: unknown) {
  const known = error instanceof MapServiceError
    ? error
    : new MapServiceError("MAP_UNKNOWN", 502, "地图服务暂时不可用，请稍后重试。");
  return Response.json({
    error: known.message,
    code: known.code,
    recoverable: known.recoverable,
  }, {
    status: known.status,
    headers: { "Cache-Control": "no-store" },
  });
}

export async function GET(request: Request) {
  try {
    const url = new URL(request.url);
    const ak = mapAk();
    if (url.searchParams.get("mode") === "status") {
      return Response.json({ configured: true });
    }
    const mode = url.searchParams.get("mode");
    const point = await resolveLocation(request, url, ak, mode !== "image");
    if (mode !== "image") {
      return Response.json({
        configured: true,
        coordinateType: "bd09ll",
        ...point,
      }, {
        headers: { "Cache-Control": point.approximate ? "private, max-age=300" : "private, max-age=3600" },
      });
    }

    const imageUrl = staticMapUrl(ak, url, point);
    let response: Response;
    try {
      response = await fetch(imageUrl, {
        headers: { Accept: "image/png,image/jpeg" },
        redirect: "error",
        signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
      });
    } catch {
      throw new MapServiceError("MAP_IMAGE_UNAVAILABLE", 502, "静态地图暂时无法获取，请稍后重试。");
    }
    const contentType = response.headers.get("content-type") || "";
    if (!response.ok || !contentType.startsWith("image/")) {
      const detail = await response.text().catch(() => "");
      let upstreamMessage = "";
      try {
        const payload = JSON.parse(detail) as { message?: unknown };
        upstreamMessage = typeof payload.message === "string" ? payload.message.trim().slice(0, 160) : "";
      } catch {
        upstreamMessage = detail.trim().slice(0, 160);
      }
      throw new MapServiceError(
        "MAP_IMAGE_UNAVAILABLE",
        502,
        `静态地图请求被拒绝${upstreamMessage ? `：${upstreamMessage}` : ""}。`,
      );
    }
    const declaredSize = Number(response.headers.get("content-length") || 0);
    if (declaredSize > MAX_IMAGE_BYTES) {
      throw new MapServiceError("MAP_IMAGE_INVALID", 502, "静态地图图片过大，无法用于屏幕预览。");
    }
    const body = await response.arrayBuffer();
    if (!body.byteLength || body.byteLength > MAX_IMAGE_BYTES) {
      throw new MapServiceError("MAP_IMAGE_INVALID", 502, "静态地图返回了无效图片。");
    }
    return new Response(body, {
      headers: {
        "Content-Type": contentType,
        "Cache-Control": point.approximate
          ? "private, max-age=300"
          : "public, max-age=600, s-maxage=3600, stale-while-revalidate=86400",
        "X-Content-Type-Options": "nosniff",
      },
    });
  } catch (error) {
    return errorResponse(error);
  }
}
