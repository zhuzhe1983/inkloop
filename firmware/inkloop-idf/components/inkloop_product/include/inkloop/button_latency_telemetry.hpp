#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "inkloop/button_latency_core.hpp"
#include "inkloop/diagnostics/serial_diagnostic_events.hpp"

namespace inkloop {

// Cross-core wrapper around ButtonLatencyCore. ISR/high-priority callers only
// copy fixed numeric records. service() is called by the lowest-priority
// Portal lane and is the sole path that touches the serial diagnostics queue.
class ButtonLatencyTelemetry final {
 public:
  static constexpr uint32_t kFeedbackTimeoutUs = 500000U;
  static constexpr size_t kMaximumEventsPerService = 16U;

  void attach(diagnostics::ISerialDiagnosticEventSink& sink);
  void detach();

  void recordCapture(uint64_t event_id, ButtonLatencyButton button,
                     uint32_t captured_us);
  void recordControlAdmission(uint64_t event_id,
                              uint32_t admitted_us);
  void recordFeedback(uint64_t event_id, ButtonLatencyOutcome feedback,
                      uint32_t feedback_us);
  void recordTerminal(uint64_t event_id, ButtonLatencyOutcome outcome,
                      uint32_t terminal_us);
  void service(uint32_t now_us);
  ButtonLatencyCoreSnapshot snapshot() const;

  static void controlAdmissionCallback(uint64_t event_id,
                                       uint32_t admitted_us,
                                       void* context);

 private:
  static diagnostics::SerialDiagnosticEvent makeEvent(
      const ButtonLatencyObservation& observation);

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  ButtonLatencyCore core_{};
  diagnostics::ISerialDiagnosticEventSink* sink_ = nullptr;
  uint32_t sink_drops_ = 0U;
};

}  // namespace inkloop
