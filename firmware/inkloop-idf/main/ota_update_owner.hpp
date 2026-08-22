#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "inkloop/ota_https_acquisition.hpp"

namespace inkloop {

enum class OtaUpdateState : std::uint8_t {
  Disabled = 0U,
  Idle = 1U,
  Requested = 2U,
  Running = 3U,
  Acquiring = 4U,
  ImageSelected = 5U,
  Failed = 6U,
};

enum class OtaUpdateCode : std::uint8_t {
  Ok = 0U,
  Ready = 1U,
  ConfigurationMissing = 2U,
  ManifestUrlRejected = 3U,
  PlaceholderEndpointRejected = 4U,
  PublicKeyRejected = 5U,
  DeadlineRejected = 6U,
  Disabled = 7U,
  InvalidRequestId = 8U,
  DuplicateRequest = 9U,
  Busy = 10U,
  NoRequest = 11U,
  RequestMismatch = 12U,
  InvalidTerminalCode = 13U,
  QuiesceFailed = 14U,
  PlatformUnavailable = 15U,
  VerifierUnavailable = 16U,
  AcquisitionInvalidState = 17U,
  AcquisitionConfigurationRejected = 18U,
  DeadlineExceeded = 19U,
  ManifestFetchFailed = 20U,
  ManifestRejected = 21U,
  ImageOriginMismatch = 22U,
  StagingBeginFailed = 23U,
  ImageFetchFailed = 24U,
  StagingFinishFailed = 25U,
  ImageSelected = 26U,
};

struct OtaUpdateSnapshot {
  OtaUpdateState state = OtaUpdateState::Disabled;
  OtaUpdateCode code = OtaUpdateCode::ConfigurationMissing;
  std::uint64_t request_id = 0U;
};

struct OtaUpdateRequest {
  std::uint64_t request_id = 0U;
};

struct OtaUpdateRawConfiguration {
  OtaTextView manifest_url{};
  OtaTextView public_key_hex{};
  std::uint32_t total_deadline_ms = 0U;
};

class OtaUpdateOwner final {
 public:
  explicit OtaUpdateOwner(const OtaUpdateRawConfiguration& configuration);

  OtaUpdateOwner(const OtaUpdateOwner&) = delete;
  OtaUpdateOwner& operator=(const OtaUpdateOwner&) = delete;
  OtaUpdateOwner(OtaUpdateOwner&&) = delete;
  OtaUpdateOwner& operator=(OtaUpdateOwner&&) = delete;

  OtaUpdateSnapshot snapshot() const;
  OtaUpdateCode request(std::uint64_t request_id);
  OtaUpdateCode take(OtaUpdateRequest& request);
  OtaUpdateCode fail(const OtaUpdateRequest& request, OtaUpdateCode code);

  // Called only after take() succeeded and root composition has quiesced
  // incompatible owners. This atomically claims the exact Running request,
  // constructs the frozen WS31/WS27/WS33 production stack, and blocks for at
  // most the configured deadline. It selects but never reboots the new image.
  OtaUpdateCode acquire(const OtaUpdateRequest& request,
                        OtaTextView device_board_sku,
                        OtaTextView current_firmware_version);

 private:
  struct PreparedConfiguration {
    std::array<char, kMaximumOtaUrlBytes + 1U> manifest_url{};
    std::size_t manifest_url_length = 0U;
    std::array<std::uint8_t, 32U> public_key{};
    std::uint32_t total_deadline_ms = 0U;
    bool ready = false;
  };

  struct alignas(16) AtomicStatus {
    std::uint64_t request_id = 0U;
    std::uint32_t state_and_code = 0U;
    std::uint32_t reserved = 0U;
  };

  static_assert(std::is_trivially_copyable<AtomicStatus>::value,
                "OTA status must be atomically copyable");
  static_assert(sizeof(AtomicStatus) == 16U,
                "OTA atomic status layout changed");

  static AtomicStatus encode(OtaUpdateState state, OtaUpdateCode code,
                             std::uint64_t request_id);
  static OtaUpdateSnapshot decode(const AtomicStatus& word);
  static OtaUpdateCode prepareConfiguration(
      const OtaUpdateRawConfiguration& input,
      PreparedConfiguration& output);
  static OtaUpdateCode mapAcquisition(
      const OtaHttpsAcquisitionObservation& observation);
  static bool validFailureCode(OtaUpdateCode code);
  OtaUpdateCode finishTerminal(const OtaUpdateRequest& request,
                               OtaUpdateState expected_state,
                               OtaUpdateCode code);

  PreparedConfiguration configuration_{};
  std::atomic<AtomicStatus> status_{};
};

OtaUpdateOwner& systemOtaUpdateOwner();

const char* otaUpdateStateName(OtaUpdateState state);
const char* otaUpdateCodeName(OtaUpdateCode code);

}  // namespace inkloop
