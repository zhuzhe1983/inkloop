#pragma once

#include "MyAiTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {
namespace myai {

enum class WssMessageKind : uint8_t { None = 0, Text = 1, Binary = 2 };

struct WssIngressChunk {
  uint8_t opcode;
  bool finalFrame;
  size_t framePayloadBytes;
  size_t frameOffset;
  const uint8_t* bytes;
  size_t length;

  WssIngressChunk()
      : opcode(0), finalFrame(false), framePayloadBytes(0), frameOffset(0),
        bytes(nullptr), length(0) {}
};

struct WssCompletedMessage {
  WssMessageKind kind;
  const uint8_t* bytes;
  size_t length;

  WssCompletedMessage()
      : kind(WssMessageKind::None), bytes(nullptr), length(0) {}
};

// Reassembles one RFC6455 message from bounded transport chunks. The returned
// view stays valid until the next append/reset. No partial text or audio is
// exposed to MyAiClient, so an oversized or malformed tail cannot make a
// prefix appear valid.
class WssIngressAssembler final {
 public:
  static constexpr size_t kMaximumMessageBytes = 12U * 1024U;

  WssIngressAssembler();

  Status append(const WssIngressChunk& chunk, bool& complete,
                WssCompletedMessage& message);
  void reset();

 private:
  static WssMessageKind kindForOpcode(uint8_t opcode);
  Status reject(const char* detail);

  std::array<uint8_t, kMaximumMessageBytes> buffer_;
  WssMessageKind messageKind_;
  size_t messageLength_;
  uint8_t frameOpcode_;
  bool frameFinal_;
  size_t frameExpected_;
  size_t frameReceived_;
  bool fragmentedMessage_;
  bool frameActive_;
};

}  // namespace myai
}  // namespace inkloop
