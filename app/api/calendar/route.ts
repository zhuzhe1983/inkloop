import { NextResponse } from "next/server";

type CalendarRequest = {
  year?: unknown;
  month?: unknown;
  customUrl?: unknown;
  customUrls?: unknown;
  presets?: unknown;
  view?: unknown;
  start?: unknown;
  end?: unknown;
  timeZone?: unknown;
};

type CalendarEvent = {
  day: number;
  text: string;
};

type AgendaEvent = {
  uid: string;
  title: string;
  start: string;
  end: string;
  allDay?: boolean;
  location?: string;
  calendar?: string;
  category?: string;
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
  const normalizedInput = input.trim().replace(/^webcal:\/\//i, "https://");
  const url = new URL(normalizedInput);
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

type IcalField = { value: string; params: Record<string, string> };

function fieldEntry(block: string, field: string): IcalField | null {
  const line = block.split(/\r?\n/).find((entry) => {
    const upper = entry.toUpperCase();
    return upper.startsWith(`${field.toUpperCase()}:`) || upper.startsWith(`${field.toUpperCase()};`);
  });
  if (!line) return null;
  const colon = line.indexOf(":");
  if (colon < 0) return null;
  const params: Record<string, string> = {};
  line.slice(0, colon).split(";").slice(1).forEach((part) => {
    const equals = part.indexOf("=");
    if (equals > 0) params[part.slice(0, equals).toUpperCase()] = part.slice(equals + 1).replace(/^"|"$/g, "");
  });
  return { value: line.slice(colon + 1).trim(), params };
}

function zonedTimeToUtc(parts: number[], timeZone: string) {
  const [year, month, day, hour, minute, second] = parts;
  const target = Date.UTC(year, month - 1, day, hour, minute, second);
  try {
    const formatter = new Intl.DateTimeFormat("en-CA", {
      timeZone,
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hourCycle: "h23",
    });
    const formatted = formatter.formatToParts(new Date(target));
    const part = (type: Intl.DateTimeFormatPartTypes) => Number(formatted.find((item) => item.type === type)?.value || 0);
    const represented = Date.UTC(part("year"), part("month") - 1, part("day"), part("hour"), part("minute"), part("second"));
    return new Date(target - (represented - target));
  } catch {
    return new Date(target);
  }
}

function parseIcalDateTime(field: IcalField | null, fallbackTimeZone: string) {
  if (!field) return null;
  const match = field.value.match(/^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})?)?(Z)?$/i);
  if (!match) return null;
  const allDay = field.params.VALUE?.toUpperCase() === "DATE" || !match[4];
  const parts = [
    Number(match[1]), Number(match[2]), Number(match[3]),
    Number(match[4] || 0), Number(match[5] || 0), Number(match[6] || 0),
  ];
  const date = allDay || match[7]
    ? new Date(Date.UTC(parts[0], parts[1] - 1, parts[2], parts[3], parts[4], parts[5]))
    : zonedTimeToUtc(parts, field.params.TZID || fallbackTimeZone);
  return { date, allDay };
}

function parseDuration(value: string) {
  const match = value.match(/^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?)?$/i);
  if (!match) return 0;
  return (Number(match[1] || 0) * 24 * 60 + Number(match[2] || 0) * 60 + Number(match[3] || 0)) * 60_000;
}

