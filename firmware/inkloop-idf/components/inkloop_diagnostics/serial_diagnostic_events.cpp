#include "inkloop/diagnostics/serial_diagnostic_events.hpp"

#include <cstdio>

namespace inkloop::diagnostics {
namespace {

const char* voiceStateName(uint8_t value) {
  switch (static_cast<SerialDiagnosticVoiceState>(value)) {
    case SerialDiagnosticVoiceState::Idle:
      return "0";
    case SerialDiagnosticVoiceState::Ready:
      return "1";
    case SerialDiagnosticVoiceState::Listening:
      return "2";
    case SerialDiagnosticVoiceState::Thinking:
      return "3";
    case SerialDiagnosticVoiceState::Speaking:
      return "4";
    case SerialDiagnosticVoiceState::Connecting:
      return "5";
    case SerialDiagnosticVoiceState::Error:
      return "6";
  }
  return nullptr;
}

const char* aigcPhaseName(uint8_t value) {
  switch (static_cast<SerialDiagnosticAigcPhase>(value)) {
    case SerialDiagnosticAigcPhase::Starting:
      return "STARTING";
    case SerialDiagnosticAigcPhase::Submitted:
      return "SUBMITTED";
    case SerialDiagnosticAigcPhase::GenerationComplete:
      return "GENERATION_COMPLETE";
    case SerialDiagnosticAigcPhase::Cached:
      return "CACHED";
    case SerialDiagnosticAigcPhase::DisplayStart:
      return "DISPLAY_START";
    case SerialDiagnosticAigcPhase::DisplayComplete:
      return "DISPLAY_COMPLETE";
  }
  return nullptr;
}

size_t finishFormat(int written, size_t capacity) {
  if (written <= 0 || static_cast<size_t>(written) >= capacity) return 0U;
  return static_cast<size_t>(written);
}

}  // namespace

size_t formatSerialDiagnosticEvent(const SerialDiagnosticEvent& event,
                                   char* output, size_t capacity) {
  if (!output || capacity == 0U) return 0U;
  output[0] = '\0';
  int written = -1;
  switch (event.kind) {
    case SerialDiagnosticEventKind::Command:
      if (event.command == SerialCommand::None) return 0U;
      written = std::snprintf(output, capacity, "INKLOOP_COMMAND:%s\n",
                              serialCommandName(event.command));
      break;
    case SerialDiagnosticEventKind::CommandRejected:
      written = std::snprintf(output, capacity,
                              "INKLOOP_COMMAND_REJECTED:%s\n",
                              serialParseCodeName(
                                  static_cast<SerialParseCode>(event.code)));
      break;
    case SerialDiagnosticEventKind::Status:
      written = std::snprintf(
          output, capacity,
          "INKLOOP_STATUS:runtime=%u,wifi=%u,storage=%u,display_busy=%u,"
          "myai_authorized=%u,myai_activation=%u,voice_state=%u\n",
          (event.flags & StatusRuntimeStarted) != 0U,
          (event.flags & StatusWifiOnline) != 0U,
          (event.flags & StatusStorageReady) != 0U,
          (event.flags & StatusDisplayBusy) != 0U,
          (event.flags & StatusMyAiAuthorized) != 0U,
          static_cast<unsigned>(event.first),
          static_cast<unsigned>(event.second));
      break;
    case SerialDiagnosticEventKind::ResetReason:
      written = std::snprintf(output, capacity,
                              "INKLOOP_RESET_REASON:%lu\n",
                              static_cast<unsigned long>(event.first));
      break;
    case SerialDiagnosticEventKind::AigcState:
      if (event.code > static_cast<uint8_t>(
                           SerialDiagnosticAigcRuntimePhase::Download))
        return 0U;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_AIGC_STATE:phase=%u,admission_pending=%u,exclusive=%u,"
          "diagnostic=%u\n",
          static_cast<unsigned>(event.code),
          (event.flags & AigcAdmissionPending) != 0U,
          (event.flags & AigcExclusive) != 0U,
          (event.flags & AigcSerialDiagnostic) != 0U);
      break;
    case SerialDiagnosticEventKind::NetworkState:
      written = std::snprintf(
          output, capacity,
          "INKLOOP_NETWORK_STATE:operation=%u,age_ms=%lu,queue_depth=%lu\n",
          static_cast<unsigned>(event.code),
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second));
      break;
    case SerialDiagnosticEventKind::SerialState:
      written = std::snprintf(
          output, capacity,
          "INKLOOP_SERIAL_STATE:drops=%lu,write_failures=%lu\n",
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second));
      break;
    case SerialDiagnosticEventKind::Album:
      written = event.flags != 0U
          ? std::snprintf(output, capacity, "INKLOOP_ALBUM:READY:%lu:%lu\n",
                          static_cast<unsigned long>(event.first),
                          static_cast<unsigned long>(event.second))
          : std::snprintf(output, capacity, "INKLOOP_ALBUM:UNAVAILABLE\n");
      break;
    case SerialDiagnosticEventKind::VoiceState: {
      const char* state = voiceStateName(event.code);
      if (!state) return 0U;
      written = std::snprintf(output, capacity, "INKLOOP_VOICE_STATE:%s\n",
                              state);
      break;
    }
    case SerialDiagnosticEventKind::VoiceAsrFinal:
      if (event.code > static_cast<uint8_t>(SerialDiagnosticAsrRoute::Remote))
        return 0U;
      written = std::snprintf(
          output, capacity, "INKLOOP_VOICE_ASR_FINAL:%s:%lu\n",
          event.code == static_cast<uint8_t>(SerialDiagnosticAsrRoute::Local)
              ? "LOCAL" : "REMOTE",
          static_cast<unsigned long>(event.first));
      break;
    case SerialDiagnosticEventKind::VoiceToolStorage:
      written = std::snprintf(output, capacity,
                              "INKLOOP_VOICE_TOOL:storage.free:%s\n",
                              event.flags != 0U ? "OK" : "FAILED");
      break;
    case SerialDiagnosticEventKind::VoiceError:
      written = std::snprintf(output, capacity,
                              "INKLOOP_VOICE_ERROR:E%u\n",
                              static_cast<unsigned>(event.code));
      break;
    case SerialDiagnosticEventKind::MyAiError:
      written = std::snprintf(output, capacity, "INKLOOP_MYAI_ERROR:E%u\n",
                              static_cast<unsigned>(event.code));
      break;
    case SerialDiagnosticEventKind::AigcError:
      written = std::snprintf(output, capacity, "INKLOOP_AIGC_ERROR:E%u\n",
                              static_cast<unsigned>(event.code));
      break;
    case SerialDiagnosticEventKind::AigcDiagnostic:
      written = std::snprintf(output, capacity,
                              "INKLOOP_AIGC_DIAGNOSTIC:%s\n",
                              event.flags != 0U ? "QUEUED" : "REJECTED");
      break;
    case SerialDiagnosticEventKind::AigcPhase: {
      const char* phase = aigcPhaseName(event.code);
      if (!phase) return 0U;
      written = std::snprintf(output, capacity, "INKLOOP_AIGC_PHASE:%s\n",
                              phase);
      break;
    }
  }
  return finishFormat(written, capacity);
}

}  // namespace inkloop::diagnostics
