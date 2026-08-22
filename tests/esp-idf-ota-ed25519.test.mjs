import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdtempSync, readFileSync, rmSync, writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_ota",
);

const sodiumHeader = String.raw`
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define crypto_sign_PUBLICKEYBYTES 32U
#define crypto_sign_BYTES 64U

int sodium_init(void);
int crypto_core_ed25519_is_valid_point(const unsigned char* point);
int crypto_sign_verify_detached(const unsigned char* signature,
                                const unsigned char* message,
                                unsigned long long message_length,
                                const unsigned char* public_key);
void sodium_memzero(void* bytes, size_t length);

#ifdef __cplusplus
}
#endif
`;

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <openssl/evp.h>

#include "inkloop/esp_ota_ed25519_verifier.hpp"

using namespace inkloop;

static int g_init_result = 0;
static bool g_force_invalid_point = false;
static std::size_t g_verify_calls = 0U;
static std::size_t g_message_scrubs = 0U;
static std::size_t g_key_scrubs = 0U;
static bool g_scrub_failure = false;
static std::vector<std::uint8_t> g_verified_message;

extern "C" int sodium_init(void) {
  return g_init_result;
}

extern "C" int crypto_core_ed25519_is_valid_point(
    const unsigned char* point) {
  if (!point || g_force_invalid_point) return 0;
  bool any_nonzero = false;
  bool all_equal = true;
  for (std::size_t at = 0U; at < 32U; ++at) {
    any_nonzero = any_nonzero || point[at] != 0U;
    all_equal = all_equal && point[at] == point[0];
  }
  if (!any_nonzero || all_equal) return 0;
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr, point, 32U);
  if (!key) return 0;
  EVP_PKEY_free(key);
  return 1;
}

extern "C" int crypto_sign_verify_detached(
    const unsigned char* signature, const unsigned char* message,
    unsigned long long message_length, const unsigned char* public_key) {
  ++g_verify_calls;
  g_verified_message.assign(
      message, message + static_cast<std::size_t>(message_length));
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr, public_key, 32U);
  if (!key) return -1;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context) {
    EVP_PKEY_free(key);
    return -1;
  }
  const int initialized = EVP_DigestVerifyInit(
      context, nullptr, nullptr, nullptr, key);
  const int verified = initialized == 1
      ? EVP_DigestVerify(context, signature, 64U, message,
                         static_cast<std::size_t>(message_length))
      : 0;
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  return verified == 1 ? 0 : -1;
}

extern "C" void sodium_memzero(void* bytes, std::size_t length) {
  auto* output = static_cast<volatile unsigned char*>(bytes);
  for (std::size_t at = 0U; at < length; ++at) output[at] = 0U;
  const auto* observed = static_cast<const unsigned char*>(bytes);
  for (std::size_t at = 0U; at < length; ++at)
    g_scrub_failure = g_scrub_failure || observed[at] != 0U;
  if (length == kMaximumCanonicalOtaManifestBytes +
                    OtaSha256Digest{}.size())
    ++g_message_scrubs;
  if (length == EspOtaEd25519Verifier::kPublicKeyBytes)
    ++g_key_scrubs;
}

static std::vector<std::uint8_t> fromHex(const std::string& input) {
  assert(input.size() % 2U == 0U);
  std::vector<std::uint8_t> output(input.size() / 2U);
  auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    assert(false);
    return 0U;
  };
  for (std::size_t at = 0U; at < output.size(); ++at)
    output[at] = static_cast<std::uint8_t>(
        (nibble(input[at * 2U]) << 4U) | nibble(input[at * 2U + 1U]));
  return output;
}

static OtaTextView policy() {
  return {kPinnedEd25519Sha256Policy,
          sizeof(kPinnedEd25519Sha256Policy) - 1U};
}

static OtaSha256Digest digestFixture() {
  OtaSha256Digest output{};
  for (std::size_t at = 0U; at < output.size(); ++at)
    output[at] = static_cast<std::uint8_t>(at);
  return output;
}

static CanonicalOtaManifest canonicalFixture() {
  constexpr char kBoard[] = "mock_minimal";
  constexpr char kVersion[] = "31.0.0";
  std::array<std::uint8_t, 64> placeholder_signature{};
  placeholder_signature.fill(0xA5U);
  ReviewedOtaManifest reviewed;
  reviewed.board_sku = {kBoard, sizeof(kBoard) - 1U};
  reviewed.firmware_version = {kVersion, sizeof(kVersion) - 1U};
  reviewed.image_size = 32U;
  reviewed.image_sha256 = digestFixture();
  reviewed.signature_policy = policy();
  reviewed.detached_signature = {
      placeholder_signature.data(), placeholder_signature.size()};
  PreparedOtaManifest prepared;
  assert(prepareOtaManifest(
             reviewed, {kBoard, sizeof(kBoard) - 1U}, prepared) ==
         OtaManifestCode::Ok);
  CanonicalOtaManifest output;
  assert(canonicalizeOtaManifest(prepared, output) == OtaManifestCode::Ok);
  assert(output.length == 118U);
  return output;
}

