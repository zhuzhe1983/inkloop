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
