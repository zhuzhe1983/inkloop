#include "inkloop/duplex_pcm_bridge.hpp"

namespace inkloop {
namespace {

DuplexStreamState playbackStateFor(AudioFlowState state) {
  switch (state) {
    case AudioFlowState::Idle:
      return DuplexStreamState::Idle;
    case AudioFlowState::Receiving:
      return DuplexStreamState::Open;
    case AudioFlowState::Draining:
      return DuplexStreamState::Draining;
    case AudioFlowState::Complete:
      return DuplexStreamState::Complete;
    case AudioFlowState::Cancelled:
      return DuplexStreamState::Cancelled;
    case AudioFlowState::Fault:
      return DuplexStreamState::Fault;
  }
  return DuplexStreamState::Fault;
}

}  // namespace

DuplexPcmBridgeCore::DuplexPcmBridgeCore(
    uint8_t* playback_storage, size_t playback_capacity,
    size_t playback_low_watermark, size_t playback_high_watermark,
    size_t maximum_playback_message, uint8_t* capture_storage,
    size_t capture_capacity, size_t capture_low_watermark,
    size_t capture_high_watermark, size_t maximum_capture_frame)
    : playback_ring_(playback_storage, playback_capacity,
                     playback_low_watermark, playback_high_watermark,
                     maximum_playback_message),
      playback_(playback_ring_),
      capture_ring_(capture_storage, capture_capacity, capture_low_watermark,
                    capture_high_watermark, maximum_capture_frame) {}

bool DuplexPcmBridgeCore::valid() const {
  return playback_ring_.valid() && capture_ring_.valid();
}

uint32_t DuplexPcmBridgeCore::beginPlayback(uint32_t sample_rate_hz,
                                            uint8_t channels) {
  if (!valid() || (playback_state_ != DuplexStreamState::Idle &&
                   playback_state_ != DuplexStreamState::Complete &&
                   playback_state_ != DuplexStreamState::Cancelled)) {
    playback_state_ = DuplexStreamState::Fault;
    return 0;
  }
  const uint32_t generation = playback_.begin(sample_rate_hz, channels);
  playback_state_ = playbackStateFor(playback_.state());
  return generation;
}

PcmTransfer DuplexPcmBridgeCore::pushPlayback(uint32_t generation,
                                              const uint8_t* bytes,
                                              size_t length) {
  const PcmTransfer transfer = playback_.accept(generation, bytes, length);
  playback_state_ = playbackStateFor(playback_.state());
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::popPlayback(uint8_t* bytes, size_t maximum) {
  const PcmTransfer transfer = playback_.drain(bytes, maximum);
  playback_state_ = playbackStateFor(playback_.state());
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::finishPlayback(uint32_t generation) {
  const PcmTransfer transfer = playback_.finish(generation);
  playback_state_ = playbackStateFor(playback_.state());
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::cancelPlayback(uint32_t generation) {
  const PcmTransfer transfer = playback_.cancel(generation);
  playback_state_ = playbackStateFor(playback_.state());
  return transfer;
}

bool DuplexPcmBridgeCore::canAcceptPlayback(size_t bytes) const {
  return playback_state_ != DuplexStreamState::Fault && bytes != 0 &&
         bytes <= playback_ring_.available();
}

bool DuplexPcmBridgeCore::playbackComplete() const {
  return playback_state_ == DuplexStreamState::Complete;
}

uint32_t DuplexPcmBridgeCore::beginCapture() {
  if (!valid() || (capture_state_ != DuplexStreamState::Idle &&
                   capture_state_ != DuplexStreamState::Complete &&
                   capture_state_ != DuplexStreamState::Cancelled)) {
    capture_state_ = DuplexStreamState::Fault;
    return 0;
  }
  const uint32_t generation = capture_.begin();
  const uint32_t ring_generation = capture_ring_.beginTurn();
  if (generation == 0 || ring_generation == 0 || generation != ring_generation) {
    if (generation != 0) capture_.cancel(generation);
    if (ring_generation != 0) capture_ring_.cancel(ring_generation);
    capture_state_ = DuplexStreamState::Fault;
    return 0;
  }
  capture_state_ = DuplexStreamState::Open;
  return generation;
}

PcmTransfer DuplexPcmBridgeCore::pushCapture(uint32_t generation,
                                             const uint8_t* bytes,
                                             size_t length) {
  const PcmFlowSignal contract = capture_.validate(generation, bytes, length);
  if (contract != PcmFlowSignal::Continue) {
    if (contract == PcmFlowSignal::LimitExceeded)
      capture_state_ = DuplexStreamState::Fault;
    return PcmTransfer{contract, 0, capture_ring_.buffered()};
  }
  const PcmTransfer transfer = capture_ring_.push(generation, bytes, length);
  if (transfer.signal == PcmFlowSignal::Full ||
      transfer.signal == PcmFlowSignal::InvalidFrame) {
    capture_.cancel(generation);
    capture_ring_.cancel(generation);
    capture_state_ = DuplexStreamState::Fault;
  }
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::popCapture(uint8_t* bytes, size_t maximum) {
  const PcmTransfer transfer = capture_ring_.pop(bytes, maximum);
  if (capture_ring_.complete()) capture_state_ = DuplexStreamState::Complete;
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::finishCapture(uint32_t generation) {
  const PcmFlowSignal contract = capture_.finish(generation);
  if (contract != PcmFlowSignal::Closed) {
    return PcmTransfer{contract, 0, capture_ring_.buffered()};
  }
  const PcmTransfer transfer = capture_ring_.finishIngress(generation);
  capture_state_ = capture_ring_.complete() ? DuplexStreamState::Complete
                                            : DuplexStreamState::Draining;
  return transfer;
}

PcmTransfer DuplexPcmBridgeCore::cancelCapture(uint32_t generation) {
  const PcmFlowSignal contract = capture_.cancel(generation);
  const PcmTransfer transfer = capture_ring_.cancel(generation);
  if (contract != PcmFlowSignal::StaleGeneration &&
      transfer.signal != PcmFlowSignal::StaleGeneration) {
    capture_state_ = DuplexStreamState::Cancelled;
  }
  return transfer;
}

bool DuplexPcmBridgeCore::captureComplete() const {
  return capture_state_ == DuplexStreamState::Complete;
}

}  // namespace inkloop
