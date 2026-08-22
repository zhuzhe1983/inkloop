#include "inkloop/esp_ota_staging.hpp"

#include <limits>

namespace inkloop {

bool EspOtaStagingAdapter::functionsValid() const {
  return functions_.get_running_partition &&
      functions_.get_next_update_partition &&
      functions_.partition_capacity && functions_.ota_begin &&
      functions_.ota_write && functions_.ota_end && functions_.ota_abort &&
      functions_.set_boot_partition;
}

int EspOtaStagingAdapter::abortOnce() {
  if (!handle_acquired_ || abort_attempted_) return 0;
  abort_attempted_ = true;
  return functions_.ota_abort(handle_);
}

EspOtaStagingObservation EspOtaStagingAdapter::fail(
    EspOtaStagingCode code, OtaStagingCode core_code, int system_status) {
  EspOtaStagingObservation observation;
  observation.code = code;
  observation.core_code = core_code;
  observation.manifest_code = core_.manifestCode();
  observation.system_status = system_status;
  return observation;
}

EspOtaStagingObservation EspOtaStagingAdapter::begin(
    const ReviewedOtaManifest& manifest, OtaTextView device_board_sku) {
  if (core_.state() != OtaStagingState::Empty)
    return fail(EspOtaStagingCode::InvalidState,
                OtaStagingCode::InvalidState);

  const OtaStagingCode prepared = core_.prepare(manifest, device_board_sku);
  if (prepared != OtaStagingCode::Ok)
    return fail(EspOtaStagingCode::ManifestRejected, prepared);
  if (!verifier_) {
    core_.abort();
    return fail(EspOtaStagingCode::VerifierUnavailable,
                OtaStagingCode::VerifierUnavailable);
  }
  const PreparedOtaManifest& prepared_manifest = core_.manifest();
  const OtaTextView policy{prepared_manifest.signature_policy.data(),
                           prepared_manifest.signature_policy_length};
  if (!verifier_->supportsPolicy(policy)) {
    core_.abort();
    return fail(EspOtaStagingCode::SignaturePolicyUnsupported,
                OtaStagingCode::SignaturePolicyUnsupported);
  }
  if (!functionsValid()) {
    core_.abort();
    return fail(EspOtaStagingCode::InvalidFunctions,
                OtaStagingCode::InvalidState);
  }

  const EspOtaPartition running = functions_.get_running_partition();
  if (!running) {
    core_.abort();
    return fail(EspOtaStagingCode::RunningPartitionUnavailable,
                OtaStagingCode::InvalidState);
  }
  target_partition_ = functions_.get_next_update_partition();
  if (!target_partition_) {
    core_.abort();
    return fail(EspOtaStagingCode::TargetPartitionUnavailable,
                OtaStagingCode::InvalidState);
  }
  if (target_partition_ == running) {
    core_.abort();
    target_partition_ = nullptr;
    return fail(EspOtaStagingCode::TargetAliasesRunning,
                OtaStagingCode::InvalidState);
  }
  if (functions_.partition_capacity(target_partition_) <
      prepared_manifest.image_size ||
      prepared_manifest.image_size >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    core_.abort();
    target_partition_ = nullptr;
    return fail(EspOtaStagingCode::TargetTooSmall,
                OtaStagingCode::InvalidState);
  }

  const int begin_status = functions_.ota_begin(
      target_partition_,
      static_cast<std::size_t>(prepared_manifest.image_size), handle_);
  if (begin_status != 0) {
    core_.abort();
    target_partition_ = nullptr;
    return fail(EspOtaStagingCode::BeginFailed,
                OtaStagingCode::InvalidState, begin_status);
  }
  handle_acquired_ = true;
  const OtaStagingCode streaming = core_.beginStream();
  if (streaming != OtaStagingCode::Ok) {
    const int abort_status = abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::InvalidState, streaming,
                abort_status);
  }
  return fail(EspOtaStagingCode::Ok, OtaStagingCode::Ok);
}

