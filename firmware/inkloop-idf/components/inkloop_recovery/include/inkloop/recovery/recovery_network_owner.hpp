#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "esp_err.h"
#include "inkloop/esp_wifi_station.hpp"
#include "inkloop/recovery/esp_recovery_server.hpp"
#include "inkloop/recovery/recovery_network_config.hpp"
#include "inkloop/recovery/recovery_portal.hpp"

namespace inkloop {
namespace recovery {

enum class RecoveryWifiStoragePolicy : uint8_t {
  // Required after a boot audit has established read-only NVS ownership.
  // Existing Wi-Fi credentials are intentionally not loaded or replaced;
  // provisioning, if needed, applies only for this Recovery boot.
  VolatileRam,
  // Allowed only in the pre-audit OTA-health refusal path.
  PersistentFlash,
};

struct RecoveryNetworkModeSnapshot {
  bool initialized = false;
  bool station_online = false;
  bool settings_ap_active = false;
  bool recovery_server_running = false;
  std::array<char, 16> actual_ipv4{};
  RecoveryEndpointGuidance guidance{};
};

// Boot-refusal-only composition owner. The caller provides a precomputed,
// read-only diagnostic cache and drives tick(); this class creates no task or
// timer and never constructs a normal product owner.
class RecoveryNetworkModeOwner final {
 public:
  explicit RecoveryNetworkModeOwner(
      const IRecoveryDiagnosticCache& cache,
      IRecoveryActionOwner* action_owner = nullptr,
      IRecoveryExportOwner* export_owner = nullptr,
      RecoveryWifiStoragePolicy wifi_storage =
          RecoveryWifiStoragePolicy::VolatileRam);
  ~RecoveryNetworkModeOwner();

  RecoveryNetworkModeOwner(const RecoveryNetworkModeOwner&) = delete;
  RecoveryNetworkModeOwner& operator=(const RecoveryNetworkModeOwner&) =
      delete;

  // Must be called before initialize. An empty value selects the Wi-Fi
  // owner's saved-home-password/default policy.
  esp_err_t setLocalAccessCodeOverride(const std::string& value);
  esp_err_t initialize(uint32_t now_ms);
  void tick(uint32_t now_ms);
  // Idempotent. A successful stop has stopped HTTP/mDNS/Wi-Fi and scrubbed all
  // access/session/CSRF material. A native stop failure leaves ownership intact
  // so the caller can retry safely.
  esp_err_t stop();

  RecoveryNetworkModeSnapshot snapshot() const;

 private:
  esp_err_t ensureWifiOwner();
  esp_err_t startForAddress(const std::array<char, 16>& actual_ipv4);
  esp_err_t stopRecoveryServices();

  const IRecoveryDiagnosticCache& cache_;
  IRecoveryActionOwner* action_owner_ = nullptr;
  IRecoveryExportOwner* export_owner_ = nullptr;
  RecoveryWifiStoragePolicy wifi_storage_ =
      RecoveryWifiStoragePolicy::VolatileRam;
  std::unique_ptr<EspWifiStationOwner> wifi_;
  std::unique_ptr<RecoveryPortalCore> core_;
  std::unique_ptr<EspRecoveryServer> server_;
  std::array<char, 16> active_ipv4_{};
  RecoveryEndpointGuidance guidance_{};
  uint32_t next_start_attempt_ms_ = 0U;
  bool initialized_ = false;
  bool mdns_started_ = false;
  bool stop_requested_ = false;
};

}  // namespace recovery
}  // namespace inkloop