function recurrenceStarts(block: string, baseStart: Date, rangeStart: Date, rangeEnd: Date) {
  const rule = fieldValue(block, "RRULE");
  if (!rule) return [baseStart];
  const frequency = rule.match(/(?:^|;)FREQ=([^;]+)/i)?.[1]?.toUpperCase();
  const interval = Math.max(1, Number(rule.match(/(?:^|;)INTERVAL=(\d+)/i)?.[1] || 1));
  const untilField = rule.match(/(?:^|;)UNTIL=([^;]+)/i)?.[1];
  const until = untilField ? parseIcalDateTime({ value: untilField, params: {} }, "UTC")?.date : null;
  const byDays = (rule.match(/(?:^|;)BYDAY=([^;]+)/i)?.[1] || "")
    .split(",")
    .map((value) => weekdayNumbers[value.replace(/^[+-]?\d+/, "").toUpperCase()])
    .filter((value): value is number => Number.isInteger(value));
  const byMonthDays = (rule.match(/(?:^|;)BYMONTHDAY=([^;]+)/i)?.[1] || "")
    .split(",").map(Number).filter((value) => value >= 1 && value <= 31);
  const excluded = new Set(fieldValues(block, "EXDATE").map((value) => value.replace(/[^0-9]/g, "").slice(0, 14)));
  const firstDay = new Date(Math.max(baseStart.getTime(), rangeStart.getTime() - 8 * 24 * 60 * 60 * 1000));
  firstDay.setUTCHours(baseStart.getUTCHours(), baseStart.getUTCMinutes(), baseStart.getUTCSeconds(), 0);
  const output: Date[] = [];
  for (let cursor = firstDay; cursor < rangeEnd && output.length < 128; cursor = new Date(cursor.getTime() + 24 * 60 * 60 * 1000)) {
    if (cursor < baseStart || (until && cursor > until)) continue;
    const days = Math.floor((Date.UTC(cursor.getUTCFullYear(), cursor.getUTCMonth(), cursor.getUTCDate())
      - Date.UTC(baseStart.getUTCFullYear(), baseStart.getUTCMonth(), baseStart.getUTCDate())) / 86_400_000);
    const months = (cursor.getUTCFullYear() - baseStart.getUTCFullYear()) * 12 + cursor.getUTCMonth() - baseStart.getUTCMonth();
    const matches = frequency === "DAILY"
      ? days % interval === 0
      : frequency === "WEEKLY"
        ? Math.floor(days / 7) % interval === 0 && (byDays.length ? byDays : [baseStart.getUTCDay()]).includes(cursor.getUTCDay())
        : frequency === "MONTHLY"
          ? months % interval === 0 && (byMonthDays.length ? byMonthDays : [baseStart.getUTCDate()]).includes(cursor.getUTCDate())
          : frequency === "YEARLY"
            ? (cursor.getUTCFullYear() - baseStart.getUTCFullYear()) % interval === 0
              && cursor.getUTCMonth() === baseStart.getUTCMonth()
              && cursor.getUTCDate() === baseStart.getUTCDate()
            : cursor.getTime() === baseStart.getTime();
    const key = `${cursor.getUTCFullYear()}${String(cursor.getUTCMonth() + 1).padStart(2, "0")}${String(cursor.getUTCDate()).padStart(2, "0")}${String(cursor.getUTCHours()).padStart(2, "0")}${String(cursor.getUTCMinutes()).padStart(2, "0")}`;
    if (matches && !excluded.has(key) && !excluded.has(key.slice(0, 8))) output.push(new Date(cursor));
  }
  return output;
}

