#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/work_contracts.hpp"

namespace inkloop {

enum class AdmissionResult : uint8_t {
  Admitted,
  NotReady,
  InvalidEnvelope,
  WrongLane,
  StaleGeneration,
  Expired,
  QueueFull,
  Underflow,
};

struct AdmissionSnapshot {
  std::array<size_t, kTaskLaneCount> used{};
  std::array<uint64_t, kWorkClassCount> generation_floor{};
};

// Portable single-lock policy used by RuntimeSupervisor. Callers provide the
// cross-core lock; this class deliberately performs no allocation or logging.
class RuntimeAdmission {
 public:
  RuntimeAdmission();
  explicit RuntimeAdmission(
      const std::array<size_t, kTaskLaneCount>& capacities);

  static TaskLane routeFor(const WorkEnvelope& envelope);
  static bool deadlineExpired(uint32_t now_ms, uint32_t deadline_ms);

  AdmissionResult admit(TaskLane lane, const WorkEnvelope& envelope,
                        uint32_t now_ms);
  AdmissionResult shouldExecute(TaskLane lane, const WorkEnvelope& envelope,
                                uint32_t now_ms) const;
  AdmissionResult release(TaskLane lane);
  AdmissionResult cancelBefore(WorkClass work_class,
                               uint64_t generation_floor);

  size_t used(TaskLane lane) const;
  size_t capacity(TaskLane lane) const;
  uint64_t generationFloor(WorkClass work_class) const;
  AdmissionSnapshot snapshot() const;

 private:
  AdmissionResult validate(TaskLane lane, const WorkEnvelope& envelope,
                           uint32_t now_ms) const;

  std::array<size_t, kTaskLaneCount> capacities_{};
  AdmissionSnapshot state_{};
};

const char* admissionResultName(AdmissionResult result);

}  // namespace inkloop
