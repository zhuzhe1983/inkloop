#pragma once

#include "inkloop/ota_https_acquisition.hpp"

namespace inkloop {

class EspOtaMonotonicClock final : public IOtaMonotonicClock {
 public:
  std::uint64_t nowMs() const override;
};

class EspOtaHttpsTransport final : public IOtaHttpsTransport {
 public:
  OtaHttpsFetchObservation get(
      const OtaHttpsFetchRequest& request,
      IOtaHttpsBodySink& sink) override;
};

}  // namespace inkloop
