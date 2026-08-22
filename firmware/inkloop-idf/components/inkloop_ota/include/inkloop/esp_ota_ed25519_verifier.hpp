#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/ota_staging.hpp"

namespace inkloop {

// Verifies the staging seam's canonical manifest and independently streamed
// image digest with a caller-provisioned Ed25519 public key. The verifier owns
// its bounded key copy and remains unavailable unless platform initialization
// and strict public-key validation both succeed.
class EspOtaEd25519Verifier final : public IPinnedOtaSignatureVerifier {
 public:
  static constexpr std::size_t kPublicKeyBytes = 32U;
  static constexpr std::size_t kDetachedSignatureBytes = 64U;

  EspOtaEd25519Verifier(const std::uint8_t* public_key,
                        std::size_t public_key_length);
  ~EspOtaEd25519Verifier() override;

  EspOtaEd25519Verifier(const EspOtaEd25519Verifier&) = delete;
  EspOtaEd25519Verifier& operator=(const EspOtaEd25519Verifier&) = delete;
  EspOtaEd25519Verifier(EspOtaEd25519Verifier&&) = delete;
  EspOtaEd25519Verifier& operator=(EspOtaEd25519Verifier&&) = delete;

  bool available() const { return key_ready_; }
  bool supportsPolicy(OtaTextView policy) const override;
  bool verify(OtaTextView policy,
              const CanonicalOtaManifest& canonical_manifest,
              const OtaSha256Digest& streamed_digest,
              OtaBytesView detached_signature) const override;

 private:
  std::array<std::uint8_t, kPublicKeyBytes> public_key_{};
  bool sodium_ready_ = false;
  bool key_ready_ = false;
};

}  // namespace inkloop
