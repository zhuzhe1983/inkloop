#include "inkloop/diagnostics/esp_serial_diagnostics.hpp"

#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

namespace inkloop::diagnostics {
namespace {

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
constexpr size_t kUsbRxBufferBytes = 256U;
constexpr size_t kUsbTxBufferBytes = 1024U;
#endif

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

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = kUsbRxBufferBytes;
    config.tx_buffer_size = kUsbTxBufferBytes;
    if (usb_serial_jtag_driver_install(&config) == ESP_OK)
      usb_driver_owned_ = true;
  }
  transport_available_ = usb_serial_jtag_is_driver_installed();
  if (transport_available_) usb_serial_jtag_vfs_use_driver();
#endif
  portENTER_CRITICAL(&mux_);
  diagnostics_ = EspSerialDiagnosticsSnapshot{};
  diagnostics_.transport_available = transport_available_;
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

void EspSerialDiagnosticsOwner::shutdown() {
  handler_ = nullptr;
  handler_context_ = nullptr;
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  if (usb_driver_owned_) {
    usb_serial_jtag_vfs_use_nonblocking();
    (void)usb_serial_jtag_driver_uninstall();
  }
#endif
  usb_driver_owned_ = false;
  transport_available_ = false;
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
  if (!event_queue_ || !transport_available_) return false;
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
  if (!event_queue_ || !transport_available_) return;
  for (size_t count = 0; count < kEventsPerTick; ++count) {
    SerialDiagnosticEvent event;
    if (xQueueReceive(event_queue_, &event, 0) != pdPASS) break;
    std::array<char, kMaximumFrameBytes> frame{};
    const size_t bytes = formatSerialDiagnosticEvent(
        event, frame.data(), frame.size());
    bool written = false;
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    written = bytes != 0U && usb_serial_jtag_write_bytes(
        frame.data(), bytes, 0) == static_cast<int>(bytes);
#endif
    if (!written) {
      portENTER_CRITICAL(&mux_);
      ++diagnostics_.write_failures;
      portEXIT_CRITICAL(&mux_);
    }
  }
}

void EspSerialDiagnosticsOwner::service() {
  if (!event_queue_ || !transport_available_) return;
  std::array<uint8_t, kReadBytesPerTick> input{};
  int read_bytes = 0;
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  read_bytes = usb_serial_jtag_read_bytes(
      input.data(), static_cast<uint32_t>(input.size()), 0);
#endif
  if (read_bytes > 0) {
    for (int index = 0; index < read_bytes; ++index) {
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
