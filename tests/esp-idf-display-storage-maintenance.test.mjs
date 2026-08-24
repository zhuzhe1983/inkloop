import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const header = readFileSync(
  join(product, "include/inkloop/native_display_service.hpp"), "utf8",
);
const source = readFileSync(join(product, "native_display_service.cpp"), "utf8");

function section(text, signature, nextSignature) {
  const start = text.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = nextSignature
    ? text.indexOf(nextSignature, start + signature.length)
    : -1;
  assert.notEqual(end, -1, `missing boundary ${nextSignature}`);
  return text.slice(start, end);
}

function ordered(text, fragments) {
  let previous = -1;
  for (const fragment of fragments) {
    const at = text.indexOf(fragment, previous + 1);
    assert.ok(at > previous, `expected ${fragment} after offset ${previous}`);
    previous = at;
  }
}

test("maintenance admission atomically gates every active Display-owned state", () => {
  assert.match(header, /bool beginStorageMaintenance\(\);/);
  assert.match(header, /bool finishStorageMaintenance\(bool reload_catalog\);/);
  assert.match(header, /bool catalog_refreshing_ = false;/);
  assert.match(header, /bool album_rendering_ = false;/);
  assert.match(header, /bool storage_maintenance_ = false;/);

  const begin = section(
    source,
    "bool NativeDisplayService::beginStorageMaintenance()",
    "bool NativeDisplayService::finishStorageMaintenance(bool reload_catalog)",
  );
  ordered(begin, [
    "portENTER_CRITICAL(&mux_)",
    "configured_ && !storage_maintenance_",
    "!catalog_refreshing_",
    "!album_rendering_",
    "!navigation_.pending()",
    "!navigation_.refreshing()",
    "!onboarding_pending_",
    "!onboarding_rendering_",
    "!onboarding_replacement_pending_",
    "!album_restore_pending_ && !interactive_selection_mailbox_.active",
    "album_store_",
    "storage_maintenance_ = true",
    "portEXIT_CRITICAL(&mux_)",
  ]);
  assert.equal((begin.match(/portENTER_CRITICAL/g) ?? []).length, 1);
  assert.equal((begin.match(/portEXIT_CRITICAL/g) ?? []).length, 1);
  assert.doesNotMatch(begin, /album_store_->active\(\)/);
});

test("catalog, onboarding, selection, tick and command admissions fail closed", () => {
  const catalog = section(
    source,
    "bool NativeDisplayService::synchronizeCatalog()",
    "bool NativeDisplayService::reloadCatalog()",
  );
  ordered(catalog, [
    "portENTER_CRITICAL(&mux_)",
    "storage_maintenance_ || catalog_refreshing_ || album_rendering_",
    "catalog_refreshing_ = true",
    "album_store_->readCatalog(index)",
    "catalog_refreshing_ = false",
  ]);

  const provisioning = section(
    source,
    "NativeDisplayPageRequestResult NativeDisplayService::requestProvisioningPage",
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
  );
  ordered(provisioning, [
    "portENTER_CRITICAL(&mux_)",
    "storage_maintenance_",
    "NativeDisplayPageRequestResult::Busy",
    "onboarding_pending_ = true",
  ]);

  const pairing = section(
    source,
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
    "AlbumStepResult NativeDisplayService::selectRelative",
  );
  ordered(pairing, [
    "portENTER_CRITICAL(&mux_)",
    "storage_maintenance_",
    "NativeDisplayPageRequestResult::Busy",
    "onboarding_pending_ = true",
  ]);

  const selection = section(
    source,
    "AlbumStepResult NativeDisplayService::selectRelative",
    "bool NativeDisplayService::refreshing() const",
  );
  assert.match(
    selection,
    /storage_maintenance_\s*\? AlbumStepResult::Busy\s*:\s*navigation_\.step/,
  );

  const service = section(
    source,
    "void NativeDisplayService::service()",
    "bool NativeDisplayService::writePanelFrame",
  );
  ordered(service, [
    "if (storage_maintenance_)",
    "renderOnboardingPage()",
    "!storage_maintenance_ && !catalog_refreshing_",
    "navigation_.takeSettled",
    "renderOrdinal(ordinal, true)",
  ]);

  const handler = section(
    source,
    "WorkDisposition NativeDisplayService::handle",
    "bool NativeDisplayService::renderOrdinal(size_t ordinal,",
  );
  ordered(handler, [
    "storage_maintenance_ || catalog_refreshing_",
    "return WorkDisposition::Busy",
    "renderOrdinal(",
    "ordinal, user_initiated, constrained_asset",
  ]);
});

