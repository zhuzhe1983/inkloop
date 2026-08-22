import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = join(process.cwd(), "firmware", "inkloop-idf");

test("product boot is guarded by a no-erase read-only upgrade inventory", () => {
  const main = readFileSync(join(root, "main", "app_main.cpp"), "utf8");
  const guard = readFileSync(
    join(root, "components", "inkloop_storage", "esp_upgrade_boot_audit.cpp"),
    "utf8",
  );
  assert.match(main, /runReadOnlyUpgradeBootAudit\(storage\)/);
  assert.match(main, /if \(!upgrade\.allowsStartup\(\)\)/);
  assert.ok(main.indexOf("runReadOnlyUpgradeBootAudit") < main.indexOf("board_initialize"));
  assert.match(main, /runReadOnlyMountedFileUpgradeAudit\([\s\S]*selected_asset_root/);
  assert.ok(
    main.indexOf("runReadOnlyMountedFileUpgradeAudit") <
      main.indexOf("EspProductRuntime runtime"),
  );
  assert.match(main, /persistenceCompatibilityContractValid\(\)/);
  assert.match(guard, /nvs_flash_init\(\)/);
  assert.match(guard, /mountInternal\(\)/);
  assert.match(guard, /EspNvsUpgradeInventory/);
  assert.match(guard, /PosixUpgradeInventory/);
  assert.match(guard, /auditUpgrade/);
  assert.match(guard, /runReadOnlyMountedFileUpgradeAudit/);
  assert.doesNotMatch(guard, /nvs_flash_erase|format|remove|unlink|rename|nvs_set|nvs_commit/);
});

test("PaperColor TF storage joins the board-owned shared SPI2 bus", () => {
  const header = readFileSync(
    join(root, "components", "inkloop_storage", "include", "inkloop", "storage", "esp_storage_mount.hpp"),
    "utf8",
  );
  const source = readFileSync(
    join(root, "components", "inkloop_storage", "esp_storage_mount.cpp"),
    "utf8",
  );
  assert.match(header, /sd_spi_host = SPI2_HOST/);
  assert.match(header, /sd_bus_already_initialized = true/);
  assert.match(source, /if \(!config_\.sd_bus_already_initialized\)/);
  assert.match(source, /if \(sd_bus_owned_\) spi_bus_free/);
  assert.doesNotMatch(header, /SPI3_HOST/);
});
