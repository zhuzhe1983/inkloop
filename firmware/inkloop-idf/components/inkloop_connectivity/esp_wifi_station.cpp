#include "inkloop/esp_wifi_station.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

namespace inkloop {
namespace {

constexpr std::time_t kMinimumValidEpoch = 1700000000;
constexpr char kTag[] = "ink-wifi";

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

template <size_t Size>
void secureZero(std::array<char, Size>& value) {
  volatile char* bytes = value.data();
  for (size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
}

}  // namespace

EspWifiStationOwner::~EspWifiStationOwner() {
  (void)shutdown();
}

esp_err_t EspWifiStationOwner::shutdown() {
  if (provisioning_server_ && provisioning_server_->running()) {
    const esp_err_t stopped = provisioning_server_->stop();
    if (stopped != ESP_OK) return stopped;
  }
  provisioning_server_.reset();
  provisioning_portal_.reset();
  if (sntp_handler_) {
    const esp_err_t unregistered = esp_event_handler_instance_unregister(
        NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, sntp_handler_);
    if (unregistered != ESP_OK) return unregistered;
    sntp_handler_ = nullptr;
  }
  if (sntp_initialized_) {
    esp_netif_sntp_deinit();
    sntp_initialized_ = false;
  }
  if (wifi_handler_) {
    const esp_err_t unregistered = esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_);
    if (unregistered != ESP_OK) return unregistered;
    wifi_handler_ = nullptr;
  }
  if (ip_handler_) {
    const esp_err_t unregistered = esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
    if (unregistered != ESP_OK) return unregistered;
    ip_handler_ = nullptr;
  }
  if (wifi_started_ && !wifi_sleep_stopped_) {
    const esp_err_t stopped = esp_wifi_stop();
    if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_STARTED)
      return stopped;
  }
  wifi_started_ = false;
  wifi_sleep_stopped_ = false;
  if (wifi_initialized_) {
    const esp_err_t deinitialized = esp_wifi_deinit();
    if (deinitialized != ESP_OK && deinitialized != ESP_ERR_WIFI_NOT_INIT)
      return deinitialized;
  }
  wifi_initialized_ = false;
  if (ap_netif_) {
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
  }
  if (station_netif_) {
    esp_netif_destroy_default_wifi(station_netif_);
    station_netif_ = nullptr;
  }
  if (submission_mutex_) {
    vSemaphoreDelete(submission_mutex_);
    submission_mutex_ = nullptr;
  }
  portENTER_CRITICAL(&mux_);
  core_ = WifiStationCore();
  ipv4_.fill('\0');
  provisioning_ssid_.fill('\0');
  provisioning_ipv4_.fill('\0');
  clock_synchronized_ = false;
  initialized_ = false;
  provisioning_active_ = false;
  local_access_override_ = false;
  submitted_credentials_pending_ = false;
  portEXIT_CRITICAL(&mux_);
  secureZero(local_access_code_);
  secureZero(default_local_access_code_);
  secureZero(submitted_ssid_);
  secureZero(submitted_password_);
  return ESP_OK;
}

bool EspWifiStationOwner::credentialFailure(uint16_t reason) {
  return reason == WIFI_REASON_AUTH_FAIL ||
         reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_AUTH_EXPIRE;
}