function parseAgenda(text: string, rangeStart: Date, rangeEnd: Date, timeZone: string, calendar: string): AgendaEvent[] {
  const unfolded = text.replace(/\r?\n[ \t]/g, "");
  const events: AgendaEvent[] = [];
  for (const match of unfolded.matchAll(/BEGIN:VEVENT\r?\n([\s\S]*?)\r?\nEND:VEVENT/g)) {
    const block = match[1];
    const parsedStart = parseIcalDateTime(fieldEntry(block, "DTSTART"), timeZone);
    if (!parsedStart) continue;
    const parsedEnd = parseIcalDateTime(fieldEntry(block, "DTEND"), timeZone);
    const duration = parseDuration(fieldValue(block, "DURATION"));
    const fallbackDuration = parsedStart.allDay ? 24 * 60 * 60 * 1000 : 60 * 60 * 1000;
    const baseEnd = parsedEnd?.date ?? new Date(parsedStart.date.getTime() + (duration || fallbackDuration));
    const eventDuration = Math.max(60_000, baseEnd.getTime() - parsedStart.date.getTime());
    const title = unescapeIcal(fieldValue(block, "SUMMARY")).slice(0, 32);
    if (!title) continue;
    const location = unescapeIcal(fieldValue(block, "LOCATION")).slice(0, 40);
    const category = fieldValues(block, "CATEGORIES")
      .map(unescapeIcal)
      .find(Boolean)
      ?.slice(0, 24);
    const uid = unescapeIcal(fieldValue(block, "UID")) || `${title}-${parsedStart.date.toISOString()}`;
    recurrenceStarts(block, parsedStart.date, rangeStart, rangeEnd).forEach((start) => {
      const end = new Date(start.getTime() + eventDuration);
      if (start >= rangeEnd || end <= rangeStart) return;
      events.push({
        uid: `${uid}:${start.toISOString()}`,
        title,
        start: start.toISOString(),
        end: end.toISOString(),
        allDay: parsedStart.allDay,
        location: location || undefined,
        calendar,
        category,
      });
    });
  }
  return events;
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
    const agendaView = body.view === "agenda";
    const year = Math.min(2100, Math.max(2020, Math.round(Number(body.year) || now.getFullYear())));
    const month = Math.min(12, Math.max(1, Math.round(Number(body.month) || now.getMonth() + 1)));
    const requestedStart = typeof body.start === "string" ? new Date(body.start) : now;
    const requestedEnd = typeof body.end === "string" ? new Date(body.end) : new Date(requestedStart.getTime() + 72 * 60 * 60 * 1000);
    const rangeStart = Number.isFinite(requestedStart.getTime()) ? requestedStart : now;
    const unclampedEnd = Number.isFinite(requestedEnd.getTime()) ? requestedEnd : new Date(rangeStart.getTime() + 72 * 60 * 60 * 1000);
    const rangeEnd = new Date(Math.max(
      rangeStart.getTime() + 60 * 60 * 1000,
      Math.min(unclampedEnd.getTime(), rangeStart.getTime() + 14 * 24 * 60 * 60 * 1000),
    ));
    const timeZone = typeof body.timeZone === "string" && body.timeZone.length <= 80 ? body.timeZone : "Asia/Shanghai";
    const feeds: Array<{ name: string; url: string }> = [];
    if (Array.isArray(body.presets)) {
      body.presets.slice(0, 2).forEach((preset) => {
        if (typeof preset === "string" && PUBLIC_CALENDARS[preset]) feeds.push(PUBLIC_CALENDARS[preset]);
      });
    }
    if (Array.isArray(body.customUrls)) {
      body.customUrls.slice(0, 5).forEach((entry, index) => {
        if (typeof entry === "string" && entry.trim()) {
          feeds.push({ name: `个人日历 ${index + 1}`, url: entry.trim() });
          return;
        }
        if (!entry || typeof entry !== "object") return;
        const candidate = entry as { name?: unknown; url?: unknown };
        if (typeof candidate.url !== "string" || !candidate.url.trim()) return;
        const name = typeof candidate.name === "string"
          ? candidate.name.trim().replace(/\s+/g, " ").slice(0, 24)
          : "";
        feeds.push({ name: name || `个人日历 ${index + 1}`, url: candidate.url.trim() });
      });
    } else if (typeof body.customUrl === "string" && body.customUrl.trim()) {
      feeds.push({ name: "个人 iCal", url: body.customUrl.trim() });
    }
    const uniqueFeeds = feeds
      .filter((feed, index) => feeds.findIndex((item) => item.url === feed.url) === index)
      .slice(0, 6);
    if (!uniqueFeeds.length) return NextResponse.json({ events: [], timedEvents: [], sources: [], warnings: [] });

    const events: CalendarEvent[] = [];
    const timedEvents: AgendaEvent[] = [];
    const sources: string[] = [];
    const warnings: string[] = [];
    await Promise.all(uniqueFeeds.map(async (feed) => {
      try {
        const text = await fetchCalendarText(feed.url);
        if (agendaView) timedEvents.push(...parseAgenda(text, rangeStart, rangeEnd, timeZone, feed.name));
        else events.push(...parseCalendar(text, year, month));
        sources.push(feed.name);
      } catch (error) {
        warnings.push(`${feed.name}：${error instanceof Error ? error.message : "读取失败"}`);
      }
    }));
    const deduped = events
      .filter((event, index) => events.findIndex((item) => item.day === event.day && item.text === event.text) === index)
      .sort((a, b) => a.day - b.day)
      .slice(0, 24);
    const dedupedTimed = timedEvents
      .filter((event, index) => timedEvents.findIndex((item) => item.uid === event.uid && item.calendar === event.calendar) === index)
      .sort((left, right) => Date.parse(left.start) - Date.parse(right.start))
      .slice(0, 80);
    const response = NextResponse.json({ events: deduped, timedEvents: dedupedTimed, sources, warnings });
    response.headers.set("cache-control", "private, no-store");
    return response;
  } catch (error) {
    return NextResponse.json(
      { events: [], sources: [], warnings: [error instanceof Error ? error.message : "日历读取失败"] },
      { status: 400, headers: { "cache-control": "private, no-store" } },
    );
  }
}
