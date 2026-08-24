import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_diagnostics");

test("serial diagnostics parser is bounded, strict, and recovers per line", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-serial-diagnostics-"));
  const harness = join(scratch, "serial_diagnostics.cpp");
  const binary = join(scratch, "serial_diagnostics");
  writeFileSync(harness, String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#include "inkloop/diagnostics/serial_command_parser.hpp"
#include "inkloop/diagnostics/serial_diagnostic_events.hpp"

using namespace inkloop::diagnostics;

SerialParseResult feed(SerialCommandParser& parser, const std::string& value) {
  SerialParseResult result;
  for (const unsigned char byte : value) result = parser.consume(byte);
  return result;
}

int main() {
  SerialCommandParser parser;
  auto result = feed(parser, "status\n");
  assert(result.code == SerialParseCode::Command);
  assert(result.command == SerialCommand::Status);
  assert(std::string(serialCommandName(result.command)) == "status");

  result = feed(parser, "  ALBUM-STATUS  \r\n");
  assert(result.code == SerialParseCode::Command);
  assert(result.command == SerialCommand::AlbumStatus);
  result = feed(parser, "Voice-Tap\n");
  assert(result.code == SerialParseCode::Command);
  assert(result.command == SerialCommand::VoiceTap);
  result = feed(parser, "aigc-test\n");
  assert(result.code == SerialParseCode::Command);
  assert(result.command == SerialCommand::AigcTest);

  result = feed(parser, "\n");
  assert(result.code == SerialParseCode::Empty);
  result = feed(parser, "reboot\n");
  assert(result.code == SerialParseCode::UnknownCommand);
  assert(result.command == SerialCommand::None);

  // An invalid byte poisons only its own line. Bytes after the invalid byte
  // are discarded rather than accidentally forming a privileged command.
  result = feed(parser, std::string("sta") + char(0x01) + "tus\n");
  assert(result.code == SerialParseCode::MalformedInput);
  assert(!parser.discarding());
  result = feed(parser, "status\n");
  assert(result.code == SerialParseCode::Command);

  std::string maximum(SerialCommandParser::kMaxLineLength, 'x');
  result = feed(parser, maximum + "\n");
  assert(result.code == SerialParseCode::UnknownCommand);
  result = feed(parser, maximum + "xstatus\n");
  assert(result.code == SerialParseCode::LineTooLong);
  assert(parser.bufferedLength() == 0);
  result = feed(parser, "album-status\n");
  assert(result.code == SerialParseCode::Command);

  // CR is transport framing only and cannot terminate a command by itself.
  result = feed(parser, "status\r");
  assert(result.code == SerialParseCode::Pending);
  result = feed(parser, "\n");
  assert(result.code == SerialParseCode::Command);

  parser.reset();
  assert(parser.bufferedLength() == 0 && !parser.discarding());
  assert(std::string(serialParseCodeName(SerialParseCode::LineTooLong)) ==
         "line_too_long");

  char frame[256]{};
  SerialDiagnosticEvent event;
  event.kind = SerialDiagnosticEventKind::Command;
  event.command = SerialCommand::VoiceTap;
  size_t bytes = formatSerialDiagnosticEvent(event, frame, sizeof(frame));
  assert(bytes == std::string("INKLOOP_COMMAND:voice-tap\n").size());
  assert(std::string(frame) == "INKLOOP_COMMAND:voice-tap\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::Status;
  event.flags = StatusRuntimeStarted | StatusWifiOnline |
                StatusMyAiAuthorized;
  event.first = 2;
  event.second = 1;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_STATUS:runtime=1,wifi=1,storage=0,display_busy=0,"
         "myai_authorized=1,myai_activation=2,voice_state=1\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::ResetReason;
  event.first = 8;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) == "INKLOOP_RESET_REASON:8\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::AigcState;
  event.code = static_cast<uint8_t>(
      SerialDiagnosticAigcRuntimePhase::PendingHandoff);
  event.flags = AigcAdmissionPending | AigcSerialDiagnostic;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AIGC_STATE:phase=1,admission_pending=1,exclusive=0,"
         "diagnostic=1\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::NetworkState;
  event.code = 8;
  event.first = 123456;
  event.second = 2;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_NETWORK_STATE:operation=8,age_ms=123456,queue_depth=2\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::SerialState;
  event.first = 3;
  event.second = 4;
  event.third = 5;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_SERIAL_STATE:drops=3,write_failures=4,"
         "button_mailbox_overflows=5\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::AudioDma;
  event.flags = AudioDiagnosticsAvailable;
  event.first = 321;
  event.second = 2;
  event.third = 1;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AUDIO_DMA:available=1,callbacks=321,underruns=2,"
         "expected_drain_overflows=1\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::AudioFeed;
  event.flags = AudioDiagnosticsAvailable;
  event.first = 4;
  event.second = 90;
  event.third = 3;
  event.fourth = 2;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AUDIO_FEED:available=1,streams=4,submits=90,"
         "late_submits=3,estimated_underruns=2\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::AudioTiming;
  event.flags = AudioDiagnosticsAvailable;
  event.first = 25000;
  event.second = 12000;
  event.third = 93000;
  event.fourth = 160;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AUDIO_TIMING:available=1,max_gap_us=25000,"
         "min_lead_us=12000,max_lead_us=93000,current_queue_frames=160\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::AudioQueue;
  event.flags = AudioDiagnosticsAvailable;
  event.first = 960;
  event.second = 1;
  event.third = 5;
  event.fourth = 6;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AUDIO_QUEUE:available=1,peak_frames=960,clamps=1,"
         "capture_timeouts=5,playback_timeouts=6\n");

  // SKU-neutral unavailable records are explicit and cannot carry stale
  // counters from a previous audio-capable composition.
  event = {};
  event.kind = SerialDiagnosticEventKind::AudioFeed;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_AUDIO_FEED:available=0,streams=0,submits=0,"
         "late_submits=0,estimated_underruns=0\n");
  event.first = 1;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);
  event.first = 0;
  event.flags = 0x80;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);

  event = {};
  event.kind = SerialDiagnosticEventKind::Album;
  event.flags = 1;
  event.first = 12;
  event.second = 4;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) == "INKLOOP_ALBUM:READY:12:4\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::VoiceState;
  event.code = static_cast<uint8_t>(SerialDiagnosticVoiceState::Listening);
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) == "INKLOOP_VOICE_STATE:2\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::VoiceAsrFinal;
  event.code = static_cast<uint8_t>(SerialDiagnosticAsrRoute::Local);
  event.first = 12;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) == "INKLOOP_VOICE_ASR_FINAL:LOCAL:12\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::MyAiError;
  event.flags = static_cast<uint8_t>(
      SerialDiagnosticMyAiErrorSource::Authorization);
  event.code = static_cast<uint8_t>(
      SerialDiagnosticMyAiErrorCode::Unauthorized);
  event.first = 401;
  event.second = 5000;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_MYAI_ERROR:source=authorization,code=unauthorized,"
         "http=401,retry_ms=5000\n");
  const std::string safe_detail = "authorization rejected by MyAI";
  std::memcpy(event.detail.data(), safe_detail.data(), safe_detail.size());
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_MYAI_ERROR:source=authorization,code=unauthorized,"
         "http=401,retry_ms=5000,detail=authorization rejected by MyAI\n");
  event.detail = {};
  const std::string unsafe_detail = "Bearer device-secret";
  std::memcpy(event.detail.data(), unsafe_detail.data(), unsafe_detail.size());
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);
  event.detail.fill('x');
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);
  event.detail = {};
  event.first = 42;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);

  event = {};
  event.kind = SerialDiagnosticEventKind::ButtonLatency;
  event.correlation = (1ULL << 63U) | 9U;
  event.code = static_cast<uint8_t>(SerialDiagnosticButton::Top);
  event.flags = static_cast<uint8_t>(SerialDiagnosticButtonOutcome::Led);
  event.first = UINT32_MAX - 10U;
  event.second = 5U;
  event.third = 20U;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_BUTTON_LATENCY:v=1,id=9223372036854775817,button=top,"
         "capture_us=4294967285,control_us=5,feedback_us=20,"
         "control_delta_us=16,feedback_delta_us=31,result=led\n");

  event = {};
  event.kind = SerialDiagnosticEventKind::ButtonLatency;
  event.correlation = (1ULL << 63U) | 10U;
  event.code = static_cast<uint8_t>(SerialDiagnosticButton::Next);
  event.flags = static_cast<uint8_t>(
      SerialDiagnosticButtonOutcome::NotReady);
  event.first = 100U;
  event.third = 500100U;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) ==
         "INKLOOP_BUTTON_LATENCY:v=1,id=9223372036854775818,button=next,"
         "capture_us=100,control_us=0,feedback_us=500100,"
         "control_delta_us=0,feedback_delta_us=500000,result=not_ready\n");
  event.correlation = 0U;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);
  event.correlation = 1U;
  event.code = 255U;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);

  event = {};
  event.kind = SerialDiagnosticEventKind::AigcPhase;
  event.code = static_cast<uint8_t>(
      SerialDiagnosticAigcPhase::DisplayComplete);
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) > 0);
  assert(std::string(frame) == "INKLOOP_AIGC_PHASE:DISPLAY_COMPLETE\n");

  // The formatter is all-or-nothing and rejects invalid enum values.
  char tiny[8]{};
  assert(formatSerialDiagnosticEvent(event, tiny, sizeof(tiny)) == 0);
  event.code = 255;
  assert(formatSerialDiagnosticEvent(event, frame, sizeof(frame)) == 0);
  return 0;
}
`);

  try {
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", join(component, "include"),
      harness,
      join(component, "serial_command_parser.cpp"),
      join(component, "diagnostic_detail.cpp"),
      join(component, "serial_diagnostic_events.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], {
      stdio: "pipe",
      // macOS system ASan aborts when leak detection is requested; bounds and
      // undefined-behaviour instrumentation remain active.
      env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0" },
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});
