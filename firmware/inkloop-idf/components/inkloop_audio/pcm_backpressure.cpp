#include "inkloop/pcm_backpressure.hpp"

#include <algorithm>
#include <cstring>

namespace inkloop {

PcmBackpressureRing::PcmBackpressureRing(
    uint8_t* storage, size_t capacity_bytes, size_t low_watermark_bytes,
    size_t high_watermark_bytes, size_t maximum_frame_bytes)
    : storage_(storage),
      capacity_(capacity_bytes),
      low_watermark_(low_watermark_bytes),
      high_watermark_(high_watermark_bytes),
      maximum_frame_(maximum_frame_bytes) {}

bool PcmBackpressureRing::valid() const {
  return storage_ != nullptr && capacity_ >= 4 && (capacity_ & 1U) == 0 &&
         low_watermark_ < high_watermark_ && high_watermark_ <= capacity_ &&
         maximum_frame_ >= 2 && maximum_frame_ <= capacity_ &&
         (low_watermark_ & 1U) == 0 && (high_watermark_ & 1U) == 0 &&
         (maximum_frame_ & 1U) == 0;
}

void PcmBackpressureRing::resetBuffer() {
  head_ = 0;
  tail_ = 0;
  size_ = 0;
  ingress_paused_ = false;
  ingress_closed_ = false;
}

uint32_t PcmBackpressureRing::beginTurn() {
  if (!valid()) return 0;
  ++generation_;
  if (generation_ == 0) ++generation_;
  resetBuffer();
  active_ = true;
  return generation_;
}

PcmTransfer PcmBackpressureRing::result(PcmFlowSignal signal,
                                        size_t bytes) const {
  return PcmTransfer{signal, bytes, size_};
}

PcmTransfer PcmBackpressureRing::push(uint32_t generation,
                                      const uint8_t* bytes, size_t length) {
  if (!active_ || ingress_closed_) return result(PcmFlowSignal::Closed);
  if (generation == 0 || generation != generation_)
    return result(PcmFlowSignal::StaleGeneration);
  if (bytes == nullptr || length == 0 || (length & 1U) != 0 ||
      length > maximum_frame_)
    return result(PcmFlowSignal::InvalidFrame);
  if (length > capacity_ - size_) return result(PcmFlowSignal::Full);

  const size_t first = std::min(length, capacity_ - tail_);
  std::memcpy(storage_ + tail_, bytes, first);
  if (length > first) std::memcpy(storage_, bytes + first, length - first);
  tail_ = (tail_ + length) % capacity_;
  size_ += length;

  if (!ingress_paused_ && size_ >= high_watermark_) {
    ingress_paused_ = true;
    return result(PcmFlowSignal::PauseIngress, length);
  }
  return result(PcmFlowSignal::Continue, length);
}

PcmTransfer PcmBackpressureRing::pop(uint8_t* output,
                                     size_t maximum_bytes) {
  if (!active_) return result(PcmFlowSignal::Closed);
  if (output == nullptr || maximum_bytes < 2)
    return result(PcmFlowSignal::InvalidFrame);
  maximum_bytes &= ~static_cast<size_t>(1U);
  const size_t length = std::min(maximum_bytes, size_);
  if (length == 0)
    return result(ingress_closed_ ? PcmFlowSignal::Closed
                                  : PcmFlowSignal::Continue);

  const size_t first = std::min(length, capacity_ - head_);
  std::memcpy(output, storage_ + head_, first);
  if (length > first) std::memcpy(output + first, storage_, length - first);
  head_ = (head_ + length) % capacity_;
  size_ -= length;

  if (ingress_paused_ && size_ <= low_watermark_) {
    ingress_paused_ = false;
    return result(PcmFlowSignal::ResumeIngress, length);
  }
  if (ingress_closed_ && size_ == 0)
    return result(PcmFlowSignal::Closed, length);
  return result(PcmFlowSignal::Continue, length);
}

PcmTransfer PcmBackpressureRing::finishIngress(uint32_t generation) {
  if (!active_) return result(PcmFlowSignal::Closed);
  if (generation == 0 || generation != generation_)
    return result(PcmFlowSignal::StaleGeneration);
  ingress_closed_ = true;
  ingress_paused_ = false;
  return result(size_ == 0 ? PcmFlowSignal::Closed
                           : PcmFlowSignal::Continue);
}

PcmTransfer PcmBackpressureRing::cancel(uint32_t generation) {
  if (!active_) return result(PcmFlowSignal::Closed);
  if (generation == 0 || generation != generation_)
    return result(PcmFlowSignal::StaleGeneration);
  resetBuffer();
  active_ = false;
  return result(PcmFlowSignal::Closed);
}

}  // namespace inkloop

