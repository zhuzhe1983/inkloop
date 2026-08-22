#pragma once

#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/myai/WssIngress.h"
#include "inkloop/myai/esp_http_adapters.hpp"

#include <map>
#include <string>

struct esp_transport_item_t;
typedef struct esp_transport_item_t* esp_transport_handle_t;

namespace inkloop {
namespace myai {

// Cooperative native WSS adapter. connect/send/poll are called only by the
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
  Status send(uint8_t opcode, const uint8_t* bytes, size_t length);
  void releaseTransport();
  void notifyClosed(int code, const char* reason);

  EspEndpointSecurity& endpointSecurity_;
  esp_transport_handle_t ssl_;
  esp_transport_handle_t websocket_;
  IWebSocketListener* listener_;
  WssIngressAssembler ingress_;
  std::string host_;
  std::string path_;
  std::string headerBlock_;
  uint16_t port_;
  size_t currentFrameOffset_;
  bool connected_;
  IngressReady ingressReady_;
  void* ingressReadyContext_;
};

}  // namespace myai
}  // namespace inkloop
