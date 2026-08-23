#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop::diagnostics {

// The physical acceptance surface is deliberately smaller than the interactive
// Arduino console. These commands are safe to invoke unattended and must still
// route through the real product owners.
enum class SerialCommand : uint8_t {
  None,
  Status,
  AlbumStatus,
  VoiceTap,
  AigcTest,
};

enum class SerialParseCode : uint8_t {
  Pending,
  Empty,
  Command,
  UnknownCommand,
  MalformedInput,
  LineTooLong,
};

struct SerialParseResult {
  SerialParseCode code = SerialParseCode::Pending;
  SerialCommand command = SerialCommand::None;
};

const char* serialCommandName(SerialCommand command);
const char* serialParseCodeName(SerialParseCode code);

// Bytewise, allocation-free parser suitable for a low-priority UART/USB-JTAG
// task. CR is ignored, LF commits one command, and an invalid/overflowing line
// is discarded through LF so a hostile or truncated frame cannot bleed into
// the next command.
class SerialCommandParser final {
 public:
  static constexpr size_t kMaxLineLength = 96U;

  SerialParseResult consume(uint8_t byte);
  void reset();

  size_t bufferedLength() const { return length_; }
  bool discarding() const { return discard_ != DiscardReason::None; }

 private:
  enum class DiscardReason : uint8_t { None, Malformed, TooLong };

  SerialParseResult finishLine();

  std::array<char, kMaxLineLength + 1U> line_{};
  size_t length_ = 0U;
  DiscardReason discard_ = DiscardReason::None;
};

}  // namespace inkloop::diagnostics
