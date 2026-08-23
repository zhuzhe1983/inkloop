#pragma once

#include <atomic>
#include <cstdint>

#include "esp_timer.h"
#include "freertos/task.h"

namespace inkloop {
namespace myai {

// ESP32-S3 has one slow-service core shared by Inkloop cloud and MyAI. A
// synchronous TLS handshake yields while waiting for the socket, which used
// to let another service start a second handshake. The two ready tasks then
// kept CPU0 continuously busy and starved IDLE0. This bounded process-wide
// lease serializes only connection setup/control-plane operations; audio and
// button work remain on their existing responsive paths.
class EspNetworkOperationLease final {
 public:
  explicit EspNetworkOperationLease(uint32_t timeout_ms) {
    const int64_t timeout_us = static_cast<int64_t>(timeout_ms) * 1000LL;
    const int64_t started = esp_timer_get_time();
    const int64_t deadline = started > INT64_MAX - timeout_us
                                 ? INT64_MAX
                                 : started + timeout_us;
    do {
      if (!active_.test_and_set(std::memory_order_acquire)) {
        acquired_ = true;
        return;
      }
      if (esp_timer_get_time() >= deadline) return;
      vTaskDelay(1U);
    } while (true);
  }

  ~EspNetworkOperationLease() {
    if (acquired_) active_.clear(std::memory_order_release);
  }

  EspNetworkOperationLease(const EspNetworkOperationLease&) = delete;
  EspNetworkOperationLease& operator=(const EspNetworkOperationLease&) = delete;

  bool acquired() const { return acquired_; }

 private:
  inline static std::atomic_flag active_ = ATOMIC_FLAG_INIT;
  bool acquired_ = false;
};

}  // namespace myai
}  // namespace inkloop
