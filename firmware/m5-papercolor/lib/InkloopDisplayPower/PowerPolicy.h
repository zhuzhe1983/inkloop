#pragma once

#include <stdint.h>

#include "RefreshControl.h"

namespace inkloop {
namespace displaypower {

enum class PowerMode : uint8_t { AlwaysAwake, BatteryOptIn };

struct SleepBlockers {
  bool audioActive;
  bool generationActive;
  bool conversionActive;
  bool writeActive;
  bool taskFinalizationActive;
  // Compatibility aggregate inputs retained for the 0.2 integration seam.
  bool voiceActive;
  bool displayActive;
  bool downloadActive;
  bool pendingJournal;
  bool portalActive;
  bool unacknowledgedTask;
  // Named blockers supplement the 0.2 portal aggregate without weakening it.
  bool pairingActive;
  bool onboardingActive;
  bool portalRequestActive;
  bool tutorialActive;
  bool externalPagePending;

  SleepBlockers()
      : audioActive(false),
        generationActive(false),
        conversionActive(false),
        writeActive(false),
        taskFinalizationActive(false),
        voiceActive(false),
        displayActive(false),
        downloadActive(false),
        pendingJournal(false),
        portalActive(false),
        unacknowledgedTask(false),
        pairingActive(false),
        onboardingActive(false),
        portalRequestActive(false),
        tutorialActive(false),
        externalPagePending(false) {}

  bool any() const;
};

struct PowerPolicyConfig {
  PowerMode mode;
  uint32_t eligibleIdleMilliseconds;
  uint32_t heartbeatWakeIntervalSeconds;
  uint32_t rtcConnectionMarginSeconds;
  uint32_t minimumUsefulSleepSeconds;

  PowerPolicyConfig()
      : mode(PowerMode::AlwaysAwake),
        eligibleIdleMilliseconds(120000),
        heartbeatWakeIntervalSeconds(300),
        rtcConnectionMarginSeconds(30),
        minimumUsefulSleepSeconds(10) {}
};

struct PowerInputs {
  uint32_t nowMilliseconds;
  uint32_t lastMeaningfulActivityMilliseconds;
  uint64_t rtcNowEpochSeconds;
  uint64_t nextLocalTaskEpochSeconds;
  uint64_t nextHeartbeatEpochSeconds;
  bool rtcSynchronized;
  bool wakeButtonsReleased;
  SleepBlockers blockers;

  PowerInputs()
      : nowMilliseconds(0),
        lastMeaningfulActivityMilliseconds(0),
        rtcNowEpochSeconds(0),
        nextLocalTaskEpochSeconds(0),
        nextHeartbeatEpochSeconds(0),
        rtcSynchronized(false),
        wakeButtonsReleased(false),
        blockers() {}
};

enum class SleepDecisionReason : uint8_t {
  Eligible,
  AlwaysAwakeMode,
  InvalidClock,
  IdlePeriodNotReached,
  AudioActive,
  GenerationActive,
  ConversionActive,
  WriteActive,
  TaskFinalizationActive,
  VoiceActive,
  DisplayActive,
  DownloadActive,
  PendingJournal,
  PortalActive,
  UnacknowledgedTask,
  PairingActive,
  OnboardingActive,
  PortalRequestActive,
  TutorialActive,
  ExternalPagePending,
  WakeButtonsHeld,
  WakeDeadlineDue,
  SleepWindowTooShort,
};

const char* sleepDecisionReasonName(SleepDecisionReason reason);

class RuntimeActivityState {
 public:
  RuntimeActivityState()
      : lastMeaningfulActivityMilliseconds_(0), externalPagePending_(false) {}

  void noteMeaningfulActivity(uint32_t nowMilliseconds) {
    lastMeaningfulActivityMilliseconds_ = nowMilliseconds;
  }
  void setExternalPagePending(bool pending, uint32_t nowMilliseconds) {
    externalPagePending_ = pending;
    noteMeaningfulActivity(nowMilliseconds);
  }
  uint32_t lastMeaningfulActivityMilliseconds() const {
    return lastMeaningfulActivityMilliseconds_;
  }
  bool externalPagePending() const { return externalPagePending_; }

 private:
  uint32_t lastMeaningfulActivityMilliseconds_;
  bool externalPagePending_;
};

struct WakePlan {
  bool timerEnabled;
  uint64_t timerWakeEpochSeconds;
  uint64_t ext1AnyLowMask;
  bool ext1AnyLow;
  uint8_t topButtonGpio;
  uint8_t previousButtonGpio;
  uint8_t nextButtonGpio;

  WakePlan()
      : timerEnabled(false),
        timerWakeEpochSeconds(0),
        ext1AnyLowMask(0),
        ext1AnyLow(true),
        topButtonGpio(1),
        previousButtonGpio(10),
        nextButtonGpio(9) {}
};

struct SleepDecision {
  bool shouldSleep;
  SleepDecisionReason reason;
  WakePlan wake;

  SleepDecision()
      : shouldSleep(false),
        reason(SleepDecisionReason::AlwaysAwakeMode),
        wake() {}
};

class PowerPolicy {
 public:
  PowerPolicy();
  explicit PowerPolicy(const PowerPolicyConfig& config);

  const PowerPolicyConfig& config() const { return config_; }
  bool setConfig(const PowerPolicyConfig& config);
  SleepDecision evaluate(const PowerInputs& inputs) const;

  static uint64_t paperColorExt1AnyLowMask();
  static bool validPowerMode(PowerMode mode);

 private:
  static bool validConfig(const PowerPolicyConfig& config);
  static SleepDecisionReason blockerReason(const SleepBlockers& blockers);
  uint64_t adjustedWakeDeadline(
      uint64_t deadline,
      uint64_t nowEpochSeconds) const;

  PowerPolicyConfig config_;
};

enum class WakeReason : uint8_t {
  ColdBoot,
  TopButton,
  PreviousButton,
  NextButton,
  MultipleButtons,
  RtcTimer,
  Unknown,
};

WakeReason wakeReasonFromExt1Mask(uint64_t ext1Mask);
bool validWakeReason(WakeReason reason);

enum class ReconnectStage : uint8_t {
  NotStarted,
  ReinitializeHardware,
  ReconnectWifi,
  SyncInkloop,
  AwaitWakeButtonsReleased,
  ArmInput,
  Ready,
  Fault,
};

class WakeReconnectState {
 public:
  WakeReconnectState();

  bool begin(WakeReason reason);
  bool markHardwareReady();
  bool markWifiConnected();
  bool markInkloopSynced();
  bool markWakeButtonsReleasedDebounced(
      bool topReleased,
      bool previousReleased,
      bool nextReleased);
  bool markInputRearmed();
  void markFault();

  WakeReason wakeReason() const { return wakeReason_; }
  ReconnectStage stage() const { return stage_; }
  bool readyForUserInput() const { return stage_ == ReconnectStage::Ready; }
  bool wifiReconnectRequired() const;

 private:
  WakeReason wakeReason_;
  ReconnectStage stage_;
};

}  // namespace displaypower
}  // namespace inkloop
