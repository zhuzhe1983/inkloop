import { NextResponse } from "next/server";

type CalendarRequest = {
  year?: unknown;
  month?: unknown;
  customUrl?: unknown;
  presets?: unknown;
};

type CalendarEvent = {
  day: number;
  text: string;
};

const MAX_ICAL_BYTES = 1024 * 1024;
const PUBLIC_CALENDARS: Record<string, { name: string; url: string }> = {
  "china-holidays": {
    name: "中国公众假期",
    url: "https://calendar.google.com/calendar/ical/zh.china%23holiday%40group.v.calendar.google.com/public/basic.ics",
  },
};

function isPrivateIpv4(hostname: string) {
  const parts = hostname.split(".").map(Number);
  if (parts.length !== 4 || parts.some((part) => !Number.isInteger(part) || part < 0 || part > 255)) return false;
  const [a, b] = parts;
  return a === 0
    || a === 10
    || a === 127
    || (a === 100 && b >= 64 && b <= 127)
    || (a === 169 && b === 254)
    || (a === 172 && b >= 16 && b <= 31)
    || (a === 192 && b === 168)
    || a >= 224;
}

function validateCalendarUrl(input: string) {
  const url = new URL(input);
  const hostname = url.hostname.toLowerCase().replace(/\.$/, "");
  if (url.protocol !== "https:") throw new Error("iCal 地址必须使用 HTTPS");
  if (url.username || url.password) throw new Error("iCal 地址不能包含账号信息");
  if (url.port && url.port !== "443") throw new Error("iCal 地址不能使用自定义端口");
  if (
    !hostname
    || hostname === "localhost"
    || hostname.endsWith(".localhost")
    || hostname.endsWith(".local")
    || hostname.endsWith(".internal")
    || hostname.includes(":")
    || isPrivateIpv4(hostname)
  ) throw new Error("该 iCal 地址不可访问");
  return url;
}

async function fetchCalendarText(input: string) {
  let current = validateCalendarUrl(input);
  for (let redirects = 0; redirects <= 3; redirects += 1) {
    const response = await fetch(current, {
      cache: "no-store",
      redirect: "manual",
      signal: AbortSignal.timeout(12_000),
      headers: { accept: "text/calendar,text/plain;q=0.9,*/*;q=0.1" },
    });
    if (response.status >= 300 && response.status < 400) {
      const location = response.headers.get("location");
      if (!location || redirects === 3) throw new Error("iCal 地址重定向过多");
      current = validateCalendarUrl(new URL(location, current).toString());
      continue;
    }
    if (!response.ok) throw new Error(`iCal 服务返回 ${response.status}`);
    const declaredLength = Number(response.headers.get("content-length") || 0);
    if (declaredLength > MAX_ICAL_BYTES) throw new Error("iCal 文件超过 1 MB");
    if (!response.body) throw new Error("iCal 服务没有返回内容");
    const reader = response.body.getReader();
    const chunks: Uint8Array[] = [];
    let size = 0;
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > MAX_ICAL_BYTES) {
        await reader.cancel();
        throw new Error("iCal 文件超过 1 MB");
      }
      chunks.push(value);
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    chunks.forEach((chunk) => {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    });
    return new TextDecoder().decode(bytes);
  }
  throw new Error("无法读取 iCal");
}

function fieldValue(block: string, field: string) {
  const match = block.match(new RegExp(`^${field}(?:;[^:]*)?:(.*)$`, "mi"));
  return match?.[1]?.trim() || "";
}

function fieldValues(block: string, field: string) {
  return Array.from(block.matchAll(new RegExp(`^${field}(?:;[^:]*)?:(.*)$`, "gmi")))
    .flatMap((match) => (match[1] || "").split(","))
    .map((value) => value.trim())
    .filter(Boolean);
}

function dateParts(value: string) {
  const match = value.match(/^(\d{4})(\d{2})(\d{2})/);
  if (!match) return null;
  return { year: Number(match[1]), month: Number(match[2]), day: Number(match[3]) };
}

function unescapeIcal(value: string) {
  return value
    .replace(/\\[nN]/g, " ")
    .replace(/\\,/g, ",")
    .replace(/\\;/g, ";")
    .replace(/\\\\/g, "\\")
    .replace(/\s+/g, " ")
    .trim();
}

const weekdayNumbers: Record<string, number> = {
  SU: 0, MO: 1, TU: 2, WE: 3, TH: 4, FR: 5, SA: 6,
};

