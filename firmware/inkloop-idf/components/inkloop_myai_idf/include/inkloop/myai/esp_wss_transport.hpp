#pragma once

#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/myai/WssKeepAlive.h"
#include "inkloop/myai/WssIngress.h"
#include "inkloop/myai/esp_http_adapters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

struct esp_transport_item_t;
typedef struct esp_transport_item_t* esp_transport_handle_t;

namespace inkloop {
namespace myai {

namespace detail {

// A zero timeout is unsafe after the WS header has disclosed a control-frame
// payload: TLS may have made only the header record available. Callers use a
// single deadline for the whole cooperative read and pass only its remainder
// to the real ESP transport adapter.
inline int remainingWssReadTimeoutMs(uint64_t now_ms, uint64_t deadline_ms) {
  if (now_ms >= deadline_ms) return 0;
  const uint64_t remaining = deadline_ms - now_ms;
  const uint64_t maximum =
      static_cast<uint64_t>(std::numeric_limits<int>::max());
  return static_cast<int>(remaining > maximum ? maximum : remaining);
}

// RFC 6455 close payload validation kept header-only so the bounded parser can
// be exercised by host ASan/UBSan tests without linking any ESP-IDF transport.
// Inkloop negotiates no extensions, therefore extension-only 1016..2999 codes
// are rejected along with the wire-forbidden reserved status codes.
inline bool validWssCloseStatusCode(uint16_t code) {
  return ((code >= 1000U && code <= 1014U) ||
          (code >= 3000U && code <= 4999U)) &&
      code != 1004U && code != 1005U && code != 1006U;
}

inline bool validWssCloseReasonUtf8(const uint8_t* bytes, size_t length) {
  if (length == 0U) return true;
  if (!bytes) return false;
  size_t at = 0U;
  while (at < length) {
    const uint8_t first = bytes[at++];
    if (first <= 0x7fU) continue;

    size_t continuation = 0U;
    uint8_t second_minimum = 0x80U;
    uint8_t second_maximum = 0xbfU;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation = 1U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation = 2U;
      if (first == 0xe0U) second_minimum = 0xa0U;
      if (first == 0xedU) second_maximum = 0x9fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation = 3U;
      if (first == 0xf0U) second_minimum = 0x90U;
      if (first == 0xf4U) second_maximum = 0x8fU;
    } else {
      return false;
    }

    if (continuation > length - at) return false;
    const uint8_t second = bytes[at++];
    if (second < second_minimum || second > second_maximum) return false;
    for (size_t index = 1U; index < continuation; ++index) {
      const uint8_t value = bytes[at++];
      if (value < 0x80U || value > 0xbfU) return false;
    }
  }
  return true;
}

inline bool decodeValidWssClosePayload(const uint8_t* bytes, size_t length,
                                       uint16_t& close_code) {
  close_code = 1000U;
  if (length == 0U) return true;
  if (!bytes || length == 1U || length > 125U) return false;
  close_code = static_cast<uint16_t>(
      (static_cast<uint16_t>(bytes[0]) << 8U) |
      static_cast<uint16_t>(bytes[1]));
  return validWssCloseStatusCode(close_code) &&
      validWssCloseReasonUtf8(bytes + 2U, length - 2U);
}

}  // namespace detail

// Cooperative native WS/WSS adapter. connect/send/poll are called only by the
// slow voice/network owner; it creates no Arduino loop and no competing task.
// pollIngress() consumes at most one 2 KiB transport chunk per call so buttons
// and audio DMA remain higher priority.
class EspWssTransport final : public IWebSocketTransport {
 public:
  using IngressReady = bool (*)(void* context);

  explicit EspWssTransport(EspEndpointSecurity& endpointSecurity);
  ~EspWssTransport() override;

  Status connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 IWebSocketListener& listener) override;
  Status sendText(const std::string& message) override;
  Status sendBinary(const uint8_t* bytes, size_t length) override;
  void close(uint16_t code, const std::string& reason) override;

  Status pollIngress();
  void setIngressReadyGate(IngressReady callback, void* context) {
    ingressReady_ = callback;
    ingressReadyContext_ = context;
  }
  bool connected() const { return connected_; }

 private:
  Status configure(const std::string& url,
                   const std::map<std::string, std::string>& headers);
  Status serviceKeepAlive();
  Status pauseForIngressBackpressure(bool allow_pong_grace);
  Status serviceKeepAliveAfterIngress();
  void noteIngressBackpressureFrameDrained();
  void finishIngressBackpressureEpisode();
  Status handleControlFrame(uint8_t opcode, bool final_frame,
                            const uint8_t* bytes, size_t length,
                            size_t frame_payload_bytes);
  Status consumeDataChunk(uint8_t opcode, bool final_frame,
                          size_t frame_payload_bytes, size_t frame_offset,
                          const uint8_t* bytes, size_t length);
  bool ingressDataReady() const;
  Status send(uint8_t opcode, const uint8_t* bytes, size_t length);
  void releaseTransport();
  void notifyClosed(int code, const char* reason);

  EspEndpointSecurity& endpointSecurity_;
  esp_transport_handle_t network_;
  esp_transport_handle_t websocket_;
  IWebSocketListener* listener_;
  WssIngressAssembler ingress_;
  WssKeepAlive keep_alive_;
  std::string host_;
  std::string path_;
  std::string headerBlock_;
  uint16_t port_;
  size_t currentFrameOffset_;
  std::array<uint8_t, 2048> deferredDataChunk_;
  size_t deferredDataLength_;
  size_t deferredFramePayloadBytes_;
  size_t deferredFrameOffset_;
  uint8_t deferredOpcode_;
  bool deferredFinalFrame_;
  bool deferredDataPending_;
  uint64_t ingressBackpressureStartedMs_;
  bool ingressBackpressureActive_;
  bool ingressBackpressureFrameDrained_;
  bool postBackpressureControlGrace_;
  bool episodeControlGraceUsed_;
  bool pendingPongBackpressureRebaseUsed_;
  bool connected_;
  IngressReady ingressReady_;
  void* ingressReadyContext_;
};

}  // namespace myai
}  // namespace inkloop
