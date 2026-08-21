import { env } from "cloudflare:workers";
import type { InkApp, ScheduleMode } from "../../lib/app-model";
import {
  isPaperColorRenderStrategy,
  normalizePaperColorRenderStrategy,
  type PaperColorRenderStrategy,
} from "../../lib/papercolor-render";

const CREATE_DEVICES = `CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY NOT NULL,
  hardware_id TEXT NOT NULL UNIQUE,
  owner_id TEXT,
  sku_id TEXT NOT NULL,
  name TEXT NOT NULL,
  secret_hash TEXT NOT NULL,
  pairing_code TEXT,
  pairing_expires_at TEXT,
  firmware_version TEXT,
  battery_percent INTEGER,
  desired_revision INTEGER NOT NULL DEFAULT 0,
  applied_revision INTEGER NOT NULL DEFAULT 0,
  last_seen_at TEXT,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
)`;
const CREATE_TASKS = `CREATE TABLE IF NOT EXISTS device_tasks (
  id TEXT PRIMARY KEY NOT NULL,
  device_id TEXT NOT NULL,
  owner_id TEXT NOT NULL,
  app_id TEXT NOT NULL,
  title TEXT NOT NULL,
  app_snapshot TEXT NOT NULL,
  schedule_mode TEXT NOT NULL,
  custom_minutes INTEGER NOT NULL DEFAULT 30,
  daily_time TEXT NOT NULL DEFAULT '08:00',
  frame_key TEXT NOT NULL,
  frame_hash TEXT NOT NULL,
  render_strategy TEXT NOT NULL DEFAULT 'official-quality',
  revision INTEGER NOT NULL,
  deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
)`;
const CREATE_PAIRING_ATTEMPTS = `CREATE TABLE IF NOT EXISTS device_pairing_attempts (
  id TEXT PRIMARY KEY NOT NULL,
  attempt_count INTEGER NOT NULL DEFAULT 0,
  window_started_at TEXT NOT NULL,
  locked_until TEXT,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
)`;

type DeviceRow = {
  id: string;
  hardware_id: string;
  owner_id: string | null;
  sku_id: string;
  name: string;
  secret_hash: string;
  pairing_code: string | null;
  pairing_expires_at: string | null;
  firmware_version: string | null;
  battery_percent: number | null;
  desired_revision: number;
  applied_revision: number;
  last_seen_at: string | null;
  created_at: string;
  updated_at: string;
};

type TaskRow = {
  id: string;
  device_id: string;
  owner_id: string;
  app_id: string;
  title: string;
  app_snapshot: string;
  schedule_mode: ScheduleMode;
  custom_minutes: number;
  daily_time: string;
  frame_key: string;
  frame_hash: string;
  render_strategy: PaperColorRenderStrategy;
  revision: number;
  deleted: number;
  created_at: string;
  updated_at: string;
};

class DeviceSchemaAttestationError extends Error {
  constructor() {
    super("DEVICE_SCHEMA_ATTESTATION_FAILED");
    this.name = "DeviceSchemaAttestationError";
  }
}

type PairingCodeIndexAttestation =
  | { ready: true }
  | { ready: false; reason: "mismatch" | "unreadable" };

async function ensureSchema() {
  if (!env.DB) throw new Error("DEVICE_DATABASE_UNAVAILABLE");
  await env.DB.batch([
    env.DB.prepare(CREATE_DEVICES),
    env.DB.prepare(CREATE_TASKS),
    env.DB.prepare(CREATE_PAIRING_ATTEMPTS),
  ]);
  const attestation = await pairingCodeIndexAttestation(env.DB);
  if (!attestation.ready) throw new DeviceSchemaAttestationError();
  return env.DB;
}

