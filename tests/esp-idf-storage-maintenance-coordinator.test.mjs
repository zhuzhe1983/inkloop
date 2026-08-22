import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_product",
);
const runtimeHeader = readFileSync(
  join(product, "include/inkloop/product_runtime.hpp"),
  "utf8",
);
const runtime = readFileSync(join(product, "product_runtime.cpp"), "utf8");
const device = readFileSync(
  join(product, "native_device_state_owner.cpp"),
  "utf8",
);
const display = readFileSync(
  join(product, "native_display_service.cpp"),
  "utf8",
);
const appMain = readFileSync(
  join(repo, "firmware/inkloop-idf/main/app_main.cpp"),
  "utf8",
);

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `${signature} is missing`);
  const open = source.indexOf("{", start);
  let depth = 0;
  for (let at = open; at < source.length; at += 1) {
    if (source[at] === "{") depth += 1;
    if (source[at] === "}") depth -= 1;
    if (depth === 0) return source.slice(open + 1, at);
  }
  assert.fail(`${signature} body is unterminated`);
}

test("confirmed TF format is owned by one composition coordinator", () => {
  assert.match(
    runtimeHeader,
    /class EspProductRuntime final : public IStorageMaintenanceCoordinator/,
  );
  assert.match(runtimeHeader, /formatTfCardConfirmed\(\) override/);
  assert.match(
    appMain,
    /attachStorageMaintenanceCoordinator\(runtime\)[\s\S]*runtime\.begin\(\)/,
  );
  const format = body(
    runtime,
    "StorageMaintenanceResult EspProductRuntime::formatTfCardConfirmed()",
  );
  const admissions = [
    "portal_.beginStorageMaintenance()",
    "voice_.beginStorageMaintenance()",
    "inkloop_.beginStorageMaintenance()",
    "display_.beginStorageMaintenance()",
    "sd_album_store_->beginMaintenance()",
    "storage_.formatSdCardConfirmed()",
  ];
  let previous = -1;
  for (const admission of admissions) {
    const at = format.indexOf(admission);
    assert.ok(at > previous, `${admission} is out of order`);
    previous = at;
  }
  assert.doesNotMatch(
    body(device, "NativeDeviceStateOwner::formatTfCard()"),
    /formatSdCardConfirmed|AssetStoragePreference/,
  );
});

test("store exclusion ends before catalog rebuild while owner gates remain", () => {
  const format = body(
    runtime,
    "StorageMaintenanceResult EspProductRuntime::formatTfCardConfirmed()",
  );
  const raw = format.indexOf("storage_.formatSdCardConfirmed()");
  const storeEnd = format.indexOf("sd_album_store_->endMaintenance()", raw);
  const displayFinish = format.indexOf("display_.finishStorageMaintenance", raw);
  const finalizing = format.indexOf(
    "storage_maintenance_phase_ = StorageMaintenancePhase::Finalizing",
    raw,
  );
  assert.ok(raw >= 0 && storeEnd > raw && displayFinish > storeEnd);
  assert.ok(finalizing > displayFinish);
  assert.match(format, /storage_changed = formatted != ESP_ERR_INVALID_STATE/);
  assert.match(
    format,
    /finishStorageMaintenance\([\s\S]*storage_maintenance_available_/,
  );
  assert.match(
    body(runtime, "EspProductRuntime::releaseStorageMaintenanceOwners()"),
    /voice_\.endStorageMaintenance\(\)[\s\S]*portal_\.endStorageMaintenance\(\)[\s\S]*inkloop_\.endStorageMaintenance/,
  );
});

test("Display maintenance admission never blocks on the album mutex inside a spinlock", () => {
  const begin = body(
    display,
    "bool NativeDisplayService::beginStorageMaintenance()",
  );
  assert.doesNotMatch(begin, /album_store_->active\(\)/);
  assert.match(begin, /portENTER_CRITICAL\(&mux_\)/);
  assert.match(begin, /storage_maintenance_ = true/);
});
