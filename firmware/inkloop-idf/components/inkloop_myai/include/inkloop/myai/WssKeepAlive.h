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
  static constexpr uint64_t kPongTimeoutMs = 20000ULL;

  void start(uint64_t now_ms) {
    active_ = true;
    last_ping_ms_ = now_ms;
    pending_ping_token_ = 0U;
    next_ping_token_ = 0U;
    awaiting_pong_ = false;
  }
  void stop() {
    active_ = false;
    last_ping_ms_ = 0ULL;
    pending_ping_token_ = 0U;
    next_ping_token_ = 0U;
    awaiting_pong_ = false;
  }
  bool pingDue(uint64_t now_ms) const {
    if (!active_ || awaiting_pong_) return false;
    return now_ms < last_ping_ms_ ||
           now_ms - last_ping_ms_ >= kPingIntervalMs;
  }
  uint32_t nextPingToken() {
    if (!active_) return 0U;
    ++next_ping_token_;
    if (next_ping_token_ == 0U) ++next_ping_token_;
    return next_ping_token_;
  }
  void notePingSent(uint64_t now_ms, uint32_t token) {
    if (!active_ || token == 0U) return;
    last_ping_ms_ = now_ms;
    pending_ping_token_ = token;
    awaiting_pong_ = true;
  }
  // Local ingress backpressure can make a valid Pong temporarily unreadable
  // behind the remainder of its data frame. Rebase only the deadline clock;
  // preserving both token fields prevents an old token from being reused or
  // an outstanding Pong from being accepted against a reset sequence.
  void rebase(uint64_t now_ms) {
    if (active_) last_ping_ms_ = now_ms;
  }
  bool notePong(uint32_t token) {
    if (!active_ || !awaiting_pong_ || token == 0U ||
        token != pending_ping_token_) {
      return false;
    }
    pending_ping_token_ = 0U;
    awaiting_pong_ = false;
    return true;
  }
  bool pongTimedOut(uint64_t now_ms) const {
    if (!active_ || !awaiting_pong_) return false;
    return now_ms < last_ping_ms_ ||
           now_ms - last_ping_ms_ >= kPongTimeoutMs;
  }
  bool active() const { return active_; }
  bool awaitingPong() const { return awaiting_pong_; }

 private:
  uint64_t last_ping_ms_ = 0ULL;
  uint32_t pending_ping_token_ = 0U;
  uint32_t next_ping_token_ = 0U;
  bool active_ = false;
  bool awaiting_pong_ = false;
};

}  // namespace myai
}  // namespace inkloop
