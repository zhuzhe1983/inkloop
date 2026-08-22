#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

enum class WorkClass : uint8_t {
  Button,
  Voice,
  Control,
  LedStatus,
  Display,
  Storage,
  InkloopNetwork,
  MyAiNetwork,
  Portal,
  Count,
};

enum class TaskLane : uint8_t {
  Input,
  Voice,
  Control,
  Led,
  Storage,
  Display,
  Network,
  Portal,
  Count,
};

enum class EnvelopeKind : uint8_t {
  Command,
  Result,
};

enum class WorkDisposition : uint8_t {
  Accepted,
  Busy,
  Cancelled,
  TimedOut,
  Failed,
  Complete,
};

// Cross-task payloads carry IDs, bounded POD fields or references to a buffer
// owned by a declared pool. Raw service objects, FILE handles and mutable C++
// containers never cross task boundaries.
struct WorkEnvelope {
  uint64_t generation = 0;
  uint64_t request_id = 0;
  uint32_t deadline_ms = 0;
  uint32_t payload_bytes = 0;
  // Product-specific operation. Any referenced object is resolved through a
  // bounded owner-managed pool keyed by request_id; pointers never live here.
  uint16_t opcode = 0;
  WorkClass work_class = WorkClass::Control;
  EnvelopeKind kind = EnvelopeKind::Command;
  WorkDisposition disposition = WorkDisposition::Accepted;
  uint8_t flags = 0;
};

inline constexpr size_t kInputQueueDepth = 32;
inline constexpr size_t kVoiceQueueDepth = 32;
inline constexpr size_t kControlQueueDepth = 32;
inline constexpr size_t kSlowWorkQueueDepth = 8;
inline constexpr size_t kPortalQueueDepth = 4;
inline constexpr size_t kLedQueueDepth = 16;
inline constexpr size_t kDisplayQueueDepth = 4;
inline constexpr size_t kTaskLaneCount = static_cast<size_t>(TaskLane::Count);
inline constexpr size_t kWorkClassCount = static_cast<size_t>(WorkClass::Count);
inline constexpr uint32_t kMaxReferencedPayloadBytes = 4U * 1024U * 1024U;

inline constexpr std::array<size_t, kTaskLaneCount> kTaskQueueDepths{{
    kInputQueueDepth,
    kVoiceQueueDepth,
    kControlQueueDepth,
    kLedQueueDepth,
    kSlowWorkQueueDepth,
    kDisplayQueueDepth,
    kSlowWorkQueueDepth,
    kPortalQueueDepth,
}};

inline constexpr size_t taskLaneIndex(TaskLane lane) {
  return static_cast<size_t>(lane);
}

inline constexpr size_t workClassIndex(WorkClass work_class) {
  return static_cast<size_t>(work_class);
}

static_assert(sizeof(WorkEnvelope) <= 32, "work envelope must stay bounded");
static_assert(kTaskQueueDepths[taskLaneIndex(TaskLane::Input)] == 32,
              "input burst capacity is a responsiveness contract");
static_assert(kTaskQueueDepths[taskLaneIndex(TaskLane::Portal)] == 4,
              "Portal must remain tightly bounded");

}  // namespace inkloop
