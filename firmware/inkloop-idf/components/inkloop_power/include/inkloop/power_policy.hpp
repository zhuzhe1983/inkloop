#pragma once

#include <cstdint>

namespace inkloop {

// Every long-lived owner reports its work through this finite vocabulary.
// The power core deliberately has no knowledge of product classes, HTTP,
// MyAI, the display driver, or a particular board.
enum class PowerBlocker : uint8_t {
  None = 0,
  PagePending,
  DisplayRefresh,
  VoiceSession,
  AudioPlayback,
  AigcGeneration,
  AssetDownload,
  ContentConversion,
  AlbumUpload,
  TaskSync,
  TaskFinalization,
  Pairing,
  PortalSession,
  StorageCommit,
  Count,
};

constexpr uint8_t kPowerBlockerCount =
    static_cast<uint8_t>(PowerBlocker::Count) - 1U;

bool validPowerBlocker(PowerBlocker blocker);
const char* powerBlockerName(PowerBlocker blocker);

enum class BlockerUpdate : uint8_t {
  Changed,
  Unchanged,
  Invalid,
};

class PowerBlockerSet final {
 public:
  BlockerUpdate set(PowerBlocker blocker, bool active);
  bool active(PowerBlocker blocker) const;
  bool any() const { return mask_ != 0; }
  PowerBlocker firstActive() const;
  uint32_t mask() const { return mask_; }

 private:
  static uint32_t bit(PowerBlocker blocker);
  uint32_t mask_ = 0;
};

// Blocker transitions are meaningful activity. In particular, clearing a
// delayed page starts a new complete idle interval instead of inheriting the
// time spent waiting for that page.
class PowerActivityState final {
 public:
  explicit PowerActivityState(uint32_t now_ms = 0)
      : last_meaningful_activity_ms_(now_ms) {}

  void noteMeaningfulActivity(uint32_t now_ms) {
    last_meaningful_activity_ms_ = now_ms;
  }
  BlockerUpdate setBlocker(PowerBlocker blocker, bool active,
                           uint32_t now_ms);

  uint32_t lastMeaningfulActivityMs() const {
    return last_meaningful_activity_ms_;
  }
  const PowerBlockerSet& blockers() const { return blockers_; }

 private:
  uint32_t last_meaningful_activity_ms_ = 0;
  PowerBlockerSet blockers_{};
};

bool elapsedAtLeast32(uint32_t now_ms, uint32_t since_ms,
                      uint32_t interval_ms);

struct SleepPolicyConfig {
  bool enabled = false;
  uint32_t eligible_idle_ms = 120000;
  uint32_t heartbeat_interval_seconds = 300;
  uint32_t rtc_margin_seconds = 30;
  uint32_t minimum_useful_sleep_seconds = 10;
};

struct PowerInputs {
  uint32_t now_ms = 0;
  uint32_t last_meaningful_activity_ms = 0;
  uint64_t rtc_epoch_seconds = 0;
  uint64_t next_task_epoch_seconds = 0;
  uint64_t next_heartbeat_epoch_seconds = 0;
  bool rtc_synchronized = false;
  bool wake_buttons_released = false;
  PowerBlockerSet blockers{};
};

enum class SleepDecisionReason : uint8_t {
  Eligible,
  Disabled,
  InvalidConfiguration,
  ClockUnavailable,
  Blocked,
  IdlePeriodNotReached,
  WakeButtonsHeld,
  WakeDeadlineDue,
  SleepWindowTooShort,
};

const char* sleepDecisionReasonName(SleepDecisionReason reason);

struct SleepDecision {
  bool should_sleep = false;
  SleepDecisionReason reason = SleepDecisionReason::Disabled;
  PowerBlocker blocker = PowerBlocker::None;
  uint64_t timer_delay_seconds = 0;
};

class SleepPolicy final {
 public:
  SleepPolicy() = default;
  explicit SleepPolicy(const SleepPolicyConfig& config);

  bool setConfig(const SleepPolicyConfig& config);
  bool configurationValid() const { return configuration_valid_; }
  const SleepPolicyConfig& config() const { return config_; }
  SleepDecision evaluate(const PowerInputs& inputs) const;

 private:
  static bool validConfig(const SleepPolicyConfig& config);
  uint64_t adjustedDeadline(uint64_t deadline, uint64_t now) const;

  SleepPolicyConfig config_{};
  bool configuration_valid_ = true;
};

}  // namespace inkloop