function eventDays(block: string, year: number, month: number) {
  const start = dateParts(fieldValue(block, "DTSTART"));
  if (!start) return [];
  const rule = fieldValue(block, "RRULE");
  const excluded = new Set(fieldValues(block, "EXDATE").map((value) => value.slice(0, 8)));
  const daysInMonth = new Date(Date.UTC(year, month, 0)).getUTCDate();
  const untilMatch = rule.match(/(?:^|;)UNTIL=([^;]+)/i);
  const until = untilMatch ? dateParts(untilMatch[1]) : null;
  const afterStart = (day: number) => {
    const candidate = year * 10000 + month * 100 + day;
    const startValue = start.year * 10000 + start.month * 100 + start.day;
    const untilValue = until ? until.year * 10000 + until.month * 100 + until.day : Number.POSITIVE_INFINITY;
    const key = `${year}${String(month).padStart(2, "0")}${String(day).padStart(2, "0")}`;
    return candidate >= startValue && candidate <= untilValue && !excluded.has(key);
  };
  if (!rule) return start.year === year && start.month === month && afterStart(start.day) ? [start.day] : [];

  const frequency = rule.match(/(?:^|;)FREQ=([^;]+)/i)?.[1]?.toUpperCase();
  const byMonth = Number(rule.match(/(?:^|;)BYMONTH=(\d+)/i)?.[1] || start.month);
  const byMonthDays = (rule.match(/(?:^|;)BYMONTHDAY=([^;]+)/i)?.[1] || String(start.day))
    .split(",").map(Number).filter((day) => day >= 1 && day <= daysInMonth);
  if (frequency === "YEARLY") return byMonth === month ? byMonthDays.filter(afterStart) : [];
  if (frequency === "MONTHLY") return byMonthDays.filter(afterStart);
  if (frequency === "DAILY") return Array.from({ length: daysInMonth }, (_, index) => index + 1).filter(afterStart);
  if (frequency === "WEEKLY") {
    const byDays = (rule.match(/(?:^|;)BYDAY=([^;]+)/i)?.[1] || "")
      .split(",")
      .map((value) => weekdayNumbers[value.replace(/^[+-]?\d+/, "").toUpperCase()])
      .filter((value): value is number => Number.isInteger(value));
    const weekdays = byDays.length ? byDays : [new Date(Date.UTC(start.year, start.month - 1, start.day)).getUTCDay()];
    return Array.from({ length: daysInMonth }, (_, index) => index + 1)
      .filter((day) => weekdays.includes(new Date(Date.UTC(year, month - 1, day)).getUTCDay()))
      .filter(afterStart);
  }
  return start.year === year && start.month === month && afterStart(start.day) ? [start.day] : [];
}

function parseCalendar(text: string, year: number, month: number): CalendarEvent[] {
  const unfolded = text.replace(/\r?\n[ \t]/g, "");
  const events: CalendarEvent[] = [];
  for (const match of unfolded.matchAll(/BEGIN:VEVENT\r?\n([\s\S]*?)\r?\nEND:VEVENT/g)) {
    const summary = unescapeIcal(fieldValue(match[1], "SUMMARY")).slice(0, 12);
    if (!summary) continue;
    eventDays(match[1], year, month).forEach((day) => events.push({ day, text: summary }));
  }
  return events;
}

export async function POST(request: Request) {
  try {
    const body = await request.json() as CalendarRequest;
    const now = new Date();
    const year = Math.min(2100, Math.max(2020, Math.round(Number(body.year) || now.getFullYear())));
    const month = Math.min(12, Math.max(1, Math.round(Number(body.month) || now.getMonth() + 1)));
    const feeds: Array<{ name: string; url: string }> = [];
    if (Array.isArray(body.presets)) {
      body.presets.slice(0, 2).forEach((preset) => {
        if (typeof preset === "string" && PUBLIC_CALENDARS[preset]) feeds.push(PUBLIC_CALENDARS[preset]);
      });
    }
    if (typeof body.customUrl === "string" && body.customUrl.trim()) {
      feeds.push({ name: "个人 iCal", url: body.customUrl.trim() });
    }
    const uniqueFeeds = feeds.filter((feed, index) => feeds.findIndex((item) => item.url === feed.url) === index).slice(0, 2);
    if (!uniqueFeeds.length) return NextResponse.json({ events: [], sources: [], warnings: [] });

    const events: CalendarEvent[] = [];
    const sources: string[] = [];
    const warnings: string[] = [];
    await Promise.all(uniqueFeeds.map(async (feed) => {
      try {
        const text = await fetchCalendarText(feed.url);
        events.push(...parseCalendar(text, year, month));
        sources.push(feed.name);
      } catch (error) {
        warnings.push(`${feed.name}：${error instanceof Error ? error.message : "读取失败"}`);
      }
    }));
    const deduped = events
      .filter((event, index) => events.findIndex((item) => item.day === event.day && item.text === event.text) === index)
      .sort((a, b) => a.day - b.day)
      .slice(0, 24);
    const response = NextResponse.json({ events: deduped, sources, warnings });
    response.headers.set("cache-control", "private, no-store");
    return response;
  } catch (error) {
    return NextResponse.json(
      { events: [], sources: [], warnings: [error instanceof Error ? error.message : "日历读取失败"] },
      { status: 400, headers: { "cache-control": "private, no-store" } },
    );
  }
}