esp_err_t EspWifiStationOwner::initialize(uint32_t now_ms) {
  if (initialized_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = esp_netif_init();
  if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) return status;
  status = esp_event_loop_create_default();
  if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) return status;
  station_netif_ = esp_netif_create_default_wifi_sta();
  if (!station_netif_) return ESP_ERR_NO_MEM;

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  const bool persistent = config_.credential_storage ==
      WifiCredentialStorage::PersistentFlash;
  // Post-audit Recovery uses VolatileRam: disabling the Wi-Fi driver's NVS
  // integration prevents initialization/calibration/config code from writing
  // through a read-only NVS owner. Product and pre-audit OTA Recovery retain
  // the existing persistent behavior.
  config.nvs_enable = persistent;
  status = esp_wifi_init(&config);
  if (status != ESP_OK) return status;
  wifi_initialized_ = true;
  status = persistent ? esp_wifi_set_storage(WIFI_STORAGE_FLASH)
                      : esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (status == ESP_OK) status = esp_wifi_set_mode(WIFI_MODE_STA);
  if (status == ESP_OK) {
    status = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &EspWifiStationOwner::eventHandler,
        this, &wifi_handler_);
  }
  if (status == ESP_OK) {
    status = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &EspWifiStationOwner::eventHandler,
        this, &ip_handler_);
  }
  if (status == ESP_OK) {
    status = esp_event_handler_instance_register(
        NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC,
        &EspWifiStationOwner::eventHandler, this, &sntp_handler_);
  }
  if (status == ESP_OK) {
    if (::setenv("TZ", "CST-8", 1) != 0) return ESP_FAIL;
    ::tzset();
    const esp_sntp_config_t time_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org",
                                    "time.cloudflare.com"));
    status = esp_netif_sntp_init(&time_config);
    if (status == ESP_OK) sntp_initialized_ = true;
  }
  if (status != ESP_OK) return status;

  wifi_config_t saved{};
  status = esp_wifi_get_config(WIFI_IF_STA, &saved);
  if (status != ESP_OK) return status;
  const bool has_saved = saved.sta.ssid[0] != '\0';
  submission_mutex_ = xSemaphoreCreateMutexStatic(&submission_mutex_storage_);
  if (!submission_mutex_) return ESP_ERR_NO_MEM;
  deriveLocalAccessCode(saved);
  uint8_t ap_mac[6]{};
  status = esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
  if (status != ESP_OK) return status;
  std::snprintf(provisioning_ssid_.data(), provisioning_ssid_.size(),
                "Inkloop-%02X%02X-Settings", ap_mac[4], ap_mac[5]);
  provisioning_portal_.reset(
      new (std::nothrow) WifiProvisioningPortal(*this));
  if (!provisioning_portal_) return ESP_ERR_NO_MEM;
  std::memset(&saved, 0, sizeof(saved));
  status = esp_wifi_start();
  if (status != ESP_OK) return status;
  wifi_started_ = true;
  initialized_ = true;
  execute(core_.begin(has_saved, now_ms), now_ms);
  return core_.phase() == WifiStationPhase::Failed ? ESP_FAIL : ESP_OK;
}

void EspWifiStationOwner::execute(WifiStationAction action,
                                  uint32_t now_ms) {
  if (action == WifiStationAction::RequireProvisioning) {
    const esp_err_t started = startProvisioningPortal();
    if (started != ESP_OK && started != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(kTag, "provisioning portal unavailable: %s",
               esp_err_to_name(started));
    }
    return;
  }
  if (action != WifiStationAction::Connect) return;
  const esp_err_t status = esp_wifi_connect();
  portENTER_CRITICAL(&mux_);
  core_.connectStarted(now_ms, status == ESP_OK);
  portEXIT_CRITICAL(&mux_);
}

void EspWifiStationOwner::tick(uint32_t now_ms) {
  serviceSubmittedCredentials(now_ms);
  portENTER_CRITICAL(&mux_);
  const WifiStationAction action = core_.tick(now_ms);
  const bool online = core_.online();
  portEXIT_CRITICAL(&mux_);
  execute(action, now_ms);
  if (online && provisioningActive()) stopProvisioningPortal();
}

