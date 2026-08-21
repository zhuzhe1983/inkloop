#pragma once

#include <stdint.h>

namespace inkloop {

static constexpr uint32_t kSavedWifiAttemptMs = 25000;
static constexpr uint32_t kWifiPortalTimeoutMs = 300000;
static constexpr uint32_t kClockSyncTimeoutMs = 15000;
static constexpr uint32_t kWifiManagerConnectCallBoundMs = 1000;
static constexpr uint32_t kProvisioningLoopIntervalMs = 50;
static constexpr uint32_t kProvisioningSerialLatencyBudgetMs = 1250;

enum class WifiProvisioningPhase : uint8_t {
  Idle,
  SavedNetworkAttempt,
  PortalShown,
  ClockSync,
  OnlineReady,
  TimedOutDegraded,
};

enum class WifiPortalReason : uint8_t {
  None,
  NoCredentials,
  SavedConnectTimeout,
};

struct WifiProvisioningActions {
  bool startPortal = false;
  bool stopPortal = false;
  bool startClockSync = false;
  bool finalizeOnline = false;
  bool reportTimeout = false;
  WifiPortalReason portalReason = WifiPortalReason::None;
};

class WifiProvisioningState {
 public:
  WifiProvisioningActions start(
    uint32_t now,
    bool connected,
    bool hasSavedCredentials
  ) {
    WifiProvisioningActions actions;
    started_ = true;
    phaseStartedAt_ = now;
    if (connected) {
      phase_ = WifiProvisioningPhase::ClockSync;
      actions.startClockSync = true;
    } else if (hasSavedCredentials) {
      phase_ = WifiProvisioningPhase::SavedNetworkAttempt;
    } else {
      phase_ = WifiProvisioningPhase::PortalShown;
      actions.startPortal = true;
      actions.portalReason = WifiPortalReason::NoCredentials;
    }
    return actions;
  }

  WifiProvisioningActions tick(
    uint32_t now,
    bool connected,
    bool clockReady
  ) {
    WifiProvisioningActions actions;
    if (!started_) return actions;
    if (phase_ == WifiProvisioningPhase::SavedNetworkAttempt) {
      if (connected) {
        phase_ = WifiProvisioningPhase::ClockSync;
        phaseStartedAt_ = now;
        actions.startClockSync = true;
      } else if (elapsed(now) >= kSavedWifiAttemptMs) {
        phase_ = WifiProvisioningPhase::PortalShown;
        phaseStartedAt_ = now;
        actions.startPortal = true;
        actions.portalReason = WifiPortalReason::SavedConnectTimeout;
      }
    } else if (phase_ == WifiProvisioningPhase::PortalShown) {
      if (connected) {
        phase_ = WifiProvisioningPhase::ClockSync;
        phaseStartedAt_ = now;
        actions.stopPortal = true;
        actions.startClockSync = true;
      } else if (elapsed(now) >= kWifiPortalTimeoutMs) {
        phase_ = WifiProvisioningPhase::TimedOutDegraded;
        actions.stopPortal = true;
        actions.reportTimeout = true;
      }
    } else if (phase_ == WifiProvisioningPhase::ClockSync &&
               (clockReady || elapsed(now) >= kClockSyncTimeoutMs)) {
      phase_ = WifiProvisioningPhase::OnlineReady;
      actions.finalizeOnline = true;
    }
    return actions;
  }

  WifiProvisioningActions failPortalStart() {
    WifiProvisioningActions actions;
    if (phase_ == WifiProvisioningPhase::PortalShown) {
      phase_ = WifiProvisioningPhase::TimedOutDegraded;
      actions.stopPortal = true;
      actions.reportTimeout = true;
    }
    return actions;
  }

  WifiProvisioningPhase phase() const { return phase_; }
  bool provisioning() const {
    return phase_ == WifiProvisioningPhase::SavedNetworkAttempt ||
      phase_ == WifiProvisioningPhase::PortalShown ||
      phase_ == WifiProvisioningPhase::ClockSync;
  }
  bool portalShown() const { return phase_ == WifiProvisioningPhase::PortalShown; }
  bool portalActive() const { return portalShown(); }
  bool onlineReady() const { return phase_ == WifiProvisioningPhase::OnlineReady; }

  const char* phaseName() const {
    switch (phase_) {
      case WifiProvisioningPhase::SavedNetworkAttempt: return "saved_network";
      case WifiProvisioningPhase::PortalShown: return "wifi_portal";
      case WifiProvisioningPhase::ClockSync: return "clock_sync";
      case WifiProvisioningPhase::OnlineReady: return "online";
      case WifiProvisioningPhase::TimedOutDegraded: return "wifi_timeout";
      default: return "idle";
    }
  }

 private:
  uint32_t elapsed(uint32_t now) const { return now - phaseStartedAt_; }

  bool started_ = false;
  WifiProvisioningPhase phase_ = WifiProvisioningPhase::Idle;
  uint32_t phaseStartedAt_ = 0;
};

constexpr const char* wifiPortalReasonName(WifiPortalReason reason) {
  return reason == WifiPortalReason::NoCredentials
    ? "NO_CREDENTIALS"
    : (reason == WifiPortalReason::SavedConnectTimeout
        ? "SAVED_CONNECT_TIMEOUT"
        : "NONE");
}

static_assert(
  kWifiManagerConnectCallBoundMs + 2 * kProvisioningLoopIntervalMs <=
    kProvisioningSerialLatencyBudgetMs,
  "provisioning serial response budget must cover one bounded portal iteration"
);

}  // namespace inkloop
