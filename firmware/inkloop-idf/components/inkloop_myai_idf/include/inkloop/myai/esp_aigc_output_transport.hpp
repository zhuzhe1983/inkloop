#pragma once

#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/myai/esp_http_adapters.hpp"

namespace inkloop {
namespace myai {

// Blocking transport intended to run only on the slow network owner. HTTP
// framing is removed by esp_http_client_read(), while the JSON/base64 body is
// decoded incrementally into the storage-owner sink.
class EspAigcOutputTransport final : public IAigcOutputTransport {
 public:
  explicit EspAigcOutputTransport(EspEndpointSecurity& endpointSecurity);

  Status postAndDecodeBase64(const HttpRequest& request,
                             size_t maxEncodedBytes,
                             size_t maxDecodedBytes, IImageSink& sink,
                             AigcOutputMetadata& metadata) override;

 private:
  EspEndpointSecurity& endpoint_security_;
};

}  // namespace myai
}  // namespace inkloop
