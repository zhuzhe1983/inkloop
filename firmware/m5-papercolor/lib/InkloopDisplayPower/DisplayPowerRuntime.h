#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "ImageProcessing.h"
#include "PngAttestation.h"
#include "PowerPolicy.h"
#include "RefreshControl.h"

namespace inkloop {
namespace displaypower {

struct EncodedFrameRequest {
  std::string assetId;
  const uint8_t* bytes;
  size_t length;
  RenderStrategy strategy;

  EncodedFrameRequest()
      : assetId(), bytes(0), length(0), strategy(RenderStrategy::OfficialQuality) {}
};

class PaletteFrame final : public IPixelSink {
 public:
  explicit PaletteFrame(const ValidatedPng& source);

  bool write(const RgbPixel& pixel) override;
  bool finish() override;
  bool valid() const;
  const std::vector<RgbPixel>& pixels() const { return pixels_; }
  const Sha256Digest& sourceDigest() const { return sourceDigest_; }

 private:
  Sha256Digest sourceDigest_;
  std::vector<RgbPixel> pixels_;
  bool sourceValid_;
  bool finished_;
};

class IPngPixelDecoder : public IPixelSource {
 public:
  virtual ~IPngPixelDecoder() {}
  virtual bool decode(
      const ValidatedPng& png,
      const uint8_t* bytes,
      size_t length) = 0;
  virtual void reset() = 0;
};

class IDisplayPowerClock {
 public:
  virtual ~IDisplayPowerClock() {}
  virtual uint32_t nowMilliseconds() const = 0;
};

class IDisplayPowerLock {
 public:
  virtual ~IDisplayPowerLock() {}
  virtual bool tryLock() = 0;
  virtual void unlock() = 0;
};

class IFullScreenDisplay;
class DisplayRefreshRuntime;

class PhysicalRefreshCapability {
 public:
  PhysicalRefreshCapability(const PhysicalRefreshCapability&) = delete;
  PhysicalRefreshCapability& operator=(const PhysicalRefreshCapability&) = delete;

  bool validFor(const IFullScreenDisplay* display, const void* writer) const {
    return display_ == display && writer_ == writer && display_ && writer_;
  }

 private:
  friend class DisplayRefreshRuntime;
  PhysicalRefreshCapability(const IFullScreenDisplay* display, const void* writer)
      : display_(display), writer_(writer) {}

  const IFullScreenDisplay* display_;
  const void* writer_;
};

class IFullScreenDisplay {
 public:
  virtual ~IFullScreenDisplay() {}
  virtual bool claimSoleWriter(const void* writer) = 0;
  virtual bool renderOfficialPng(
      const PhysicalRefreshCapability& capability,
      const ValidatedPng& png,
      const uint8_t* bytes,
      size_t length) = 0;
  virtual bool renderExperimentalPalette(
      const PhysicalRefreshCapability& capability,
      const PaletteFrame& frame) = 0;
};

class IImageLedSink {
 public:
  virtual ~IImageLedSink() {}
  virtual bool setImageState(ImageLedState state, uint32_t nowMilliseconds) = 0;
  virtual bool tick(uint32_t nowMilliseconds) = 0;
  virtual bool quiesce(uint32_t nowMilliseconds) = 0;
};

struct DisplayRefreshRuntimeConfig {
  bool enabled;
  bool experimentalPrequantizationEnabled;
  uint32_t cooldownMilliseconds;
  size_t maximumEncodedPngBytes;

  DisplayRefreshRuntimeConfig()
      : enabled(false),
        experimentalPrequantizationEnabled(false),
        cooldownMilliseconds(30000),
        maximumEncodedPngBytes(16U * 1024U * 1024U) {}
};

enum class DisplayRefreshResult : uint8_t {
  Complete,
  Unchanged,
  Disabled,
  InvalidRequest,
  InvalidEncodedPng,
  ExperimentalDisabled,
  DecodeFailed,
  ConversionFailed,
  Busy,
  Cooldown,
  LedUnavailable,
  DisplayFailed,
  CapabilityFault,
  SoleWriterUnavailable,
};

class DisplayRefreshRuntime {
 public:
  DisplayRefreshRuntime(
      IFullScreenDisplay& display,
      IImageLedSink& imageLed,
      IPngPixelDecoder& decoder,
      IDisplayPowerLock& lock,
      IDisplayPowerClock& clock,
      const DisplayRefreshRuntimeConfig& config);

