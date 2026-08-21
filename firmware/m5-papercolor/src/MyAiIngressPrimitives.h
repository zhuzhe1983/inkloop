#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

// The underlying WebSockets transport rejects frames above 15 KiB before its
// allocation. These lower application caps provide a second, protocol-aware
// boundary before std::string construction or audio copying.
static const size_t kMaximumMyAiWebSocketTextFrameBytes = 12U * 1024U;
static const size_t kMaximumMyAiWebSocketAudioFrameBytes = 12U * 1024U;

enum class MyAiIngressFrameKind : uint8_t { Text, Audio };

inline bool acceptMyAiIngressFrame(
    MyAiIngressFrameKind kind, const uint8_t* payload, size_t length) {
  if (!payload || length == 0) return false;
  const size_t maximum = kind == MyAiIngressFrameKind::Text
      ? kMaximumMyAiWebSocketTextFrameBytes
      : kMaximumMyAiWebSocketAudioFrameBytes;
  return length <= maximum;
}

}  // namespace inkloop