function normalizedIndexPredicate(definition: string) {
  const where = definition.match(/\bWHERE\b([\s\S]+)$/i)?.[1];
  if (!where) return "";
  return where
    .replace(/["`\[\]]/g, "")
    .replace(/\bdevices\s*\.\s*/gi, "")
    .replace(/[();]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .toLowerCase();
}

async function pairingCodeIndexAttestation(db: D1Database): Promise<PairingCodeIndexAttestation> {
  try {
    const indexes = await db.prepare("PRAGMA index_list('devices')").all<{
      name: string;
      unique: number;
      partial: number;
    }>();
    const target = indexes.results.find((index) => index.name === "idx_devices_pairing_code_unique");
    if (!target || target.unique !== 1 || target.partial !== 1) return { ready: false, reason: "mismatch" };

    const columns = await db.prepare("PRAGMA index_info('idx_devices_pairing_code_unique')").all<{
      seqno: number;
      name: string | null;
    }>();
    if (columns.results.length !== 1 || columns.results[0]?.seqno !== 0
      || columns.results[0]?.name !== "pairing_code") return { ready: false, reason: "mismatch" };

    const definition = await db.prepare(`SELECT tbl_name, sql FROM sqlite_master
      WHERE type = 'index' AND name = ? LIMIT 1`)
      .bind("idx_devices_pairing_code_unique")
      .first<{ tbl_name: string; sql: string | null }>();
    return definition?.tbl_name === "devices" && typeof definition.sql === "string"
      && normalizedIndexPredicate(definition.sql) === "pairing_code is not null"
      ? { ready: true }
      : { ready: false, reason: "mismatch" };
  } catch {
    return { ready: false, reason: "unreadable" };
  }
}

function cleanText(value: unknown, max: number) {
  return typeof value === "string" ? value.trim().slice(0, max) : "";
}

function ownerId(request: Request) {
  const authenticated = cleanText(request.headers.get("oai-authenticated-user-id"), 160);
  if (authenticated) return `oai:${authenticated}`;
  const anonymous = cleanText(request.headers.get("x-inkloop-owner"), 80);
  return /^[a-f0-9-]{32,64}$/i.test(anonymous) ? `browser:${anonymous}` : null;
}

function requireOwner(request: Request) {
  const owner = ownerId(request);
  if (!owner) throw Response.json({ error: "缺少设备所有者凭据" }, { status: 401 });
  return owner;
}

async function sha256(value: string | Uint8Array) {
  const bytes = typeof value === "string" ? new TextEncoder().encode(value) : value;
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes)));
  return Array.from(digest, (part) => part.toString(16).padStart(2, "0")).join("");
}

