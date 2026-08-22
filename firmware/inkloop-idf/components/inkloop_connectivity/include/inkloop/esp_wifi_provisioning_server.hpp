#pragma once

#include "esp_err.h"
#include "inkloop/wifi_provisioning_portal.hpp"

namespace inkloop {

class EspWifiProvisioningServer final {
 public:
  explicit EspWifiProvisioningServer(WifiProvisioningPortal& portal);
  ~EspWifiProvisioningServer();

  EspWifiProvisioningServer(const EspWifiProvisioningServer&) = delete;
  EspWifiProvisioningServer& operator=(const EspWifiProvisioningServer&) =
      delete;

  esp_err_t start();
  esp_err_t stop();
  bool running() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace inkloop
