import { env } from "cloudflare:workers";

import { outboundUrl } from "../../lib/outbound-url";

type GeocodingResponse = {
  results?: Array<{
    name?: string;
    latitude?: number;
    longitude?: number;
    timezone?: string;
  }>;
};

type ForecastResponse = {
  current?: { temperature_2m?: number; weather_code?: number };
  daily?: {
    temperature_2m_max?: number[];
    temperature_2m_min?: number[];
    precipitation_probability_max?: number[];
    weather_code?: number[];
  };
};

type WttrResponse = {
  current_condition?: Array<{
    temp_C?: string;
    weatherDesc?: Array<{ value?: string }>;
  }>;
  weather?: Array<{
    mintempC?: string;
    maxtempC?: string;
    hourly?: Array<{ chanceofrain?: string }>;
  }>;
};

type Location = { name: string; latitude: number; longitude: number; timezone: string };

const COMMON_CITIES: Record<string, Omit<Location, "name">> = {
  上海: { latitude: 31.2304, longitude: 121.4737, timezone: "Asia/Shanghai" },
  北京: { latitude: 39.9042, longitude: 116.4074, timezone: "Asia/Shanghai" },
  深圳: { latitude: 22.5431, longitude: 114.0579, timezone: "Asia/Shanghai" },
  广州: { latitude: 23.1291, longitude: 113.2644, timezone: "Asia/Shanghai" },
  杭州: { latitude: 30.2741, longitude: 120.1551, timezone: "Asia/Shanghai" },
  成都: { latitude: 30.5728, longitude: 104.0668, timezone: "Asia/Shanghai" },
  重庆: { latitude: 29.4316, longitude: 106.9123, timezone: "Asia/Shanghai" },
  南京: { latitude: 32.0603, longitude: 118.7969, timezone: "Asia/Shanghai" },
  苏州: { latitude: 31.2989, longitude: 120.5853, timezone: "Asia/Shanghai" },
  武汉: { latitude: 30.5928, longitude: 114.3055, timezone: "Asia/Shanghai" },
};

function conditionFor(code: number) {
  if (code === 0) return "晴";
  if (code === 1) return "大致晴朗";
  if (code === 2) return "局部多云";
  if (code === 3) return "阴天";
  if (code === 45 || code === 48) return "有雾";
  if (code >= 51 && code <= 57) return "毛毛雨";
  if (code >= 61 && code <= 67) return "有雨";
  if (code >= 71 && code <= 77) return "有雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code >= 85 && code <= 86) return "阵雪";
  if (code >= 95) return "雷雨";
  return "天气多变";
}

function validNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

async function fetchJson<T>(url: URL | string, label: string) {
  const response = await fetch(outboundUrl(url, env.OUTBOUND_PROXY_BASE_URL), {
    headers: { Accept: "application/json" },
    signal: AbortSignal.timeout(8_000),
  });
  if (!response.ok) throw new Error(`${label} ${response.status}`);
  return response.json() as Promise<T>;
}

async function resolveLocation(city: string): Promise<Location> {
  const commonName = Object.keys(COMMON_CITIES).find((name) => city.includes(name));
  if (commonName) return { name: commonName, ...COMMON_CITIES[commonName] };

  const url = new URL("https://geocoding-api.open-meteo.com/v1/search");
  url.search = new URLSearchParams({ name: city, count: "1", language: "zh", format: "json" }).toString();
  const payload = await fetchJson<GeocodingResponse>(url, "Open-Meteo geocoding");
  const result = payload.results?.[0];
  if (!result || !validNumber(result.latitude) || !validNumber(result.longitude)) {
    throw new Error(`没有找到城市：${city}`);
  }
  return {
    name: result.name || city,
    latitude: result.latitude,
    longitude: result.longitude,
    timezone: result.timezone || "auto",
  };
}

async function getOpenMeteo(city: string) {
  const location = await resolveLocation(city);
  const url = new URL("https://api.open-meteo.com/v1/forecast");
  url.search = new URLSearchParams({
    latitude: String(location.latitude),
    longitude: String(location.longitude),
    current: "temperature_2m,weather_code",
    daily: "temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code",
    timezone: location.timezone,
    forecast_days: "1",
  }).toString();
  const forecast = await fetchJson<ForecastResponse>(url, "Open-Meteo forecast");
  const temperature = forecast.current?.temperature_2m;
  const low = forecast.daily?.temperature_2m_min?.[0];
  const high = forecast.daily?.temperature_2m_max?.[0];
  if (!validNumber(temperature) || !validNumber(low) || !validNumber(high)) {
    throw new Error("Open-Meteo data incomplete");
  }
  const rainProbability = forecast.daily?.precipitation_probability_max?.[0] ?? 0;
  const weatherCode = forecast.current?.weather_code ?? forecast.daily?.weather_code?.[0] ?? -1;
  return {
    city: location.name,
    temperature,
    low,
    high,
    rainProbability: validNumber(rainProbability) ? rainProbability : 0,
    condition: conditionFor(weatherCode),
    source: "Open-Meteo",
  };
}

async function getWttr(city: string) {
  const payload = await fetchJson<WttrResponse>(
    `https://wttr.in/${encodeURIComponent(city)}?format=j1`,
    "wttr.in",
  );
  const current = payload.current_condition?.[0];
  const day = payload.weather?.[0];
  const temperature = Number(current?.temp_C);
  const low = Number(day?.mintempC);
  const high = Number(day?.maxtempC);
  const rainChances = day?.hourly?.map((hour) => Number(hour.chanceofrain)).filter(Number.isFinite) || [];
  if (![temperature, low, high].every(Number.isFinite)) throw new Error("wttr.in data incomplete");
  return {
    city,
    temperature,
    low,
    high,
    rainProbability: rainChances.length ? Math.max(...rainChances) : 0,
    condition: current?.weatherDesc?.[0]?.value || "天气多变",
    source: "wttr.in",
  };
}

export async function GET(request: Request) {
  const city = new URL(request.url).searchParams.get("city")?.trim().slice(0, 30) || "上海";
  const failures: string[] = [];

  for (const provider of [getOpenMeteo, getWttr]) {
    try {
      const weather = await provider(city);
      return Response.json(
        { available: true, ...weather, updatedAt: new Date().toISOString() },
        { headers: { "Cache-Control": "public, max-age=600, stale-while-revalidate=86400" } },
      );
    } catch (error) {
      failures.push(error instanceof Error ? error.message : "unknown provider error");
    }
  }

  console.warn("weather-providers-unavailable", { city, failures });
  return Response.json(
    { available: false, city, error: "天气服务暂时不可用", updatedAt: new Date().toISOString() },
    { headers: { "Cache-Control": "no-store" } },
  );
}