function safeEqual(left: string, right: string) {
  if (left.length !== right.length) return false;
  let difference = 0;
  for (let index = 0; index < left.length; index += 1) {
    difference |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return difference === 0;
}

function randomPairingCode() {
  const value = new Uint32Array(1);
  crypto.getRandomValues(value);
  return String(value[0] % 1_000_000).padStart(6, "0");
}

function pairingExpiry() {
  return new Date(Date.now() + 10 * 60 * 1000).toISOString();
}

type PairingCodeRequest =
  | { provided: false }
  | { provided: true; code: string };

function requestedPairingCode(payload: Record<string, unknown>): PairingCodeRequest | Response {
  if (!Object.prototype.hasOwnProperty.call(payload, "pairingCode")) return { provided: false };
  if (typeof payload.pairingCode !== "string" || !/^[0-9]{6}$/.test(payload.pairingCode)) {
    return Response.json({ error: "设备码必须是六位 ASCII 数字" }, { status: 422 });
  }
  return { provided: true, code: payload.pairingCode };
}

function activePairing(row: Pick<DeviceRow, "pairing_code" | "pairing_expires_at">, now = Date.now()) {
  if (!row.pairing_code || !row.pairing_expires_at) return false;
  const expiresAt = new Date(row.pairing_expires_at).getTime();
  return Number.isFinite(expiresAt) && expiresAt > now;
}

function pairingCodeConflict() {
  return Response.json({ error: "设备码不可用，请重新获取" }, { status: 409 });
}

function deviceServiceUnavailable() {
  return Response.json({ error: "设备服务正在升级，请稍后重试" }, { status: 503 });
}

function safeDeviceError(error: unknown) {
  if (error instanceof Response) return error;
  if (error instanceof DeviceSchemaAttestationError) return deviceServiceUnavailable();
  return deviceServiceUnavailable();
}

function uniqueConstraint(error: unknown) {
  const message = error instanceof Error ? error.message : String(error);
  return /(?:UNIQUE constraint failed|SQLITE_CONSTRAINT)/i.test(message);
}

async function retireExpiredPairingCode(
  db: D1Database,
  pairingCode: string,
  hardwareId: string,
  now: string,
) {
  await db.prepare(`UPDATE devices SET pairing_code = NULL, pairing_expires_at = NULL, updated_at = ?
    WHERE pairing_code = ? AND hardware_id <> ? AND owner_id IS NULL
      AND (pairing_expires_at IS NULL OR pairing_expires_at <= ?)`)
    .bind(now, pairingCode, hardwareId, now).run();
}

async function pairingAttemptId(request: Request, owner: string) {
  const network = cleanText(request.headers.get("cf-connecting-ip"), 80) || "local";
  return sha256(`${owner}|${network}`);
}

async function pairingRateLimit(request: Request, db: D1Database, owner: string) {
  const id = await pairingAttemptId(request, owner);
  const row = await db.prepare(`SELECT attempt_count, window_started_at, locked_until
    FROM device_pairing_attempts WHERE id = ? LIMIT 1`).bind(id).first<{
      attempt_count: number;
      window_started_at: string;
      locked_until: string | null;
    }>();
  const now = Date.now();
  if (row?.locked_until && new Date(row.locked_until).getTime() > now) {
    throw Response.json({ error: "设备码尝试过多，请稍后再试" }, {
      status: 429,
      headers: { "Retry-After": "900" },
    });
  }
  const windowStart = row ? new Date(row.window_started_at).getTime() : 0;
  if (!row || !Number.isFinite(windowStart) || now - windowStart > 10 * 60 * 1000) {
    const timestamp = new Date(now).toISOString();
    await db.prepare(`INSERT INTO device_pairing_attempts
      (id, attempt_count, window_started_at, locked_until, updated_at) VALUES (?, 0, ?, NULL, ?)
      ON CONFLICT(id) DO UPDATE SET attempt_count = 0, window_started_at = excluded.window_started_at,
        locked_until = NULL, updated_at = excluded.updated_at`)
      .bind(id, timestamp, timestamp).run();
  }
  return id;
}

async function recordPairingFailure(db: D1Database, id: string) {
  const now = new Date().toISOString();
  await db.prepare(`UPDATE device_pairing_attempts SET attempt_count = attempt_count + 1,
    locked_until = CASE WHEN attempt_count + 1 >= 10 THEN ? ELSE NULL END, updated_at = ? WHERE id = ?`)
    .bind(new Date(Date.now() + 15 * 60 * 1000).toISOString(), now, id).run();
}

function isOnline(lastSeenAt: string | null) {
  if (!lastSeenAt) return false;
  const timestamp = new Date(lastSeenAt).getTime();
  return Number.isFinite(timestamp) && Date.now() - timestamp < 45_000;
}

function taskJson(row: TaskRow) {
  return {
    id: row.id,
    app: JSON.parse(row.app_snapshot) as InkApp,
    scheduleMode: row.schedule_mode,
    customMinutes: row.custom_minutes,
    dailyTime: row.daily_time,
    revision: row.revision,
    renderStrategy: normalizePaperColorRenderStrategy(row.render_strategy),
    updatedAt: row.updated_at,
  };
}

async function tasksForDevice(db: D1Database, deviceId: string, includeDeleted = false) {
  const result = await db.prepare(`SELECT id, device_id, owner_id, app_id, title, app_snapshot,
    schedule_mode, custom_minutes, daily_time, frame_key, frame_hash, render_strategy,
    revision, deleted,
    created_at, updated_at FROM device_tasks
    WHERE device_id = ? ${includeDeleted ? "" : "AND deleted = 0"}
    ORDER BY updated_at DESC`).bind(deviceId).all<TaskRow>();
  return result.results;
}

async function deviceJson(db: D1Database, row: DeviceRow) {
  const tasks = await tasksForDevice(db, row.id);
  return {
    id: row.id,
    name: row.name,
    skuId: row.sku_id,
    hardwareId: row.hardware_id,
    firmwareVersion: row.firmware_version,
    batteryPercent: row.battery_percent,
    lastSeenAt: row.last_seen_at,
    online: isOnline(row.last_seen_at),
    desiredRevision: row.desired_revision,
    appliedRevision: row.applied_revision,
    tasks: tasks.map(taskJson),
  };
}

async function ownedDevice(db: D1Database, deviceId: string, owner: string) {
  return db.prepare(`SELECT * FROM devices WHERE id = ? AND owner_id = ? LIMIT 1`)
    .bind(deviceId, owner).first<DeviceRow>();
}

async function authenticatedDevice(request: Request, db: D1Database) {
  const authorization = request.headers.get("authorization") ?? "";
  const match = authorization.match(/^InkloopDevice ([a-z0-9-]{20,80}):([a-f0-9]{64})$/i);
  if (!match) throw Response.json({ error: "设备认证失败" }, { status: 401 });
  const row = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1")
    .bind(match[1]).first<DeviceRow>();
  const providedHash = await sha256(match[2].toLowerCase());
  if (!row || !safeEqual(row.secret_hash, providedHash)) {
    throw Response.json({ error: "设备认证失败" }, { status: 401 });
  }
  return row;
}

function parsePngDataUrl(value: unknown) {
  if (typeof value !== "string") throw new Response("缺少设备画面", { status: 400 });
  const match = value.match(/^data:image\/png;base64,([a-z0-9+/=]+)$/i);
  if (!match) throw new Response("设备画面必须是 PNG", { status: 400 });
  const binary = atob(match[1]);
  if (binary.length > 1_500_000) throw new Response("设备画面过大", { status: 413 });
  const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0));
  if (bytes.length < 24 || bytes[0] !== 0x89 || String.fromCharCode(...bytes.slice(1, 4)) !== "PNG") {
    throw new Response("PNG 文件无效", { status: 400 });
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const width = view.getUint32(16);
  const height = view.getUint32(20);
  const validSize = (width === 400 && height === 600) || (width === 600 && height === 400);
  if (!validSize) throw new Response("M5 PaperColor 画面必须是 400×600 或 600×400", { status: 400 });
  return bytes;
}