void EspWifiStationOwner::deriveLocalAccessCode(
    const wifi_config_t& station) {
  std::array<char, 64> next{};
  const char* password = reinterpret_cast<const char*>(station.sta.password);
  const size_t length = strnlen(password, sizeof(station.sta.password));
  bool printable = length >= 8U && length <= 63U;
  for (size_t at = 0; printable && at < length; ++at) {
    const uint8_t ch = static_cast<uint8_t>(password[at]);
    printable = ch >= 0x20U && ch <= 0x7eU;
  }
  if (printable) {
    std::memcpy(next.data(), password, length);
  } else {
    uint8_t mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
      std::snprintf(next.data(), next.size(), "inkloop-%02x%02x",
                    mac[4], mac[5]);
    } else {
      std::memcpy(next.data(), "inkloop-local", 13U);
    }
  }
  portENTER_CRITICAL(&mux_);
  default_local_access_code_ = next;
  if (!local_access_override_) local_access_code_ = next;
  portEXIT_CRITICAL(&mux_);
  secureZero(next);
}

esp_err_t EspWifiStationOwner::setLocalAccessCodeOverride(
    const std::string& value) {
  if (value.size() > 63U) return ESP_ERR_INVALID_ARG;
  if (!value.empty() && value.size() < 8U) return ESP_ERR_INVALID_ARG;
  for (const unsigned char ch : value) {
    if (ch < 0x20U || ch > 0x7eU) return ESP_ERR_INVALID_ARG;
  }
  std::array<char, 64> next{};
  portENTER_CRITICAL(&mux_);
  local_access_override_ = !value.empty();
  next = local_access_override_ ? std::array<char, 64>{}
                                : default_local_access_code_;
  if (!value.empty()) std::copy(value.begin(), value.end(), next.begin());
  local_access_code_ = next;
  portEXIT_CRITICAL(&mux_);
  secureZero(next);
  return ESP_OK;
}

WifiProvisioningSubmitResult EspWifiStationOwner::trySubmitCredentials(
    const std::string& ssid, const std::string& password) {
  if (!WifiProvisioningPortal::validSsid(ssid) ||
      !WifiProvisioningPortal::validPassword(password)) {
    return WifiProvisioningSubmitResult::Invalid;
  }
  if (!provisioningActive())
    return WifiProvisioningSubmitResult::Unavailable;
  if (!submission_mutex_ || xSemaphoreTake(submission_mutex_, 0) != pdTRUE)
    return WifiProvisioningSubmitResult::Busy;
  if (submitted_credentials_pending_) {
    xSemaphoreGive(submission_mutex_);
    return WifiProvisioningSubmitResult::Busy;
  }
  submitted_ssid_.fill('\0');
  submitted_password_.fill('\0');
  std::copy(ssid.begin(), ssid.end(), submitted_ssid_.begin());
  std::copy(password.begin(), password.end(), submitted_password_.begin());
  submitted_credentials_pending_ = true;
  xSemaphoreGive(submission_mutex_);
  return WifiProvisioningSubmitResult::Accepted;
}

void EspWifiStationOwner::serviceSubmittedCredentials(uint32_t now_ms) {
  if (!submission_mutex_ ||
      xSemaphoreTake(submission_mutex_, 0) != pdTRUE) return;
  if (!submitted_credentials_pending_) {
    xSemaphoreGive(submission_mutex_);
    return;
  }
  wifi_config_t station{};
  std::memcpy(station.sta.ssid, submitted_ssid_.data(),
              sizeof(station.sta.ssid));
  std::memcpy(station.sta.password, submitted_password_.data(),
              sizeof(station.sta.password));
  submitted_credentials_pending_ = false;
  submitted_ssid_.fill('\0');
  submitted_password_.fill('\0');
  xSemaphoreGive(submission_mutex_);

  const bool persistent = config_.credential_storage ==
      WifiCredentialStorage::PersistentFlash;
  esp_err_t status = persistent
      ? esp_wifi_set_storage(WIFI_STORAGE_FLASH)
      : esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (status == ESP_OK) status = esp_wifi_set_config(WIFI_IF_STA, &station);
  if (status != ESP_OK) {
    ESP_LOGE(kTag, "submitted Wi-Fi credential commit failed: %s",
             esp_err_to_name(status));
    std::memset(&station, 0, sizeof(station));
    return;
  }
  deriveLocalAccessCode(station);
  std::memset(&station, 0, sizeof(station));
  esp_wifi_disconnect();
  portENTER_CRITICAL(&mux_);
  core_ = WifiStationCore();
  const WifiStationAction action = core_.begin(true, now_ms);
  ipv4_.fill('\0');
  portEXIT_CRITICAL(&mux_);
  if (persistent) {
    ESP_LOGI(kTag, "submitted Wi-Fi credentials persisted; connecting");
  } else {
    ESP_LOGI(kTag,
             "submitted Wi-Fi credentials kept in RAM for Recovery; "
             "connecting");
  }
  execute(action, now_ms);
}

