#pragma once

#include <cstddef>
#include <cstdint>

#include "inkloop/diagnostics/serial_command_parser.hpp"

namespace inkloop::diagnostics {

enum class SerialDiagnosticEventKind : uint8_t {
  Command,
  CommandRejected,
  Status,
  Album,
  VoiceState,
  VoiceAsrFinal,
  VoiceToolStorage,
  VoiceError,
  MyAiError,
  AigcError,
  AigcDiagnostic,
  AigcPhase,
};

// These values are a stable serial protocol and intentionally do not expose
// the internal MyAI enum ordering. The physical acceptance harness depends on
// Ready=1, Listening=2, Thinking=3 and Speaking=4.
enum class SerialDiagnosticVoiceState : uint8_t {
  Idle = 0,
  Ready = 1,
  Listening = 2,
  Thinking = 3,
  Speaking = 4,
  Connecting = 5,
  Error = 6,
};

enum class SerialDiagnosticAsrRoute : uint8_t { Local, Remote };

enum class SerialDiagnosticAigcPhase : uint8_t {
  Starting,
  Submitted,
  GenerationComplete,
  Cached,
  DisplayStart,
  DisplayComplete,
};

enum SerialDiagnosticStatusFlag : uint8_t {
  StatusRuntimeStarted = 1U << 0U,
  StatusWifiOnline = 1U << 1U,
  StatusStorageReady = 1U << 2U,
  StatusDisplayBusy = 1U << 3U,
  StatusMyAiAuthorized = 1U << 4U,
};

// Fixed POD envelope shared by high-priority owners and the low-priority
// serial drain. It carries only booleans/enums/counts: never transcript text,
// URLs, device codes, tokens, cookies, credentials or remote response bodies.
struct SerialDiagnosticEvent {
  SerialDiagnosticEventKind kind = SerialDiagnosticEventKind::CommandRejected;
  SerialCommand command = SerialCommand::None;
  uint32_t first = 0U;
  uint32_t second = 0U;
  uint8_t code = 0U;
  uint8_t flags = 0U;
};

class ISerialDiagnosticEventSink {
 public:
  virtual ~ISerialDiagnosticEventSink() = default;
  // Non-blocking. False means the bounded diagnostic queue was full or the
  // transport was not configured; product work must continue regardless.
  virtual bool postSerialDiagnosticEvent(
      const SerialDiagnosticEvent& event) = 0;
};

// Writes exactly one LF-terminated INKLOOP_* record. Returns zero when the
// typed event is invalid or the destination cannot hold the complete frame.
size_t formatSerialDiagnosticEvent(const SerialDiagnosticEvent& event,
                                   char* output, size_t capacity);

}  // namespace inkloop::diagnostics
