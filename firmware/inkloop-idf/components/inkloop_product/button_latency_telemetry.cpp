#include "inkloop/button_latency_telemetry.hpp"

#include <limits>

namespace inkloop {

static_assert(static_cast<uint8_t>(ButtonLatencyButton::Previous) ==
              static_cast<uint8_t>(
                  diagnostics::SerialDiagnosticButton::Previous));
static_assert(static_cast<uint8_t>(ButtonLatencyButton::Top) ==
              static_cast<uint8_t>(diagnostics::SerialDiagnosticButton::Top));
static_assert(static_cast<uint8_t>(ButtonLatencyOutcome::Led) ==
              static_cast<uint8_t>(
                  diagnostics::SerialDiagnosticButtonOutcome::Led));
static_assert(static_cast<uint8_t>(ButtonLatencyOutcome::NotReady) ==
              static_cast<uint8_t>(
                  diagnostics::SerialDiagnosticButtonOutcome::NotReady));

void ButtonLatencyTelemetry::attach(
    diagnostics::ISerialDiagnosticEventSink& sink) {
  portENTER_CRITICAL(&mux_);
  sink_ = &sink;
  portEXIT_CRITICAL(&mux_);
}

void ButtonLatencyTelemetry::detach() {
  portENTER_CRITICAL(&mux_);
  sink_ = nullptr;
  portEXIT_CRITICAL(&mux_);
}

void ButtonLatencyTelemetry::recordCapture(
    uint64_t event_id, ButtonLatencyButton button, uint32_t captured_us) {
  if (!isButtonLatencyEventId(event_id)) return;
  portENTER_CRITICAL(&mux_);
  (void)core_.recordCapture(event_id, button, captured_us);
  portEXIT_CRITICAL(&mux_);
}

void ButtonLatencyTelemetry::recordControlAdmission(
    uint64_t event_id, uint32_t admitted_us) {
  if (!isButtonLatencyEventId(event_id)) return;
  portENTER_CRITICAL(&mux_);
  (void)core_.recordControlAdmission(event_id, admitted_us);
  portEXIT_CRITICAL(&mux_);
}

void ButtonLatencyTelemetry::recordFeedback(
    uint64_t event_id, ButtonLatencyOutcome feedback, uint32_t feedback_us) {
  if (!isButtonLatencyEventId(event_id)) return;
  if (feedback != ButtonLatencyOutcome::Led &&
      feedback != ButtonLatencyOutcome::Navigation) {
    return;
  }
  portENTER_CRITICAL(&mux_);
  (void)core_.complete(event_id, feedback, feedback_us);
  portEXIT_CRITICAL(&mux_);
}

void ButtonLatencyTelemetry::recordTerminal(
    uint64_t event_id, ButtonLatencyOutcome outcome, uint32_t terminal_us) {
  if (!isButtonLatencyEventId(event_id)) return;
  if (outcome != ButtonLatencyOutcome::Debounced &&
      outcome != ButtonLatencyOutcome::NotReady) {
    return;
  }
  portENTER_CRITICAL(&mux_);
  (void)core_.complete(event_id, outcome, terminal_us);
  portEXIT_CRITICAL(&mux_);
}

diagnostics::SerialDiagnosticEvent ButtonLatencyTelemetry::makeEvent(
    const ButtonLatencyObservation& observation) {
  diagnostics::SerialDiagnosticEvent event;
  event.kind = diagnostics::SerialDiagnosticEventKind::ButtonLatency;
  event.correlation = observation.event_id;
  event.first = observation.captured_us;
  event.second = observation.control_admitted
      ? observation.control_admitted_us : 0U;
  event.third = observation.terminal_us;
  event.code = static_cast<uint8_t>(observation.button);
  event.flags = static_cast<uint8_t>(observation.outcome);
  return event;
}

void ButtonLatencyTelemetry::service(uint32_t now_us) {
  diagnostics::ISerialDiagnosticEventSink* sink = nullptr;
  portENTER_CRITICAL(&mux_);
  core_.expire(now_us, kFeedbackTimeoutUs);
  sink = sink_;
  portEXIT_CRITICAL(&mux_);
  if (!sink) return;

  for (size_t count = 0U; count < kMaximumEventsPerService; ++count) {
    ButtonLatencyObservation observation;
    portENTER_CRITICAL(&mux_);
    const bool available = core_.peek(observation);
    portEXIT_CRITICAL(&mux_);
    if (!available) break;
    const diagnostics::SerialDiagnosticEvent event = makeEvent(observation);
    const bool posted = sink->postSerialDiagnosticEvent(event);
    portENTER_CRITICAL(&mux_);
    if (!posted && sink_drops_ != std::numeric_limits<uint32_t>::max()) {
      ++sink_drops_;
    }
    (void)core_.pop(observation.event_id);
    portEXIT_CRITICAL(&mux_);
  }
}

ButtonLatencyCoreSnapshot ButtonLatencyTelemetry::snapshot() const {
  portENTER_CRITICAL(&mux_);
  ButtonLatencyCoreSnapshot output = core_.snapshot();
  output.sink_drops = sink_drops_;
  portEXIT_CRITICAL(&mux_);
  return output;
}

void ButtonLatencyTelemetry::controlAdmissionCallback(
    uint64_t event_id, uint32_t admitted_us, void* context) {
  if (context) {
    static_cast<ButtonLatencyTelemetry*>(context)->recordControlAdmission(
        event_id, admitted_us);
  }
}

}  // namespace inkloop
