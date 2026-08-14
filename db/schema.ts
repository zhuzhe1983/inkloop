import { sql } from "drizzle-orm";
import { index, integer, sqliteTable, text } from "drizzle-orm/sqlite-core";

export const publicApps = sqliteTable(
  "public_apps",
  {
    id: text("id").primaryKey(),
    title: text("title").notNull(),
    description: text("description").notNull(),
    prompt: text("prompt").notNull(),
    spec: text("spec").notNull(),
    code: text("code").notNull(),
    scheduleMode: text("schedule_mode").notNull(),
    customMinutes: integer("custom_minutes").notNull().default(30),
    dailyTime: text("daily_time").notNull().default("08:00"),
    author: text("author").notNull().default("匿名创作者"),
    listed: integer("listed", { mode: "boolean" }).notNull().default(true),
    createdAt: text("created_at").notNull().default(sql`CURRENT_TIMESTAMP`),
  },
  (table) => [index("idx_public_apps_listed_created_at").on(table.listed, table.createdAt)],
);

export const devices = sqliteTable(
  "devices",
  {
    id: text("id").primaryKey(),
    hardwareId: text("hardware_id").notNull().unique(),
    ownerId: text("owner_id"),
    skuId: text("sku_id").notNull(),
    name: text("name").notNull(),
    secretHash: text("secret_hash").notNull(),
    pairingCode: text("pairing_code"),
    pairingExpiresAt: text("pairing_expires_at"),
    firmwareVersion: text("firmware_version"),
    batteryPercent: integer("battery_percent"),
    desiredRevision: integer("desired_revision").notNull().default(0),
    appliedRevision: integer("applied_revision").notNull().default(0),
    lastSeenAt: text("last_seen_at"),
    createdAt: text("created_at").notNull().default(sql`CURRENT_TIMESTAMP`),
    updatedAt: text("updated_at").notNull().default(sql`CURRENT_TIMESTAMP`),
  },
  (table) => [
    index("idx_devices_owner_updated_at").on(table.ownerId, table.updatedAt),
    index("idx_devices_pairing_code").on(table.pairingCode),
  ],
);

export const deviceTasks = sqliteTable(
  "device_tasks",
  {
    id: text("id").primaryKey(),
    deviceId: text("device_id").notNull(),
    ownerId: text("owner_id").notNull(),
    appId: text("app_id").notNull(),
    title: text("title").notNull(),
    appSnapshot: text("app_snapshot").notNull(),
    scheduleMode: text("schedule_mode").notNull(),
    customMinutes: integer("custom_minutes").notNull().default(30),
    dailyTime: text("daily_time").notNull().default("08:00"),
    frameKey: text("frame_key").notNull(),
    frameHash: text("frame_hash").notNull(),
    revision: integer("revision").notNull(),
    deleted: integer("deleted", { mode: "boolean" }).notNull().default(false),
    createdAt: text("created_at").notNull().default(sql`CURRENT_TIMESTAMP`),
    updatedAt: text("updated_at").notNull().default(sql`CURRENT_TIMESTAMP`),
  },
  (table) => [
    index("idx_device_tasks_device_revision").on(table.deviceId, table.revision),
    index("idx_device_tasks_owner_updated_at").on(table.ownerId, table.updatedAt),
  ],
);

export const devicePairingAttempts = sqliteTable(
  "device_pairing_attempts",
  {
    id: text("id").primaryKey(),
    attemptCount: integer("attempt_count").notNull().default(0),
    windowStartedAt: text("window_started_at").notNull(),
    lockedUntil: text("locked_until"),
    updatedAt: text("updated_at").notNull().default(sql`CURRENT_TIMESTAMP`),
  },
  (table) => [index("idx_device_pairing_attempts_updated_at").on(table.updatedAt)],
);
