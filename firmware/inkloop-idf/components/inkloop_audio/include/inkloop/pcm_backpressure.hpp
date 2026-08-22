#pragma once

#include <cstddef>
#include <cstdint>

namespace inkloop {

enum class PcmFlowSignal : uint8_t {
  Continue,
  PauseIngress,
  ResumeIngress,
  Full,
  LimitExceeded,
  StaleGeneration,
  InvalidFrame,
  Closed,
};

struct PcmTransfer {
  PcmFlowSignal signal = PcmFlowSignal::Continue;
  size_t bytes = 0;
  size_t buffered = 0;
};

// A bounded PCM16 ring shared by the network and I2S owners through explicit
// messages. It is deliberately not internally locked: exactly one audio owner
// mutates the ring, while network/I2S tasks submit commands carrying generation.
// This keeps cancellation deterministic and prevents stale WebSocket callbacks
// from writing into a later voice turn.
class PcmBackpressureRing {
 public:
  PcmBackpressureRing(uint8_t* storage, size_t capacity_bytes,
                      size_t low_watermark_bytes,
                      size_t high_watermark_bytes,
                      size_t maximum_frame_bytes);

  bool valid() const;
  uint32_t beginTurn();
  PcmTransfer push(uint32_t generation, const uint8_t* bytes, size_t length);
  PcmTransfer pop(uint8_t* output, size_t maximum_bytes);
  PcmTransfer finishIngress(uint32_t generation);
  PcmTransfer cancel(uint32_t generation);

  uint32_t generation() const { return generation_; }
  size_t buffered() const { return size_; }
  size_t available() const { return capacity_ >= size_ ? capacity_ - size_ : 0; }
  size_t capacity() const { return capacity_; }
  bool ingressPaused() const { return ingress_paused_; }
  bool active() const { return active_; }
  bool complete() const { return active_ && ingress_closed_ && size_ == 0; }

 private:
  PcmTransfer result(PcmFlowSignal signal, size_t bytes = 0) const;
  void resetBuffer();

  uint8_t* storage_;
  size_t capacity_;
  size_t low_watermark_;
  size_t high_watermark_;
  size_t maximum_frame_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
  uint32_t generation_ = 0;
  bool ingress_paused_ = false;
  bool ingress_closed_ = false;
  bool active_ = false;
};

}  // namespace inkloop
