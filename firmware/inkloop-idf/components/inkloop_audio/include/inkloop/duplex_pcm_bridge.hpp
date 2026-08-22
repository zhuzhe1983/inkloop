#pragma once

#include <cstddef>
#include <cstdint>

#include "inkloop/audio_flow.hpp"
#include "inkloop/pcm_backpressure.hpp"

namespace inkloop {

enum class DuplexStreamState : uint8_t {
  Idle,
  Open,
  Draining,
  Complete,
  Cancelled,
  Fault,
};

// Portable state machine behind the cross-core IDF adapter. Callers serialize
// each method with one short critical section; hardware and socket I/O always
// happen outside that lock.
class DuplexPcmBridgeCore final {
 public:
  DuplexPcmBridgeCore(uint8_t* playback_storage, size_t playback_capacity,
                      size_t playback_low_watermark,
                      size_t playback_high_watermark,
                      size_t maximum_playback_message,
                      uint8_t* capture_storage, size_t capture_capacity,
                      size_t capture_low_watermark,
                      size_t capture_high_watermark,
                      size_t maximum_capture_frame);

  bool valid() const;

  uint32_t beginPlayback(uint32_t sample_rate_hz, uint8_t channels);
  PcmTransfer pushPlayback(uint32_t generation, const uint8_t* bytes,
                           size_t length);
  PcmTransfer popPlayback(uint8_t* bytes, size_t maximum);
  PcmTransfer finishPlayback(uint32_t generation);
  PcmTransfer cancelPlayback(uint32_t generation);
  bool canAcceptPlayback(size_t bytes) const;
  bool playbackComplete() const;
  DuplexStreamState playbackState() const { return playback_state_; }
  uint32_t playbackGeneration() const { return playback_.generation(); }

  uint32_t beginCapture();
  PcmTransfer pushCapture(uint32_t generation, const uint8_t* bytes,
                          size_t length);
  PcmTransfer popCapture(uint8_t* bytes, size_t maximum);
  PcmTransfer finishCapture(uint32_t generation);
  PcmTransfer cancelCapture(uint32_t generation);
  bool captureComplete() const;
  DuplexStreamState captureState() const { return capture_state_; }
  uint32_t captureGeneration() const { return capture_.generation(); }

 private:
  PcmBackpressureRing playback_ring_;
  AudioPlaybackFlow playback_;
  PcmBackpressureRing capture_ring_;
  AudioCaptureContract capture_;
  DuplexStreamState playback_state_ = DuplexStreamState::Idle;
  DuplexStreamState capture_state_ = DuplexStreamState::Idle;
};

}  // namespace inkloop