  DisplayRefreshRuntime(const DisplayRefreshRuntime&) = delete;
  DisplayRefreshRuntime& operator=(const DisplayRefreshRuntime&) = delete;

  DisplayRefreshResult refresh(const EncodedFrameRequest& request);
  bool tickImageLed();
  bool busy() const;
  bool enabled() const { return config_.enabled; }

 private:
  IFullScreenDisplay& display_;
  IImageLedSink& imageLed_;
  IPngPixelDecoder& decoder_;
  IDisplayPowerLock& lock_;
  IDisplayPowerClock& clock_;
  DisplayRefreshRuntimeConfig config_;
  RefreshArbiter arbiter_;
  bool writerClaimed_;
  bool hasLastSuccessfulFrame_;
  Sha256Digest lastSuccessfulDigest_;
  RenderStrategy lastSuccessfulStrategy_;
};

class IDeepSleepPlatform {
 public:
  virtual ~IDeepSleepPlatform() {}
  virtual bool resetWakeSources() = 0;
  virtual bool enableTimerWakeAfterSeconds(uint64_t seconds) = 0;
  virtual bool enableAnyLowWake(uint64_t gpioMask) = 0;
  virtual bool enterDeepSleep() = 0;
};

enum class DeepSleepExecutionResult : uint8_t {
  Entered,
  NotEligible,
  InvalidWakePlan,
  WakeSourceResetFailed,
  TimerConfigurationFailed,
  ButtonConfigurationFailed,
  EnterReturned,
};

DeepSleepExecutionResult executeDeepSleepPlan(
    const SleepDecision& decision,
    uint64_t rtcNowEpochSeconds,
    IDeepSleepPlatform& platform);

const char* deepSleepExecutionResultName(DeepSleepExecutionResult result);

enum class PowerSnapshotResult : uint8_t {
  Captured,
  InvalidTarget,
  ClockUnsynchronized,
  TaskStoreUnavailable,
  TaskScheduleInvalid,
  UnknownFailure,
};

const char* powerSnapshotResultName(PowerSnapshotResult result);

class IPreSleepQuiescenceHooks {
 public:
  virtual ~IPreSleepQuiescenceHooks() {}
  virtual bool capturePowerInputs(PowerInputs* inputs) = 0;
  virtual PowerSnapshotResult capturePowerInputsDetailed(PowerInputs* inputs);
  virtual bool finalizeTaskAndDisplay() = 0;
  virtual bool stopAudio() = 0;
  virtual bool stopImageRgb() = 0;
  virtual bool closeNetwork() = 0;
};

enum class PrepareSleepResult : uint8_t {
  Entered,
  NotEligible,
  SnapshotFailed,
  TaskOrDisplayFinalizationFailed,
  AudioQuiescenceFailed,
  RgbQuiescenceFailed,
  NetworkQuiescenceFailed,
  RecheckNotEligible,
  DeepSleepRejected,
};

const char* prepareSleepResultName(PrepareSleepResult result);

enum class PowerSnapshotPhase : uint8_t {
  None,
  Initial,
  Final,
};

const char* powerSnapshotPhaseName(PowerSnapshotPhase phase);

struct PrepareSleepOutcome {
  PrepareSleepResult result;
  PowerSnapshotResult snapshotResult;
  PowerSnapshotPhase snapshotPhase;
  SleepDecisionReason decisionReason;
  DeepSleepExecutionResult deepSleepResult;

