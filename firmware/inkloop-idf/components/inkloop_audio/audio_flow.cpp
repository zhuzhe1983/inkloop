#include "inkloop/audio_flow.hpp"

#include <algorithm>
#include <limits>

namespace inkloop {

PcmTransfer AudioPlaybackFlow::reject(PcmFlowSignal signal) const {
  return PcmTransfer{signal, 0, ring_.buffered()};
}

void AudioPlaybackFlow::observe(const PcmTransfer& transfer) {
  diagnostics_.peak_buffered_bytes =
      std::max(diagnostics_.peak_buffered_bytes, transfer.buffered);
  switch (transfer.signal) {
    case PcmFlowSignal::PauseIngress:
      ++diagnostics_.pause_signals;
      break;
    case PcmFlowSignal::ResumeIngress:
      ++diagnostics_.resume_signals;
      break;
    case PcmFlowSignal::Full:
      ++diagnostics_.full_rejections;
      break;
    case PcmFlowSignal::LimitExceeded:
      ++diagnostics_.limit_rejections;
      break;
    default:
      break;
  }
}

uint32_t AudioPlaybackFlow::begin(uint32_t sample_rate_hz, uint8_t channels) {
  if (!ring_.valid() || sample_rate_hz < kMinimumSampleRateHz ||
      sample_rate_hz > kMaximumSampleRateHz ||
      (channels != 1 && channels != 2)) {
    state_ = AudioFlowState::Fault;
    return 0;
  }
  const uint64_t limit = static_cast<uint64_t>(sample_rate_hz) * channels *
                         2U * kMaximumDurationSeconds;
  if (limit == 0 || limit > std::numeric_limits<size_t>::max()) {
    state_ = AudioFlowState::Fault;
    return 0;
  }
  generation_ = ring_.beginTurn();
  if (generation_ == 0) {
    state_ = AudioFlowState::Fault;
    return 0;
  }
  sample_rate_hz_ = sample_rate_hz;
  channels_ = channels;
  byte_limit_ = static_cast<size_t>(limit);
  accepted_bytes_ = 0;
  empty_observed_ = false;
  diagnostics_ = AudioFlowDiagnostics{};
  state_ = AudioFlowState::Receiving;
  return generation_;
}

PcmTransfer AudioPlaybackFlow::accept(uint32_t generation,
                                      const uint8_t* bytes, size_t length) {
  if (state_ != AudioFlowState::Receiving) return reject(PcmFlowSignal::Closed);
  if (generation == 0 || generation != generation_)
    return reject(PcmFlowSignal::StaleGeneration);
  const size_t frame_bytes = static_cast<size_t>(channels_) * 2U;
  if (!bytes || length == 0 || length % frame_bytes != 0)
    return reject(PcmFlowSignal::InvalidFrame);
  if (length > byte_limit_ - accepted_bytes_) {
    ++diagnostics_.limit_rejections;
    state_ = AudioFlowState::Fault;
    ring_.cancel(generation_);
    return reject(PcmFlowSignal::LimitExceeded);
  }
  const PcmTransfer result = ring_.push(generation, bytes, length);
  observe(result);
  if (result.bytes != 0) {
    accepted_bytes_ += result.bytes;
    diagnostics_.accepted_bytes = accepted_bytes_;
    empty_observed_ = false;
  }
  return result;
}

PcmTransfer AudioPlaybackFlow::drain(uint8_t* output, size_t maximum_bytes) {
  if (state_ != AudioFlowState::Receiving &&
      state_ != AudioFlowState::Draining)
    return reject(PcmFlowSignal::Closed);
  const PcmTransfer result = ring_.pop(output, maximum_bytes);
  observe(result);
  diagnostics_.drained_bytes += result.bytes;
  if (result.bytes == 0 && result.signal == PcmFlowSignal::Continue) {
    if (!empty_observed_) ++diagnostics_.underruns;
    empty_observed_ = true;
  } else if (result.bytes != 0) {
    empty_observed_ = false;
  }
  if (result.signal == PcmFlowSignal::Closed && ring_.complete()) {
    state_ = AudioFlowState::Complete;
  }
  return result;
}

PcmTransfer AudioPlaybackFlow::finish(uint32_t generation) {
  if (state_ != AudioFlowState::Receiving) return reject(PcmFlowSignal::Closed);
  const PcmTransfer result = ring_.finishIngress(generation);
  observe(result);
  if (result.signal == PcmFlowSignal::StaleGeneration) return result;
  if (result.signal == PcmFlowSignal::Closed) {
    state_ = AudioFlowState::Complete;
  } else {
    state_ = AudioFlowState::Draining;
  }
  return result;
}

PcmTransfer AudioPlaybackFlow::cancel(uint32_t generation) {
  const PcmTransfer result = ring_.cancel(generation);
  if (result.signal != PcmFlowSignal::StaleGeneration) {
    ++diagnostics_.cancellations;
    state_ = AudioFlowState::Cancelled;
  }
  return result;
}

uint32_t AudioCaptureContract::begin() {
  ++generation_;
  if (generation_ == 0) ++generation_;
  accepted_bytes_ = 0;
  active_ = true;
  return generation_;
}

PcmFlowSignal AudioCaptureContract::validate(uint32_t generation,
                                             const uint8_t* bytes,
                                             size_t length) {
  if (!active_) return PcmFlowSignal::Closed;
  if (generation == 0 || generation != generation_)
    return PcmFlowSignal::StaleGeneration;
  if (!bytes || length == 0 || (length & 1U) != 0 ||
      length > kMaximumFrameBytes)
    return PcmFlowSignal::InvalidFrame;
  if (length > kMaximumCaptureBytes - accepted_bytes_) {
    active_ = false;
    return PcmFlowSignal::LimitExceeded;
  }
  accepted_bytes_ += length;
  return PcmFlowSignal::Continue;
}

PcmFlowSignal AudioCaptureContract::finish(uint32_t generation) {
  if (!active_) return PcmFlowSignal::Closed;
  if (generation == 0 || generation != generation_)
    return PcmFlowSignal::StaleGeneration;
  active_ = false;
  return PcmFlowSignal::Closed;
}

PcmFlowSignal AudioCaptureContract::cancel(uint32_t generation) {
  return finish(generation);
}

}  // namespace inkloop