EspOtaStagingObservation EspOtaStagingAdapter::write(
    const std::uint8_t* bytes, std::size_t length) {
  if (!handle_acquired_ || abort_attempted_ || target_selected_ ||
      core_.state() != OtaStagingState::Streaming)
    return fail(EspOtaStagingCode::InvalidState,
                OtaStagingCode::InvalidState);

  const OtaStagingCode accepted = core_.acceptChunk(bytes, length);
  if (accepted != OtaStagingCode::Ok) {
    const int abort_status = abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::ChunkRejected, accepted, abort_status);
  }
  const int write_status = functions_.ota_write(handle_, bytes, length);
  if (write_status != 0) {
    abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::WriteFailed,
                OtaStagingCode::Ok, write_status);
  }
  return fail(EspOtaStagingCode::Ok, OtaStagingCode::Ok);
}

EspOtaStagingObservation EspOtaStagingAdapter::finish() {
  if (!handle_acquired_ || abort_attempted_ || target_selected_ ||
      core_.state() != OtaStagingState::Streaming)
    return fail(EspOtaStagingCode::InvalidState,
                OtaStagingCode::InvalidState);

  const OtaStagingCode finalized = core_.finalizeContent();
  if (finalized != OtaStagingCode::Ok) {
    const int abort_status = abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::FinalizeRejected, finalized,
                abort_status);
  }
  const OtaStagingCode signature = core_.verifySignature(verifier_);
  if (signature != OtaStagingCode::Ok) {
    const int abort_status = abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::SignatureRejected, signature,
                abort_status);
  }
  const int end_status = functions_.ota_end(handle_);
  if (end_status != 0) {
    abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::EndFailed,
                OtaStagingCode::Ok, end_status);
  }
  const OtaStagingCode completed = core_.markImageComplete();
  if (completed != OtaStagingCode::Ok) {
    const int abort_status = abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::InvalidState, completed,
                abort_status);
  }
  const int select_status = functions_.set_boot_partition(target_partition_);
  if (select_status != 0) {
    abortOnce();
    core_.abort();
    return fail(EspOtaStagingCode::SelectFailed,
                OtaStagingCode::Ok, select_status);
  }
  target_selected_ = true;
  const OtaStagingCode selected = core_.markBootSelected();
  if (selected != OtaStagingCode::Ok)
    return fail(EspOtaStagingCode::InvalidState, selected);
  return fail(EspOtaStagingCode::Ok, OtaStagingCode::Ok);
}

EspOtaStagingObservation EspOtaStagingAdapter::abort() {
  if (target_selected_)
    return fail(EspOtaStagingCode::InvalidState,
                OtaStagingCode::InvalidState);
  const int status = abortOnce();
  core_.abort();
  return fail(EspOtaStagingCode::Aborted, OtaStagingCode::Ok, status);
}

const char* espOtaStagingCodeName(EspOtaStagingCode code) {
  switch (code) {
    case EspOtaStagingCode::Ok: return "OK";
    case EspOtaStagingCode::InvalidFunctions: return "INVALID_FUNCTIONS";
    case EspOtaStagingCode::InvalidState: return "INVALID_STATE";
    case EspOtaStagingCode::ManifestRejected: return "MANIFEST_REJECTED";
    case EspOtaStagingCode::VerifierUnavailable:
      return "VERIFIER_UNAVAILABLE";
    case EspOtaStagingCode::SignaturePolicyUnsupported:
      return "SIGNATURE_POLICY_UNSUPPORTED";
    case EspOtaStagingCode::RunningPartitionUnavailable:
      return "RUNNING_PARTITION_UNAVAILABLE";
    case EspOtaStagingCode::TargetPartitionUnavailable:
      return "TARGET_PARTITION_UNAVAILABLE";
    case EspOtaStagingCode::TargetAliasesRunning:
      return "TARGET_ALIASES_RUNNING";
    case EspOtaStagingCode::TargetTooSmall: return "TARGET_TOO_SMALL";
    case EspOtaStagingCode::BeginFailed: return "BEGIN_FAILED";
    case EspOtaStagingCode::ChunkRejected: return "CHUNK_REJECTED";
    case EspOtaStagingCode::WriteFailed: return "WRITE_FAILED";
    case EspOtaStagingCode::FinalizeRejected: return "FINALIZE_REJECTED";
    case EspOtaStagingCode::SignatureRejected:
      return "SIGNATURE_REJECTED";
    case EspOtaStagingCode::EndFailed: return "END_FAILED";
    case EspOtaStagingCode::SelectFailed: return "SELECT_FAILED";
    case EspOtaStagingCode::Aborted: return "ABORTED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
