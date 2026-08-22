#include "inkloop/esp_ota_ed25519_verifier.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <sodium.h>

namespace inkloop {
namespace {

constexpr std::size_t kPolicyLength =
    sizeof(kPinnedEd25519Sha256Policy) - 1U;
constexpr std::size_t kMaximumSignedMessageBytes =
    kMaximumCanonicalOtaManifestBytes + OtaSha256Digest{}.size();

bool bytesPresent(const std::uint8_t* bytes, std::size_t length) {
  return bytes &&
      std::any_of(bytes, bytes + length,
                  [](std::uint8_t value) { return value != 0U; });
}

}  // namespace

static_assert(crypto_sign_PUBLICKEYBYTES ==
              EspOtaEd25519Verifier::kPublicKeyBytes);
static_assert(crypto_sign_BYTES ==
              EspOtaEd25519Verifier::kDetachedSignatureBytes);

EspOtaEd25519Verifier::EspOtaEd25519Verifier(
    const std::uint8_t* public_key, std::size_t public_key_length) {
  if (public_key_length != public_key_.size() ||
      !bytesPresent(public_key, public_key_length) || sodium_init() < 0)
    return;

  sodium_ready_ = true;
  std::copy_n(public_key, public_key_.size(), public_key_.begin());
  if (crypto_core_ed25519_is_valid_point(public_key_.data()) != 1) {
    sodium_memzero(public_key_.data(), public_key_.size());
    return;
  }
  key_ready_ = true;
}

EspOtaEd25519Verifier::~EspOtaEd25519Verifier() {
  key_ready_ = false;
  if (sodium_ready_)
    sodium_memzero(public_key_.data(), public_key_.size());
  else
    std::fill(public_key_.begin(), public_key_.end(), 0U);
  sodium_ready_ = false;
}

bool EspOtaEd25519Verifier::supportsPolicy(OtaTextView policy) const {
  return key_ready_ && policy.data && policy.length == kPolicyLength &&
      std::memcmp(policy.data, kPinnedEd25519Sha256Policy,
                  kPolicyLength) == 0;
}

bool EspOtaEd25519Verifier::verify(
    OtaTextView policy, const CanonicalOtaManifest& canonical_manifest,
    const OtaSha256Digest& streamed_digest,
    OtaBytesView detached_signature) const {
  if (!supportsPolicy(policy) || canonical_manifest.length == 0U ||
      canonical_manifest.length > canonical_manifest.bytes.size() ||
      !bytesPresent(streamed_digest.data(), streamed_digest.size()) ||
      !detached_signature.data ||
      detached_signature.length != kDetachedSignatureBytes)
    return false;

  std::array<std::uint8_t, kMaximumSignedMessageBytes> signed_message{};
  std::copy_n(canonical_manifest.bytes.data(), canonical_manifest.length,
              signed_message.begin());
  std::copy(streamed_digest.begin(), streamed_digest.end(),
            signed_message.begin() + canonical_manifest.length);
  const std::size_t signed_message_length =
      canonical_manifest.length + streamed_digest.size();
  const bool verified = crypto_sign_verify_detached(
      detached_signature.data, signed_message.data(), signed_message_length,
      public_key_.data()) == 0;
  sodium_memzero(signed_message.data(), signed_message.size());
  return verified;
}

}  // namespace inkloop
