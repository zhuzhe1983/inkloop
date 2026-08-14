CREATE TABLE `device_pairing_attempts` (
	`id` text PRIMARY KEY NOT NULL,
	`attempt_count` integer DEFAULT 0 NOT NULL,
	`window_started_at` text NOT NULL,
	`locked_until` text,
	`updated_at` text DEFAULT CURRENT_TIMESTAMP NOT NULL
);
--> statement-breakpoint
CREATE INDEX `idx_device_pairing_attempts_updated_at` ON `device_pairing_attempts` (`updated_at`);