test("an admitted album render holds an explicit lease across all album I/O", () => {
  const admission = section(
    source,
    "bool NativeDisplayService::renderOrdinal(size_t ordinal,",
    "bool NativeDisplayService::renderOrdinalAdmitted(size_t ordinal,",
  );
  ordered(admission, [
    "portENTER_CRITICAL(&mux_)",
    "storage_maintenance_ || catalog_refreshing_ || album_rendering_",
    "album_rendering_ = true",
    "renderOrdinalAdmitted(",
    "ordinal, user_initiated, expected_asset_id",
    "album_rendering_ = false",
    "portEXIT_CRITICAL(&mux_)",
  ]);
  assert.doesNotMatch(admission, /readCatalog|absoluteAssetPath|readExactFile/);

  const decoder = section(
    source,
    "bool decodePngFile(const std::string& path",
    "}  // namespace",
  );
  ordered(decoder, [
    "::open(path.c_str(), O_RDONLY)",
    "::fstat(descriptor, &status)",
    "static_cast<uint64_t>(status.st_size) == expected",
    "::read(",
    "pngle_feed(png, feed.get(), buffered)",
    "::close(descriptor)",
    "read_total == expected",
  ]);
  assert.doesNotMatch(decoder, /std::vector|readExactFile/);

  const admitted = section(
    source,
    "bool NativeDisplayService::renderOrdinalAdmitted(size_t ordinal,",
    "NativeDisplayDiagnostics NativeDisplayService::diagnostics() const",
  );
  ordered(admitted, [
    "album_store_->readCatalog(index)",
    "const storage::AlbumIndexAsset asset = index.assets[ordinal]",
    "renderer->supportsRenderStrategy(asset.render_strategy)",
    "album_store_->absoluteAssetPath(asset, path)",
    "decodePngFile(path, asset.bytes, rgb.get(), rgb.size()",
    "renderer->renderRgbFullFrame(",
    "rgb_view, asset.render_strategy",
    "writePanelFrame(frame.get(), frame.size())",
    "album_store_->markCurrent(asset.id)",
  ]);
  // Every return from the admitted body returns through renderOrdinal(), which
  // is the sole place that releases the lease after the complete operation.
  assert.doesNotMatch(admitted, /album_rendering_\s*=\s*false/);
});

test("maintenance reloads under the gate, releases atomically, and fails closed", () => {
  const busy = section(
    source,
    "bool NativeDisplayService::busy() const",
    "WorkDisposition NativeDisplayService::handler",
  );
  assert.match(busy, /album_rendering_ \|\| storage_maintenance_/);

  const finish = section(
    source,
    "bool NativeDisplayService::finishStorageMaintenance(bool reload_catalog)",
    "bool NativeDisplayService::synchronizeCatalog()",
  );
  ordered(finish, [
    "portENTER_CRITICAL(&mux_)",
    "if (!reload_catalog)",
    "storage_maintenance_ = false",
    "portEXIT_CRITICAL(&mux_)",
    "album_store_->readCatalog(index)",
    "portENTER_CRITICAL(&mux_)",
    "navigation_.synchronize(index.assets.size(), current)",
    "catalog_known_empty_ = index.assets.empty()",
    "storage_maintenance_ = false",
    "navigation_.invalidate()",
    "catalog_refreshing_ = false",
    "portEXIT_CRITICAL(&mux_)",
  ]);
  const releaseAt = finish.lastIndexOf("storage_maintenance_ = false");
  const synchronizeAt = finish.indexOf("navigation_.synchronize");
  const finalEnterAt = finish.lastIndexOf("portENTER_CRITICAL(&mux_)");
  const finalExitAt = finish.lastIndexOf("portEXIT_CRITICAL(&mux_)");
  assert.ok(finalEnterAt < synchronizeAt && synchronizeAt < releaseAt);
  assert.ok(releaseAt < finalExitAt);
  const failureAt = finish.indexOf("} else {", synchronizeAt);
  const failureEnd = finish.indexOf("catalog_refreshing_ = false", failureAt);
  assert.doesNotMatch(
    finish.slice(failureAt, failureEnd),
    /storage_maintenance_ = false/,
  );

  assert.doesNotMatch(
    source,
    /\bf_mkfs\s*\(|esp_vfs_fat_sdcard_format\s*\(|esp_restart\s*\(|stopServer\s*\(/,
  );
});
