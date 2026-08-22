#include "inkloop/ota_staging.hpp"

#include <algorithm>
#include <cstring>

namespace inkloop {
namespace {

constexpr char kCanonicalDomain[] = "INKLOOP-OTA-MANIFEST-V1";

bool viewValid(OtaTextView view, std::size_t maximum, bool version) {
  if (!view.data || view.length == 0U || view.length > maximum) return false;
  for (std::size_t at = 0U; at < view.length; ++at) {
    const char value = view.data[at];
    const bool alphanumeric =
        (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9');
    if (!alphanumeric && value != '-' && value != '_' && value != '.' &&
        (!version || value != '+'))
      return false;
  }
  return true;
}

bool viewEqual(OtaTextView left, OtaTextView right) {
  return left.length == right.length && left.data && right.data &&
      std::memcmp(left.data, right.data, left.length) == 0;
}

bool digestPresent(const OtaSha256Digest& digest) {
  return std::any_of(digest.begin(), digest.end(),
                     [](std::uint8_t value) { return value != 0U; });
}

OtaTextView policyView(const PreparedOtaManifest& manifest) {
  return {manifest.signature_policy.data(),
          manifest.signature_policy_length};
}

bool policyKnown(OtaTextView policy) {
  constexpr std::size_t kPolicyLength =
      sizeof(kPinnedEd25519Sha256Policy) - 1U;
  return policy.length == kPolicyLength && policy.data &&
      std::memcmp(policy.data, kPinnedEd25519Sha256Policy,
                  kPolicyLength) == 0;
}

class CanonicalWriter final {
 public:
  explicit CanonicalWriter(CanonicalOtaManifest& output) : output_(output) {
    output_ = CanonicalOtaManifest{};
  }

  bool bytes(const void* data, std::size_t length) {
    if ((!data && length != 0U) ||
        length > output_.bytes.size() - output_.length)
      return false;
    if (length != 0U)
      std::memcpy(output_.bytes.data() + output_.length, data, length);
    output_.length += length;
    return true;
  }

  bool u8(std::uint8_t value) { return bytes(&value, sizeof(value)); }

  bool u16(std::uint16_t value) {
    const std::uint8_t encoded[] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8U)};
    return bytes(encoded, sizeof(encoded));
  }

