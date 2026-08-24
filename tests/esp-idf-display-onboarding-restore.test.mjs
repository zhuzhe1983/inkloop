import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const header = readFileSync(
  join(product, "include/inkloop/native_display_service.hpp"), "utf8",
);
const display = readFileSync(join(product, "native_display_service.cpp"), "utf8");
const runtime = readFileSync(join(product, "product_runtime.cpp"), "utf8");

function section(text, signature, nextSignature) {
  const start = text.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = text.indexOf(nextSignature, start + signature.length);
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

test("album restore is a bounded secret-free Display request", () => {
  assert.match(header, /NativeDisplayPageRequestResult requestAlbumRestore\(\);/);
  assert.match(header, /bool album_restore_pending_ = false;/);
  assert.doesNotMatch(
    header,
    /requestAlbumRestore\([^)]*(ssid|access|code|url|char|String|string_view)/i,
  );

  const request = section(
    display,
    "NativeDisplayPageRequestResult NativeDisplayService::requestAlbumRestore()",
    "AlbumStepResult NativeDisplayService::selectRelative",
  );
  ordered(request, [
    "portENTER_CRITICAL(&mux_)",
    "storage_maintenance_",
    "NativeDisplayPageRequestResult::Busy",
    "onboarding_pending_ = false",
    "clearMailbox(onboarding_mailbox_)",
    "album_restore_pending_ = true",
    "portEXIT_CRITICAL(&mux_)",
  ]);
  assert.doesNotMatch(request, /readCatalog|open\s*\(|readExactFile|writePanelFrame/);
});

test("Display tick retries restoration and never writes from the Portal lane", () => {
  const service = section(
    display,
    "void NativeDisplayService::service()",
    "bool NativeDisplayService::writePanelFrame",
  );
  ordered(service, [
    "if (storage_maintenance_)",
    "album_restore_pending_",
    "restore_album = true",
    "onboarding_replacement_pending_ = true",
    "album_store_->active()",
    "onboarding_replacement_pending_ = false",
    "renderOrdinal(AlbumNavigationCore::kNoOrdinal)",
    "if (restored || !onboarding_visible_) album_restore_pending_ = false",
  ]);
  assert.doesNotMatch(service, /album_restore_pending_ = false[\s\S]{0,160}renderOrdinal\(AlbumNavigationCore::kNoOrdinal\)/);

  const stablePages = section(
    runtime,
    "void EspProductRuntime::serviceStableDisplayPages",
    "void EspProductRuntime::servicePortal()",
  );
  ordered(stablePages, [
    "if (wifi.provisioning_ap)",
    "requestProvisioningPage",
    "if (!onboarding.pairing_view_available)",
    "display_.requestAlbumRestore()",
    "requestMyAiPairingPage",
  ]);
  assert.doesNotMatch(stablePages, /writeFullFrame|readCatalog|absoluteAssetPath/);
});

test("authoritative onboarding cannot cache Unchanged during album replacement", () => {
  const provisioning = section(
    display,
    "NativeDisplayPageRequestResult NativeDisplayService::requestProvisioningPage",
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
  );
  ordered(provisioning, [
    "onboarding_replacement_pending_ || album_rendering_",
    "NativeDisplayPageRequestResult::Busy",
    "visible_onboarding_fingerprint_ == content",
    "NativeDisplayPageRequestResult::Unchanged",
  ]);

  const pairing = section(
    display,
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
    "NativeDisplayPageRequestResult NativeDisplayService::requestAlbumRestore()",
  );
  ordered(pairing, [
    "onboarding_replacement_pending_ || album_rendering_",
    "NativeDisplayPageRequestResult::Busy",
    "visible_onboarding_fingerprint_ == content",
    "NativeDisplayPageRequestResult::Unchanged",
  ]);
});

test("restore resolves persisted current, falls back to zero, and is empty-safe", () => {
  const render = section(
    display,
    "bool NativeDisplayService::renderOrdinalAdmitted(size_t ordinal,",
    "NativeDisplayDiagnostics NativeDisplayService::diagnostics() const",
  );
  ordered(render, [
    "const bool restore_onboarding = ordinal == AlbumNavigationCore::kNoOrdinal",
    "album_store_->readCatalog(index)",
    "index.assets[at].id == index.current",
    "if (restore_onboarding && index.assets.empty())",
    "navigation_.finish(0, AlbumNavigationCore::kNoOrdinal)",
    "return true",
    "ordinal = current == AlbumNavigationCore::kNoOrdinal ? 0U : current",
    "const storage::AlbumIndexAsset asset = index.assets[ordinal]",
  ]);
  assert.ok(
    render.indexOf("if (restore_onboarding && index.assets.empty())") <
      render.indexOf("index.assets[ordinal]"),
  );
});

test("maintenance and busy state include pending album restoration", () => {
  const begin = section(
    display,
    "bool NativeDisplayService::beginStorageMaintenance()",
    "bool NativeDisplayService::finishStorageMaintenance(bool reload_catalog)",
  );
  assert.match(begin, /!album_restore_pending_/);

  const busy = section(
    display,
    "bool NativeDisplayService::busy() const",
    "WorkDisposition NativeDisplayService::handler",
  );
  assert.match(busy, /onboarding_replacement_pending_ \|\| album_restore_pending_/);
});
