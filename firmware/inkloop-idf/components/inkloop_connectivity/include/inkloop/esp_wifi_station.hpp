#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inkloop/esp_wifi_provisioning_server.hpp"
#include "inkloop/wifi_station_core.hpp"

namespace inkloop {

enum class WifiCredentialStorage : uint8_t {
  PersistentFlash,
  VolatileRam,
};

struct EspWifiStationConfig {
  WifiCredentialStorage credential_storage =
      WifiCredentialStorage::PersistentFlash;
};

struct EspWifiStationSnapshot {
  WifiStationPhase phase = WifiStationPhase::Uninitialized;
  std::array<char, 16> ipv4{};
  uint16_t last_disconnect_reason = 0;
  uint8_t retry_count = 0;
  bool saved_credentials = false;
  bool clock_synchronized = false;
  bool provisioning_ap = false;
  std::array<char, 33> provisioning_ssid{};
  // Actual address read back from the AP netif after the Settings server is
  // running. Consumers must not assume the ESP-IDF default 192.168.4.1.
  std::array<char, 16> provisioning_ipv4{};
};

// Sole native Wi-Fi owner. Existing ESP Wi-Fi NVS credentials are read and
// reused. Only an explicitly submitted provisioning request may replace the
// STA config; this class never performs a broad restore/erase. Association,
// retry, AP provisioning and credential commit are non-blocking.
class EspWifiStationOwner final : public IWifiProvisioningSink {
 public:
  explicit EspWifiStationOwner(EspWifiStationConfig config = {})
      : config_(config) {}
  ~EspWifiStationOwner();

  EspWifiStationOwner(const EspWifiStationOwner&) = delete;
  EspWifiStationOwner& operator=(const EspWifiStationOwner&) = delete;

  esp_err_t initialize(uint32_t now_ms);
  // Idempotent owner teardown. Stops provisioning HTTP/AP and STA, unregisters
  // all event/SNTP callbacks, destroys owned netifs and clears credentials.
  // Process-global esp_netif/default-event-loop state is deliberately kept so
  // a recovery network owner can initialize immediately afterward.
  esp_err_t shutdown();
  void tick(uint32_t now_ms);
  EspWifiStationSnapshot snapshot() const;
  bool online() const;
  bool provisioningRequired() const;
  bool provisioningActive() const;
  // Shared local credential: the provisioning Settings AP and the normal
  // local Portal use exactly the same default password for this boot.
  std::array<char, 64> localAccessCode() const;
  // The settings owner persists any user override and injects it here. This
  // Wi-Fi owner only keeps the bounded runtime copy; an empty value restores
  // the saved home-Wi-Fi-password default.
  esp_err_t setLocalAccessCodeOverride(const std::string& value);
  esp_err_t prepareForSleep();
  esp_err_t restoreAfterSleep(uint32_t now_ms);

  WifiProvisioningSubmitResult trySubmitCredentials(
      const std::string& ssid, const std::string& password) override;

 private:
  static void eventHandler(void* context, esp_event_base_t event_base,
                           int32_t event_id, void* event_data);
  void handleEvent(esp_event_base_t event_base, int32_t event_id,
                   void* event_data);
  void execute(WifiStationAction action, uint32_t now_ms);
  void serviceSubmittedCredentials(uint32_t now_ms);
  esp_err_t startProvisioningPortal();
  esp_err_t stopProvisioningPortal();
  void deriveLocalAccessCode(const wifi_config_t& station);
  static bool credentialFailure(uint16_t reason);

  EspWifiStationConfig config_{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  WifiStationCore core_{};
  std::array<char, 16> ipv4_{};
  std::array<char, 33> provisioning_ssid_{};
  std::array<char, 16> provisioning_ipv4_{};
  std::array<char, 64> local_access_code_{};
  std::array<char, 64> default_local_access_code_{};
  std::array<char, kMaximumWifiSsidBytes + 1U> submitted_ssid_{};
  std::array<char, kMaximumWifiPasswordBytes + 1U> submitted_password_{};
  esp_netif_t* station_netif_ = nullptr;
  esp_netif_t* ap_netif_ = nullptr;
  esp_event_handler_instance_t wifi_handler_ = nullptr;
  esp_event_handler_instance_t ip_handler_ = nullptr;
  esp_event_handler_instance_t sntp_handler_ = nullptr;
  bool wifi_initialized_ = false;
  bool wifi_started_ = false;
  bool sntp_initialized_ = false;
  bool clock_synchronized_ = false;
  bool initialized_ = false;
  bool provisioning_active_ = false;
  bool local_access_override_ = false;
  bool submitted_credentials_pending_ = false;
  bool wifi_sleep_stopped_ = false;
  StaticSemaphore_t submission_mutex_storage_{};
  SemaphoreHandle_t submission_mutex_ = nullptr;
  std::unique_ptr<WifiProvisioningPortal> provisioning_portal_;
  std::unique_ptr<EspWifiProvisioningServer> provisioning_server_;
};

}  // namespace inkloop
