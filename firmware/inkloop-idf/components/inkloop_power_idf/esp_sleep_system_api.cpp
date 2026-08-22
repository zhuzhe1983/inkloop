#include "inkloop/esp_deep_sleep_adapter.hpp"

#include "esp_err.h"
#include "esp_sleep.h"

namespace inkloop {
namespace {

int resetWakeSources() {
  const esp_err_t status =
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  return status == ESP_OK || status == ESP_ERR_INVALID_STATE
             ? 0
             : static_cast<int>(status);
}

int enableTimerWakeupUs(uint64_t microseconds) {
  return static_cast<int>(esp_sleep_enable_timer_wakeup(microseconds));
}

int enableExt1AnyLow(uint64_t gpio_mask) {
  return static_cast<int>(
      esp_sleep_enable_ext1_wakeup_io(gpio_mask, ESP_EXT1_WAKEUP_ANY_LOW));
}

uint32_t wakeupFlags() {
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  uint32_t flags = 0;
  if ((causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0) {
    flags |= kSystemWakeFlagTimer;
  }
  if ((causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) != 0) {
    flags |= kSystemWakeFlagExt1;
  }
  const uint32_t known_causes = (1UL << ESP_SLEEP_WAKEUP_TIMER) |
                                (1UL << ESP_SLEEP_WAKEUP_EXT1);
  if ((causes & ~known_causes) != 0) flags |= 1UL << 31U;
  return flags;
}

uint64_t ext1WakeupMask() {
  return esp_sleep_get_ext1_wakeup_status();
}

void startDeepSleep() {
  esp_deep_sleep_start();
}

constexpr EspSleepFunctions kSystemFunctions{
    &resetWakeSources,
    &enableTimerWakeupUs,
    &enableExt1AnyLow,
    &wakeupFlags,
    &ext1WakeupMask,
    &startDeepSleep,
};

}  // namespace

const EspSleepFunctions& systemEspSleepFunctions() {
  return kSystemFunctions;
}

}  // namespace inkloop