esp_err_t EspWifiStationOwner::startProvisioningPortal() {
  if (provisioningActive()) return ESP_ERR_INVALID_STATE;
  if (!wifi_initialized_ || !wifi_started_ || !provisioning_portal_)
    return ESP_ERR_INVALID_STATE;
  if (!ap_netif_) {
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (!ap_netif_) return ESP_ERR_NO_MEM;
  }
  wifi_config_t access_point{};
  std::memcpy(access_point.ap.ssid, provisioning_ssid_.data(),
              provisioning_ssid_.size() - 1U);
  std::array<char, 64> password = localAccessCode();
  const size_t password_length = strnlen(password.data(), password.size());
  std::memcpy(access_point.ap.password, password.data(), password_length);
  secureZero(password);
  access_point.ap.ssid_len = static_cast<uint8_t>(
      strnlen(provisioning_ssid_.data(), provisioning_ssid_.size()));
  access_point.ap.channel = 1;
  access_point.ap.max_connection = 2;
  access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
  esp_err_t status = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (status == ESP_OK)
    status = esp_wifi_set_config(WIFI_IF_AP, &access_point);
  std::memset(&access_point, 0, sizeof(access_point));
  if (status != ESP_OK) return status;
  provisioning_server_.reset(
      new (std::nothrow) EspWifiProvisioningServer(*provisioning_portal_));
  if (!provisioning_server_) return ESP_ERR_NO_MEM;
  status = provisioning_server_->start();
  if (status != ESP_OK) {
    provisioning_server_.reset();
    esp_wifi_set_mode(WIFI_MODE_STA);
    return status;
  }
  esp_netif_ip_info_t ap_ip{};
  status = esp_netif_get_ip_info(ap_netif_, &ap_ip);
  std::array<char, 16> actual_ipv4{};
  if (status == ESP_OK && ap_ip.ip.addr != 0U &&
      !esp_ip4addr_ntoa(&ap_ip.ip, actual_ipv4.data(), actual_ipv4.size())) {
    status = ESP_FAIL;
  }
  if (status != ESP_OK || actual_ipv4[0] == '\0') {
    provisioning_server_->stop();
    provisioning_server_.reset();
    esp_wifi_set_mode(WIFI_MODE_STA);
    return status == ESP_OK ? ESP_FAIL : status;
  }
  portENTER_CRITICAL(&mux_);
  provisioning_active_ = true;
  provisioning_ipv4_ = actual_ipv4;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGI(kTag, "settings AP ready ssid=%s url=http://%s/",
           provisioning_ssid_.data(), actual_ipv4.data());
  return ESP_OK;
}

esp_err_t EspWifiStationOwner::stopProvisioningPortal() {
  if (!provisioningActive()) return ESP_ERR_INVALID_STATE;
  esp_err_t status = ESP_OK;
  if (provisioning_server_ && provisioning_server_->running())
    status = provisioning_server_->stop();
  if (status != ESP_OK) return status;
  provisioning_server_.reset();
  status = esp_wifi_set_mode(WIFI_MODE_STA);
  if (status != ESP_OK) return status;
  if (ap_netif_) {
    esp_netif_destroy_default_wifi(ap_netif_);
    ap_netif_ = nullptr;
  }
  portENTER_CRITICAL(&mux_);
  provisioning_active_ = false;
  provisioning_ipv4_.fill('\0');
  portEXIT_CRITICAL(&mux_);
  return ESP_OK;
}

