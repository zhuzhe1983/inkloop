#pragma once

#include <cstddef>
#include <cstdint>

#include "inkloop/pcm_backpressure.hpp"

namespace inkloop {

enum class AudioFlowState : uint8_t {
  Idle,
  Receiving,
  Draining,
  Complete,
  Cancelled,
  Fault,
};

struct AudioFlowDiagnostics {
  uint32_t pause_signals = 0;
  uint32_t resume_signals = 0;
  uint32_t full_rejections = 0;
  uint32_t limit_rejections = 0;
  uint32_t underruns = 0;
  uint32_t cancellations = 0;
  size_t peak_buffered_bytes = 0;
  size_t accepted_bytes = 0;
  size_t drained_bytes = 0;
};

// Sole-owner playback contract between WSS ingress and the I2S voice task.
// It bounds an answer to 60 seconds, carries a generation through cancel, and
// exposes high/low-watermark feedback without allocating another audio queue.
class AudioPlaybackFlow final {
 public:
  static constexpr uint32_t kMaximumDurationSeconds = 60;
  static constexpr uint32_t kMinimumSampleRateHz = 8000;
  static constexpr uint32_t kMaximumSampleRateHz = 48000;

  explicit AudioPlaybackFlow(PcmBackpressureRing& ring) : ring_(ring) {}

  uint32_t begin(uint32_t sample_rate_hz, uint8_t channels);
  PcmTransfer accept(uint32_t generation, const uint8_t* bytes, size_t length);
  PcmTransfer drain(uint8_t* output, size_t maximum_bytes);
  PcmTransfer finish(uint32_t generation);
  PcmTransfer cancel(uint32_t generation);

  AudioFlowState state() const { return state_; }
  uint32_t generation() const { return generation_; }
  uint32_t sampleRateHz() const { return sample_rate_hz_; }
  uint8_t channels() const { return channels_; }
  size_t byteLimit() const { return byte_limit_; }
  const AudioFlowDiagnostics& diagnostics() const { return diagnostics_; }

 private:
  PcmTransfer reject(PcmFlowSignal signal) const;
  void observe(const PcmTransfer& transfer);

  PcmBackpressureRing& ring_;
  AudioFlowState state_ = AudioFlowState::Idle;
  AudioFlowDiagnostics diagnostics_{};
  uint32_t generation_ = 0;
  uint32_t sample_rate_hz_ = 0;
  uint8_t channels_ = 0;
  size_t byte_limit_ = 0;
  size_t accepted_bytes_ = 0;
  bool empty_observed_ = false;
};

// Fixed MyAI capture contract. DMA buffers are read by the voice owner and
// handed to the network owner as bounded PCM16 frames; no audio bytes enter the
// control queue or local chat log.
class AudioCaptureContract final {
 public:
  static constexpr uint32_t kSampleRateHz = 16000;
  static constexpr uint8_t kChannels = 1;
  static constexpr size_t kMaximumFrameBytes = 2048;
  static constexpr size_t kMaximumCaptureBytes =
      static_cast<size_t>(kSampleRateHz) * kChannels * 2U * 60U;

  uint32_t begin();
  PcmFlowSignal validate(uint32_t generation, const uint8_t* bytes,
                         size_t length);
  PcmFlowSignal finish(uint32_t generation);
  PcmFlowSignal cancel(uint32_t generation);

  bool active() const { return active_; }
  uint32_t generation() const { return generation_; }
  size_t acceptedBytes() const { return accepted_bytes_; }

 private:
  uint32_t generation_ = 0;
  size_t accepted_bytes_ = 0;
  bool active_ = false;
};

}  // namespace inkloop