async function registerDevice(payload: Record<string, unknown>) {
  const db = await ensureSchema();
  const hardwareId = cleanText(payload.hardwareId, 80).toUpperCase();
  const secret = cleanText(payload.secret, 64).toLowerCase();
  const skuId = cleanText(payload.skuId, 80);
  const firmwareVersion = cleanText(payload.firmwareVersion, 40) || null;
  const pairingRequest = requestedPairingCode(payload);
  if (pairingRequest instanceof Response) return pairingRequest;
  if (!/^[A-Z0-9:_-]{6,80}$/.test(hardwareId) || !/^[a-f0-9]{64}$/.test(secret)) {
    return Response.json({ error: "设备身份格式无效" }, { status: 400 });
  }
  if (skuId !== "m5-papercolor-c151") {
    return Response.json({ error: "暂不支持这个 ESP32 型号" }, { status: 400 });
  }
  const now = new Date().toISOString();
  const secretHash = await sha256(secret);
  let row = await db.prepare("SELECT * FROM devices WHERE hardware_id = ? LIMIT 1")
    .bind(hardwareId).first<DeviceRow>();
  if (row && !safeEqual(row.secret_hash, secretHash)) {
    return Response.json({ error: "设备身份冲突，请重新刷机" }, { status: 409 });
  }

  if (row?.owner_id) {
    await db.prepare(`UPDATE devices SET pairing_code = NULL, pairing_expires_at = NULL,
      firmware_version = ?, last_seen_at = ?, updated_at = ? WHERE id = ?`)
      .bind(firmwareVersion, now, now, row.id).run();
    if (pairingRequest.provided) return pairingCodeConflict();
    row = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1").bind(row.id).first<DeviceRow>();
  }

  if (!row) {
    const id = `esp32-${crypto.randomUUID()}`;
    const attempts = pairingRequest.provided ? 1 : 8;
    for (let attempt = 0; attempt < attempts && !row; attempt += 1) {
      const code = pairingRequest.provided ? pairingRequest.code : randomPairingCode();
      if (pairingRequest.provided) {
        await retireExpiredPairingCode(db, code, hardwareId, now);
        const used = await db.prepare("SELECT * FROM devices WHERE pairing_code = ? LIMIT 1")
          .bind(code).first<DeviceRow>();
        if (used) {
          if (used.hardware_id === hardwareId && safeEqual(used.secret_hash, secretHash)) {
            row = used;
            break;
          }
          return pairingCodeConflict();
        }
      }
      try {
        await db.prepare(`INSERT INTO devices
          (id, hardware_id, owner_id, sku_id, name, secret_hash, pairing_code, pairing_expires_at,
           firmware_version, desired_revision, applied_revision, last_seen_at, created_at, updated_at)
          VALUES (?, ?, NULL, ?, ?, ?, ?, ?, ?, 0, 0, ?, ?, ?)
          ON CONFLICT(hardware_id) DO NOTHING`)
          .bind(id, hardwareId, skuId, "M5 PaperColor", secretHash, code, pairingExpiry(),
            firmwareVersion, now, now, now)
          .run();
      } catch (error) {
        const concurrent = await db.prepare("SELECT * FROM devices WHERE hardware_id = ? LIMIT 1")
          .bind(hardwareId).first<DeviceRow>();
        if (concurrent) {
          row = concurrent;
          break;
        }
        if (pairingRequest.provided && uniqueConstraint(error)) return pairingCodeConflict();
        if (!uniqueConstraint(error)) throw error;
        continue;
      }
      row = await db.prepare("SELECT * FROM devices WHERE hardware_id = ? LIMIT 1")
        .bind(hardwareId).first<DeviceRow>();
    }
    if (!row) {
      return Response.json({ error: "暂时无法分配设备码，请重试" }, { status: 503 });
    }
  }

  if (!safeEqual(row.secret_hash, secretHash)) {
    return Response.json({ error: "设备身份冲突，请重新刷机" }, { status: 409 });
  }
  if (row.owner_id && pairingRequest.provided) {
    await db.prepare(`UPDATE devices SET pairing_code = NULL, pairing_expires_at = NULL,
      firmware_version = ?, last_seen_at = ?, updated_at = ? WHERE id = ?`)
      .bind(firmwareVersion, now, now, row.id).run();
    return pairingCodeConflict();
  }

  if (!row.owner_id && pairingRequest.provided) {
    if (row.pairing_code === pairingRequest.code) {
      if (!activePairing(row)) return pairingCodeConflict();
      await db.prepare(`UPDATE devices SET pairing_code = ?, pairing_expires_at = ?,
        firmware_version = ?, last_seen_at = ?, updated_at = ? WHERE id = ?`)
        .bind(pairingRequest.code, row.pairing_expires_at, firmwareVersion, now, now, row.id)
        .run();
    } else {
      if (activePairing(row)) return pairingCodeConflict();
      await retireExpiredPairingCode(db, pairingRequest.code, hardwareId, now);
      const used = await db.prepare("SELECT id FROM devices WHERE pairing_code = ? AND id <> ? LIMIT 1")
        .bind(pairingRequest.code, row.id).first<{ id: string }>();
      if (used) return pairingCodeConflict();
      try {
        const rotated = await db.prepare(`UPDATE devices SET pairing_code = ?, pairing_expires_at = ?,
          firmware_version = ?, last_seen_at = ?, updated_at = ?
          WHERE id = ? AND owner_id IS NULL
            AND (pairing_code IS NULL OR pairing_expires_at IS NULL OR pairing_expires_at <= ?)`)
          .bind(pairingRequest.code, pairingExpiry(), firmwareVersion, now, now, row.id, now).run();
        if (rotated.meta.changes !== 1) {
          const concurrent = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1")
            .bind(row.id).first<DeviceRow>();
          if (!concurrent || concurrent.owner_id || concurrent.pairing_code !== pairingRequest.code
            || !activePairing(concurrent)) return pairingCodeConflict();
        }
      } catch (error) {
        if (uniqueConstraint(error)) return pairingCodeConflict();
        throw error;
      }
    }
  } else if (!row.owner_id && !activePairing(row)) {
    let allocated = false;
    for (let attempt = 0; attempt < 8 && !allocated; attempt += 1) {
      try {
        const result = await db.prepare(`UPDATE devices SET pairing_code = ?, pairing_expires_at = ?,
          firmware_version = ?, last_seen_at = ?, updated_at = ? WHERE id = ? AND owner_id IS NULL`)
          .bind(randomPairingCode(), pairingExpiry(), firmwareVersion, now, now, row.id).run();
        allocated = result.meta.changes === 1;
      } catch (error) {
        if (!uniqueConstraint(error)) throw error;
      }
    }
    if (!allocated) return Response.json({ error: "暂时无法分配设备码，请重试" }, { status: 503 });
  } else {
    await db.prepare(`UPDATE devices SET pairing_code = ?, pairing_expires_at = ?,
      firmware_version = ?, last_seen_at = ?, updated_at = ? WHERE id = ?`)
      .bind(row.owner_id ? null : row.pairing_code, row.owner_id ? null : row.pairing_expires_at,
        firmwareVersion, now, now, row.id)
      .run();
  }
  row = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1").bind(row.id).first<DeviceRow>();
  if (!row) return Response.json({ error: "设备注册失败" }, { status: 500 });
  return Response.json({
    deviceId: row.id,
    paired: Boolean(row.owner_id),
    pairingCode: row.owner_id ? null : row.pairing_code,
    pairingExpiresAt: row.owner_id ? null : row.pairing_expires_at,
    pollSeconds: 15,
  });
}

