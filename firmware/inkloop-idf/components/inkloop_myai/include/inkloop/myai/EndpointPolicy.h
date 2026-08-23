#pragma once

#include "MyAiTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace inkloop {
namespace myai {

struct HttpsEndpoint {
  std::string host;
  uint16_t port;
  bool tls;

  HttpsEndpoint() : port(443), tls(true) {}
};

// Portable parsing and address classification shared by every native MyAI
// transport. It is deliberately stricter than a generic URL parser because
// these endpoints receive device and gateway bearer credentials.
class EndpointPolicy final {
 public:
  static constexpr size_t kMaximumUrlBytes = 2048;

  static Status parseHttpsUrl(const std::string& url, HttpsEndpoint& endpoint);
  static Status parsePublicUrl(const std::string& url,
                               bool allowPlaintextHttp,
                               HttpsEndpoint& endpoint);
  static bool isPublicIpv4(const std::array<uint8_t, 4>& address);
  static bool isPublicIpv6(const std::array<uint8_t, 16>& address);
};

}  // namespace myai
}  // namespace inkloop
