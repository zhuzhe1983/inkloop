CREATE TABLE `public_apps` (
	`id` text PRIMARY KEY NOT NULL,
	`title` text NOT NULL,
	`description` text NOT NULL,
	`prompt` text NOT NULL,
	`spec` text NOT NULL,
	`code` text NOT NULL,
	`schedule_mode` text NOT NULL,
	`custom_minutes` integer DEFAULT 30 NOT NULL,
	`daily_time` text DEFAULT '08:00' NOT NULL,
	`author` text DEFAULT '匿名创作者' NOT NULL,
	`created_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL
);
