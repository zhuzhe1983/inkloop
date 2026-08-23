#include "inkloop/diagnostics/serial_command_parser.hpp"

#include <cstring>

namespace inkloop::diagnostics {
namespace {

char asciiLower(char value) {
  return value >= 'A' && value <= 'Z'
      ? static_cast<char>(value + ('a' - 'A'))
      : value;
}

SerialCommand parseCommand(const char* value) {
  if (std::strcmp(value, "status") == 0) return SerialCommand::Status;
  if (std::strcmp(value, "album-status") == 0)
    return SerialCommand::AlbumStatus;
  if (std::strcmp(value, "voice-tap") == 0) return SerialCommand::VoiceTap;
  if (std::strcmp(value, "aigc-test") == 0) return SerialCommand::AigcTest;
  return SerialCommand::None;
}

}  // namespace

const char* serialCommandName(SerialCommand command) {
  switch (command) {
    case SerialCommand::Status:
      return "status";
    case SerialCommand::AlbumStatus:
      return "album-status";
    case SerialCommand::VoiceTap:
      return "voice-tap";
    case SerialCommand::AigcTest:
      return "aigc-test";
    case SerialCommand::None:
      return "none";
  }
  return "none";
}

const char* serialParseCodeName(SerialParseCode code) {
  switch (code) {
    case SerialParseCode::Pending:
      return "pending";
    case SerialParseCode::Empty:
      return "empty";
    case SerialParseCode::Command:
      return "command";
    case SerialParseCode::UnknownCommand:
      return "unknown_command";
    case SerialParseCode::MalformedInput:
      return "malformed_input";
    case SerialParseCode::LineTooLong:
      return "line_too_long";
  }
  return "malformed_input";
}

SerialParseResult SerialCommandParser::consume(uint8_t byte) {
  if (byte == '\r') return {};
  if (byte == '\n') return finishLine();
  if (discard_ != DiscardReason::None) return {};

  // Commands are a narrow ASCII control surface. Tabs, control bytes and all
  // high-bit bytes are rejected instead of being normalized ambiguously.
  if (byte < 0x20U || byte > 0x7eU) {
    discard_ = DiscardReason::Malformed;
    return {};
  }
  if (length_ >= kMaxLineLength) {
    discard_ = DiscardReason::TooLong;
    return {};
  }
  line_[length_++] = asciiLower(static_cast<char>(byte));
  line_[length_] = '\0';
  return {};
}

void SerialCommandParser::reset() {
  line_.fill('\0');
  length_ = 0U;
  discard_ = DiscardReason::None;
}

SerialParseResult SerialCommandParser::finishLine() {
  if (discard_ != DiscardReason::None) {
    const SerialParseCode code = discard_ == DiscardReason::TooLong
        ? SerialParseCode::LineTooLong
        : SerialParseCode::MalformedInput;
    reset();
    return {code, SerialCommand::None};
  }

  size_t first = 0U;
  while (first < length_ && line_[first] == ' ') ++first;
  size_t last = length_;
  while (last > first && line_[last - 1U] == ' ') --last;
  if (first == last) {
    reset();
    return {SerialParseCode::Empty, SerialCommand::None};
  }

  const size_t trimmed = last - first;
  if (first != 0U) std::memmove(line_.data(), line_.data() + first, trimmed);
  line_[trimmed] = '\0';
  const SerialCommand command = parseCommand(line_.data());
  reset();
  return command == SerialCommand::None
      ? SerialParseResult{SerialParseCode::UnknownCommand,
                          SerialCommand::None}
      : SerialParseResult{SerialParseCode::Command, command};
}

}  // namespace inkloop::diagnostics