  PrepareSleepOutcome()
      : result(PrepareSleepResult::SnapshotFailed),
        snapshotResult(PowerSnapshotResult::UnknownFailure),
        snapshotPhase(PowerSnapshotPhase::Initial),
        decisionReason(SleepDecisionReason::AlwaysAwakeMode),
        deepSleepResult(DeepSleepExecutionResult::NotEligible) {}
};

PrepareSleepOutcome prepareAndExecuteSleepDetailed(
    const PowerPolicy& policy,
    IPreSleepQuiescenceHooks& hooks,
    IDeepSleepPlatform& platform);

PrepareSleepResult prepareAndExecuteSleep(
    const PowerPolicy& policy,
    IPreSleepQuiescenceHooks& hooks,
    IDeepSleepPlatform& platform);

struct SleepAttemptRuntimeConfig {
  uint32_t blockerRecheckMilliseconds;
  uint32_t minimumFailureRetryMilliseconds;
  uint32_t maximumFailureRetryMilliseconds;
  uint32_t summaryIntervalMilliseconds;

  SleepAttemptRuntimeConfig()
      : blockerRecheckMilliseconds(1000),
        minimumFailureRetryMilliseconds(5000),
        maximumFailureRetryMilliseconds(60000),
        summaryIntervalMilliseconds(60000) {}
};

struct SleepAttemptObservation {
  bool attempted;
  bool transition;
  bool summary;
  uint32_t suppressedAttempts;
  PrepareSleepOutcome outcome;

  SleepAttemptObservation()
      : attempted(false),
        transition(false),
        summary(false),
        suppressedAttempts(0),
        outcome() {}
};

class SleepAttemptRuntime {
 public:
  SleepAttemptRuntime();
  explicit SleepAttemptRuntime(const SleepAttemptRuntimeConfig& config);

  SleepAttemptObservation poll(
      uint32_t nowMilliseconds,
      const PowerPolicy& policy,
      IPreSleepQuiescenceHooks& hooks,
      IDeepSleepPlatform& platform);
  bool configurationValid() const { return configurationValid_; }
  uint32_t currentRetryMilliseconds() const { return currentRetryMilliseconds_; }

 private:
  static bool sameOutcome(
      const PrepareSleepOutcome& left,
      const PrepareSleepOutcome& right);
  static bool operationalFailure(PrepareSleepResult result);

  SleepAttemptRuntimeConfig config_;
  bool configurationValid_;
  bool hasAttempt_;
  uint32_t lastAttemptAtMilliseconds_;
  uint32_t currentRetryMilliseconds_;
  bool hasOutcome_;
  PrepareSleepOutcome lastOutcome_;
  uint32_t lastReportAtMilliseconds_;
  uint32_t suppressedAttempts_;
};

class IWakeRecoveryHooks {
 public:
  virtual ~IWakeRecoveryHooks() {}
  virtual bool reconnectWifi() = 0;
  virtual bool syncInkloopSchedules() = 0;
  virtual bool allWakeButtonsReleased() = 0;
  virtual bool rearmButtonInput() = 0;
};

struct WakeRecoveryConfig {
  uint32_t releaseDebounceMilliseconds;
  uint32_t retryIntervalMilliseconds;
  uint16_t maximumAttemptsPerStage;

  WakeRecoveryConfig()
      : releaseDebounceMilliseconds(50),
        retryIntervalMilliseconds(1000),
        maximumAttemptsPerStage(120) {}
};

class WakeRecoveryRuntime {
 public:
  WakeRecoveryRuntime(IWakeRecoveryHooks& hooks, const WakeRecoveryConfig& config);
  WakeRecoveryRuntime(IWakeRecoveryHooks& hooks, uint32_t releaseDebounceMilliseconds);

  bool beginAfterHardwareReady(WakeReason reason);
  bool poll(uint32_t nowMilliseconds);
  const WakeReconnectState& state() const { return state_; }
  bool configurationValid() const { return configurationValid_; }

 private:
  bool networkAttemptDue(uint32_t nowMilliseconds) const;
  void recordNetworkAttempt(uint32_t nowMilliseconds, bool succeeded);

  IWakeRecoveryHooks& hooks_;
  WakeReconnectState state_;
  WakeRecoveryConfig config_;
  bool configurationValid_;
  uint32_t releaseCandidateAtMilliseconds_;
  bool releaseCandidateActive_;
  uint32_t lastNetworkAttemptAtMilliseconds_;
  uint16_t networkAttempts_;
  bool hasNetworkAttempt_;
};

}  // namespace displaypower
}  // namespace inkloop
