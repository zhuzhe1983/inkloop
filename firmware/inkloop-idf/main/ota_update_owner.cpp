#include "ota_update_owner.hpp"

#include <algorithm>
#include <cstring>

#ifdef ESP_PLATFORM
#include "inkloop/esp_ota_ed25519_verifier.hpp"
#include "inkloop/esp_ota_https_transport.hpp"
#include "inkloop/esp_ota_staging.hpp"
#include "sdkconfig.h"
#endif

namespace inkloop {
namespace {

constexpr std::uint32_t kStateMask = 0xFFU;
constexpr std::uint32_t kCodeMask = 0xFFU;
constexpr unsigned kCodeShift = 8U;

bool textMissing(OtaTextView value) {
  return !value.data || value.length == 0U;
}

bool lowerHexNibble(char value, std::uint8_t& output) {
  if (value >= '0' && value <= '9') {
    output = static_cast<std::uint8_t>(value - '0');
    return true;
  }
  if (value >= 'a' && value <= 'f') {
    output = static_cast<std::uint8_t>(value - 'a' + 10U);
    return true;
  }
  return false;
}

bool decodePublicKey(OtaTextView input,
                     std::array<std::uint8_t, 32U>& output) {
  output.fill(0U);
  if (!input.data || input.length != output.size() * 2U) return false;
  bool nonzero = false;
  for (std::size_t at = 0U; at < output.size(); ++at) {
    std::uint8_t high = 0U;
    std::uint8_t low = 0U;
    if (!lowerHexNibble(input.data[at * 2U], high) ||
        !lowerHexNibble(input.data[at * 2U + 1U], low)) {
      output.fill(0U);
      return false;
    }
    output[at] = static_cast<std::uint8_t>((high << 4U) | low);
    nonzero = nonzero || output[at] != 0U;
  }
  if (!nonzero) output.fill(0U);
  return nonzero;
}

bool hostSuffix(const ParsedOtaHttpsUrl& endpoint, const char* suffix) {
  const std::size_t suffix_length = std::strlen(suffix);
  if (suffix_length > endpoint.host_length) return false;
  const std::size_t offset = endpoint.host_length - suffix_length;
  if (std::memcmp(endpoint.host.data() + offset, suffix, suffix_length) != 0)
    return false;
  return offset == 0U || endpoint.host[offset - 1U] == '.';
}

bool hostLabel(const ParsedOtaHttpsUrl& endpoint, const char* label) {
  const std::size_t label_length = std::strlen(label);
  if (label_length == 0U || label_length > endpoint.host_length) return false;
  for (std::size_t at = 0U;
       at + label_length <= endpoint.host_length; ++at) {
    const bool left_boundary = at == 0U || endpoint.host[at - 1U] == '.';
    const bool right_boundary = at + label_length == endpoint.host_length ||
        endpoint.host[at + label_length] == '.';
    if (left_boundary && right_boundary &&
        std::memcmp(endpoint.host.data() + at, label, label_length) == 0)
      return true;
  }
  return false;
}

bool placeholderEndpoint(const ParsedOtaHttpsUrl& endpoint) {
  return hostSuffix(endpoint, "example.com") ||
      hostSuffix(endpoint, "example.net") ||
      hostSuffix(endpoint, "example.org") ||
      hostSuffix(endpoint, "invalid") || hostSuffix(endpoint, "test") ||
      hostSuffix(endpoint, "local") || hostLabel(endpoint, "placeholder") ||
      hostLabel(endpoint, "changeme") || hostLabel(endpoint, "change-me");
}

}  // namespace

OtaUpdateOwner::AtomicStatus OtaUpdateOwner::encode(
    OtaUpdateState state, OtaUpdateCode code, std::uint64_t request_id) {
  AtomicStatus output;
  output.request_id = request_id;
  output.state_and_code = static_cast<std::uint32_t>(state) |
      (static_cast<std::uint32_t>(code) << kCodeShift);
  return output;
}

OtaUpdateSnapshot OtaUpdateOwner::decode(const AtomicStatus& word) {
  OtaUpdateSnapshot output;
  output.state = static_cast<OtaUpdateState>(
      word.state_and_code & kStateMask);
  output.code = static_cast<OtaUpdateCode>(
      (word.state_and_code >> kCodeShift) & kCodeMask);
  output.request_id = word.request_id;
  return output;
}

OtaUpdateCode OtaUpdateOwner::prepareConfiguration(
    const OtaUpdateRawConfiguration& input,
    PreparedConfiguration& output) {
  output = PreparedConfiguration{};
  if (textMissing(input.manifest_url) || textMissing(input.public_key_hex))
    return OtaUpdateCode::ConfigurationMissing;
  if (input.total_deadline_ms == 0U ||
      input.total_deadline_ms > kMaximumOtaAcquisitionDeadlineMs)
    return OtaUpdateCode::DeadlineRejected;
  ParsedOtaHttpsUrl endpoint;
  if (parseOtaHttpsUrl(input.manifest_url, endpoint) != OtaHttpsUrlCode::Ok)
    return OtaUpdateCode::ManifestUrlRejected;
  if (placeholderEndpoint(endpoint))
    return OtaUpdateCode::PlaceholderEndpointRejected;
  if (!decodePublicKey(input.public_key_hex, output.public_key))
    return OtaUpdateCode::PublicKeyRejected;
  std::memcpy(output.manifest_url.data(), input.manifest_url.data,
              input.manifest_url.length);
  output.manifest_url_length = input.manifest_url.length;
  output.total_deadline_ms = input.total_deadline_ms;
  output.ready = true;
  return OtaUpdateCode::Ready;
}

OtaUpdateOwner::OtaUpdateOwner(
    const OtaUpdateRawConfiguration& configuration) {
  const OtaUpdateCode prepared =
      prepareConfiguration(configuration, configuration_);
  const OtaUpdateState initial = configuration_.ready
      ? OtaUpdateState::Idle : OtaUpdateState::Disabled;
  status_.store(encode(initial, prepared, 0U), std::memory_order_release);
}

OtaUpdateSnapshot OtaUpdateOwner::snapshot() const {
  return decode(status_.load(std::memory_order_acquire));
}

OtaUpdateCode OtaUpdateOwner::request(std::uint64_t request_id) {
  if (request_id == 0U) return OtaUpdateCode::InvalidRequestId;
  AtomicStatus current = status_.load(std::memory_order_acquire);
  OtaUpdateSnapshot observed = decode(current);
  if (observed.state == OtaUpdateState::Disabled)
    return OtaUpdateCode::Disabled;
  if (observed.state != OtaUpdateState::Idle) {
    return observed.request_id == request_id
        ? OtaUpdateCode::DuplicateRequest : OtaUpdateCode::Busy;
  }
  const AtomicStatus requested = encode(
      OtaUpdateState::Requested, OtaUpdateCode::Ok, request_id);
  if (status_.compare_exchange_strong(
          current, requested, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return OtaUpdateCode::Ok;
  observed = decode(current);
  return observed.request_id == request_id
      ? OtaUpdateCode::DuplicateRequest : OtaUpdateCode::Busy;
}

OtaUpdateCode OtaUpdateOwner::take(OtaUpdateRequest& request) {
  request = OtaUpdateRequest{};
  AtomicStatus current = status_.load(std::memory_order_acquire);
  const OtaUpdateSnapshot observed = decode(current);
  if (observed.state == OtaUpdateState::Disabled)
    return OtaUpdateCode::Disabled;
  if (observed.state != OtaUpdateState::Requested)
    return OtaUpdateCode::NoRequest;
  const AtomicStatus running = encode(
      OtaUpdateState::Running, OtaUpdateCode::Ok, observed.request_id);
  if (!status_.compare_exchange_strong(
          current, running, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return OtaUpdateCode::NoRequest;
  request.request_id = observed.request_id;
  return OtaUpdateCode::Ok;
}

OtaUpdateCode OtaUpdateOwner::mapAcquisition(
    const OtaHttpsAcquisitionObservation& observation) {
  switch (observation.code) {
    case OtaHttpsAcquisitionCode::Ok:
      return OtaUpdateCode::ImageSelected;
    case OtaHttpsAcquisitionCode::InvalidState:
      return OtaUpdateCode::AcquisitionInvalidState;
    case OtaHttpsAcquisitionCode::InvalidConfiguration:
      return OtaUpdateCode::AcquisitionConfigurationRejected;
    case OtaHttpsAcquisitionCode::DeadlineExceeded:
      return OtaUpdateCode::DeadlineExceeded;
    case OtaHttpsAcquisitionCode::ManifestFetchFailed:
      return OtaUpdateCode::ManifestFetchFailed;
    case OtaHttpsAcquisitionCode::ManifestRejected:
      return OtaUpdateCode::ManifestRejected;
    case OtaHttpsAcquisitionCode::ImageOriginMismatch:
      return OtaUpdateCode::ImageOriginMismatch;
    case OtaHttpsAcquisitionCode::StagingBeginFailed:
      return OtaUpdateCode::StagingBeginFailed;
    case OtaHttpsAcquisitionCode::ImageFetchFailed:
      return OtaUpdateCode::ImageFetchFailed;
    case OtaHttpsAcquisitionCode::StagingFinishFailed:
      return OtaUpdateCode::StagingFinishFailed;
  }
  return OtaUpdateCode::AcquisitionInvalidState;
}

bool OtaUpdateOwner::validFailureCode(OtaUpdateCode code) {
  switch (code) {
    case OtaUpdateCode::QuiesceFailed:
    case OtaUpdateCode::PlatformUnavailable:
    case OtaUpdateCode::VerifierUnavailable:
    case OtaUpdateCode::AcquisitionInvalidState:
    case OtaUpdateCode::AcquisitionConfigurationRejected:
    case OtaUpdateCode::DeadlineExceeded:
    case OtaUpdateCode::ManifestFetchFailed:
    case OtaUpdateCode::ManifestRejected:
    case OtaUpdateCode::ImageOriginMismatch:
    case OtaUpdateCode::StagingBeginFailed:
    case OtaUpdateCode::ImageFetchFailed:
    case OtaUpdateCode::StagingFinishFailed:
      return true;
    case OtaUpdateCode::Ok:
    case OtaUpdateCode::Ready:
    case OtaUpdateCode::ConfigurationMissing:
    case OtaUpdateCode::ManifestUrlRejected:
    case OtaUpdateCode::PlaceholderEndpointRejected:
    case OtaUpdateCode::PublicKeyRejected:
    case OtaUpdateCode::DeadlineRejected:
    case OtaUpdateCode::Disabled:
    case OtaUpdateCode::InvalidRequestId:
    case OtaUpdateCode::DuplicateRequest:
    case OtaUpdateCode::Busy:
    case OtaUpdateCode::NoRequest:
    case OtaUpdateCode::RequestMismatch:
    case OtaUpdateCode::InvalidTerminalCode:
    case OtaUpdateCode::ImageSelected:
      return false;
  }
  return false;
}

OtaUpdateCode OtaUpdateOwner::finishTerminal(
    const OtaUpdateRequest& request, OtaUpdateState expected_state,
    OtaUpdateCode code) {
  AtomicStatus current = status_.load(std::memory_order_acquire);
  const OtaUpdateSnapshot observed = decode(current);
  if (request.request_id == 0U ||
      observed.request_id != request.request_id)
    return OtaUpdateCode::RequestMismatch;
  if (observed.state != expected_state)
    return OtaUpdateCode::NoRequest;
  const OtaUpdateState terminal = code == OtaUpdateCode::ImageSelected
      ? OtaUpdateState::ImageSelected : OtaUpdateState::Failed;
  const AtomicStatus finished = encode(terminal, code, request.request_id);
  if (!status_.compare_exchange_strong(
          current, finished, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return OtaUpdateCode::NoRequest;
  return code;
}

OtaUpdateCode OtaUpdateOwner::fail(
    const OtaUpdateRequest& request, OtaUpdateCode code) {
  if (!validFailureCode(code)) return OtaUpdateCode::InvalidTerminalCode;
  return finishTerminal(request, OtaUpdateState::Running, code);
}

OtaUpdateCode OtaUpdateOwner::acquire(
    const OtaUpdateRequest& request, OtaTextView device_board_sku,
    OtaTextView current_firmware_version) {
  AtomicStatus current = status_.load(std::memory_order_acquire);
  const OtaUpdateSnapshot observed = decode(current);
  if (request.request_id == 0U ||
      observed.request_id != request.request_id)
    return OtaUpdateCode::RequestMismatch;
  if (observed.state != OtaUpdateState::Running)
    return OtaUpdateCode::NoRequest;
  const AtomicStatus acquiring = encode(
      OtaUpdateState::Acquiring, OtaUpdateCode::Ok, request.request_id);
  if (!status_.compare_exchange_strong(
          current, acquiring, std::memory_order_acq_rel,
          std::memory_order_acquire))
    return OtaUpdateCode::NoRequest;
  if (textMissing(device_board_sku) ||
      device_board_sku.length > kMaximumOtaBoardSkuBytes ||
      textMissing(current_firmware_version) ||
      current_firmware_version.length > kMaximumOtaFirmwareVersionBytes) {
    return finishTerminal(
        request, OtaUpdateState::Acquiring,
        OtaUpdateCode::AcquisitionConfigurationRejected);
  }
#ifdef ESP_PLATFORM
  EspOtaEd25519Verifier verifier(configuration_.public_key.data(),
                                 configuration_.public_key.size());
  if (!verifier.available())
    return finishTerminal(request, OtaUpdateState::Acquiring,
                          OtaUpdateCode::VerifierUnavailable);
  EspOtaStagingAdapter staging(systemEspOtaWriterFunctions(), &verifier);
  EspOtaMonotonicClock clock;
  EspOtaHttpsTransport transport;
  OtaHttpsAcquisition acquisition(clock, transport, staging);
  const OtaHttpsAcquisitionConfig acquisition_configuration{
      {configuration_.manifest_url.data(),
       configuration_.manifest_url_length},
      device_board_sku,
      current_firmware_version,
      configuration_.total_deadline_ms};
  return finishTerminal(
      request, OtaUpdateState::Acquiring,
      mapAcquisition(acquisition.run(acquisition_configuration)));
#else
  return finishTerminal(request, OtaUpdateState::Acquiring,
                        OtaUpdateCode::PlatformUnavailable);
#endif
}

OtaUpdateOwner& systemOtaUpdateOwner() {
#ifdef ESP_PLATFORM
  static constexpr char kManifestUrl[] = CONFIG_INKLOOP_OTA_MANIFEST_URL;
  static constexpr char kPublicKeyHex[] =
      CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX;
  static OtaUpdateOwner owner({
      {kManifestUrl, sizeof(kManifestUrl) - 1U},
      {kPublicKeyHex, sizeof(kPublicKeyHex) - 1U},
      CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS});
#else
  static OtaUpdateOwner owner({{}, {}, 0U});
#endif
  return owner;
}

const char* otaUpdateStateName(OtaUpdateState state) {
  switch (state) {
    case OtaUpdateState::Disabled: return "DISABLED";
    case OtaUpdateState::Idle: return "IDLE";
    case OtaUpdateState::Requested: return "REQUESTED";
    case OtaUpdateState::Running: return "RUNNING";
    case OtaUpdateState::Acquiring: return "ACQUIRING";
    case OtaUpdateState::ImageSelected: return "IMAGE_SELECTED";
    case OtaUpdateState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

const char* otaUpdateCodeName(OtaUpdateCode code) {
  switch (code) {
    case OtaUpdateCode::Ok: return "OK";
    case OtaUpdateCode::Ready: return "READY";
    case OtaUpdateCode::ConfigurationMissing: return "CONFIGURATION_MISSING";
    case OtaUpdateCode::ManifestUrlRejected: return "MANIFEST_URL_REJECTED";
    case OtaUpdateCode::PlaceholderEndpointRejected:
      return "PLACEHOLDER_ENDPOINT_REJECTED";
    case OtaUpdateCode::PublicKeyRejected: return "PUBLIC_KEY_REJECTED";
    case OtaUpdateCode::DeadlineRejected: return "DEADLINE_REJECTED";
    case OtaUpdateCode::Disabled: return "DISABLED";
    case OtaUpdateCode::InvalidRequestId: return "INVALID_REQUEST_ID";
    case OtaUpdateCode::DuplicateRequest: return "DUPLICATE_REQUEST";
    case OtaUpdateCode::Busy: return "BUSY";
    case OtaUpdateCode::NoRequest: return "NO_REQUEST";
    case OtaUpdateCode::RequestMismatch: return "REQUEST_MISMATCH";
    case OtaUpdateCode::InvalidTerminalCode:
      return "INVALID_TERMINAL_CODE";
    case OtaUpdateCode::QuiesceFailed: return "QUIESCE_FAILED";
    case OtaUpdateCode::PlatformUnavailable: return "PLATFORM_UNAVAILABLE";
    case OtaUpdateCode::VerifierUnavailable: return "VERIFIER_UNAVAILABLE";
    case OtaUpdateCode::AcquisitionInvalidState:
      return "ACQUISITION_INVALID_STATE";
    case OtaUpdateCode::AcquisitionConfigurationRejected:
      return "ACQUISITION_CONFIGURATION_REJECTED";
    case OtaUpdateCode::DeadlineExceeded: return "DEADLINE_EXCEEDED";
    case OtaUpdateCode::ManifestFetchFailed: return "MANIFEST_FETCH_FAILED";
    case OtaUpdateCode::ManifestRejected: return "MANIFEST_REJECTED";
    case OtaUpdateCode::ImageOriginMismatch: return "IMAGE_ORIGIN_MISMATCH";
    case OtaUpdateCode::StagingBeginFailed: return "STAGING_BEGIN_FAILED";
    case OtaUpdateCode::ImageFetchFailed: return "IMAGE_FETCH_FAILED";
    case OtaUpdateCode::StagingFinishFailed: return "STAGING_FINISH_FAILED";
    case OtaUpdateCode::ImageSelected: return "IMAGE_SELECTED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
