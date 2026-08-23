#include "inkloop/wifi_station_core.hpp"

#include <algorithm>

namespace inkloop {

WifiStationCore::WifiStationCore(WifiStationPolicy policy) : policy_(policy) {}

bool WifiStationCore::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

uint32_t WifiStationCore::retryDelayMs() const {
  const uint8_t shift = std::min<uint8_t>(retry_count_, 3U);
  const uint64_t delay =
      static_cast<uint64_t>(policy_.retry_base_ms) << shift;
  return static_cast<uint32_t>(
      std::min<uint64_t>(delay, policy_.retry_max_ms));
}

WifiStationAction WifiStationCore::begin(bool has_saved_credentials,
                                         uint32_t now_ms) {
  if (phase_ != WifiStationPhase::Uninitialized)
    return WifiStationAction::None;
  has_saved_credentials_ = has_saved_credentials;
  if (!has_saved_credentials_) {
    phase_ = WifiStationPhase::NoCredentials;
    return WifiStationAction::RequireProvisioning;
  }
  overall_deadline_ms_ = now_ms + policy_.saved_connect_timeout_ms;
  phase_ = WifiStationPhase::Connecting;
  return WifiStationAction::Connect;
}

void WifiStationCore::connectStarted(uint32_t now_ms, bool accepted) {
  (void)now_ms;
  if (phase_ != WifiStationPhase::Connecting &&
      phase_ != WifiStationPhase::RetryWaiting)
    return;
  if (!accepted) {
    phase_ = WifiStationPhase::Failed;
  } else {
    phase_ = WifiStationPhase::Connecting;
  }
}

void WifiStationCore::connected() {
  if (!has_saved_credentials_ || phase_ == WifiStationPhase::Failed) return;
  phase_ = WifiStationPhase::Online;
  retry_count_ = 0;
  credential_failures_ = 0;
  last_disconnect_reason_ = 0;
}

void WifiStationCore::disconnected(uint16_t reason, bool credential_failure,
                                   uint32_t now_ms) {
  if (!has_saved_credentials_ || phase_ == WifiStationPhase::Failed ||
      phase_ == WifiStationPhase::ProvisioningRequired)
    return;
  const bool was_online = phase_ == WifiStationPhase::Online;
  last_disconnect_reason_ = reason;
  if (was_online) {
    overall_deadline_ms_ = now_ms + policy_.saved_connect_timeout_ms;
    retry_count_ = 0;
    credential_failures_ = 0;
  }
  if (credential_failure && credential_failures_ < UINT8_MAX)
    ++credential_failures_;
  if (credential_failures_ >= policy_.credential_failure_limit) {
    phase_ = WifiStationPhase::ProvisioningRequired;
    return;
  }
  if (retry_count_ < UINT8_MAX) ++retry_count_;
  retry_at_ms_ = now_ms + retryDelayMs();
  phase_ = WifiStationPhase::RetryWaiting;
}

WifiStationAction WifiStationCore::tick(uint32_t now_ms) {
  if (phase_ == WifiStationPhase::NoCredentials) {
    phase_ = WifiStationPhase::ProvisioningRequired;
    return WifiStationAction::RequireProvisioning;
  }
  if ((phase_ == WifiStationPhase::Connecting ||
       phase_ == WifiStationPhase::RetryWaiting) &&
      due(now_ms, overall_deadline_ms_)) {
    phase_ = WifiStationPhase::ProvisioningRequired;
    return WifiStationAction::RequireProvisioning;
  }
  if (phase_ == WifiStationPhase::RetryWaiting && due(now_ms, retry_at_ms_)) {
    phase_ = WifiStationPhase::Connecting;
    return WifiStationAction::Connect;
  }
  return WifiStationAction::None;
}

void WifiStationCore::fail() { phase_ = WifiStationPhase::Failed; }

const char* wifiStationPhaseName(WifiStationPhase phase) {
  switch (phase) {
    case WifiStationPhase::Uninitialized:
      return "UNINITIALIZED";
    case WifiStationPhase::NoCredentials:
      return "NO_CREDENTIALS";
    case WifiStationPhase::Connecting:
      return "CONNECTING";
    case WifiStationPhase::RetryWaiting:
      return "RETRY_WAITING";
    case WifiStationPhase::Online:
      return "ONLINE";
    case WifiStationPhase::ProvisioningRequired:
      return "PROVISIONING_REQUIRED";
    case WifiStationPhase::Failed:
      return "FAILED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
