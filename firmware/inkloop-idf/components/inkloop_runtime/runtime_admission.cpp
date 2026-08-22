#include "inkloop/runtime_admission.hpp"

namespace inkloop {
namespace {

bool validLane(TaskLane lane) {
  return taskLaneIndex(lane) < kTaskLaneCount;
}

bool validWorkClass(WorkClass work_class) {
  return workClassIndex(work_class) < kWorkClassCount;
}

}  // namespace

RuntimeAdmission::RuntimeAdmission() : RuntimeAdmission(kTaskQueueDepths) {}

RuntimeAdmission::RuntimeAdmission(
    const std::array<size_t, kTaskLaneCount>& capacities)
    : capacities_(capacities) {}

TaskLane RuntimeAdmission::routeFor(const WorkEnvelope& envelope) {
  if (envelope.kind == EnvelopeKind::Result) return TaskLane::Control;

  switch (envelope.work_class) {
    case WorkClass::Button:
      return TaskLane::Input;
    case WorkClass::Voice:
      return TaskLane::Voice;
    case WorkClass::Control:
      return TaskLane::Control;
    case WorkClass::LedStatus:
      return TaskLane::Led;
    case WorkClass::Display:
      return TaskLane::Display;
    case WorkClass::Storage:
      return TaskLane::Storage;
    case WorkClass::InkloopNetwork:
    case WorkClass::MyAiNetwork:
      return TaskLane::Network;
    case WorkClass::Portal:
      return TaskLane::Portal;
    case WorkClass::Count:
      return TaskLane::Count;
  }
  return TaskLane::Count;
}

bool RuntimeAdmission::deadlineExpired(uint32_t now_ms,
                                       uint32_t deadline_ms) {
  if (deadline_ms == 0) return false;
  // Deadlines are required to be within INT32_MAX milliseconds of admission.
  // Signed subtraction then remains correct across the uint32_t wrap boundary.
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

AdmissionResult RuntimeAdmission::validate(TaskLane lane,
                                           const WorkEnvelope& envelope,
                                           uint32_t now_ms) const {
  if (!validLane(lane) || !validWorkClass(envelope.work_class) ||
      envelope.generation == 0 || envelope.request_id == 0 ||
      envelope.payload_bytes > kMaxReferencedPayloadBytes ||
      (envelope.kind == EnvelopeKind::Command &&
       envelope.disposition != WorkDisposition::Accepted) ||
      (envelope.kind == EnvelopeKind::Result &&
       envelope.disposition == WorkDisposition::Accepted)) {
    return AdmissionResult::InvalidEnvelope;
  }
  if (routeFor(envelope) != lane) return AdmissionResult::WrongLane;
  if (envelope.generation <
      state_.generation_floor[workClassIndex(envelope.work_class)]) {
    return AdmissionResult::StaleGeneration;
  }
  if (deadlineExpired(now_ms, envelope.deadline_ms)) {
    return AdmissionResult::Expired;
  }
  return AdmissionResult::Admitted;
}

AdmissionResult RuntimeAdmission::admit(TaskLane lane,
                                        const WorkEnvelope& envelope,
                                        uint32_t now_ms) {
  const AdmissionResult validation = validate(lane, envelope, now_ms);
  if (validation != AdmissionResult::Admitted) return validation;

  const size_t index = taskLaneIndex(lane);
  if (capacities_[index] == 0 || state_.used[index] >= capacities_[index]) {
    return AdmissionResult::QueueFull;
  }
  ++state_.used[index];
  return AdmissionResult::Admitted;
}

AdmissionResult RuntimeAdmission::shouldExecute(
    TaskLane lane, const WorkEnvelope& envelope, uint32_t now_ms) const {
  return validate(lane, envelope, now_ms);
}

AdmissionResult RuntimeAdmission::release(TaskLane lane) {
  if (!validLane(lane)) return AdmissionResult::WrongLane;
  const size_t index = taskLaneIndex(lane);
  if (state_.used[index] == 0) return AdmissionResult::Underflow;
  --state_.used[index];
  return AdmissionResult::Admitted;
}

AdmissionResult RuntimeAdmission::cancelBefore(WorkClass work_class,
                                               uint64_t generation_floor) {
  if (!validWorkClass(work_class) || generation_floor == 0) {
    return AdmissionResult::InvalidEnvelope;
  }
  uint64_t& current = state_.generation_floor[workClassIndex(work_class)];
  if (generation_floor > current) current = generation_floor;
  return AdmissionResult::Admitted;
}

size_t RuntimeAdmission::used(TaskLane lane) const {
  return validLane(lane) ? state_.used[taskLaneIndex(lane)] : 0;
}

size_t RuntimeAdmission::capacity(TaskLane lane) const {
  return validLane(lane) ? capacities_[taskLaneIndex(lane)] : 0;
}

uint64_t RuntimeAdmission::generationFloor(WorkClass work_class) const {
  return validWorkClass(work_class)
             ? state_.generation_floor[workClassIndex(work_class)]
             : 0;
}

AdmissionSnapshot RuntimeAdmission::snapshot() const { return state_; }

const char* admissionResultName(AdmissionResult result) {
  switch (result) {
    case AdmissionResult::Admitted:
      return "ADMITTED";
    case AdmissionResult::NotReady:
      return "NOT_READY";
    case AdmissionResult::InvalidEnvelope:
      return "INVALID_ENVELOPE";
    case AdmissionResult::WrongLane:
      return "WRONG_LANE";
    case AdmissionResult::StaleGeneration:
      return "STALE_GENERATION";
    case AdmissionResult::Expired:
      return "EXPIRED";
    case AdmissionResult::QueueFull:
      return "QUEUE_FULL";
    case AdmissionResult::Underflow:
      return "UNDERFLOW";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
