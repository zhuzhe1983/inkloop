#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/ota_sha256.hpp"

namespace inkloop {

inline constexpr std::uint16_t kOtaManifestSchemaVersion = 1U;
inline constexpr std::size_t kMaximumOtaBoardSkuBytes = 32U;
inline constexpr std::size_t kMaximumOtaFirmwareVersionBytes = 64U;
inline constexpr std::size_t kMaximumOtaSignaturePolicyBytes = 48U;
inline constexpr std::size_t kMaximumOtaDetachedSignatureBytes = 512U;
inline constexpr std::size_t kMaximumCanonicalOtaManifestBytes = 256U;
inline constexpr std::uint64_t kMaximumOtaImageBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaximumOtaChunkBytes = 64U * 1024U;
inline constexpr char kPinnedEd25519Sha256Policy[] =
    "inkloop-pinned-ed25519-sha256-v1";

struct OtaBytesView {
  const std::uint8_t* data = nullptr;
  std::size_t length = 0U;
};

struct OtaTextView {
  const char* data = nullptr;
  std::size_t length = 0U;
};

struct ReviewedOtaManifest {
  std::uint16_t schema_version = kOtaManifestSchemaVersion;
  OtaTextView board_sku{};
  OtaTextView firmware_version{};
  std::uint64_t image_size = 0U;
  OtaSha256Digest image_sha256{};
  OtaTextView signature_policy{};
  OtaBytesView detached_signature{};
};

struct PreparedOtaManifest {
  std::uint16_t schema_version = 0U;
  std::array<char, kMaximumOtaBoardSkuBytes> board_sku{};
  std::size_t board_sku_length = 0U;
  std::array<char, kMaximumOtaFirmwareVersionBytes> firmware_version{};
  std::size_t firmware_version_length = 0U;
  std::uint64_t image_size = 0U;
  OtaSha256Digest image_sha256{};
  std::array<char, kMaximumOtaSignaturePolicyBytes> signature_policy{};
  std::size_t signature_policy_length = 0U;
  std::array<std::uint8_t,
             kMaximumOtaDetachedSignatureBytes> detached_signature{};
  std::size_t detached_signature_length = 0U;
};

struct CanonicalOtaManifest {
  std::array<std::uint8_t,
             kMaximumCanonicalOtaManifestBytes> bytes{};
  std::size_t length = 0U;
};

enum class OtaManifestCode : std::uint8_t {
  Ok,
  InvalidArgument,
  UnsupportedSchema,
  BoardMismatch,
  InvalidBoardSku,
  InvalidFirmwareVersion,
  InvalidImageSize,
  InvalidDigest,
  UnknownSignaturePolicy,
  InvalidSignature,
  CanonicalOverflow,
};

OtaManifestCode prepareOtaManifest(const ReviewedOtaManifest& input,
                                   OtaTextView device_board_sku,
                                   PreparedOtaManifest& output);
OtaManifestCode canonicalizeOtaManifest(
    const PreparedOtaManifest& manifest, CanonicalOtaManifest& output);

class IPinnedOtaSignatureVerifier {
 public:
  virtual ~IPinnedOtaSignatureVerifier() = default;
  virtual bool supportsPolicy(OtaTextView policy) const = 0;
  virtual bool verify(OtaTextView policy,
                      const CanonicalOtaManifest& canonical_manifest,
                      const OtaSha256Digest& streamed_digest,
                      OtaBytesView detached_signature) const = 0;
};

enum class OtaStagingState : std::uint8_t {
  Empty,
  Prepared,
  Streaming,
  ContentVerified,
  SignatureVerified,
  ImageComplete,
  BootSelected,
  Aborted,
  Failed,
};

enum class OtaStagingCode : std::uint8_t {
  Ok,
  InvalidState,
  InvalidArgument,
  ManifestRejected,
  ChunkTooLarge,
  ImageTooLarge,
  ImageIncomplete,
  DigestMismatch,
  VerifierUnavailable,
  SignaturePolicyUnsupported,
  SignatureRejected,
  HashFailure,
};

class OtaStagingCore final {
 public:
  OtaStagingCode prepare(const ReviewedOtaManifest& manifest,
                         OtaTextView device_board_sku);
  OtaStagingCode beginStream();
  OtaStagingCode acceptChunk(const std::uint8_t* bytes,
                             std::size_t length);
  OtaStagingCode finalizeContent();
  OtaStagingCode verifySignature(
      const IPinnedOtaSignatureVerifier* verifier);
  OtaStagingCode markImageComplete();
  OtaStagingCode markBootSelected();
  void abort();

  OtaStagingState state() const { return state_; }
  std::uint64_t bytesAccepted() const { return bytes_accepted_; }
  const PreparedOtaManifest& manifest() const { return manifest_; }
  const OtaSha256Digest& streamedDigest() const { return streamed_digest_; }
  OtaManifestCode manifestCode() const { return manifest_code_; }

 private:
  OtaStagingCode fail(OtaStagingCode code);

  OtaStagingState state_ = OtaStagingState::Empty;
  OtaManifestCode manifest_code_ = OtaManifestCode::InvalidArgument;
  PreparedOtaManifest manifest_{};
  OtaSha256 hash_{};
  OtaSha256Digest streamed_digest_{};
  std::uint64_t bytes_accepted_ = 0U;
};

const char* otaManifestCodeName(OtaManifestCode code);
const char* otaStagingStateName(OtaStagingState state);
const char* otaStagingCodeName(OtaStagingCode code);

}  // namespace inkloop
