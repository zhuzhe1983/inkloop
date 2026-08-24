import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const components = join(repo, "firmware/inkloop-idf/components");
const product = join(components, "inkloop_product");
const header = readFileSync(
  join(product, "include/inkloop/native_display_service.hpp"), "utf8",
);
const source = readFileSync(
  join(product, "native_display_service.cpp"), "utf8",
);
const productCmake = readFileSync(join(product, "CMakeLists.txt"), "utf8");
const onboardingCmake = readFileSync(
  join(components, "inkloop_onboarding/CMakeLists.txt"), "utf8",
);
const pairingAdapter = readFileSync(
  join(components, "inkloop_onboarding_idf/esp_pairing_frame.cpp"), "utf8",
);
const boardRoot = join(repo, "firmware/inkloop-idf/boards/m5_papercolor_c151");
const boardRenderer = readFileSync(
  join(boardRoot, "papercolor_renderer.cpp"), "utf8",
);
const boardCmake = readFileSync(join(boardRoot, "CMakeLists.txt"), "utf8");

function body(text, signature, nextSignature) {
  const start = text.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = nextSignature
    ? text.indexOf(nextSignature, start + signature.length)
    : -1;
  return text.slice(start, end < 0 ? undefined : end);
}

test("display exposes bounded typed provisioning and authoritative MyAI requests", () => {
  assert.match(header, /struct NativeProvisioningPageRequest\s*{[\s\S]*std::string_view ssid;[\s\S]*std::string_view access_value;[\s\S]*std::string_view local_host;[\s\S]*std::string_view local_ip;/);
  assert.match(header, /struct NativeMyAiPairingPageRequest\s*{[\s\S]*std::string_view six_digit_code;[\s\S]*std::string_view binding_url;/);
  assert.match(header, /enum class NativeDisplayPageRequestResult[\s\S]*Accepted[\s\S]*AlreadyPending[\s\S]*Unchanged[\s\S]*NotReady[\s\S]*Busy[\s\S]*InvalidInput/);
  assert.match(header, /std::array<char, 33> ssid/);
  assert.match(header, /std::array<char, 64> access_value/);
  assert.match(header, /std::array<char, 65> local_host/);
  assert.match(header, /std::array<char, 65> local_ip/);
  assert.match(header, /std::array<char, 7> six_digit_code/);
  assert.match(header, /std::array<char, 257> binding_url/);
  assert.match(source, /descriptor\.width == 0U/);
  assert.match(source, /descriptor\.height == 0U/);
  assert.match(source, /descriptor\.palette_colors > 16U/);
  assert.match(source, /!board_\.renderer\(\)/);
  assert.doesNotMatch(source, /descriptor\.width != 400U|descriptor\.height != 600U/);

  const provisioning = body(
    source,
    "NativeDisplayPageRequestResult NativeDisplayService::requestProvisioningPage",
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
  );
  assert.match(provisioning, /printable\(request\.ssid, 1U, 32U\)/);
  assert.match(provisioning, /printable\(request\.access_value, 8U, 63U\)/);
  assert.match(provisioning, /onboarding_pending_ = true/);
  assert.doesNotMatch(
    provisioning,
    /writeFullFrame|renderProvisioningFrame4Bpp|heap_caps_malloc|ESP_LOG/,
  );

  const pairing = body(
    source,
    "NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage",
    "AlbumStepResult NativeDisplayService::selectRelative",
  );
  assert.match(pairing, /validMyAiPairingInputs\([\s\S]*request\.six_digit_code[\s\S]*request\.binding_url/);
  assert.match(pairing, /onboarding_pending_ = true/);
  assert.doesNotMatch(
    pairing,
    /writeFullFrame|renderEspMyAiPairingFrame4Bpp|heap_caps_malloc|ESP_LOG/,
  );
});

test("only the Display service consumes onboarding and emits one complete 4bpp frame", () => {
  const service = body(
    source,
    "void NativeDisplayService::service()",
    "bool NativeDisplayService::writePanelFrame",
  );
  assert.match(service, /onboarding_pending_[\s\S]*onboarding_rendering_[\s\S]*renderOnboardingPage\(\)/);

  const render = body(
    source,
    "bool NativeDisplayService::renderOnboardingPage()",
    "AdmissionResult NativeDisplayService::postImageLed",
  );
  assert.match(render, /HeapBytes frame\(descriptor\.packed4BppFrameBytes\(\), descriptor\.has_psram\)/);
  assert.match(render, /renderer->renderProvisioningFrame\(/);
  assert.match(render, /renderer->renderMyAiPairingFrame\(/);
  assert.match(render, /rendered == ESP_OK && writePanelFrame\(frame\.get\(\), frame\.size\(\)\)/);
  assert.doesNotMatch(render, /std::array<uint8_t,\s*(?:120000|kFrameBytes)>/);

  assert.equal((source.match(/writeFullFrame\s*\(/g) ?? []).length, 1);
  const writer = body(
    source,
    "bool NativeDisplayService::writePanelFrame",
    "bool NativeDisplayService::renderOnboardingPage",
  );
  assert.match(writer, /BoardFrameView view\{[\s\S]*descriptor\.width, descriptor\.height,[\s\S]*BoardFrameFormat::Palette4Bpp/);
  assert.match(writer, /display->writeFullFrame\(view\) == ESP_OK/);
  assert.doesNotMatch(render, /boot|connecting|ESP_LOG[DIWVE]/i);
});

test("onboarding is fail-closed, deduplicated, and does not expose secrets", () => {
  assert.match(source, /onboarding_visible_[\s\S]*onboarding_unchanged_skips/);
  assert.match(source, /clearMailbox\(onboarding_mailbox_\)/);
  assert.match(source, /volatile uint8_t\* bytes[\s\S]*sizeof\(mailbox\)/);
  assert.doesNotMatch(source, /ESP_LOG[DIWVE][\s\S]{0,200}(?:binding_url|access_value|six_digit_code|ssid)/);
  assert.doesNotMatch(source, /\bprintf\s*\(|\bputs\s*\(/);
  assert.doesNotMatch(pairingAdapter, /^(?!\s*\/\/).*esp_qrcode_generate\s*\(|ESP_LOG[DIWVE]/m);
  assert.match(pairingAdapter, /qrcodegen_encodeText/);
  assert.match(pairingAdapter, /kConfirmedQrVersion, kConfirmedQrVersion/);
});

test("album render and acknowledgement semantics remain synchronous", () => {
  const handler = body(
    source,
    "WorkDisposition NativeDisplayService::handle",
    "bool NativeDisplayService::renderOrdinal",
  );
  assert.match(handler, /DisplayAlbumOrdinal/);
  assert.match(
    handler,
    /const bool rendered = renderOrdinal\([\s\S]{0,80}ordinal, user_initiated, constrained_asset\)/,
  );
  assert.match(handler, /return rendered \? WorkDisposition::Complete/);
  assert.match(handler, /DisplayDiagnosticAigcOrdinal/);
  assert.match(handler, /DisplayInteractiveAlbumOrdinal/);
  assert.match(handler, /SerialDiagnosticAigcPhase::DisplayStart/);
  assert.match(handler, /SerialDiagnosticAigcPhase::DisplayComplete/);

  const album = body(
    source,
    "bool NativeDisplayService::renderOrdinal",
    "NativeDisplayDiagnostics NativeDisplayService::diagnostics",
  );
  const writeAt = album.indexOf("writePanelFrame(frame.get(), frame.size())");
  const replaceAt = album.indexOf("onboarding_visible_ = false", writeAt);
  const persistAt = album.indexOf("album_store_->markCurrent(asset.id)");
  const finishAt = album.lastIndexOf("navigation_.finish(index.assets.size(), ordinal)");
  assert.ok(
    writeAt >= 0 && replaceAt > writeAt &&
      persistAt > replaceAt && finishAt > persistAt,
  );
  assert.match(album, /decode_failures;[\s\S]{0,120}onboarding_replacement_pending_ = false/);
  assert.match(album, /render_failures;[\s\S]{0,120}onboarding_replacement_pending_ = false/);
  assert.match(album, /renderer->supportsRenderStrategy\(asset\.render_strategy\)/);
  assert.match(album, /!onboarding_visible && index\.current == asset\.id &&[\s\S]*index\.current_render_strategy == asset\.render_strategy/);
});

test("ESP-IDF component graph includes both portable renderers and fixed QR adapter", () => {
  assert.match(onboardingCmake, /SRCS[\s\S]*pairing_frame\.cpp[\s\S]*provisioning_frame\.cpp/);
  assert.match(productCmake, /REQUIRES[\s\S]*inkloop_onboarding/);
  assert.doesNotMatch(productCmake, /inkloop_onboarding_idf|inkloop_render/);
  assert.match(boardCmake, /papercolor_renderer\.cpp/);
  assert.match(boardCmake, /inkloop_onboarding_idf/);
  assert.match(boardCmake, /inkloop_render/);
  assert.match(boardRenderer, /renderProvisioningFrame4Bpp/);
  assert.match(boardRenderer, /renderEspMyAiPairingFrame4Bpp/);
  assert.match(boardRenderer, /streamRenderPixels/);
});
