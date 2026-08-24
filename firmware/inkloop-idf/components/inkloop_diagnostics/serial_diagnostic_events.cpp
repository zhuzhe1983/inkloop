#include "inkloop/diagnostics/serial_diagnostic_events.hpp"

#include <cstdio>
#include <string>

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

const char* buttonName(uint8_t value) {
  switch (static_cast<SerialDiagnosticButton>(value)) {
    case SerialDiagnosticButton::Previous: return "previous";
    case SerialDiagnosticButton::Next: return "next";
    case SerialDiagnosticButton::Top: return "top";
  }
  return nullptr;
}

const char* buttonOutcomeName(uint8_t value) {
  switch (static_cast<SerialDiagnosticButtonOutcome>(value)) {
    case SerialDiagnosticButtonOutcome::Led: return "led";
    case SerialDiagnosticButtonOutcome::Navigation: return "navigation";
    case SerialDiagnosticButtonOutcome::Debounced: return "debounced";
    case SerialDiagnosticButtonOutcome::NotReady: return "not_ready";
  }
  return nullptr;
}

const char* myAiErrorSourceName(uint8_t value) {
  switch (static_cast<SerialDiagnosticMyAiErrorSource>(value)) {
    case SerialDiagnosticMyAiErrorSource::Command: return "command";
    case SerialDiagnosticMyAiErrorSource::Tick: return "tick";
    case SerialDiagnosticMyAiErrorSource::Initialize: return "initialize";
    case SerialDiagnosticMyAiErrorSource::ApplyPrompt: return "apply_prompt";
    case SerialDiagnosticMyAiErrorSource::Pairing: return "pairing";
    case SerialDiagnosticMyAiErrorSource::Authorization:
      return "authorization";
    case SerialDiagnosticMyAiErrorSource::Aigc: return "aigc";
    case SerialDiagnosticMyAiErrorSource::VoiceConnect:
      return "voice_connect";
    case SerialDiagnosticMyAiErrorSource::VoiceIngress:
      return "voice_ingress";
    case SerialDiagnosticMyAiErrorSource::CaptureUpload:
      return "capture_upload";
    case SerialDiagnosticMyAiErrorSource::Heartbeat: return "heartbeat";
  }
  return nullptr;
}

const char* myAiErrorCodeName(uint8_t value) {
  switch (static_cast<SerialDiagnosticMyAiErrorCode>(value)) {
    case SerialDiagnosticMyAiErrorCode::InvalidArgument:
      return "invalid_argument";
    case SerialDiagnosticMyAiErrorCode::InvalidState: return "invalid_state";
    case SerialDiagnosticMyAiErrorCode::Storage: return "storage";
    case SerialDiagnosticMyAiErrorCode::Security: return "security";
    case SerialDiagnosticMyAiErrorCode::Transport: return "transport";
    case SerialDiagnosticMyAiErrorCode::Protocol: return "protocol";
    case SerialDiagnosticMyAiErrorCode::Unauthorized: return "unauthorized";
    case SerialDiagnosticMyAiErrorCode::PaymentRequired:
      return "payment_required";
    case SerialDiagnosticMyAiErrorCode::RecoveryRequired:
      return "recovery_required";
    case SerialDiagnosticMyAiErrorCode::PairingExpired:
      return "pairing_expired";
    case SerialDiagnosticMyAiErrorCode::Conflict: return "conflict";
    case SerialDiagnosticMyAiErrorCode::AppNotRegistered:
      return "app_not_registered";
    case SerialDiagnosticMyAiErrorCode::NoGateway: return "no_gateway";
    case SerialDiagnosticMyAiErrorCode::TooLarge: return "too_large";
    case SerialDiagnosticMyAiErrorCode::Cancelled: return "cancelled";
  }
  return nullptr;
}

size_t finishFormat(int written, size_t capacity) {
  if (written <= 0 || static_cast<size_t>(written) >= capacity) return 0U;
  return static_cast<size_t>(written);
}

bool serialDetail(const SerialDiagnosticEvent& event, std::string& output) {
  output.clear();
  size_t length = 0U;
  while (length < event.detail.size() && event.detail[length] != '\0')
    ++length;
  if (length == 0U) return true;
  if (length == event.detail.size()) return false;
  output.assign(event.detail.data(), length);
  return isCanonicalDiagnosticDetail(
      output, kMaximumSerialDiagnosticDetailBytes);
}