async function syncDevice(request: Request, payload: Record<string, unknown>) {
  const db = await ensureSchema();
  const device = await authenticatedDevice(request, db);
  const appliedRevision = Math.max(0, Number(payload.appliedRevision) || 0);
  const battery = Number(payload.batteryPercent);
  const batteryPercent = Number.isFinite(battery) ? Math.max(0, Math.min(100, Math.round(battery))) : null;
  const firmwareVersion = cleanText(payload.firmwareVersion, 40) || device.firmware_version;
  const now = new Date().toISOString();
  await db.prepare(`UPDATE devices SET applied_revision = ?, battery_percent = ?, firmware_version = ?,
    last_seen_at = ?, updated_at = ? WHERE id = ?`)
    .bind(appliedRevision, batteryPercent, firmwareVersion, now, now, device.id).run();
  const refreshed = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1")
    .bind(device.id).first<DeviceRow>();
  if (!refreshed) return Response.json({ error: "设备不存在" }, { status: 404 });
  if (!refreshed.owner_id) {
    return Response.json({ paired: false, revision: refreshed.desired_revision, tasks: [] });
  }
  if (appliedRevision === refreshed.desired_revision) {
    return Response.json({ paired: true, changed: false, revision: refreshed.desired_revision, pollSeconds: 15 });
  }
  const tasks = await tasksForDevice(db, refreshed.id);
  const origin = new URL(request.url).origin;
  return Response.json({
    paired: true,
    changed: true,
    replace: true,
    revision: refreshed.desired_revision,
    pollSeconds: 15,
    tasks: tasks.map((task) => ({
      id: task.id,
      title: task.title,
      scheduleMode: task.schedule_mode,
      customMinutes: task.custom_minutes,
      dailyTime: task.daily_time,
      revision: task.revision,
      frameUrl: `${origin}/api/devices?mode=frame&taskId=${encodeURIComponent(task.id)}`,
      frameHash: task.frame_hash,
      renderStrategy: normalizePaperColorRenderStrategy(task.render_strategy),
    })),
  });
}

