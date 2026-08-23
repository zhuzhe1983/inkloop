#pragma once

#include "inkloop/myai/MyAiAdapters.h"

namespace inkloop {
namespace myai {

class EspEndpointSecurity final : public IEndpointSecurity {
 public:
  Status validatePublicTlsEndpoint(const std::string& httpsUrl) override;
  Status validatePublicEndpoint(const std::string& url) override;
  Status validateConnectedSocket(int socket) const;
};

class EspHttpTransport final : public IHttpTransport {
 public:
  explicit EspHttpTransport(EspEndpointSecurity& endpointSecurity);

  Status perform(const HttpRequest& request, HttpResponse& response) override;

 private:
  EspEndpointSecurity& endpointSecurity_;
};

class EspClock final : public IClock {
 public:
  uint64_t monotonicMs() const override;
  std::string utcIso8601() const override;
};

class EspGatewayProbeSet final : public IGatewayProbeSet {
 public:
  explicit EspGatewayProbeSet(IClock& clock);

  Status probeConcurrent(
      const std::vector<GatewayCandidate>& candidates,
      const std::map<std::string, std::string>& headers,
      uint32_t totalDeadlineMs, std::vector<GatewayProbe>& results) override;

 private:
  IClock& clock_;
};

}  // namespace myai
}  // namespace inkloop
