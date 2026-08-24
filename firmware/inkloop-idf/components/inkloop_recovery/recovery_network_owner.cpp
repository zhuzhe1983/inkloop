#include "inkloop/recovery/recovery_network_owner.hpp"

#include <array>
#include <cstring>
#include <exception>
#include <new>
#include <string>

#include "esp_log.h"
#include "esp_random.h"
#include "inkloop/recovery/recovery_network_stop.hpp"
#include "mdns.h"

namespace inkloop {
namespace recovery {
namespace {

constexpr char kTag[] = "ink-recovery-net";
constexpr uint32_t kStartRetryIntervalMs = 1000U;
constexpr size_t kTokenEntropyBytes = 24U;
constexpr unsigned kDestructorStopAttempts = 8U;

template <size_t Size>
void secureZero(std::array<char, Size>& value) {
  volatile char* bytes = value.data();
  for (size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
}

template <size_t Size>
void secureZero(std::array<uint8_t, Size>& value) {
  volatile uint8_t* bytes = value.data();
  for (size_t index = 0; index < value.size(); ++index) bytes[index] = 0U;
}

void secureZero(std::string& value) {
  volatile char* bytes = value.empty() ? nullptr : &value[0];
  for (size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
  value.clear();
}

void scrubAccess(RecoveryAccessConfig& access) {
  secureZero(access.access_code);
  secureZero(access.session_id);
  secureZero(access.csrf_token);
  for (std::string& host : access.allowed_hosts) secureZero(host);
  for (std::string& origin : access.allowed_origins) secureZero(origin);
  access.allowed_host_count = 0U;
  access.allowed_origin_count = 0U;
  access.session_lifetime_seconds = 0U;
}

std::string bootToken() {
  constexpr char kHex[] = "0123456789abcdef";
  std::array<uint8_t, kTokenEntropyBytes> entropy{};
  esp_fill_random(entropy.data(), entropy.size());
  std::string token(entropy.size() * 2U, '0');
  for (size_t index = 0; index < entropy.size(); ++index) {
    token[index * 2U] = kHex[entropy[index] >> 4U];
    token[index * 2U + 1U] = kHex[entropy[index] & 0x0fU];
  }
  secureZero(entropy);
  return token;
}

bool due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool sameAddress(const std::array<char, 16>& left,
                 const std::array<char, 16>& right) {
  return std::strncmp(left.data(), right.data(), left.size()) == 0;
}

std::array<char, 16> readyAddress(const EspWifiStationSnapshot& wifi) {
  if (wifi.phase == WifiStationPhase::Online && wifi.ipv4[0] != '\0')
    return wifi.ipv4;
  if (wifi.provisioning_ap && wifi.provisioning_ipv4[0] != '\0')
    return wifi.provisioning_ipv4;
  return {};
}

}  // namespace

RecoveryNetworkModeOwner::RecoveryNetworkModeOwner(
    const IRecoveryDiagnosticCache& cache,
    IRecoveryActionOwner* action_owner,
    IRecoveryExportOwner* export_owner,
    RecoveryWifiStoragePolicy wifi_storage)
    : cache_(cache),
      action_owner_(action_owner),
      export_owner_(export_owner),
      wifi_storage_(wifi_storage) {}

RecoveryNetworkModeOwner::~RecoveryNetworkModeOwner() {
  esp_err_t status = ESP_FAIL;
  for (unsigned attempt = 1U;
       attempt <= kDestructorStopAttempts && status != ESP_OK; ++attempt) {
    status = stop();
  }
  if (status == ESP_OK) return;

  // Letting unique_ptr destructors run after a native stop failure could free
  // callback contexts still owned by HTTP/Wi-Fi. Fail-stop instead; explicit
  // callers retain the object and can retry without reaching this boundary.
  ESP_LOGE(kTag, "recovery network destructor could not stop: %s",
           esp_err_to_name(status));
  std::terminate();
}

esp_err_t RecoveryNetworkModeOwner::ensureWifiOwner() {
  if (stop_requested_) return ESP_ERR_INVALID_STATE;
  if (wifi_) return ESP_OK;
  if (wifi_storage_ != RecoveryWifiStoragePolicy::VolatileRam)
    return ESP_ERR_INVALID_ARG;
  EspWifiStationConfig wifi_config;
  wifi_config.credential_storage = WifiCredentialStorage::VolatileRam;
  wifi_.reset(new (std::nothrow) EspWifiStationOwner(wifi_config));
  return wifi_ ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t RecoveryNetworkModeOwner::setLocalAccessCodeOverride(
    const std::string& value) {
  if (initialized_ || stop_requested_) return ESP_ERR_INVALID_STATE;
  const esp_err_t allocated = ensureWifiOwner();
  return allocated == ESP_OK ? wifi_->setLocalAccessCodeOverride(value)
                             : allocated;
}

esp_err_t RecoveryNetworkModeOwner::initialize(uint32_t now_ms) {
  if (initialized_ || stop_requested_) return ESP_ERR_INVALID_STATE;
  const esp_err_t allocated = ensureWifiOwner();
  if (allocated != ESP_OK) return allocated;
  const esp_err_t status = wifi_->initialize(now_ms);
  if (status != ESP_OK) {
    wifi_.reset();
    return status;
  }
  initialized_ = true;
  next_start_attempt_ms_ = now_ms;
  return ESP_OK;
}

esp_err_t RecoveryNetworkModeOwner::startForAddress(
    const std::array<char, 16>& actual_ipv4) {
  if (!wifi_ || !validRecoveryLocalIpv4(actual_ipv4.data()))
    return ESP_ERR_INVALID_STATE;

  std::array<char, 64> local_access = wifi_->localAccessCode();
  std::string session_token = bootToken();
  std::string csrf_token = bootToken();
  RecoveryAccessConfig access;
  RecoveryEndpointGuidance next_guidance;
  const bool configured = buildRecoveryAccessConfig(
      local_access, actual_ipv4.data(), session_token, csrf_token, access,
      next_guidance);
  secureZero(local_access);
  secureZero(session_token);
  secureZero(csrf_token);
  if (!configured) {
    scrubAccess(access);
    return ESP_ERR_INVALID_ARG;
  }

  std::unique_ptr<RecoveryPortalCore> next_core(
      new (std::nothrow) RecoveryPortalCore(
          access, cache_, action_owner_, export_owner_));
  scrubAccess(access);
  if (!next_core) return ESP_ERR_NO_MEM;
  if (!next_core->ready()) return ESP_ERR_INVALID_STATE;

  EspRecoveryServerConfig config;
  config.port = kRecoveryHttpPort;
  std::unique_ptr<EspRecoveryServer> next_server(
      new (std::nothrow) EspRecoveryServer(*next_core, config));
  if (!next_server) return ESP_ERR_NO_MEM;

  esp_err_t status = mdns_init();
  const bool mdns_initialized = status == ESP_OK;
  if (status == ESP_OK) status = mdns_hostname_set("inkloop");
  if (status == ESP_OK)
    status = mdns_instance_name_set("Inkloop read-only recovery");
  if (status == ESP_OK) {
    status = mdns_service_add("Inkloop recovery", "_http", "_tcp",
                              kRecoveryHttpPort, nullptr, 0U);
  }
  if (status != ESP_OK) {
    if (mdns_initialized) mdns_free();
    return status;
  }
  status = next_server->start();
  if (status != ESP_OK) {
    mdns_service_remove("_http", "_tcp");
    mdns_free();
    return status;
  }

  core_ = std::move(next_core);
  server_ = std::move(next_server);
  active_ipv4_ = actual_ipv4;
  guidance_ = next_guidance;
  mdns_started_ = true;
  ESP_LOGI(kTag, "read-only recovery ready mdns=%s local=%s",
           guidance_.mdns_url.data(), guidance_.local_ip_url.data());
  return ESP_OK;
}

esp_err_t RecoveryNetworkModeOwner::stopRecoveryServices() {
  if (server_ && server_->running()) {
    const esp_err_t status = server_->stop();
    if (status != ESP_OK) return status;
  }
  server_.reset();
  core_.reset();
  if (mdns_started_) {
    mdns_service_remove("_http", "_tcp");
    mdns_free();
    mdns_started_ = false;
  }
  secureZero(active_ipv4_);
  secureZero(guidance_.mdns_url);
  secureZero(guidance_.local_ip_url);
  return ESP_OK;
}

void RecoveryNetworkModeOwner::tick(uint32_t now_ms) {
  if (!initialized_ || !wifi_ || stop_requested_) return;
  wifi_->tick(now_ms);
  const EspWifiStationSnapshot wifi = wifi_->snapshot();
  const std::array<char, 16> address = readyAddress(wifi);
  if (address[0] == '\0') {
    if (server_) stopRecoveryServices();
    return;
  }
  if (server_ && sameAddress(address, active_ipv4_)) return;
  if (server_ && stopRecoveryServices() != ESP_OK) return;
  if (!due(now_ms, next_start_attempt_ms_)) return;
  const esp_err_t status = startForAddress(address);
  if (status != ESP_OK) {
    next_start_attempt_ms_ = now_ms + kStartRetryIntervalMs;
    ESP_LOGE(kTag, "recovery endpoint start failed: %s",
             esp_err_to_name(status));
  }
}

esp_err_t RecoveryNetworkModeOwner::stop() {
  stop_requested_ = true;
  const esp_err_t status = detail::stopRecoveryNetworkOwners(
      wifi_, [this] { return stopRecoveryServices(); });
  if (status != ESP_OK) return status;
  initialized_ = false;
  next_start_attempt_ms_ = 0U;
  stop_requested_ = false;
  return ESP_OK;
}

RecoveryNetworkModeSnapshot RecoveryNetworkModeOwner::snapshot() const {
  RecoveryNetworkModeSnapshot output;
  output.initialized = initialized_;
  if (wifi_) {
    const EspWifiStationSnapshot wifi = wifi_->snapshot();
    output.station_online = wifi.phase == WifiStationPhase::Online;
    output.settings_ap_active = wifi.provisioning_ap;
  }
  output.recovery_server_running = server_ && server_->running();
  output.actual_ipv4 = active_ipv4_;
  output.guidance = guidance_;
  return output;
}

}  // namespace recovery
}  // namespace inkloop
