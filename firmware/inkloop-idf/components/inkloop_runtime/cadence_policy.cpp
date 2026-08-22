#include "inkloop/cadence_policy.hpp"

namespace inkloop {

CadencePolicy::CadencePolicy(CadenceIntervals intervals)
    : intervals_(intervals) {}

bool CadencePolicy::elapsed(uint64_t now_ms, uint64_t since_ms,
                            uint64_t interval_ms) {
  return now_ms >= since_ms && now_ms - since_ms >= interval_ms;
}

void CadencePolicy::setNetworkReady(bool ready) {
  if (ready && !network_ready_) inkloop_dirty_ = true;
  network_ready_ = ready;
}

void CadencePolicy::markInkloopDirty() { inkloop_dirty_ = true; }

bool CadencePolicy::inkloopSyncDue(uint64_t now_ms) const {
  if (!network_ready_) return false;
  if (inkloop_dirty_ || !inkloop_has_sync_) return true;
  return elapsed(now_ms, last_inkloop_sync_ms_, intervals_.inkloop_sync_ms);
}

void CadencePolicy::acknowledgeInkloopSync(uint64_t now_ms, bool success) {
  if (!success) return;
  last_inkloop_sync_ms_ = now_ms;
  inkloop_has_sync_ = true;
  inkloop_dirty_ = false;
}

void CadencePolicy::beginAigcStatusPolling(uint64_t now_ms) {
  aigc_active_ = true;
  last_aigc_status_ms_ = now_ms;
}

void CadencePolicy::finishAigcStatusPolling() { aigc_active_ = false; }

bool CadencePolicy::aigcStatusDue(uint64_t now_ms) const {
  return network_ready_ && aigc_active_ &&
         elapsed(now_ms, last_aigc_status_ms_, intervals_.aigc_status_ms);
}

void CadencePolicy::acknowledgeAigcStatus(uint64_t now_ms, bool terminal) {
  last_aigc_status_ms_ = now_ms;
  if (terminal) aigc_active_ = false;
}

void CadencePolicy::openVoiceLease(uint64_t now_ms) {
  voice_lease_active_ = true;
  last_heartbeat_ms_ = now_ms;
}

void CadencePolicy::closeVoiceLease() { voice_lease_active_ = false; }

bool CadencePolicy::myaiHeartbeatDue(uint64_t now_ms) const {
  return network_ready_ && voice_lease_active_ &&
         elapsed(now_ms, last_heartbeat_ms_, intervals_.myai_heartbeat_ms);
}

void CadencePolicy::acknowledgeMyAiHeartbeat(uint64_t now_ms, bool success) {
  if (success) last_heartbeat_ms_ = now_ms;
}

}  // namespace inkloop

