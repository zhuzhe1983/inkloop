#include "inkloop/button_latency_core.hpp"

#include <limits>

namespace inkloop {

uint32_t ButtonLatencyCore::increment(uint32_t value) {
  return value == std::numeric_limits<uint32_t>::max() ? value : value + 1U;
}

size_t ButtonLatencyCore::activeIndex(uint64_t event_id) {
  return static_cast<size_t>(event_id % kActiveCapacity);
}

void ButtonLatencyCore::clearActive(size_t index) {
  if (index >= active_.size() || active_[index].event_id == 0U) return;
  active_[index] = ActiveEvent{};
  if (diagnostics_.active != 0U) --diagnostics_.active;
}

bool ButtonLatencyCore::recordCapture(uint64_t event_id,
                                      ButtonLatencyButton button,
                                      uint32_t captured_us) {
  if (event_id == 0U ||
      button > ButtonLatencyButton::Top) {
    return false;
  }
  const size_t index = activeIndex(event_id);
  if (active_[index].event_id != 0U) {
    diagnostics_.active_overwrites = increment(
        diagnostics_.active_overwrites);
    clearActive(index);
  }
  active_[index].event_id = event_id;
  active_[index].captured_us = captured_us;
  active_[index].button = button;
  ++diagnostics_.active;
  diagnostics_.captured = increment(diagnostics_.captured);
  return true;
}

bool ButtonLatencyCore::recordControlAdmission(
    uint64_t event_id, uint32_t control_admitted_us) {
  if (event_id == 0U) return false;
  ActiveEvent& active = active_[activeIndex(event_id)];
  if (active.event_id != event_id || active.control_admitted) {
    diagnostics_.orphan_updates = increment(diagnostics_.orphan_updates);
    return false;
  }
  active.control_admitted_us = control_admitted_us;
  active.control_admitted = true;
  return true;
}

bool ButtonLatencyCore::queueCompletion(const ActiveEvent& active,
                                        ButtonLatencyOutcome outcome,
                                        uint32_t terminal_us) {
  if (completed_count_ >= completed_.size()) {
    diagnostics_.completion_drops = increment(
        diagnostics_.completion_drops);
    return false;
  }
  const size_t tail =
      (completed_head_ + completed_count_) % completed_.size();
  completed_[tail] = {
      active.event_id,
      active.captured_us,
      active.control_admitted_us,
      terminal_us,
      active.button,
      outcome,
      active.control_admitted,
  };
  ++completed_count_;
  diagnostics_.pending = completed_count_;
  diagnostics_.completed = increment(diagnostics_.completed);
  return true;
}

bool ButtonLatencyCore::complete(uint64_t event_id,
                                 ButtonLatencyOutcome outcome,
                                 uint32_t terminal_us) {
  if (event_id == 0U || outcome > ButtonLatencyOutcome::NotReady) {
    return false;
  }
  const size_t index = activeIndex(event_id);
  const ActiveEvent active = active_[index];
  if (active.event_id != event_id) {
    diagnostics_.orphan_updates = increment(diagnostics_.orphan_updates);
    return false;
  }
  // A successful first-feedback sample is meaningful only after the exact
  // Input result was admitted to Control. Terminal failure/debounce records
  // remain useful even when Control could not accept the event.
  if ((outcome == ButtonLatencyOutcome::Led ||
       outcome == ButtonLatencyOutcome::Navigation) &&
      !active.control_admitted) {
    diagnostics_.orphan_updates = increment(diagnostics_.orphan_updates);
    return false;
  }
  const bool queued = queueCompletion(active, outcome, terminal_us);
  clearActive(index);
  return queued;
}

void ButtonLatencyCore::expire(uint32_t now_us, uint32_t timeout_us) {
  if (timeout_us == 0U) return;
  for (size_t index = 0U; index < active_.size(); ++index) {
    const ActiveEvent active = active_[index];
    if (active.event_id == 0U ||
        elapsedUs(now_us, active.captured_us) < timeout_us) {
      continue;
    }
    (void)queueCompletion(active, ButtonLatencyOutcome::NotReady, now_us);
    clearActive(index);
  }
}

bool ButtonLatencyCore::peek(ButtonLatencyObservation& observation) const {
  if (completed_count_ == 0U) return false;
  observation = completed_[completed_head_];
  return true;
}

bool ButtonLatencyCore::pop(uint64_t expected_event_id) {
  if (completed_count_ == 0U || expected_event_id == 0U ||
      completed_[completed_head_].event_id != expected_event_id) {
    return false;
  }
  completed_[completed_head_] = ButtonLatencyObservation{};
  completed_head_ = (completed_head_ + 1U) % completed_.size();
  --completed_count_;
  diagnostics_.pending = completed_count_;
  return true;
}

ButtonLatencyCoreSnapshot ButtonLatencyCore::snapshot() const {
  ButtonLatencyCoreSnapshot output = diagnostics_;
  output.active = diagnostics_.active;
  output.pending = completed_count_;
  return output;
}

}  // namespace inkloop
