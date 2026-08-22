#include "inkloop/myai/WssIngress.h"

#include <cstring>

namespace inkloop {
namespace myai {

WssIngressAssembler::WssIngressAssembler() { reset(); }

void WssIngressAssembler::reset() {
  messageKind_ = WssMessageKind::None;
  messageLength_ = 0;
  frameOpcode_ = 0;
  frameFinal_ = false;
  frameExpected_ = 0;
  frameReceived_ = 0;
  fragmentedMessage_ = false;
  frameActive_ = false;
}

WssMessageKind WssIngressAssembler::kindForOpcode(uint8_t opcode) {
  if (opcode == 0x01U) return WssMessageKind::Text;
  if (opcode == 0x02U) return WssMessageKind::Binary;
  return WssMessageKind::None;
}

Status WssIngressAssembler::reject(const char* detail) {
  reset();
  return Status(ErrorCode::Protocol, 0, detail);
}

Status WssIngressAssembler::append(const WssIngressChunk& chunk,
                                   bool& complete,
                                   WssCompletedMessage& message) {
  complete = false;
  message = WssCompletedMessage();
  if ((!chunk.bytes && chunk.length != 0) || chunk.framePayloadBytes == 0 ||
      chunk.length == 0 || chunk.frameOffset > chunk.framePayloadBytes ||
      chunk.length > chunk.framePayloadBytes - chunk.frameOffset) {
    return reject("invalid WebSocket ingress chunk");
  }

  if (chunk.frameOffset == 0) {
    if (frameActive_) return reject("overlapping WebSocket frames");
    const WssMessageKind initialKind = kindForOpcode(chunk.opcode);
    if (chunk.opcode == 0x00U) {
      if (!fragmentedMessage_ || messageKind_ == WssMessageKind::None) {
        return reject("orphan WebSocket continuation");
      }
    } else if (initialKind != WssMessageKind::None) {
      if (fragmentedMessage_ || messageKind_ != WssMessageKind::None) {
        return reject("nested WebSocket message");
      }
      messageKind_ = initialKind;
    } else {
      return reject("unsupported WebSocket opcode");
    }
    if (chunk.framePayloadBytes > kMaximumMessageBytes - messageLength_) {
      reset();
      return Status(ErrorCode::TooLarge, 0,
                    "WebSocket message exceeds byte limit");
    }
    frameOpcode_ = chunk.opcode;
    frameFinal_ = chunk.finalFrame;
    frameExpected_ = chunk.framePayloadBytes;
    frameReceived_ = 0;
    frameActive_ = true;
  } else if (!frameActive_ || chunk.opcode != frameOpcode_ ||
             chunk.finalFrame != frameFinal_ ||
             chunk.framePayloadBytes != frameExpected_ ||
             chunk.frameOffset != frameReceived_) {
    return reject("inconsistent WebSocket frame continuation");
  }

  if (chunk.frameOffset != frameReceived_ ||
      chunk.length > kMaximumMessageBytes - messageLength_) {
    if (chunk.frameOffset == frameReceived_) {
      reset();
      return Status(ErrorCode::TooLarge, 0,
                    "WebSocket message exceeds byte limit");
    }
    return reject("non-contiguous WebSocket ingress");
  }
  std::memcpy(buffer_.data() + messageLength_, chunk.bytes, chunk.length);
  messageLength_ += chunk.length;
  frameReceived_ += chunk.length;
  if (frameReceived_ < frameExpected_) return Status::success();
  if (frameReceived_ != frameExpected_) {
    return reject("WebSocket frame length mismatch");
  }

  frameActive_ = false;
  frameExpected_ = 0;
  frameReceived_ = 0;
  if (!frameFinal_) {
    fragmentedMessage_ = true;
    return Status::success();
  }

  complete = true;
  message.kind = messageKind_;
  message.bytes = buffer_.data();
  message.length = messageLength_;
  messageKind_ = WssMessageKind::None;
  messageLength_ = 0;
  fragmentedMessage_ = false;
  frameOpcode_ = 0;
  frameFinal_ = false;
  return Status::success();
}

}  // namespace myai
}  // namespace inkloop
