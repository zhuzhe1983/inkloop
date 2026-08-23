#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "inkloop/diagnostics/serial_command_parser.hpp"
#include "inkloop/diagnostics/serial_diagnostic_events.hpp"

namespace inkloop::diagnostics {

using SerialDiagnosticCommandHandler = void (*)(SerialCommand command,
                                                 void* context);

struct EspSerialDiagnosticsSnapshot {
  uint32_t parsed_commands = 0U;
  uint32_t rejected_lines = 0U;
  uint32_t event_queue_drops = 0U;
  uint32_t write_failures = 0U;
  bool transport_available = false;
};

// ESP-IDF transport for the native USB Serial/JTAG port. It does not own a
// task: Product calls service() from the existing lowest-priority Portal lane.
// That preserves button/voice responsiveness and avoids a ninth scheduler
// owner solely for diagnostics.
class EspSerialDiagnosticsOwner final : public ISerialDiagnosticEventSink {
 public:
  EspSerialDiagnosticsOwner() = default;
  ~EspSerialDiagnosticsOwner();

  EspSerialDiagnosticsOwner(const EspSerialDiagnosticsOwner&) = delete;
  EspSerialDiagnosticsOwner& operator=(const EspSerialDiagnosticsOwner&) =
      delete;

  // Optional on SKUs without native USB Serial/JTAG: configuration succeeds
  // with transport_available=false rather than preventing the product boot.
  esp_err_t configure(SerialDiagnosticCommandHandler handler, void* context);
  void shutdown();
  void service();
  bool postSerialDiagnosticEvent(
      const SerialDiagnosticEvent& event) override;
  EspSerialDiagnosticsSnapshot snapshot() const;

 private:
  static constexpr size_t kEventDepth = 32U;
  static constexpr size_t kReadBytesPerTick = 64U;
  static constexpr size_t kEventsPerTick = 16U;
  static constexpr size_t kMaximumFrameBytes = 192U;

  void handleParseResult(const SerialParseResult& result);
  void drainEvents();

  SerialCommandParser parser_{};
  StaticQueue_t event_queue_storage_{};
  alignas(SerialDiagnosticEvent)
      std::array<uint8_t, sizeof(SerialDiagnosticEvent) * kEventDepth>
          event_queue_bytes_{};
  QueueHandle_t event_queue_ = nullptr;
  SerialDiagnosticCommandHandler handler_ = nullptr;
  void* handler_context_ = nullptr;
  int descriptor_ = -1;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  EspSerialDiagnosticsSnapshot diagnostics_{};
};

}  // namespace inkloop::diagnostics