async function serveFrame(request: Request) {
  const db = await ensureSchema();
  const device = await authenticatedDevice(request, db);
  const taskId = cleanText(new URL(request.url).searchParams.get("taskId"), 100);
  const task = await db.prepare(`SELECT * FROM device_tasks
    WHERE id = ? AND device_id = ? AND deleted = 0 LIMIT 1`)
    .bind(taskId, device.id).first<TaskRow>();
  if (!task) return Response.json({ error: "画面任务不存在" }, { status: 404 });
  if (!env.ASSETS) return Response.json({ error: "设备画面存储不可用" }, { status: 503 });
  const object = await env.ASSETS.get(task.frame_key);
  if (!object) return Response.json({ error: "设备画面不存在" }, { status: 404 });
  return new Response(object.body, {
    headers: {
      "Content-Type": object.httpMetadata?.contentType || "image/png",
      "Content-Length": String(object.size),
      ETag: task.frame_hash,
      "Cache-Control": "private, max-age=300",
    },
  });
}

export async function GET(request: Request) {
  try {
    const url = new URL(request.url);
    if (url.searchParams.get("mode") === "frame") return await serveFrame(request);
    const owner = requireOwner(request);
    const db = await ensureSchema();
    const result = await db.prepare("SELECT * FROM devices WHERE owner_id = ? ORDER BY updated_at DESC")
      .bind(owner).all<DeviceRow>();
    return Response.json({ devices: await Promise.all(result.results.map((row) => deviceJson(db, row))) });
  } catch (error) {
    return safeDeviceError(error);
  }
}

