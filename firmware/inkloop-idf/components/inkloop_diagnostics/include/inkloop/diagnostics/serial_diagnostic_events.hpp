#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/diagnostics/diagnostic_detail.hpp"
#include "inkloop/diagnostics/serial_command_parser.hpp"

namespace inkloop::diagnostics {

enum class SerialDiagnosticEventKind : uint8_t {
  Command,
  CommandRejected,
  Status,
  ResetReason,
  AigcState,
  NetworkState,
  SerialState,
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

// Stable, credential-free AIGC execution snapshot. The values describe only
// Inkloop's local ownership handoff, never a provider response or credential.
enum class SerialDiagnosticAigcRuntimePhase : uint8_t {
  Idle = 0,
  PendingHandoff = 1,
  Start = 2,
  Poll = 3,
  Download = 4,
};

enum SerialDiagnosticAigcStateFlag : uint8_t {
  AigcAdmissionPending = 1U << 0U,
  AigcExclusive = 1U << 1U,
  AigcSerialDiagnostic = 1U << 2U,
};

// Stable numeric mirrors used only by the credential-free serial protocol.
// Product composition maps its private MyAI/network enums into these fields.
enum class SerialDiagnosticMyAiErrorSource : uint8_t {
  Command = 1,
  Tick = 2,
  Initialize = 3,
  ApplyPrompt = 4,
  Pairing = 5,
  Authorization = 6,
  Aigc = 7,
  VoiceConnect = 8,
  VoiceIngress = 9,
  CaptureUpload = 10,
  Heartbeat = 11,
};

enum class SerialDiagnosticMyAiErrorCode : uint8_t {
  InvalidArgument = 1,
  InvalidState = 2,
  Storage = 3,
  Security = 4,
  Transport = 5,
  Protocol = 6,
  Unauthorized = 7,
  PaymentRequired = 8,
  RecoveryRequired = 9,
  PairingExpired = 10,
  Conflict = 11,
  AppNotRegistered = 12,
  NoGateway = 13,
  TooLarge = 14,
  Cancelled = 15,
};

inline constexpr size_t kMaximumSerialDiagnosticDetailBytes = 96U;

// Fixed POD envelope shared by high-priority owners and the low-priority
// serial drain. The optional detail is accepted only for MyAI errors and must
// already be the canonical, credential-free output of
// sanitizeDiagnosticDetail(). It is intentionally too small for response
// bodies and is validated again immediately before USB serialization.
struct SerialDiagnosticEvent {
  SerialDiagnosticEventKind kind = SerialDiagnosticEventKind::CommandRejected;
  SerialCommand command = SerialCommand::None;
  uint32_t first = 0U;
  uint32_t second = 0U;
  uint8_t code = 0U;
  uint8_t flags = 0U;
  std::array<char, kMaximumSerialDiagnosticDetailBytes + 1U> detail{};
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
