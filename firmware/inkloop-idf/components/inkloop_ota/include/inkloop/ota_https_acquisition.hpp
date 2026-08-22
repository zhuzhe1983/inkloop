#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/esp_ota_staging.hpp"

namespace inkloop {

inline constexpr std::size_t kMaximumOtaUrlBytes = 512U;
inline constexpr std::size_t kMaximumOtaHostBytes = 253U;
inline constexpr std::size_t kMaximumOtaManifestDocumentBytes = 4096U;
inline constexpr std::size_t kMaximumOtaHttpsTransportChunkBytes = 4096U;
inline constexpr std::uint32_t kMaximumOtaAcquisitionDeadlineMs = 120000U;
inline constexpr std::size_t kEd25519DetachedSignatureBytes = 64U;

enum class OtaHttpsUrlCode : std::uint8_t {
  Ok,
  InvalidArgument,
  TooLong,
  NonHttps,
  CredentialsRejected,
  InvalidHost,
  InvalidPort,
  InvalidPath,
};

struct ParsedOtaHttpsUrl {
  std::array<char, kMaximumOtaHostBytes> host{};
  std::size_t host_length = 0U;
  std::uint16_t port = 443U;
};

OtaHttpsUrlCode parseOtaHttpsUrl(OtaTextView url,
                                 ParsedOtaHttpsUrl& output);
bool sameOtaHttpsOrigin(const ParsedOtaHttpsUrl& left,
                        const ParsedOtaHttpsUrl& right);

enum class OtaManifestParseCode : std::uint8_t {
  Ok,
  InvalidArgument,
  DocumentTooLarge,
  MalformedJson,
  DuplicateField,
  UnknownField,
  MissingField,
  InvalidSchema,
  InvalidBoard,
  InvalidVersion,
  TargetNotNewer,
  InvalidImageUrl,
  InvalidImageSize,
  InvalidDigest,
  InvalidSignaturePolicy,
  InvalidSignature,
  ManifestRejected,
};

struct AcquiredOtaManifest {
  std::uint16_t schema_version = 0U;
  std::array<char, kMaximumOtaBoardSkuBytes> board_sku{};
  std::size_t board_sku_length = 0U;
  std::array<char, kMaximumOtaFirmwareVersionBytes> firmware_version{};
  std::size_t firmware_version_length = 0U;
  std::array<char, kMaximumOtaUrlBytes> image_url{};
  std::size_t image_url_length = 0U;
  std::uint64_t image_size = 0U;
  OtaSha256Digest image_sha256{};
  std::array<char, kMaximumOtaSignaturePolicyBytes> signature_policy{};
  std::size_t signature_policy_length = 0U;
  std::array<std::uint8_t, kEd25519DetachedSignatureBytes>
      detached_signature{};

  ReviewedOtaManifest reviewed() const;
  OtaTextView imageUrl() const;
};

OtaManifestParseCode parseOtaManifestDocument(
    OtaBytesView document, OtaTextView device_board_sku,
    OtaTextView current_firmware_version, AcquiredOtaManifest& output);

class IOtaMonotonicClock {
 public:
  virtual ~IOtaMonotonicClock() = default;
  virtual std::uint64_t nowMs() const = 0;
};

enum class OtaHttpsFetchCode : std::uint8_t {
  Ok,
  InvalidRequest,
  DeadlineExceeded,
  UrlRejected,
  ClientUnavailable,
  ConnectionFailed,
  PeerRejected,
  RedirectRejected,
  HttpStatusRejected,
  ContentLengthRequired,
  ContentLengthMismatch,
  ResponseTooLarge,
  ReadFailed,
  Truncated,
  SinkRejected,
};

struct OtaHttpsFetchRequest {
  OtaTextView url{};
  std::uint64_t deadline_ms = 0U;
  std::uint64_t expected_content_length = 0U;
  std::uint64_t maximum_content_length = 0U;
  std::size_t maximum_chunk_bytes = 0U;
};

struct OtaHttpsFetchObservation {
  OtaHttpsFetchCode code = OtaHttpsFetchCode::Ok;
  int system_status = 0;
  int http_status = 0;
  std::uint64_t content_length = 0U;
  std::uint64_t bytes_received = 0U;
};

class IOtaHttpsBodySink {
 public:
  virtual ~IOtaHttpsBodySink() = default;
  virtual bool append(const std::uint8_t* bytes, std::size_t length) = 0;
};

class IOtaHttpsTransport {
 public:
  virtual ~IOtaHttpsTransport() = default;
  virtual OtaHttpsFetchObservation get(
      const OtaHttpsFetchRequest& request, IOtaHttpsBodySink& sink) = 0;
};

struct OtaHttpsAcquisitionConfig {
  OtaTextView manifest_url{};
  OtaTextView device_board_sku{};
  OtaTextView current_firmware_version{};
  std::uint32_t total_deadline_ms = 0U;
};

enum class OtaHttpsAcquisitionCode : std::uint8_t {
  Ok,
  InvalidState,
  InvalidConfiguration,
  DeadlineExceeded,
  ManifestFetchFailed,
  ManifestRejected,
  ImageOriginMismatch,
  StagingBeginFailed,
  ImageFetchFailed,
  StagingFinishFailed,
};

struct OtaHttpsAcquisitionObservation {
  OtaHttpsAcquisitionCode code = OtaHttpsAcquisitionCode::Ok;
  OtaHttpsFetchObservation fetch{};
  OtaManifestParseCode manifest_code = OtaManifestParseCode::Ok;
  EspOtaStagingObservation staging{};
  std::uint64_t deadline_ms = 0U;
};

class OtaHttpsAcquisition final {
 public:
  OtaHttpsAcquisition(IOtaMonotonicClock& clock,
                      IOtaHttpsTransport& transport,
                      EspOtaStagingAdapter& staging)
      : clock_(clock), transport_(transport), staging_(staging) {}

  OtaHttpsAcquisitionObservation run(
      const OtaHttpsAcquisitionConfig& config);

 private:
  IOtaMonotonicClock& clock_;
  IOtaHttpsTransport& transport_;
  EspOtaStagingAdapter& staging_;
  bool attempted_ = false;
};

const char* otaHttpsUrlCodeName(OtaHttpsUrlCode code);
const char* otaManifestParseCodeName(OtaManifestParseCode code);
const char* otaHttpsFetchCodeName(OtaHttpsFetchCode code);
const char* otaHttpsAcquisitionCodeName(OtaHttpsAcquisitionCode code);

}  // namespace inkloop
