#pragma once

#include <cstdint>

namespace inkloop {

struct CadenceIntervals {
  uint64_t inkloop_sync_ms = 30000;
  uint64_t aigc_status_ms = 5000;
  uint64_t myai_heartbeat_ms = 30000;
};

// Pure scheduling policy. It does not poll Portal: ESP-IDF httpd sleeps on
// sockets and browser push/poll policy belongs to the authenticated session.
class CadencePolicy {
 public:
  explicit CadencePolicy(CadenceIntervals intervals = CadenceIntervals());

  void setNetworkReady(bool ready);
  void markInkloopDirty();
  bool inkloopSyncDue(uint64_t now_ms) const;
  void acknowledgeInkloopSync(uint64_t now_ms, bool success);

  void beginAigcStatusPolling(uint64_t now_ms);
  void finishAigcStatusPolling();
  bool aigcStatusDue(uint64_t now_ms) const;
  void acknowledgeAigcStatus(uint64_t now_ms, bool terminal);

  void openVoiceLease(uint64_t now_ms);
  void closeVoiceLease();
  bool myaiHeartbeatDue(uint64_t now_ms) const;
  void acknowledgeMyAiHeartbeat(uint64_t now_ms, bool success);

  bool networkReady() const { return network_ready_; }
  bool aigcActive() const { return aigc_active_; }
  bool voiceLeaseActive() const { return voice_lease_active_; }

 private:
  static bool elapsed(uint64_t now_ms, uint64_t since_ms,
                      uint64_t interval_ms);

  CadenceIntervals intervals_;
  uint64_t last_inkloop_sync_ms_ = 0;
  uint64_t last_aigc_status_ms_ = 0;
  uint64_t last_heartbeat_ms_ = 0;
  bool network_ready_ = false;
  bool inkloop_dirty_ = false;
  bool inkloop_has_sync_ = false;
  bool aigc_active_ = false;
  bool voice_lease_active_ = false;
};

}  // namespace inkloop

