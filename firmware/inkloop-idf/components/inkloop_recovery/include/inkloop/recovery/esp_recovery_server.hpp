#pragma once

#include <cstdint>

#include "esp_err.h"
#include "inkloop/recovery/recovery_network_config.hpp"
#include "inkloop/recovery/recovery_portal.hpp"

namespace inkloop {
namespace recovery {

struct EspRecoveryServerConfig {
  uint16_t port = kRecoveryHttpPort;
  uint8_t maximum_open_sockets = 4U;
};

// Caller-owned, caller-started HTTP seam. It has no worker, polling loop,
// timer, storage adapter or product owner dependency.
class EspRecoveryServer {
 public:
  explicit EspRecoveryServer(RecoveryPortalCore& core,
                             const EspRecoveryServerConfig& config = {});
  ~EspRecoveryServer();

  EspRecoveryServer(const EspRecoveryServer&) = delete;
  EspRecoveryServer& operator=(const EspRecoveryServer&) = delete;

  esp_err_t start();
  esp_err_t stop();
  bool running() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace recovery
}  // namespace inkloop