bool validAudioEvent(const SerialDiagnosticEvent& event) {
  if ((event.flags & ~static_cast<uint8_t>(AudioDiagnosticsAvailable)) != 0U)
    return false;
  if ((event.flags & AudioDiagnosticsAvailable) != 0U) return true;
  return event.first == 0U && event.second == 0U && event.third == 0U &&
         event.fourth == 0U;
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
          "INKLOOP_SERIAL_STATE:drops=%lu,write_failures=%lu,"
          "button_mailbox_overflows=%lu\n",
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third));
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
      if (const char* source = myAiErrorSourceName(event.flags)) {
        if (const char* code = myAiErrorCodeName(event.code)) {
          if (event.first != 0U &&
              (event.first < 100U || event.first > 599U)) {
            return 0U;
          }
          std::string detail;
          if (!serialDetail(event, detail)) return 0U;
          written = detail.empty()
              ? std::snprintf(
                    output, capacity,
                    "INKLOOP_MYAI_ERROR:source=%s,code=%s,http=%lu,"
                    "retry_ms=%lu\n",
                    source, code, static_cast<unsigned long>(event.first),
                    static_cast<unsigned long>(event.second))
              : std::snprintf(
                    output, capacity,
                    "INKLOOP_MYAI_ERROR:source=%s,code=%s,http=%lu,"
                    "retry_ms=%lu,detail=%s\n",
                    source, code, static_cast<unsigned long>(event.first),
                    static_cast<unsigned long>(event.second), detail.c_str());
        }
      }
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
    case SerialDiagnosticEventKind::AudioDma:
      if (!validAudioEvent(event)) return 0U;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_AUDIO_DMA:available=%u,callbacks=%lu,underruns=%lu,"
          "expected_drain_overflows=%lu\n",
          (event.flags & AudioDiagnosticsAvailable) != 0U,
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third));
      break;
    case SerialDiagnosticEventKind::AudioFeed:
      if (!validAudioEvent(event)) return 0U;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_AUDIO_FEED:available=%u,streams=%lu,submits=%lu,"
          "late_submits=%lu,estimated_underruns=%lu\n",
          (event.flags & AudioDiagnosticsAvailable) != 0U,
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third),
          static_cast<unsigned long>(event.fourth));
      break;
    case SerialDiagnosticEventKind::AudioTiming:
      if (!validAudioEvent(event)) return 0U;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_AUDIO_TIMING:available=%u,max_gap_us=%lu,min_lead_us=%lu,"
          "max_lead_us=%lu,current_queue_frames=%lu\n",
          (event.flags & AudioDiagnosticsAvailable) != 0U,
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third),
          static_cast<unsigned long>(event.fourth));
      break;
    case SerialDiagnosticEventKind::AudioQueue:
      if (!validAudioEvent(event)) return 0U;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_AUDIO_QUEUE:available=%u,peak_frames=%lu,clamps=%lu,"
          "capture_timeouts=%lu,playback_timeouts=%lu\n",
          (event.flags & AudioDiagnosticsAvailable) != 0U,
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third),
          static_cast<unsigned long>(event.fourth));
      break;
    case SerialDiagnosticEventKind::ButtonLatency: {
      const char* button = buttonName(event.code);
      const char* outcome = buttonOutcomeName(event.flags);
      if (!button || !outcome || event.correlation == 0U) return 0U;
      const bool feedback =
          event.flags == static_cast<uint8_t>(
                             SerialDiagnosticButtonOutcome::Led) ||
          event.flags == static_cast<uint8_t>(
                             SerialDiagnosticButtonOutcome::Navigation);
      if (feedback && event.second == 0U) return 0U;
      const uint32_t control_delta =
          event.second == 0U ? 0U : event.second - event.first;
      const uint32_t terminal_delta = event.third - event.first;
      written = std::snprintf(
          output, capacity,
          "INKLOOP_BUTTON_LATENCY:v=1,id=%llu,button=%s,capture_us=%lu,"
          "control_us=%lu,feedback_us=%lu,control_delta_us=%lu,"
          "feedback_delta_us=%lu,result=%s\n",
          static_cast<unsigned long long>(event.correlation), button,
          static_cast<unsigned long>(event.first),
          static_cast<unsigned long>(event.second),
          static_cast<unsigned long>(event.third),
          static_cast<unsigned long>(control_delta),
          static_cast<unsigned long>(terminal_delta), outcome);
      break;
    }
  }
  return finishFormat(written, capacity);
}

}  // namespace inkloop::diagnostics
