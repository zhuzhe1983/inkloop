#pragma once

#include <stdint.h>

namespace inkloop {
namespace myai {

// Pure scheduling core for the native WebSocket transport. The ESP adapter
// owns the actual RFC 6455 control frame; this class makes the 20-second
// cadence deterministic and host-testable without creating another task or
// timer callback.
class WssKeepAlive {
 public:
  static constexpr uint64_t kPingIntervalMs = 20000ULL;

  void start(uint64_t now_ms) {
    active_ = true;
    last_ping_ms_ = now_ms;
  }
  void stop() {
    active_ = false;
    last_ping_ms_ = 0ULL;
  }
  bool pingDue(uint64_t now_ms) const {
    if (!active_) return false;
    return now_ms < last_ping_ms_ ||
           now_ms - last_ping_ms_ >= kPingIntervalMs;
  }
  void notePingSent(uint64_t now_ms) {
    if (active_) last_ping_ms_ = now_ms;
  }
  bool active() const { return active_; }

 private:
  uint64_t last_ping_ms_ = 0ULL;
  bool active_ = false;
};

}  // namespace myai
}  // namespace inkloop