  bool u64(std::uint64_t value) {
    std::uint8_t encoded[8]{};
    for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
      encoded[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
    return bytes(encoded, sizeof(encoded));
  }

  bool text(const char* data, std::size_t length) {
    return length <= 0xFFU && u8(static_cast<std::uint8_t>(length)) &&
        bytes(data, length);
  }

 private:
  CanonicalOtaManifest& output_;
};

}  // namespace

OtaManifestCode prepareOtaManifest(const ReviewedOtaManifest& input,
                                   OtaTextView device_board_sku,
                                   PreparedOtaManifest& output) {
  output = PreparedOtaManifest{};
  if (input.schema_version != kOtaManifestSchemaVersion)
    return OtaManifestCode::UnsupportedSchema;
  if (!viewValid(device_board_sku, kMaximumOtaBoardSkuBytes, false) ||
      !viewValid(input.board_sku, kMaximumOtaBoardSkuBytes, false))
    return OtaManifestCode::InvalidBoardSku;
  if (!viewEqual(input.board_sku, device_board_sku))
    return OtaManifestCode::BoardMismatch;
  if (!viewValid(input.firmware_version,
                 kMaximumOtaFirmwareVersionBytes, true))
    return OtaManifestCode::InvalidFirmwareVersion;
  if (input.image_size == 0U || input.image_size > kMaximumOtaImageBytes)
    return OtaManifestCode::InvalidImageSize;
  if (!digestPresent(input.image_sha256))
    return OtaManifestCode::InvalidDigest;
  if (!viewValid(input.signature_policy,
                 kMaximumOtaSignaturePolicyBytes, false) ||
      !policyKnown(input.signature_policy))
    return OtaManifestCode::UnknownSignaturePolicy;
  if (!input.detached_signature.data ||
      input.detached_signature.length == 0U ||
      input.detached_signature.length > kMaximumOtaDetachedSignatureBytes)
    return OtaManifestCode::InvalidSignature;

  output.schema_version = input.schema_version;
  output.board_sku_length = input.board_sku.length;
  std::copy_n(input.board_sku.data, input.board_sku.length,
              output.board_sku.begin());
  output.firmware_version_length = input.firmware_version.length;
  std::copy_n(input.firmware_version.data, input.firmware_version.length,
              output.firmware_version.begin());
  output.image_size = input.image_size;
  output.image_sha256 = input.image_sha256;
  output.signature_policy_length = input.signature_policy.length;
  std::copy_n(input.signature_policy.data, input.signature_policy.length,
              output.signature_policy.begin());
  output.detached_signature_length = input.detached_signature.length;
  std::copy_n(input.detached_signature.data, input.detached_signature.length,
              output.detached_signature.begin());
  return OtaManifestCode::Ok;
}

OtaManifestCode canonicalizeOtaManifest(
    const PreparedOtaManifest& manifest, CanonicalOtaManifest& output) {
  output = CanonicalOtaManifest{};
  if (manifest.schema_version != kOtaManifestSchemaVersion)
    return OtaManifestCode::UnsupportedSchema;
  const OtaTextView board{manifest.board_sku.data(),
                          manifest.board_sku_length};
  const OtaTextView version{manifest.firmware_version.data(),
                            manifest.firmware_version_length};
  const OtaTextView policy = policyView(manifest);
  if (!viewValid(board, kMaximumOtaBoardSkuBytes, false))
    return OtaManifestCode::InvalidBoardSku;
  if (!viewValid(version, kMaximumOtaFirmwareVersionBytes, true))
    return OtaManifestCode::InvalidFirmwareVersion;
  if (manifest.image_size == 0U ||
      manifest.image_size > kMaximumOtaImageBytes)
    return OtaManifestCode::InvalidImageSize;
  if (!digestPresent(manifest.image_sha256))
    return OtaManifestCode::InvalidDigest;
  if (!viewValid(policy, kMaximumOtaSignaturePolicyBytes, false) ||
      !policyKnown(policy))
    return OtaManifestCode::UnknownSignaturePolicy;
  if (manifest.detached_signature_length == 0U ||
      manifest.detached_signature_length >
          kMaximumOtaDetachedSignatureBytes)
    return OtaManifestCode::InvalidSignature;

  CanonicalWriter writer(output);
  if (!writer.bytes(kCanonicalDomain, sizeof(kCanonicalDomain) - 1U) ||
      !writer.u16(manifest.schema_version) ||
      !writer.text(board.data, board.length) ||
      !writer.text(version.data, version.length) ||
      !writer.u64(manifest.image_size) ||
      !writer.bytes(manifest.image_sha256.data(),
                    manifest.image_sha256.size()) ||
      !writer.text(policy.data, policy.length))
    return OtaManifestCode::CanonicalOverflow;
  return OtaManifestCode::Ok;
}

OtaStagingCode OtaStagingCore::fail(OtaStagingCode code) {
  state_ = OtaStagingState::Failed;
  return code;
}

OtaStagingCode OtaStagingCore::prepare(
    const ReviewedOtaManifest& manifest, OtaTextView device_board_sku) {
  if (state_ != OtaStagingState::Empty)
    return OtaStagingCode::InvalidState;
  manifest_code_ = prepareOtaManifest(manifest, device_board_sku, manifest_);
  if (manifest_code_ != OtaManifestCode::Ok)
    return fail(OtaStagingCode::ManifestRejected);
  state_ = OtaStagingState::Prepared;
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::beginStream() {
  if (state_ != OtaStagingState::Prepared)
    return OtaStagingCode::InvalidState;
  state_ = OtaStagingState::Streaming;
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::acceptChunk(const std::uint8_t* bytes,
                                           std::size_t length) {
  if (state_ != OtaStagingState::Streaming)
    return OtaStagingCode::InvalidState;
  if (!bytes || length == 0U) return fail(OtaStagingCode::InvalidArgument);
  if (length > kMaximumOtaChunkBytes)
    return fail(OtaStagingCode::ChunkTooLarge);
  if (length > manifest_.image_size - bytes_accepted_)
    return fail(OtaStagingCode::ImageTooLarge);
  if (!hash_.update(bytes, length)) return fail(OtaStagingCode::HashFailure);
  bytes_accepted_ += static_cast<std::uint64_t>(length);
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::finalizeContent() {
  if (state_ != OtaStagingState::Streaming)
    return OtaStagingCode::InvalidState;
  if (bytes_accepted_ != manifest_.image_size)
    return fail(OtaStagingCode::ImageIncomplete);
  if (!hash_.finish(streamed_digest_))
    return fail(OtaStagingCode::HashFailure);
  if (streamed_digest_ != manifest_.image_sha256)
    return fail(OtaStagingCode::DigestMismatch);
  state_ = OtaStagingState::ContentVerified;
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::verifySignature(
    const IPinnedOtaSignatureVerifier* verifier) {
  if (state_ != OtaStagingState::ContentVerified)
    return OtaStagingCode::InvalidState;
  if (!verifier) return fail(OtaStagingCode::VerifierUnavailable);
  const OtaTextView policy = policyView(manifest_);
  if (!verifier->supportsPolicy(policy))
    return fail(OtaStagingCode::SignaturePolicyUnsupported);
  CanonicalOtaManifest canonical;
  if (canonicalizeOtaManifest(manifest_, canonical) != OtaManifestCode::Ok)
    return fail(OtaStagingCode::ManifestRejected);
  const OtaBytesView signature{manifest_.detached_signature.data(),
                               manifest_.detached_signature_length};
  if (!verifier->verify(policy, canonical, streamed_digest_, signature))
    return fail(OtaStagingCode::SignatureRejected);
  state_ = OtaStagingState::SignatureVerified;
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::markImageComplete() {
  if (state_ != OtaStagingState::SignatureVerified)
    return OtaStagingCode::InvalidState;
  state_ = OtaStagingState::ImageComplete;
  return OtaStagingCode::Ok;
}

OtaStagingCode OtaStagingCore::markBootSelected() {
  if (state_ != OtaStagingState::ImageComplete)
    return OtaStagingCode::InvalidState;
  state_ = OtaStagingState::BootSelected;
  return OtaStagingCode::Ok;
}

void OtaStagingCore::abort() {
  if (state_ != OtaStagingState::BootSelected)
    state_ = OtaStagingState::Aborted;
}

const char* otaManifestCodeName(OtaManifestCode code) {
  switch (code) {
    case OtaManifestCode::Ok: return "OK";
    case OtaManifestCode::InvalidArgument: return "INVALID_ARGUMENT";
    case OtaManifestCode::UnsupportedSchema: return "UNSUPPORTED_SCHEMA";
    case OtaManifestCode::BoardMismatch: return "BOARD_MISMATCH";
    case OtaManifestCode::InvalidBoardSku: return "INVALID_BOARD_SKU";
    case OtaManifestCode::InvalidFirmwareVersion:
      return "INVALID_FIRMWARE_VERSION";
    case OtaManifestCode::InvalidImageSize: return "INVALID_IMAGE_SIZE";
    case OtaManifestCode::InvalidDigest: return "INVALID_DIGEST";
    case OtaManifestCode::UnknownSignaturePolicy:
      return "UNKNOWN_SIGNATURE_POLICY";
    case OtaManifestCode::InvalidSignature: return "INVALID_SIGNATURE";
    case OtaManifestCode::CanonicalOverflow: return "CANONICAL_OVERFLOW";
  }
  return "UNKNOWN";
}

const char* otaStagingStateName(OtaStagingState state) {
  switch (state) {
    case OtaStagingState::Empty: return "EMPTY";
    case OtaStagingState::Prepared: return "PREPARED";
    case OtaStagingState::Streaming: return "STREAMING";
    case OtaStagingState::ContentVerified: return "CONTENT_VERIFIED";
    case OtaStagingState::SignatureVerified: return "SIGNATURE_VERIFIED";
    case OtaStagingState::ImageComplete: return "IMAGE_COMPLETE";
    case OtaStagingState::BootSelected: return "BOOT_SELECTED";
    case OtaStagingState::Aborted: return "ABORTED";
    case OtaStagingState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

const char* otaStagingCodeName(OtaStagingCode code) {
  switch (code) {
    case OtaStagingCode::Ok: return "OK";
    case OtaStagingCode::InvalidState: return "INVALID_STATE";
    case OtaStagingCode::InvalidArgument: return "INVALID_ARGUMENT";
    case OtaStagingCode::ManifestRejected: return "MANIFEST_REJECTED";
    case OtaStagingCode::ChunkTooLarge: return "CHUNK_TOO_LARGE";
    case OtaStagingCode::ImageTooLarge: return "IMAGE_TOO_LARGE";
    case OtaStagingCode::ImageIncomplete: return "IMAGE_INCOMPLETE";
    case OtaStagingCode::DigestMismatch: return "DIGEST_MISMATCH";
    case OtaStagingCode::VerifierUnavailable:
      return "VERIFIER_UNAVAILABLE";
    case OtaStagingCode::SignaturePolicyUnsupported:
      return "SIGNATURE_POLICY_UNSUPPORTED";
    case OtaStagingCode::SignatureRejected: return "SIGNATURE_REJECTED";
    case OtaStagingCode::HashFailure: return "HASH_FAILURE";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
