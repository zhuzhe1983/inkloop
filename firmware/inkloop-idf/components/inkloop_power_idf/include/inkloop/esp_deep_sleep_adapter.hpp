#pragma once

#include <cstdint>

#include "inkloop/power_runtime.hpp"

namespace inkloop {

enum class WakeButton : uint8_t {
  Voice,
  Previous,
  Next,
};

// Board components expose wake pins through this narrow capability contract.
// A future SKU can change pins without copying the sleep or recovery policy.
class IWakePinCapabilities {
 public:
  virtual ~IWakePinCapabilities() = default;
  virtual bool supportsWakeButton(WakeButton button) const = 0;
  virtual int wakePin(WakeButton button) const = 0;
  virtual bool wakeButtonPressed(WakeButton button) const = 0;
};

constexpr uint32_t kSystemWakeFlagTimer = 1U << 0U;
constexpr uint32_t kSystemWakeFlagExt1 = 1U << 1U;
constexpr uint32_t kKnownSystemWakeFlags =
    kSystemWakeFlagTimer | kSystemWakeFlagExt1;

// Function table isolates irreversible ESP sleep entry from policy and host
// tests. systemEspSleepFunctions() is the only production implementation.
struct EspSleepFunctions {
  int (*reset_wake_sources)() = nullptr;
  int (*enable_timer_wakeup_us)(uint64_t microseconds) = nullptr;
  int (*enable_ext1_any_low)(uint64_t gpio_mask) = nullptr;
  uint32_t (*wakeup_flags)() = nullptr;
  uint64_t (*ext1_wakeup_mask)() = nullptr;
  void (*start_deep_sleep)() = nullptr;
};

const EspSleepFunctions& systemEspSleepFunctions();

class EspDeepSleepAdapter final : public IDeepSleepDriver {
 public:
  EspDeepSleepAdapter(IWakePinCapabilities& capabilities,
                      const EspSleepFunctions& functions);

  DeepSleepResult enterAfterSeconds(uint64_t timer_delay_seconds) override;
  WakeCause wakeCause() const;
  uint64_t wakeButtonMask() const;
  bool wakeButtonsReleased() const;
  bool capabilitiesValid() const;

 private:
  WakeCause decodeExt1Mask(uint64_t mask) const;
  bool functionsValid() const;
  void rollbackWakeSources() const;

  IWakePinCapabilities& capabilities_;
  const EspSleepFunctions& functions_;
};

}  // namespace inkloop
