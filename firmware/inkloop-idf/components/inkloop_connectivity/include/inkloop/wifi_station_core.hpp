#pragma once

#include <cstdint>

namespace inkloop {

enum class WifiStationPhase : uint8_t {
  Uninitialized,
  NoCredentials,
  Connecting,
  RetryWaiting,
  Online,
  ProvisioningRequired,
  Failed,
};

enum class WifiStationAction : uint8_t {
  None,
  Connect,
  RequireProvisioning,
};

struct WifiStationPolicy {
  // Retained for settings compatibility. A timeout alone never discards a
  // saved network or opens Settings AP; only repeated authentication failures
  // prove that the credential itself is invalid.
  uint32_t saved_connect_timeout_ms = 25000;
  uint32_t retry_base_ms = 1000;
  uint32_t retry_max_ms = 8000;
  uint8_t credential_failure_limit = 2;
};

// Pure state machine for saved-credential association. It never mutates or
// clears Wi-Fi credentials. Temporary AP loss retries silently forever; UI is
// expected to stay on the previous stable e-paper frame. ProvisioningRequired
// is reserved for no credential or repeated authentication rejection.
class WifiStationCore final {
 public:
  explicit WifiStationCore(WifiStationPolicy policy = {});

  WifiStationAction begin(bool has_saved_credentials, uint32_t now_ms);
  void connectStarted(uint32_t now_ms, bool accepted);
  void connected();
  void disconnected(uint16_t reason, bool credential_failure,
                    uint32_t now_ms);
  WifiStationAction tick(uint32_t now_ms);
  void fail();

  WifiStationPhase phase() const { return phase_; }
  bool online() const { return phase_ == WifiStationPhase::Online; }
  bool hasSavedCredentials() const { return has_saved_credentials_; }
  uint8_t retryCount() const { return retry_count_; }
  uint16_t lastDisconnectReason() const { return last_disconnect_reason_; }

 private:
  static bool due(uint32_t now_ms, uint32_t deadline_ms);
  uint32_t retryDelayMs() const;

  WifiStationPolicy policy_;
  WifiStationPhase phase_ = WifiStationPhase::Uninitialized;
  uint32_t overall_deadline_ms_ = 0;
  uint32_t retry_at_ms_ = 0;
  uint8_t retry_count_ = 0;
  uint8_t credential_failures_ = 0;
  uint16_t last_disconnect_reason_ = 0;
  bool has_saved_credentials_ = false;
};

const char* wifiStationPhaseName(WifiStationPhase phase);

}  // namespace inkloop
