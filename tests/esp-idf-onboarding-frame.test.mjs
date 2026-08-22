import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const portable = join(
  repo, "firmware/inkloop-idf/components/inkloop_onboarding",
);
const idf = join(
  repo, "firmware/inkloop-idf/components/inkloop_onboarding_idf",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include "inkloop/onboarding/pairing_frame.hpp"

using namespace inkloop::onboarding;

class VersionEight final : public IQrMatrix {
 public:
  int side() const override { return 49; }
  bool module(int x, int y) const override {
    return (x < 7 && y < 7) || (x >= 42 && y < 7) ||
           (x < 7 && y >= 42) || ((x + y) % 5 == 0);
  }
};

uint8_t pixel(const std::array<uint8_t, 120000>& frame, int x, int y) {
  const size_t at = static_cast<size_t>(y) * 400 + x;
  return (at & 1U) == 0U ? frame[at >> 1U] >> 4U
                         : frame[at >> 1U] & 0x0fU;
}

int main() {
  assert(validMyAiPairingInputs("692639",
      "https://myai.mess.host/activate?code=692639"));
  for (const std::string& bad : {
      "", "69263", "69263x", "1234567"}) {
    assert(!validMyAiPairingInputs(
        bad, "https://myai.mess.host/activate"));
  }
  for (const std::string& bad : {
      "http://myai.mess.host/activate", "https://",
      "https://user@myai.mess.host/activate", "https://myai.mess.host/a b",
      "https://myai.mess.host/a\\b", "https://:443/activate",
      "https://.myai.mess.host/activate", "https://myai..mess.host/activate",
      "https://myai.mess.host:0/activate",
      "https://myai.mess.host:65536/activate"}) {
    assert(!validMyAiPairingInputs("692639", bad));
  }

  VersionEight qr;
  std::array<uint8_t, 120000> frame{};
  PairingFrameLayout layout;
  const PairingFrameSpec spec{};
  assert(renderPairingFrame4Bpp(
      spec, "692639", "https://myai.mess.host/activate?code=692639",
      qr, frame.data(), frame.size(), &layout) == PairingFrameResult::Ok);
  assert(layout.qr_x == 86);
  assert(layout.qr_y == 140);
  assert(layout.qr_pixels == 228);
  assert(layout.qr_scale == 4);
  assert(layout.code_x == 60);
  assert(layout.code_y == 404);
  assert(layout.digit_scale == 8);

  // White canvas and four-module quiet zone remain intact.
  assert(pixel(frame, 0, 0) == 1);
  assert(pixel(frame, layout.qr_x, layout.qr_y) == 1);
  assert(pixel(frame, layout.qr_x + 15, layout.qr_y + 15) == 1);
  assert(pixel(frame, layout.qr_x + 16, layout.qr_y + 16) == 0);
  assert(pixel(frame, 60, 404) == 1);  // top-left of digit six is blank
  assert(pixel(frame, 68, 404) == 0);  // first lit cell of digit six
  assert(pixel(frame, 339, 459) == 1); // safe area after the final digit
  for (const uint8_t value : frame) {
    assert((value >> 4U) <= 1U);
    assert((value & 0x0fU) <= 1U);
  }

  // Board-independent packed 4-bpp rendering accepts all native nibble values;
  // ED2208 legality remains enforced by its board sink.
  PairingFrameSpec future_spec{};
  future_spec.black_index = 4;
  future_spec.white_index = 7;
  assert(renderPairingFrame4Bpp(
      future_spec, "692639", "https://myai.mess.host/activate", qr,
      frame.data(), frame.size()) == PairingFrameResult::Ok);
  future_spec.black_index = 16;
  assert(renderPairingFrame4Bpp(
      future_spec, "692639", "https://myai.mess.host/activate", qr,
      frame.data(), frame.size()) == PairingFrameResult::InvalidFrame);

  PairingFrameSpec compact_spec{};
  compact_spec.width = 200;
  compact_spec.height = 200;
  std::array<uint8_t, 20000> compact_frame{};
  PairingFrameLayout compact_layout{};
  assert(renderPairingFrame4Bpp(
      compact_spec, "692639", "https://myai.mess.host/activate", qr,
      compact_frame.data(), compact_frame.size(), &compact_layout) ==
      PairingFrameResult::Ok);
  assert(compact_layout.qr_scale == 2);
  assert(compact_layout.qr_pixels == 114);
  assert(compact_layout.code_y + 7 * compact_layout.digit_scale <= 200);

  std::array<uint8_t, 119999> short_frame{};
  assert(renderPairingFrame4Bpp(
      spec, "692639", "https://myai.mess.host/activate", qr,
      short_frame.data(), short_frame.size()) ==
      PairingFrameResult::InvalidFrame);
  return 0;
}
`;

const officialQrHarness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include "inkloop/onboarding/pairing_frame.hpp"
#include "qrcodegen.h"

using namespace inkloop::onboarding;

class OfficialVersionEight final : public IQrMatrix {
 public:
  explicit OfficialVersionEight(const uint8_t* qr) : qr_(qr) {}
  int side() const override { return qrcodegen_getSize(qr_); }
  bool module(int x, int y) const override {
    return qrcodegen_getModule(qr_, x, y);
  }
 private:
  const uint8_t* qr_;
};

int main() {
  constexpr const char* url =
      "https://myai.mess.host/activate?code=692639";
  std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(8)> temporary{};
  std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(8)> qrcode{};
  assert(qrcodegen_encodeText(
      url, temporary.data(), qrcode.data(), qrcodegen_Ecc_MEDIUM,
      8, 8, qrcodegen_Mask_AUTO, true));
  OfficialVersionEight matrix(qrcode.data());
  assert(matrix.side() == 49);
  std::array<uint8_t, 120000> frame{};
  PairingFrameLayout layout{};
  assert(renderPairingFrame4Bpp(
      PairingFrameSpec{}, "692639", url, matrix, frame.data(), frame.size(),
      &layout) == PairingFrameResult::Ok);
  assert(layout.qr_x == 86 && layout.qr_y == 140);
  assert(layout.qr_pixels == 228 && layout.qr_scale == 4);
  assert(layout.code_x == 60 && layout.code_y == 404);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-onboarding-"));
  try {
    const source = join(scratch, "test.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(portable, "include"), source,
      join(portable, "pairing_frame.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

function buildAndRunOfficialQr() {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-onboarding-official-"));
  try {
    const source = join(scratch, "test.cpp");
    const qrcodeObject = join(scratch, "qrcodegen.o");
    const binary = join(scratch, "official");
    const qrcode = join(
      repo, "firmware/inkloop-idf/managed_components/espressif__qrcode",
    );
    writeFileSync(source, officialQrHarness);
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", qrcode, "-c", join(qrcode, "qrcodegen.c"), "-o", qrcodeObject,
    ], { stdio: "pipe" });
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(portable, "include"), "-I", qrcode, source,
      join(portable, "pairing_frame.cpp"), qrcodeObject, "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("pairing frame preserves the confirmed centered 400x600 layout", () => {
  buildAndRun(false);
});

test("pairing frame is memory-safe under ASan/UBSan", () => {
  buildAndRun(true);
});

test("official qrcode 0.2.0 produces the confirmed fixed version-8 layout", () => {
  buildAndRunOfficialQr();
});

test("ESP-IDF adapter fixes QR version and never calls the URL-logging wrapper", () => {
  const source = readFileSync(join(idf, "esp_pairing_frame.cpp"), "utf8");
  const manifest = readFileSync(join(idf, "idf_component.yml"), "utf8");
  assert.match(source, /qrcodegen_encodeText/);
  assert.match(source, /qrcodegen_Ecc_MEDIUM/);
  assert.match(source, /kConfirmedQrVersion, kConfirmedQrVersion/);
  assert.doesNotMatch(
    source, /^(?!\s*\/\/).*esp_qrcode_generate\s*\(|ESP_LOG[DIWVE]/m,
  );
  assert.match(manifest, /espressif\/qrcode:[\s\S]*==0\.2\.0/);
});