static const std::vector<std::uint8_t> kPublicKey = fromHex(
    "d75a980182b10ab7d54bfed3c964073a"
    "0ee172f3daa62325af021a68f707511a");
static const std::vector<std::uint8_t> kSignature = fromHex(
    "bfdf40edeb6eca88401faaa155448ea2"
    "20a5f93add4b68764f8be2c60cda3726"
    "a8febc262cc7a85618c341cb563ee1dd"
    "90543d0d04e4194d790e33f8f6c6870b");

static bool verify(EspOtaEd25519Verifier& verifier,
                   const CanonicalOtaManifest& canonical,
                   const OtaSha256Digest& digest,
                   const std::vector<std::uint8_t>& signature) {
  return verifier.verify(policy(), canonical, digest,
                         {signature.data(), signature.size()});
}

static void knownAnswerAndWrongInputMatrix() {
  assert(kPublicKey.size() == EspOtaEd25519Verifier::kPublicKeyBytes);
  assert(kSignature.size() ==
         EspOtaEd25519Verifier::kDetachedSignatureBytes);
  CanonicalOtaManifest canonical = canonicalFixture();
  OtaSha256Digest digest = digestFixture();

  std::vector<std::uint8_t> caller_key = kPublicKey;
  {
    EspOtaEd25519Verifier verifier(caller_key.data(), caller_key.size());
    assert(verifier.available());
    assert(verifier.supportsPolicy(policy()));
    std::fill(caller_key.begin(), caller_key.end(), 0U);
    assert(verify(verifier, canonical, digest, kSignature));
    std::vector<std::uint8_t> expected(
        canonical.bytes.begin(), canonical.bytes.begin() + canonical.length);
    expected.insert(expected.end(), digest.begin(), digest.end());
    assert(g_verified_message == expected);

    CanonicalOtaManifest wrong_manifest = canonical;
    wrong_manifest.bytes[3] ^= 0x01U;
    assert(!verify(verifier, wrong_manifest, digest, kSignature));

    OtaSha256Digest wrong_digest = digest;
    wrong_digest[17] ^= 0x80U;
    assert(!verify(verifier, canonical, wrong_digest, kSignature));

    std::vector<std::uint8_t> wrong_signature = kSignature;
    wrong_signature[7] ^= 0x04U;
    assert(!verify(verifier, canonical, digest, wrong_signature));

    // Add the Ed25519 group order L to the scalar S. The represented scalar
    // is congruent but non-canonical and therefore must not be accepted.
    constexpr std::array<std::uint8_t, 32> kGroupOrderLittleEndian{
        0xedU, 0xd3U, 0xf5U, 0x5cU, 0x1aU, 0x63U, 0x12U, 0x58U,
        0xd6U, 0x9cU, 0xf7U, 0xa2U, 0xdeU, 0xf9U, 0xdeU, 0x14U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U};
    std::vector<std::uint8_t> malleated = kSignature;
    unsigned int carry = 0U;
    for (std::size_t at = 0U; at < kGroupOrderLittleEndian.size(); ++at) {
      const unsigned int sum = static_cast<unsigned int>(malleated[32U + at]) +
          static_cast<unsigned int>(kGroupOrderLittleEndian[at]) + carry;
      malleated[32U + at] = static_cast<std::uint8_t>(sum & 0xFFU);
      carry = sum >> 8U;
    }
    assert(carry == 0U);
    assert(!verify(verifier, canonical, digest, malleated));

    const std::size_t calls_before_boundaries = g_verify_calls;
    assert(!verifier.verify(policy(), canonical, digest,
                            {kSignature.data(), 63U}));
    assert(!verifier.verify(policy(), canonical, digest,
                            {kSignature.data(), 65U}));
    assert(!verifier.verify(policy(), canonical, digest, {nullptr, 64U}));
    CanonicalOtaManifest empty = canonical;
    empty.length = 0U;
    assert(!verify(verifier, empty, digest, kSignature));
    CanonicalOtaManifest overflow = canonical;
    overflow.length = overflow.bytes.size() + 1U;
    assert(!verify(verifier, overflow, digest, kSignature));
    OtaSha256Digest empty_digest{};
    assert(!verify(verifier, canonical, empty_digest, kSignature));
    assert(g_verify_calls == calls_before_boundaries);

    const char wrong_policy[] = "inkloop-pinned-ed25519-sha256-v2";
    assert(!verifier.supportsPolicy(
        {wrong_policy, sizeof(wrong_policy) - 1U}));
    assert(!verifier.supportsPolicy(
        {kPinnedEd25519Sha256Policy, policy().length - 1U}));
    assert(!verifier.supportsPolicy({nullptr, policy().length}));
    assert(!verifier.verify(
        {wrong_policy, sizeof(wrong_policy) - 1U}, canonical, digest,
        {kSignature.data(), kSignature.size()}));
  }
  assert(g_key_scrubs >= 1U);
  assert(g_message_scrubs == g_verify_calls);
  assert(!g_scrub_failure);

  std::vector<std::uint8_t> wrong_key = kPublicKey;
  wrong_key[31] ^= 0x40U;
  EspOtaEd25519Verifier wrong_verifier(wrong_key.data(), wrong_key.size());
  assert(wrong_verifier.available());
  assert(!verify(wrong_verifier, canonical, digest, kSignature));
}

