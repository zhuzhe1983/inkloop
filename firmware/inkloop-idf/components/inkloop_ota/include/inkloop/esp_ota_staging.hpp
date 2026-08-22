#pragma once

#include <cstddef>
#include <cstdint>

#include "inkloop/ota_staging.hpp"

namespace inkloop {

using EspOtaPartition = const void*;
using EspOtaHandle = std::uintptr_t;

struct EspOtaWriterFunctions {
  EspOtaPartition (*get_running_partition)() = nullptr;
  EspOtaPartition (*get_next_update_partition)() = nullptr;
  std::uint64_t (*partition_capacity)(EspOtaPartition partition) = nullptr;
  int (*ota_begin)(EspOtaPartition partition, std::size_t image_size,
                   EspOtaHandle& handle) = nullptr;
  int (*ota_write)(EspOtaHandle handle, const std::uint8_t* bytes,
                   std::size_t length) = nullptr;
  int (*ota_end)(EspOtaHandle handle) = nullptr;
  int (*ota_abort)(EspOtaHandle handle) = nullptr;
  int (*set_boot_partition)(EspOtaPartition partition) = nullptr;
};

const EspOtaWriterFunctions& systemEspOtaWriterFunctions();

enum class EspOtaStagingCode : std::uint8_t {
  Ok,
  InvalidFunctions,
  InvalidState,
  ManifestRejected,
  VerifierUnavailable,
  SignaturePolicyUnsupported,
  RunningPartitionUnavailable,
  TargetPartitionUnavailable,
  TargetAliasesRunning,
  TargetTooSmall,
  BeginFailed,
  ChunkRejected,
  WriteFailed,
  FinalizeRejected,
  SignatureRejected,
  EndFailed,
  SelectFailed,
  Aborted,
};

struct EspOtaStagingObservation {
  EspOtaStagingCode code = EspOtaStagingCode::Ok;
  OtaStagingCode core_code = OtaStagingCode::Ok;
  OtaManifestCode manifest_code = OtaManifestCode::Ok;
  int system_status = 0;
};

class EspOtaStagingAdapter final {
 public:
  EspOtaStagingAdapter(const EspOtaWriterFunctions& functions,
                       const IPinnedOtaSignatureVerifier* verifier)
      : functions_(functions), verifier_(verifier) {}

  EspOtaStagingObservation begin(const ReviewedOtaManifest& manifest,
                                 OtaTextView device_board_sku);
  EspOtaStagingObservation write(const std::uint8_t* bytes,
                                 std::size_t length);
  EspOtaStagingObservation finish();
  EspOtaStagingObservation abort();

  const OtaStagingCore& core() const { return core_; }
  EspOtaPartition targetPartition() const { return target_partition_; }
  bool handleAcquired() const { return handle_acquired_; }
  bool abortAttempted() const { return abort_attempted_; }
  bool targetSelected() const { return target_selected_; }

 private:
  bool functionsValid() const;
  int abortOnce();
  EspOtaStagingObservation fail(EspOtaStagingCode code,
                                OtaStagingCode core_code,
                                int system_status = 0);

  const EspOtaWriterFunctions& functions_;
  const IPinnedOtaSignatureVerifier* verifier_ = nullptr;
  OtaStagingCore core_{};
  EspOtaPartition target_partition_ = nullptr;
  EspOtaHandle handle_ = 0U;
  bool handle_acquired_ = false;
  bool abort_attempted_ = false;
  bool target_selected_ = false;
};

const char* espOtaStagingCodeName(EspOtaStagingCode code);

}  // namespace inkloop
