#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

inline constexpr uint64_t kButtonLatencyEventMarker = 1ULL << 63U;

inline constexpr uint64_t buttonLatencyEventId(uint64_t sequence) {
  const uint64_t bounded = sequence & ~kButtonLatencyEventMarker;
  return kButtonLatencyEventMarker | (bounded == 0U ? 1U : bounded);
}

inline constexpr bool isButtonLatencyEventId(uint64_t event_id) {
  return (event_id & kButtonLatencyEventMarker) != 0U;
}

// Stable values used by both the fixed-size runtime tracker and the serial
// bench protocol. Do not reorder: recorded beta evidence is parsed by value.
enum class ButtonLatencyButton : uint8_t {
  Previous = 0U,
  Next = 1U,
  Top = 2U,
};

// Led and Navigation are successful first-feedback milestones. Debounced is a
// valid raw GPIO edge rejected by the input policy; NotReady is a fail-closed
// terminal event (queue rejection, missing owner feedback, or timeout).
enum class ButtonLatencyOutcome : uint8_t {
  Led = 0U,
  Navigation = 1U,
  Debounced = 2U,
  NotReady = 3U,
};

struct ButtonLatencyObservation {
  uint64_t event_id = 0U;
  uint32_t captured_us = 0U;
  uint32_t control_admitted_us = 0U;
  uint32_t terminal_us = 0U;
  ButtonLatencyButton button = ButtonLatencyButton::Previous;
  ButtonLatencyOutcome outcome = ButtonLatencyOutcome::NotReady;
  bool control_admitted = false;
};

struct ButtonLatencyCoreSnapshot {
  uint32_t captured = 0U;
  uint32_t completed = 0U;
  uint32_t active_overwrites = 0U;
  uint32_t completion_drops = 0U;
  // A completed sample gets exactly one non-blocking delivery attempt. A full
  // or unavailable serial sink increments this counter instead of turning a
  // single button edge into periodic retry traffic.
  uint32_t sink_drops = 0U;
  uint32_t orphan_updates = 0U;
  size_t active = 0U;
  size_t pending = 0U;
};

// Allocation-free state machine. External owners provide cross-core
// serialization; this class performs no logging, I/O, heap work, or locking.
class ButtonLatencyCore final {
 public:
  static constexpr size_t kActiveCapacity = 64U;
  static constexpr size_t kCompletedCapacity = 64U;

  bool recordCapture(uint64_t event_id, ButtonLatencyButton button,
                     uint32_t captured_us);
  bool recordControlAdmission(uint64_t event_id,
                              uint32_t control_admitted_us);
  bool complete(uint64_t event_id, ButtonLatencyOutcome outcome,
                uint32_t terminal_us);
  void expire(uint32_t now_us, uint32_t timeout_us);

  bool peek(ButtonLatencyObservation& observation) const;
  bool pop(uint64_t expected_event_id);
  ButtonLatencyCoreSnapshot snapshot() const;

  static constexpr uint32_t elapsedUs(uint32_t later, uint32_t earlier) {
    return later - earlier;
  }

 private:
  struct ActiveEvent {
    uint64_t event_id = 0U;
    uint32_t captured_us = 0U;
    uint32_t control_admitted_us = 0U;
    ButtonLatencyButton button = ButtonLatencyButton::Previous;
    bool control_admitted = false;
  };

  static uint32_t increment(uint32_t value);
  static size_t activeIndex(uint64_t event_id);
  bool queueCompletion(const ActiveEvent& active,
                       ButtonLatencyOutcome outcome,
                       uint32_t terminal_us);
  void clearActive(size_t index);

  std::array<ActiveEvent, kActiveCapacity> active_{};
  std::array<ButtonLatencyObservation, kCompletedCapacity> completed_{};
  size_t completed_head_ = 0U;
  size_t completed_count_ = 0U;
  ButtonLatencyCoreSnapshot diagnostics_{};
};

}  // namespace inkloop