static void keyAndPlatformRejectionMatrix() {
  static_assert(!std::is_copy_constructible_v<EspOtaEd25519Verifier>);
  static_assert(!std::is_copy_assignable_v<EspOtaEd25519Verifier>);
  static_assert(!std::is_move_constructible_v<EspOtaEd25519Verifier>);
  CanonicalOtaManifest canonical = canonicalFixture();
  OtaSha256Digest digest = digestFixture();

  for (std::size_t length : {0U, 31U, 33U}) {
    EspOtaEd25519Verifier invalid(kPublicKey.data(), length);
    assert(!invalid.available());
    assert(!invalid.supportsPolicy(policy()));
    assert(!verify(invalid, canonical, digest, kSignature));
  }
  EspOtaEd25519Verifier absent(nullptr, 32U);
  assert(!absent.available());
  std::array<std::uint8_t, 32> zero{};
  EspOtaEd25519Verifier all_zero(zero.data(), zero.size());
  assert(!all_zero.available());
  std::array<std::uint8_t, 32> malformed{};
  malformed.fill(0x01U);
  EspOtaEd25519Verifier malformed_verifier(
      malformed.data(), malformed.size());
  assert(!malformed_verifier.available());

  g_force_invalid_point = true;
  EspOtaEd25519Verifier rejected_point(kPublicKey.data(), kPublicKey.size());
  g_force_invalid_point = false;
  assert(!rejected_point.available());

  g_init_result = -1;
  EspOtaEd25519Verifier unsupported(kPublicKey.data(), kPublicKey.size());
  g_init_result = 0;
  assert(!unsupported.available());
  assert(!verify(unsupported, canonical, digest, kSignature));
}

int main() {
  knownAnswerAndWrongInputMatrix();
  keyAndPlatformRejectionMatrix();
  assert(!g_scrub_failure);
  return 0;
}
`;

function opensslFlags() {
  return execFileSync("pkg-config", ["--cflags", "--libs", "openssl"], {
    encoding: "utf8",
  }).trim().split(/\s+/).filter(Boolean);
}

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-ed25519-"));
  try {
    writeFileSync(join(scratch, "sodium.h"), sodiumHeader);
    const source = join(scratch, "ota-ed25519.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", scratch, "-I", join(component, "include"), source,
      join(component, "esp_ota_ed25519_verifier.cpp"),
      join(component, "ota_sha256.cpp"),
      join(component, "ota_staging.cpp"),
      ...opensslFlags(), "-o", binary,
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

test("ESP OTA Ed25519 verifier passes deterministic known-answer matrix", () => {
  run(false);
});

test("ESP OTA Ed25519 verifier boundaries pass ASan/UBSan", () => {
  run(true);
});

test("ESP OTA verifier source is primitive-backed and fail-closed", () => {
  const source = readFileSync(
    join(component, "esp_ota_ed25519_verifier.cpp"), "utf8",
  );
  const header = readFileSync(join(
    component, "include/inkloop/esp_ota_ed25519_verifier.hpp",
  ), "utf8");
  const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");
  const manifest = readFileSync(
    join(component, "idf_component.yml"), "utf8",
  );

  assert.match(source, /#include <sodium\.h>/);
  assert.equal(source.match(/crypto_sign_verify_detached/g)?.length, 1);
  assert.equal(
    source.match(/crypto_core_ed25519_is_valid_point/g)?.length, 1,
  );
  assert.match(source, /sodium_init\(\) < 0/);
  assert.match(source, /sodium_memzero\(signed_message\.data\(\)/);
  assert.match(source, /detached_signature\.length != kDetachedSignatureBytes/);
  assert.match(header, /std::array<std::uint8_t, kPublicKeyBytes>/);
  assert.match(header, /const std::uint8_t\* public_key/);
  assert.doesNotMatch(
    source + header,
    /crypto_sign_(?:detached|keypair|seed_keypair)|private[_ -]?key|secret[_ -]?key|development[_ -]?(?:key|bypass)|unsigned[_ -]?mode|algorithm[_ -]?fallback/i,
  );
  assert.doesNotMatch(
    source + header,
    /ESP_LOG|printf|fprintf|fopen|nvs_|storage|https?:|bearer|credential|download/i,
  );
  assert.doesNotMatch(
    source,
    /fe_(?:add|sub|mul|sq)|ge_(?:add|sub|scalarmult)|curve25519|sc_reduce/i,
  );
  assert.match(cmake, /"esp_ota_ed25519_verifier\.cpp"/);
  assert.match(cmake, /REQUIRES app_update inkloop_runtime libsodium/);
  assert.match(manifest, /espressif\/libsodium:/);
  assert.match(manifest, /version: "==1\.0\.22"/);
  assert.doesNotMatch(manifest, />=|\^|~|\*/);
});
