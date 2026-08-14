CREATE TABLE `device_tasks` (
	`id` text PRIMARY KEY NOT NULL,
	`device_id` text NOT NULL,
	`owner_id` text NOT NULL,
	`app_id` text NOT NULL,
	`title` text NOT NULL,
	`app_snapshot` text NOT NULL,
	`schedule_mode` text NOT NULL,
	`custom_minutes` integer DEFAULT 30 NOT NULL,
	`daily_time` text DEFAULT '08:00' NOT NULL,
	`frame_key` text NOT NULL,
	`frame_hash` text NOT NULL,
	`revision` integer NOT NULL,
	`deleted` integer DEFAULT false NOT NULL,
	`created_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL,
	`updated_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL
);
--> statement-breakpoint
CREATE INDEX `idx_device_tasks_device_revision` ON `device_tasks` (`device_id`,`revision`);--> statement-breakpoint
CREATE INDEX `idx_device_tasks_owner_updated_at` ON `device_tasks` (`owner_id`,`updated_at`);--> statement-breakpoint
CREATE TABLE `devices` (
	`id` text PRIMARY KEY NOT NULL,
	`hardware_id` text NOT NULL,
	`owner_id` text,
	`sku_id` text NOT NULL,
	`name` text NOT NULL,
	`secret_hash` text NOT NULL,
	`pairing_code` text,
	`pairing_expires_at` text,
	`firmware_version` text,
	`battery_percent` integer,
	`desired_revision` integer DEFAULT 0 NOT NULL,
	`applied_revision` integer DEFAULT 0 NOT NULL,
	`last_seen_at` text,
	`created_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL,
	`updated_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL
);
--> statement-breakpoint
CREATE UNIQUE INDEX `devices_hardware_id_unique` ON `devices` (`hardware_id`);--> statement-breakpoint
CREATE INDEX `idx_devices_owner_updated_at` ON `devices` (`owner_id`,`updated_at`);--> statement-breakpoint
CREATE INDEX `idx_devices_pairing_code` ON `devices` (`pairing_code`);