export async function POST(request: Request) {
  try {
    const payload = await request.json() as Record<string, unknown>;
    const action = cleanText(payload.action, 40);
    if (action === "register") return await registerDevice(payload);
    if (action === "sync") return await syncDevice(request, payload);

    const owner = requireOwner(request);
    const db = await ensureSchema();
    if (action === "claim") {
      const code = cleanText(payload.code, 6);
      if (!/^\d{6}$/.test(code)) return Response.json({ error: "请输入六位设备码" }, { status: 400 });
      const attemptId = await pairingRateLimit(request, db, owner);
      const row = await db.prepare("SELECT * FROM devices WHERE pairing_code = ? LIMIT 1")
        .bind(code).first<DeviceRow>();
      if (!row || !row.pairing_expires_at || new Date(row.pairing_expires_at).getTime() <= Date.now()) {
        await recordPairingFailure(db, attemptId);
        return Response.json({ error: "设备码无效或已过期" }, { status: 404 });
      }
      if (row.owner_id && row.owner_id !== owner) {
        return Response.json({ error: "这台设备已经绑定" }, { status: 409 });
      }
      const now = new Date().toISOString();
      const claimedResult = await db.prepare(`UPDATE devices SET owner_id = ?, pairing_code = NULL,
        pairing_expires_at = NULL, updated_at = ?
        WHERE id = ? AND owner_id IS NULL AND pairing_code = ? AND pairing_expires_at > ?`)
        .bind(owner, now, row.id, code, now).run();
      if (claimedResult.meta.changes !== 1) {
        return Response.json({ error: "设备码不可用，请重新获取" }, { status: 409 });
      }
      await db.prepare("DELETE FROM device_pairing_attempts WHERE id = ?").bind(attemptId).run();
      const claimed = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1").bind(row.id).first<DeviceRow>();
      return Response.json({ device: await deviceJson(db, claimed!) });
    }

    const deviceId = cleanText(payload.deviceId, 100);
    const device = await ownedDevice(db, deviceId, owner);
    if (!device) return Response.json({ error: "没有找到这台设备" }, { status: 404 });

    if (action === "upsert-task") {
      if (!env.ASSETS) return Response.json({ error: "设备画面存储不可用" }, { status: 503 });
      const source = payload.app as Partial<InkApp> | undefined;
      if (!source?.id || !source.title || !source.spec) {
        return Response.json({ error: "任务模版不完整" }, { status: 400 });
      }
      const scheduleMode = (["once", "hourly", "daily", "custom"] as const).includes(source.scheduleMode as ScheduleMode)
        ? source.scheduleMode as ScheduleMode
        : "once";
      const customMinutes = Math.max(1, Math.min(10080, Number(source.customMinutes) || 30));
      const dailyTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(source.dailyTime ?? "") ? source.dailyTime! : "08:00";
      const frame = parsePngDataUrl(payload.frameDataUrl);
      const sourceDisplay = source.spec && typeof source.spec === "object"
        ? (source.spec as { display?: { renderMode?: unknown } }).display
        : undefined;
      const requestedStrategy = payload.renderStrategy ?? sourceDisplay?.renderMode;
      if (payload.renderStrategy !== undefined &&
          !isPaperColorRenderStrategy(payload.renderStrategy)) {
        return Response.json({ error: "设备渲染方式无效" }, { status: 400 });
      }
      const renderStrategy = normalizePaperColorRenderStrategy(requestedStrategy);
      const app: InkApp = {
        ...(source as InkApp),
        localImage: undefined,
        scheduleMode,
        customMinutes,
        dailyTime,
      };
      const snapshot = JSON.stringify(app);
      if (snapshot.length > 96 * 1024) return Response.json({ error: "任务模版过大" }, { status: 413 });
      const existing = await db.prepare(`SELECT * FROM device_tasks
        WHERE device_id = ? AND app_id = ? ORDER BY updated_at DESC LIMIT 1`)
        .bind(device.id, app.id).first<TaskRow>();
      const taskId = existing?.id ?? `dtask-${crypto.randomUUID()}`;
      await db.prepare(`UPDATE devices SET desired_revision = desired_revision + 1,
        updated_at = ? WHERE id = ? AND owner_id = ?`)
        .bind(new Date().toISOString(), device.id, owner).run();
      const revisionRow = await db.prepare("SELECT desired_revision FROM devices WHERE id = ? LIMIT 1")
        .bind(device.id).first<{ desired_revision: number }>();
      const revision = revisionRow?.desired_revision ?? device.desired_revision + 1;
      const frameKey = `devices/${device.id}/tasks/${taskId}/${revision}.png`;
      const frameHash = await sha256(frame);
      await env.ASSETS.put(frameKey, frame, {
        httpMetadata: { contentType: "image/png" },
        customMetadata: {
          deviceId: device.id,
          taskId,
          revision: String(revision),
          sha256: frameHash,
          renderStrategy,
        },
      });
      const now = new Date().toISOString();
      await db.prepare(`INSERT INTO device_tasks
        (id, device_id, owner_id, app_id, title, app_snapshot, schedule_mode, custom_minutes,
         daily_time, frame_key, frame_hash, render_strategy, revision, deleted, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)
        ON CONFLICT(id) DO UPDATE SET title = excluded.title, app_snapshot = excluded.app_snapshot,
          schedule_mode = excluded.schedule_mode, custom_minutes = excluded.custom_minutes,
          daily_time = excluded.daily_time, frame_key = excluded.frame_key,
          frame_hash = excluded.frame_hash, render_strategy = excluded.render_strategy,
          revision = excluded.revision, deleted = 0,
          updated_at = excluded.updated_at`)
        .bind(taskId, device.id, owner, cleanText(app.id, 100), cleanText(app.title, 100), snapshot,
          scheduleMode, customMinutes, dailyTime, frameKey, frameHash,
          renderStrategy, revision, now, now)
        .run();
      if (existing?.frame_key && existing.frame_key !== frameKey) {
        await env.ASSETS.delete(existing.frame_key).catch(() => undefined);
      }
      const refreshed = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1").bind(device.id).first<DeviceRow>();
      const task = await db.prepare("SELECT * FROM device_tasks WHERE id = ? LIMIT 1").bind(taskId).first<TaskRow>();
      return Response.json({ device: await deviceJson(db, refreshed!), task: taskJson(task!) });
    }

    if (action === "delete-task") {
      const taskId = cleanText(payload.taskId, 100);
      const task = await db.prepare(`SELECT * FROM device_tasks
        WHERE id = ? AND device_id = ? AND owner_id = ? AND deleted = 0 LIMIT 1`)
        .bind(taskId, device.id, owner).first<TaskRow>();
      if (!task) return Response.json({ error: "任务不存在" }, { status: 404 });
      const now = new Date().toISOString();
      await db.batch([
        db.prepare(`UPDATE devices SET desired_revision = desired_revision + 1,
          updated_at = ? WHERE id = ? AND owner_id = ?`).bind(now, device.id, owner),
        db.prepare("UPDATE device_tasks SET deleted = 1, updated_at = ? WHERE id = ?")
          .bind(now, task.id),
      ]);
      if (env.ASSETS) await env.ASSETS.delete(task.frame_key).catch(() => undefined);
      const refreshed = await db.prepare("SELECT * FROM devices WHERE id = ? LIMIT 1").bind(device.id).first<DeviceRow>();
      return Response.json({ device: await deviceJson(db, refreshed!) });
    }

    return Response.json({ error: "未知设备操作" }, { status: 400 });
  } catch (error) {
    return safeDeviceError(error);
  }
}
