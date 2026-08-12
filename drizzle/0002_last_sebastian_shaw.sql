DROP INDEX `idx_public_apps_created_at`;--> statement-breakpoint
ALTER TABLE `public_apps` ADD `listed` integer DEFAULT true NOT NULL;--> statement-breakpoint
CREATE INDEX `idx_public_apps_listed_created_at` ON `public_apps` (`listed`,`created_at`);