void EspWifiStationOwner::eventHandler(void* context,
                                       esp_event_base_t event_base,
                                       int32_t event_id, void* event_data) {
  if (context) {
    static_cast<EspWifiStationOwner*>(context)->handleEvent(
        event_base, event_id, event_data);
  }
}

void EspWifiStationOwner::handleEvent(esp_event_base_t event_base,
                                      int32_t event_id, void* event_data) {
  const uint32_t captured_ms = nowMs();
  portENTER_CRITICAL(&mux_);
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
      event_data) {
    const auto* disconnected =
        static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    ipv4_.fill('\0');
    core_.disconnected(disconnected->reason,
                       credentialFailure(disconnected->reason), captured_ms);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP &&
             event_data) {
    const auto* acquired = static_cast<const ip_event_got_ip_t*>(event_data);
    esp_ip4addr_ntoa(&acquired->ip_info.ip, ipv4_.data(), ipv4_.size());
    core_.connected();
  } else if (event_base == NETIF_SNTP_EVENT &&
             event_id == NETIF_SNTP_TIME_SYNC && event_data) {
    const auto* synchronized =
        static_cast<const esp_netif_sntp_time_sync_t*>(event_data);
    clock_synchronized_ = synchronized->tv.tv_sec >= kMinimumValidEpoch;
  }
  portEXIT_CRITICAL(&mux_);
}

EspWifiStationSnapshot EspWifiStationOwner::snapshot() const {
  portENTER_CRITICAL(&mux_);
  EspWifiStationSnapshot value;
  value.phase = core_.phase();
  value.ipv4 = ipv4_;
  value.last_disconnect_reason = core_.lastDisconnectReason();
  value.retry_count = core_.retryCount();
  value.saved_credentials = core_.hasSavedCredentials();
  value.provisioning_ap = provisioning_active_;
  value.provisioning_ssid = provisioning_ssid_;
  value.provisioning_ipv4 = provisioning_ipv4_;
  const std::time_t current = std::time(nullptr);
  value.clock_synchronized = clock_synchronized_ ||
      current >= kMinimumValidEpoch;
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool EspWifiStationOwner::online() const {
  portENTER_CRITICAL(&mux_);
  const bool value = core_.online();
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool EspWifiStationOwner::provisioningRequired() const {
  portENTER_CRITICAL(&mux_);
  const bool value =
      core_.phase() == WifiStationPhase::ProvisioningRequired ||
      core_.phase() == WifiStationPhase::NoCredentials;
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool EspWifiStationOwner::provisioningActive() const {
  portENTER_CRITICAL(&mux_);
  const bool value = provisioning_active_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

std::array<char, 64> EspWifiStationOwner::localAccessCode() const {
  portENTER_CRITICAL(&mux_);
  const std::array<char, 64> value = local_access_code_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

esp_err_t EspWifiStationOwner::prepareForSleep() {
  if (!initialized_ || provisioningActive()) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = esp_wifi_stop();
  if (status == ESP_OK) wifi_sleep_stopped_ = true;
  return status;
}

esp_err_t EspWifiStationOwner::restoreAfterSleep(uint32_t now_ms) {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (!wifi_sleep_stopped_) return ESP_OK;
  esp_err_t status = esp_wifi_start();
  if (status == ESP_OK) {
    wifi_sleep_stopped_ = false;
    status = esp_wifi_connect();
  }
  if (status != ESP_OK) {
    portENTER_CRITICAL(&mux_);
    core_.connectStarted(now_ms, false);
    portEXIT_CRITICAL(&mux_);
  }
  return status;
}

}  // namespace inkloop
