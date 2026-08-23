#include "inkloop/diagnostics/esp_serial_diagnostics.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "sdkconfig.h"

namespace inkloop::diagnostics {
namespace {

const char* serialDevicePath() {
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
  return "/dev/secondary";
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return "/dev/usbserjtag";
#else
  return nullptr;
#endif
}

}  // namespace

EspSerialDiagnosticsOwner::~EspSerialDiagnosticsOwner() { shutdown(); }

esp_err_t EspSerialDiagnosticsOwner::configure(
    SerialDiagnosticCommandHandler handler, void* context) {
  if (!handler || event_queue_) return ESP_ERR_INVALID_STATE;
  event_queue_ = xQueueCreateStatic(
      static_cast<UBaseType_t>(kEventDepth), sizeof(SerialDiagnosticEvent),
      event_queue_bytes_.data(), &event_queue_storage_);
  if (!event_queue_) return ESP_ERR_NO_MEM;
  handler_ = handler;
  handler_context_ = context;

  const char* path = serialDevicePath();
  if (path) descriptor_ = ::open(path, O_RDWR | O_NONBLOCK);
  portENTER_CRITICAL(&mux_);
  diagnostics_ = EspSerialDiagnosticsSnapshot{};
  diagnostics_.transport_available = descriptor_ >= 0;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

void EspSerialDiagnosticsOwner::shutdown() {
  handler_ = nullptr;
  handler_context_ = nullptr;
  if (descriptor_ >= 0) {
    ::close(descriptor_);
    descriptor_ = -1;
  }
  if (event_queue_) {
    xQueueReset(event_queue_);
    vQueueDelete(event_queue_);
    event_queue_ = nullptr;
  }
  parser_.reset();
  portENTER_CRITICAL(&mux_);
  diagnostics_.transport_available = false;
  portEXIT_CRITICAL(&mux_);
}

bool EspSerialDiagnosticsOwner::postSerialDiagnosticEvent(
    const SerialDiagnosticEvent& event) {
  if (!event_queue_ || descriptor_ < 0) return false;
  if (xQueueSend(event_queue_, &event, 0) == pdPASS) return true;
  portENTER_CRITICAL(&mux_);
  ++diagnostics_.event_queue_drops;
  portEXIT_CRITICAL(&mux_);
  return false;
}

void EspSerialDiagnosticsOwner::handleParseResult(
    const SerialParseResult& result) {
  if (result.code == SerialParseCode::Pending ||
      result.code == SerialParseCode::Empty) {
    return;
  }
  if (result.code != SerialParseCode::Command) {
    SerialDiagnosticEvent rejected;
    rejected.kind = SerialDiagnosticEventKind::CommandRejected;
    rejected.code = static_cast<uint8_t>(result.code);
    (void)postSerialDiagnosticEvent(rejected);
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.rejected_lines;
    portEXIT_CRITICAL(&mux_);
    return;
  }

  SerialDiagnosticEvent accepted;
  accepted.kind = SerialDiagnosticEventKind::Command;
  accepted.command = result.command;
  (void)postSerialDiagnosticEvent(accepted);
  portENTER_CRITICAL(&mux_);
  ++diagnostics_.parsed_commands;
  portEXIT_CRITICAL(&mux_);
  if (handler_) handler_(result.command, handler_context_);
}

void EspSerialDiagnosticsOwner::drainEvents() {
  if (!event_queue_ || descriptor_ < 0) return;
  for (size_t count = 0; count < kEventsPerTick; ++count) {
    SerialDiagnosticEvent event;
    if (xQueueReceive(event_queue_, &event, 0) != pdPASS) break;
    std::array<char, kMaximumFrameBytes> frame{};
    const size_t bytes = formatSerialDiagnosticEvent(
        event, frame.data(), frame.size());
    if (bytes == 0U || ::write(descriptor_, frame.data(), bytes) !=
                           static_cast<ssize_t>(bytes)) {
      portENTER_CRITICAL(&mux_);
      ++diagnostics_.write_failures;
      portEXIT_CRITICAL(&mux_);
    }
  }
}

void EspSerialDiagnosticsOwner::service() {
  if (!event_queue_ || descriptor_ < 0) return;
  std::array<uint8_t, kReadBytesPerTick> input{};
  const ssize_t read_bytes = ::read(descriptor_, input.data(), input.size());
  if (read_bytes > 0) {
    for (ssize_t index = 0; index < read_bytes; ++index) {
      handleParseResult(parser_.consume(input[static_cast<size_t>(index)]));
    }
  }
  drainEvents();
}

EspSerialDiagnosticsSnapshot EspSerialDiagnosticsOwner::snapshot() const {
  portENTER_CRITICAL(&mux_);
  const EspSerialDiagnosticsSnapshot output = diagnostics_;
  portEXIT_CRITICAL(&mux_);
  return output;
}

}  // namespace inkloop::diagnostics
