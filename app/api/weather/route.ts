type GeocodingResponse = {
  results?: Array<{
    name?: string;
    latitude?: number;
    longitude?: number;
    timezone?: string;
  }>;
};

type ForecastResponse = {
  current?: {
    temperature_2m?: number;
    weather_code?: number;
  };
  daily?: {
    temperature_2m_max?: number[];
    temperature_2m_min?: number[];
    precipitation_probability_max?: number[];
    weather_code?: number[];
  };
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

export async function GET(request: Request) {
  const city = new URL(request.url).searchParams.get("city")?.trim().slice(0, 30) || "上海";
  try {
    const locationUrl = new URL("https://geocoding-api.open-meteo.com/v1/search");
    locationUrl.search = new URLSearchParams({
      name: city,
      count: "1",
      language: "zh",
      format: "json",
    }).toString();
    const locationResponse = await fetch(locationUrl, { signal: AbortSignal.timeout(8_000) });
    if (!locationResponse.ok) throw new Error("城市查询暂时不可用");
    const locationPayload = (await locationResponse.json()) as GeocodingResponse;
    const location = locationPayload.results?.[0];
    if (!location || !validNumber(location.latitude) || !validNumber(location.longitude)) {
      return Response.json({ error: `没有找到城市：${city}` }, { status: 404 });
    }

    const forecastUrl = new URL("https://api.open-meteo.com/v1/forecast");
    forecastUrl.search = new URLSearchParams({
      latitude: String(location.latitude),
      longitude: String(location.longitude),
      current: "temperature_2m,weather_code",
      daily: "temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code",
      timezone: location.timezone || "auto",
      forecast_days: "1",
    }).toString();
    const forecastResponse = await fetch(forecastUrl, { signal: AbortSignal.timeout(8_000) });
    if (!forecastResponse.ok) throw new Error("天气服务暂时不可用");
    const forecast = (await forecastResponse.json()) as ForecastResponse;
    const temperature = forecast.current?.temperature_2m;
    const low = forecast.daily?.temperature_2m_min?.[0];
    const high = forecast.daily?.temperature_2m_max?.[0];
    const rainProbability = forecast.daily?.precipitation_probability_max?.[0] ?? 0;
    const weatherCode = forecast.current?.weather_code ?? forecast.daily?.weather_code?.[0] ?? -1;
    if (!validNumber(temperature) || !validNumber(low) || !validNumber(high)) {
      throw new Error("天气数据不完整");
    }

    return Response.json(
      {
        city: location.name || city,
        temperature,
        low,
        high,
        rainProbability: validNumber(rainProbability) ? rainProbability : 0,
        condition: conditionFor(weatherCode),
        updatedAt: new Date().toISOString(),
      },
      { headers: { "Cache-Control": "public, max-age=300" } },
    );
  } catch (error) {
    return Response.json(
      { error: error instanceof Error ? error.message : "无法获取天气" },
      { status: 502 },
    );
  }
}
