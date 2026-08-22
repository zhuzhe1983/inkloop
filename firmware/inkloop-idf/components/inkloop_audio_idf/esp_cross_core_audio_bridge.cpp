#include "inkloop/esp_cross_core_audio_bridge.hpp"

#include <array>
#include <new>

#include "esp_heap_caps.h"

namespace inkloop {
namespace {

constexpr size_t kPlaybackLowWatermark = 8U * 1024U;
constexpr size_t kPlaybackHighWatermark = 20U * 1024U;
constexpr size_t kCaptureLowWatermark = 4U * 1024U;
constexpr size_t kCaptureHighWatermark = 12U * 1024U;

bool transferAccepted(const PcmTransfer& transfer) {
  return transfer.signal == PcmFlowSignal::Continue ||
         transfer.signal == PcmFlowSignal::PauseIngress ||
         transfer.signal == PcmFlowSignal::ResumeIngress ||
         transfer.signal == PcmFlowSignal::Closed;
}

}  // namespace

EspCrossCoreAudioBridge::EspCrossCoreAudioBridge() = default;

EspCrossCoreAudioBridge::~EspCrossCoreAudioBridge() { releaseBuffers(); }

myai::Status EspCrossCoreAudioBridge::audioError(
    myai::ErrorCode code, const char* detail, uint32_t retry_ms) const {
  return myai::Status(code, 0, detail, retry_ms);
}

esp_err_t EspCrossCoreAudioBridge::initialize() {
  if (core_) return ESP_ERR_INVALID_STATE;
  playback_storage_ = static_cast<uint8_t*>(heap_caps_calloc(
      1, kPlaybackCapacityBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  capture_storage_ = static_cast<uint8_t*>(heap_caps_calloc(
      1, kCaptureCapacityBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!playback_storage_ || !capture_storage_) {
    releaseBuffers();
    return ESP_ERR_NO_MEM;
  }
  core_ = new (std::nothrow) DuplexPcmBridgeCore(
      playback_storage_, kPlaybackCapacityBytes, kPlaybackLowWatermark,
      kPlaybackHighWatermark, kMaximumWssAudioMessageBytes, capture_storage_,
      kCaptureCapacityBytes, kCaptureLowWatermark, kCaptureHighWatermark,
      kPumpFrameBytes);
  if (!core_ || !core_->valid()) {
    releaseBuffers();
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

void EspCrossCoreAudioBridge::releaseBuffers() {
  if (core_) {
    delete core_;
    core_ = nullptr;
  }
  if (playback_storage_) {
    heap_caps_free(playback_storage_);
    playback_storage_ = nullptr;
  }
  if (capture_storage_) {
    heap_caps_free(capture_storage_);
    capture_storage_ = nullptr;
  }
}

myai::Status EspCrossCoreAudioBridge::begin(uint32_t sample_rate_hz,
                                            uint8_t channels) {
  portENTER_CRITICAL(&mux_);
  if (!core_ || playback_hardware_ != PlaybackHardwareState::Idle) {
    portEXIT_CRITICAL(&mux_);
    return audioError(myai::ErrorCode::InvalidState,
                      "playback bridge is busy");
  }
  const uint32_t generation = core_->beginPlayback(sample_rate_hz, channels);
  if (generation == 0) {
    playback_hardware_ = PlaybackHardwareState::Fault;
    portEXIT_CRITICAL(&mux_);
    return audioError(myai::ErrorCode::InvalidArgument,
                      "playback format rejected");
  }
  playback_hardware_generation_ = generation;
  playback_sample_rate_hz_ = sample_rate_hz;
  playback_channels_ = channels;
  playback_hardware_ = PlaybackHardwareState::StartPending;
  portEXIT_CRITICAL(&mux_);
  return myai::Status::success();
}

myai::Status EspCrossCoreAudioBridge::write(const uint8_t* bytes,
                                             size_t length) {
  if (!bytes || length == 0 || length > kMaximumWssAudioMessageBytes) {
    return audioError(myai::ErrorCode::InvalidArgument,
                      "bounded playback frame rejected");
  }
  portENTER_CRITICAL(&mux_);
  if (!core_ || !core_->canAcceptPlayback(length)) {
    if (core_) ++diagnostics_.playback_overflows;
    portEXIT_CRITICAL(&mux_);
    return audioError(myai::ErrorCode::Transport,
                      "playback backpressure overflow", 5);
  }
  const PcmTransfer transfer = core_->pushPlayback(
      playback_hardware_generation_, bytes, length);
  if (transfer.buffered > diagnostics_.peak_playback_bytes)
    diagnostics_.peak_playback_bytes = transfer.buffered;
  const bool accepted = transferAccepted(transfer) && transfer.bytes == length;
  if (!accepted) ++diagnostics_.playback_overflows;
  portEXIT_CRITICAL(&mux_);
  return accepted ? myai::Status::success()
                  : audioError(myai::ErrorCode::Transport,
                               "playback ingress rejected", 5);
}

myai::Status EspCrossCoreAudioBridge::end() {
  portENTER_CRITICAL(&mux_);
  if (!core_ || playback_hardware_generation_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return audioError(myai::ErrorCode::InvalidState,
                      "playback stream is not open");
  }
  const PcmTransfer transfer =
      core_->finishPlayback(playback_hardware_generation_);
  const bool accepted = transferAccepted(transfer);
  portEXIT_CRITICAL(&mux_);
  return accepted ? myai::Status::success()
                  : audioError(myai::ErrorCode::InvalidState,
                               "playback stream close rejected");
}

void EspCrossCoreAudioBridge::abort() {
  portENTER_CRITICAL(&mux_);
  if (core_ && playback_hardware_generation_ != 0) {
    core_->cancelPlayback(playback_hardware_generation_);
    playback_hardware_ = PlaybackHardwareState::AbortPending;
    ++diagnostics_.cancellations;
  }
  portEXIT_CRITICAL(&mux_);
}

bool EspCrossCoreAudioBridge::canPollWssIngress() const {
  portENTER_CRITICAL(&mux_);
  const bool ready = !core_ ||
      (core_->playbackState() != DuplexStreamState::Open &&
       core_->playbackState() != DuplexStreamState::Draining) ||
      core_->canAcceptPlayback(kMaximumWssAudioMessageBytes);
  portEXIT_CRITICAL(&mux_);
  return ready;
}

bool EspCrossCoreAudioBridge::ingressReady(void* context) {
  return context &&
         static_cast<EspCrossCoreAudioBridge*>(context)->canPollWssIngress();
}

esp_err_t EspCrossCoreAudioBridge::servicePlayback(
    EspI2sAudioDevice& device) {
  uint32_t generation = 0;
  uint32_t sample_rate = 0;
  uint8_t channels = 0;
  PlaybackHardwareState action = PlaybackHardwareState::Idle;
  portENTER_CRITICAL(&mux_);
  if (!core_) {
    portEXIT_CRITICAL(&mux_);
    return ESP_ERR_INVALID_STATE;
  }
  action = playback_hardware_;
  generation = playback_hardware_generation_;
  sample_rate = playback_sample_rate_hz_;
  channels = playback_channels_;
  if (action == PlaybackHardwareState::StartPending)
    playback_hardware_ = PlaybackHardwareState::Starting;
  portEXIT_CRITICAL(&mux_);

  if (action == PlaybackHardwareState::StartPending) {
    const esp_err_t started = device.beginPlayback(sample_rate, channels);
    portENTER_CRITICAL(&mux_);
    if (playback_hardware_generation_ == generation &&
        playback_hardware_ == PlaybackHardwareState::Starting) {
      playback_hardware_ = started == ESP_OK ? PlaybackHardwareState::Active
                                             : PlaybackHardwareState::Fault;
      if (started != ESP_OK) {
        core_->cancelPlayback(generation);
        ++diagnostics_.playback_hardware_failures;
      }
    }
    portEXIT_CRITICAL(&mux_);
    return started;
  }
  if (action == PlaybackHardwareState::AbortPending) {
    device.abort();
    portENTER_CRITICAL(&mux_);
    playback_hardware_ = PlaybackHardwareState::Idle;
    playback_hardware_generation_ = 0;
    portEXIT_CRITICAL(&mux_);
    return ESP_OK;
  }
  if (action == PlaybackHardwareState::StopPending) {
    const esp_err_t stopped = device.endPlayback();
    portENTER_CRITICAL(&mux_);
    playback_hardware_ = stopped == ESP_OK ? PlaybackHardwareState::Idle
                                           : PlaybackHardwareState::Fault;
    if (stopped != ESP_OK) ++diagnostics_.playback_hardware_failures;
    if (stopped == ESP_OK) playback_hardware_generation_ = 0;
    portEXIT_CRITICAL(&mux_);
    return stopped;
  }
  if (action != PlaybackHardwareState::Active) return ESP_OK;

  std::array<uint8_t, kPumpFrameBytes> bytes{};
  PcmTransfer transfer;
  portENTER_CRITICAL(&mux_);
  transfer = core_->popPlayback(bytes.data(), bytes.size());
  portEXIT_CRITICAL(&mux_);
  if (transfer.bytes != 0) {
    const esp_err_t written =
        device.writePlayback(bytes.data(), transfer.bytes, 20);
    if (written != ESP_OK) {
      portENTER_CRITICAL(&mux_);
      core_->cancelPlayback(generation);
      playback_hardware_ = PlaybackHardwareState::AbortPending;
      ++diagnostics_.playback_hardware_failures;
      portEXIT_CRITICAL(&mux_);
      return written;
    }
  }
  portENTER_CRITICAL(&mux_);
  if (core_->playbackComplete() &&
      playback_hardware_ == PlaybackHardwareState::Active)
    playback_hardware_ = PlaybackHardwareState::StopPending;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

esp_err_t EspCrossCoreAudioBridge::beginCapture(EspI2sAudioDevice& device) {
  if (!core_) return ESP_ERR_INVALID_STATE;
  const esp_err_t started = device.beginCapture();
  if (started != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.capture_hardware_failures;
    portEXIT_CRITICAL(&mux_);
    return started;
  }
  portENTER_CRITICAL(&mux_);
  capture_generation_ = core_->beginCapture();
  capture_stop_sent_ = false;
  const bool valid = capture_generation_ != 0;
  portEXIT_CRITICAL(&mux_);
  if (!valid) {
    device.abort();
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t EspCrossCoreAudioBridge::captureStep(EspI2sAudioDevice& device,
                                                uint32_t timeout_ms) {
  std::array<int16_t, kPumpFrameBytes / sizeof(int16_t)> samples{};
  size_t count = 0;
  const esp_err_t read =
      device.readCapture(samples.data(), samples.size(), count, timeout_ms);
  if (read == ESP_ERR_TIMEOUT) return ESP_OK;
  if (read != ESP_OK || count == 0) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.capture_hardware_failures;
    portEXIT_CRITICAL(&mux_);
    return read == ESP_OK ? ESP_ERR_INVALID_SIZE : read;
  }
  const size_t bytes = count * sizeof(int16_t);
  portENTER_CRITICAL(&mux_);
  const PcmTransfer transfer = core_->pushCapture(
      capture_generation_, reinterpret_cast<const uint8_t*>(samples.data()),
      bytes);
  if (transfer.buffered > diagnostics_.peak_capture_bytes)
    diagnostics_.peak_capture_bytes = transfer.buffered;
  const bool accepted = transferAccepted(transfer) && transfer.bytes == bytes;
  if (!accepted) ++diagnostics_.capture_overflows;
  portEXIT_CRITICAL(&mux_);
  if (!accepted) {
    device.abort();
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t EspCrossCoreAudioBridge::finishCapture(EspI2sAudioDevice& device) {
  const esp_err_t stopped = device.endCapture();
  portENTER_CRITICAL(&mux_);
  const PcmTransfer transfer = stopped == ESP_OK && core_
      ? core_->finishCapture(capture_generation_)
      : PcmTransfer{PcmFlowSignal::Closed, 0, 0};
  if (stopped != ESP_OK) ++diagnostics_.capture_hardware_failures;
  portEXIT_CRITICAL(&mux_);
  return stopped == ESP_OK && transferAccepted(transfer)
             ? ESP_OK
             : (stopped == ESP_OK ? ESP_ERR_INVALID_STATE : stopped);
}

void EspCrossCoreAudioBridge::cancelCapture(EspI2sAudioDevice& device) {
  device.abort();
  portENTER_CRITICAL(&mux_);
  if (core_ && capture_generation_ != 0)
    core_->cancelCapture(capture_generation_);
  capture_generation_ = 0;
  capture_stop_sent_ = false;
  ++diagnostics_.cancellations;
  portEXIT_CRITICAL(&mux_);
}

myai::Status EspCrossCoreAudioBridge::pumpCaptureToNetwork(
    myai::MyAiClient& client) {
  std::array<uint8_t, kPumpFrameBytes> bytes{};
  PcmTransfer transfer;
  bool should_stop = false;
  portENTER_CRITICAL(&mux_);
  if (!core_ || capture_generation_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return audioError(myai::ErrorCode::InvalidState,
                      "capture bridge is not active");
  }
  transfer = core_->popCapture(bytes.data(), bytes.size());
  should_stop = core_->captureComplete() && !capture_stop_sent_;
  portEXIT_CRITICAL(&mux_);
  if (transfer.bytes != 0) {
    const myai::Status sent = client.sendPcm16(bytes.data(), transfer.bytes);
    if (!sent.ok()) {
      portENTER_CRITICAL(&mux_);
      ++diagnostics_.capture_network_failures;
      portEXIT_CRITICAL(&mux_);
      return sent;
    }
  }
  if (should_stop) {
    const myai::Status stopped = client.endVoiceTurn();
    if (!stopped.ok()) {
      portENTER_CRITICAL(&mux_);
      ++diagnostics_.capture_network_failures;
      portEXIT_CRITICAL(&mux_);
      return stopped;
    }
    portENTER_CRITICAL(&mux_);
    capture_stop_sent_ = true;
    capture_generation_ = 0;
    portEXIT_CRITICAL(&mux_);
  }
  return myai::Status::success();
}

bool EspCrossCoreAudioBridge::playbackBusy() const {
  portENTER_CRITICAL(&mux_);
  const bool busy = playback_hardware_ != PlaybackHardwareState::Idle;
  portEXIT_CRITICAL(&mux_);
  return busy;
}

bool EspCrossCoreAudioBridge::captureBusy() const {
  portENTER_CRITICAL(&mux_);
  const bool busy = capture_generation_ != 0;
  portEXIT_CRITICAL(&mux_);
  return busy;
}

bool EspCrossCoreAudioBridge::playbackComplete() const {
  portENTER_CRITICAL(&mux_);
  const bool complete = core_ && core_->playbackComplete() &&
      playback_hardware_ == PlaybackHardwareState::Idle;
  portEXIT_CRITICAL(&mux_);
  return complete;
}

uint32_t EspCrossCoreAudioBridge::captureGeneration() const {
  portENTER_CRITICAL(&mux_);
  const uint32_t value = capture_generation_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

CrossCoreAudioDiagnostics EspCrossCoreAudioBridge::diagnostics() const {
  portENTER_CRITICAL(&mux_);
  const CrossCoreAudioDiagnostics value = diagnostics_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

}  // namespace inkloop
