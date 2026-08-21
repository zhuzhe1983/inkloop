#include "DisplayPowerRuntime.h"

#include <limits>

namespace inkloop {
namespace displaypower {

namespace {

class RuntimeLockGuard {
 public:
  explicit RuntimeLockGuard(IDisplayPowerLock& lock)
      : lock_(lock), locked_(lock.tryLock()) {}
  ~RuntimeLockGuard() {
    if (locked_) lock_.unlock();
  }
  bool locked() const { return locked_; }

 private:
  IDisplayPowerLock& lock_;
  bool locked_;
};

class DecoderResetGuard {
 public:
  explicit DecoderResetGuard(IPngPixelDecoder& decoder) : decoder_(decoder) {}
  ~DecoderResetGuard() { decoder_.reset(); }

 private:
  IPngPixelDecoder& decoder_;
};

class LedRenderProgress final : public IRenderProgress {
 public:
  LedRenderProgress(IImageLedSink& led, IDisplayPowerClock& clock)
      : led_(led), clock_(clock) {}
  void onRenderProgress(size_t, size_t) override {
    led_.tick(clock_.nowMilliseconds());
  }

 private:
  IImageLedSink& led_;
  IDisplayPowerClock& clock_;
};

WakeRecoveryConfig debounceOnlyConfig(uint32_t releaseDebounceMilliseconds) {
  WakeRecoveryConfig config;
  config.releaseDebounceMilliseconds = releaseDebounceMilliseconds;
  return config;
}

}  // namespace

PaletteFrame::PaletteFrame(const ValidatedPng& source)
    : sourceDigest_(source.digest()),
      pixels_(),
      sourceValid_(source.valid() && source.width() == kPaperColorWidth &&
          source.height() == kPaperColorHeight),
      finished_(false) {
  pixels_.reserve(static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight);
}

bool PaletteFrame::write(const RgbPixel& pixel) {
  const size_t expected = static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight;
  if (!sourceValid_ || finished_ || pixels_.size() >= expected ||
      !isPaperColorPalettePixel(pixel)) {
    return false;
  }
  pixels_.push_back(pixel);
  return true;
}

bool PaletteFrame::finish() {
  const size_t expected = static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight;
  if (!sourceValid_ || finished_ || pixels_.size() != expected) return false;
  finished_ = true;
  return true;
}

bool PaletteFrame::valid() const {
  if (!sourceValid_ || !finished_ ||
      pixels_.size() != static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight) {
    return false;
  }
  for (size_t index = 0; index < pixels_.size(); ++index) {
    if (!isPaperColorPalettePixel(pixels_[index])) return false;
  }
  return true;
}

DisplayRefreshRuntime::DisplayRefreshRuntime(
    IFullScreenDisplay& display,
    IImageLedSink& imageLed,
    IPngPixelDecoder& decoder,
    IDisplayPowerLock& lock,
    IDisplayPowerClock& clock,
    const DisplayRefreshRuntimeConfig& config)
    : display_(display),
      imageLed_(imageLed),
      decoder_(decoder),
      lock_(lock),
      clock_(clock),
      config_(config),
      arbiter_(config.cooldownMilliseconds),
      writerClaimed_(!config.enabled || display.claimSoleWriter(this)),
      hasLastSuccessfulFrame_(false),
      lastSuccessfulDigest_(),
      lastSuccessfulStrategy_(RenderStrategy::OfficialQuality) {}

DisplayRefreshResult DisplayRefreshRuntime::refresh(
    const EncodedFrameRequest& request) {
  if (!config_.enabled) return DisplayRefreshResult::Disabled;
  if (!writerClaimed_) return DisplayRefreshResult::SoleWriterUnavailable;
  if (!request.bytes || request.length == 0 || !validRenderStrategy(request.strategy)) {
    return DisplayRefreshResult::InvalidRequest;
  }
  if (request.strategy != RenderStrategy::OfficialQuality &&
      !config_.experimentalPrequantizationEnabled) {
    return DisplayRefreshResult::ExperimentalDisabled;
  }

  RuntimeLockGuard runtimeLock(lock_);
  if (!runtimeLock.locked()) return DisplayRefreshResult::Busy;
  const PngValidationResult validation = validatePaperColorPng(
      request.bytes, request.length, config_.maximumEncodedPngBytes);
  if (validation.error != PngValidationError::None || !validation.png.valid()) {
    return DisplayRefreshResult::InvalidEncodedPng;
  }
  // E-paper refreshes are slow and consume panel life. Asset IDs are not a
  // sufficient identity (the same bytes may arrive from different sources),
  // so suppress only an exact attested frame+strategy match after a previous
  // successful physical write.
  if (hasLastSuccessfulFrame_ &&
      validation.png.digest() == lastSuccessfulDigest_ &&
      request.strategy == lastSuccessfulStrategy_) {
    return DisplayRefreshResult::Unchanged;
  }

  const uint32_t acquiredAt = clock_.nowMilliseconds();
  RefreshRequest refreshRequest;
  refreshRequest.assetId = request.assetId;
  refreshRequest.strategy = request.strategy;
  const RefreshAcquire acquisition = arbiter_.acquire(refreshRequest, acquiredAt);
  if (!acquisition.accepted()) {
    if (acquisition.result() == RefreshAcquireResult::Busy) {
      return DisplayRefreshResult::Busy;
    }
    if (acquisition.result() == RefreshAcquireResult::Cooldown) {
      return DisplayRefreshResult::Cooldown;
    }
    return DisplayRefreshResult::InvalidRequest;
  }
  const RefreshTicket* ticket = acquisition.ticket();
  if (!ticket || !ticket->fullScreenRefreshRequired()) {
    return DisplayRefreshResult::CapabilityFault;
  }

  const PhysicalRefreshCapability capability(&display_, this);
  bool rendered = false;
  const bool bottomDownLandscape =
      validation.png.width() == kPaperColorHeight &&
      validation.png.height() == kPaperColorWidth;
  if (request.strategy == RenderStrategy::OfficialQuality ||
      bottomDownLandscape) {
    if (!imageLed_.setImageState(ImageLedState::Writing, clock_.nowMilliseconds())) {
      arbiter_.finish(*ticket, clock_.nowMilliseconds());
      return DisplayRefreshResult::LedUnavailable;
    }
    rendered = display_.renderOfficialPng(
        capability, validation.png, request.bytes, request.length);
  } else {
    if (!imageLed_.setImageState(ImageLedState::Converting, clock_.nowMilliseconds())) {
      arbiter_.finish(*ticket, clock_.nowMilliseconds());
      return DisplayRefreshResult::LedUnavailable;
    }
    if (!decoder_.decode(validation.png, request.bytes, request.length)) {
      decoder_.reset();
      arbiter_.finish(*ticket, clock_.nowMilliseconds());
      imageLed_.setImageState(ImageLedState::Error, clock_.nowMilliseconds());
      return DisplayRefreshResult::DecodeFailed;
    }
    DecoderResetGuard resetDecoder(decoder_);
    PaletteFrame paletteFrame(validation.png);
    LedRenderProgress progress(imageLed_, clock_);
    std::string conversionError;
    if (!streamRenderPixels(
            decoder_,
            paletteFrame,
            request.strategy,
            &conversionError,
            &progress) || !paletteFrame.valid() ||
        paletteFrame.sourceDigest() != validation.png.digest()) {
      arbiter_.finish(*ticket, clock_.nowMilliseconds());
      imageLed_.setImageState(ImageLedState::Error, clock_.nowMilliseconds());
      return DisplayRefreshResult::ConversionFailed;
    }
    if (!imageLed_.setImageState(ImageLedState::Writing, clock_.nowMilliseconds())) {
      arbiter_.finish(*ticket, clock_.nowMilliseconds());
      return DisplayRefreshResult::LedUnavailable;
    }
    rendered = display_.renderExperimentalPalette(capability, paletteFrame);
  }

  const RefreshFinishResult finish = arbiter_.finish(
      *ticket, clock_.nowMilliseconds());
  if (finish != RefreshFinishResult::Finished) {
    imageLed_.setImageState(ImageLedState::Error, clock_.nowMilliseconds());
    return DisplayRefreshResult::CapabilityFault;
  }
  if (rendered) {
    hasLastSuccessfulFrame_ = true;
    lastSuccessfulDigest_ = validation.png.digest();
    lastSuccessfulStrategy_ = request.strategy;
  }
  if (!imageLed_.setImageState(
          rendered ? ImageLedState::Complete : ImageLedState::Error,
          clock_.nowMilliseconds())) {
    return DisplayRefreshResult::LedUnavailable;
  }
  return rendered ? DisplayRefreshResult::Complete : DisplayRefreshResult::DisplayFailed;
}

bool DisplayRefreshRuntime::tickImageLed() {
  return imageLed_.tick(clock_.nowMilliseconds());
}

bool DisplayRefreshRuntime::busy() const {
  RuntimeLockGuard guard(lock_);
  return !guard.locked() || arbiter_.busy();
}

DeepSleepExecutionResult executeDeepSleepPlan(
    const SleepDecision& decision,
    uint64_t rtcNowEpochSeconds,
    IDeepSleepPlatform& platform) {
  if (!decision.shouldSleep || decision.reason != SleepDecisionReason::Eligible) {
    return DeepSleepExecutionResult::NotEligible;
  }
  if (!decision.wake.timerEnabled ||
      decision.wake.timerWakeEpochSeconds <= rtcNowEpochSeconds ||
      !decision.wake.ext1AnyLow ||
      decision.wake.ext1AnyLowMask != PowerPolicy::paperColorExt1AnyLowMask()) {
    return DeepSleepExecutionResult::InvalidWakePlan;
  }
  const uint64_t seconds = decision.wake.timerWakeEpochSeconds - rtcNowEpochSeconds;
  if (seconds == 0 || seconds > std::numeric_limits<uint64_t>::max() / 1000000ULL) {
    return DeepSleepExecutionResult::InvalidWakePlan;
  }
  if (!platform.resetWakeSources()) {
    return DeepSleepExecutionResult::WakeSourceResetFailed;
  }
  if (!platform.enableTimerWakeAfterSeconds(seconds)) {
    platform.resetWakeSources();
    return DeepSleepExecutionResult::TimerConfigurationFailed;
  }
  if (!platform.enableAnyLowWake(decision.wake.ext1AnyLowMask)) {
    platform.resetWakeSources();
    return DeepSleepExecutionResult::ButtonConfigurationFailed;
  }
  if (platform.enterDeepSleep()) return DeepSleepExecutionResult::Entered;
  platform.resetWakeSources();
  return DeepSleepExecutionResult::EnterReturned;
}

const char* deepSleepExecutionResultName(DeepSleepExecutionResult result) {
  switch (result) {
    case DeepSleepExecutionResult::Entered: return "ENTERED";
    case DeepSleepExecutionResult::NotEligible: return "NOT_ELIGIBLE";
    case DeepSleepExecutionResult::InvalidWakePlan: return "INVALID_WAKE_PLAN";
    case DeepSleepExecutionResult::WakeSourceResetFailed:
      return "WAKE_SOURCE_RESET_FAILED";
    case DeepSleepExecutionResult::TimerConfigurationFailed:
      return "TIMER_CONFIGURATION_FAILED";
    case DeepSleepExecutionResult::ButtonConfigurationFailed:
      return "BUTTON_CONFIGURATION_FAILED";
    case DeepSleepExecutionResult::EnterReturned: return "ENTER_RETURNED";
  }
  return "UNKNOWN_DEEP_SLEEP_RESULT";
}

const char* powerSnapshotResultName(PowerSnapshotResult result) {
  switch (result) {
    case PowerSnapshotResult::Captured: return "CAPTURED";
    case PowerSnapshotResult::InvalidTarget: return "INVALID_TARGET";
    case PowerSnapshotResult::ClockUnsynchronized:
      return "CLOCK_UNSYNCHRONIZED";
    case PowerSnapshotResult::TaskStoreUnavailable:
      return "TASK_STORE_UNAVAILABLE";
    case PowerSnapshotResult::TaskScheduleInvalid:
      return "TASK_SCHEDULE_INVALID";
    case PowerSnapshotResult::UnknownFailure: return "UNKNOWN_FAILURE";
  }
  return "UNKNOWN_SNAPSHOT_RESULT";
}

PowerSnapshotResult IPreSleepQuiescenceHooks::capturePowerInputsDetailed(
    PowerInputs* inputs) {
  return capturePowerInputs(inputs)
      ? PowerSnapshotResult::Captured
      : PowerSnapshotResult::UnknownFailure;
}

const char* prepareSleepResultName(PrepareSleepResult result) {
  switch (result) {
    case PrepareSleepResult::Entered: return "ENTERED";
    case PrepareSleepResult::NotEligible: return "NOT_ELIGIBLE";
    case PrepareSleepResult::SnapshotFailed: return "SNAPSHOT_FAILED";
    case PrepareSleepResult::TaskOrDisplayFinalizationFailed:
      return "TASK_OR_DISPLAY_FINALIZATION_FAILED";
    case PrepareSleepResult::AudioQuiescenceFailed:
      return "AUDIO_QUIESCENCE_FAILED";
    case PrepareSleepResult::RgbQuiescenceFailed:
      return "RGB_QUIESCENCE_FAILED";
    case PrepareSleepResult::NetworkQuiescenceFailed:
      return "NETWORK_QUIESCENCE_FAILED";
    case PrepareSleepResult::RecheckNotEligible:
      return "RECHECK_NOT_ELIGIBLE";
    case PrepareSleepResult::DeepSleepRejected: return "DEEP_SLEEP_REJECTED";
  }
  return "UNKNOWN_PREPARE_SLEEP_RESULT";
}

const char* powerSnapshotPhaseName(PowerSnapshotPhase phase) {
  switch (phase) {
    case PowerSnapshotPhase::None: return "NONE";
    case PowerSnapshotPhase::Initial: return "INITIAL";
    case PowerSnapshotPhase::Final: return "FINAL";
  }
  return "UNKNOWN_SNAPSHOT_PHASE";
}

PrepareSleepOutcome prepareAndExecuteSleepDetailed(
    const PowerPolicy& policy,
    IPreSleepQuiescenceHooks& hooks,
    IDeepSleepPlatform& platform) {
  PrepareSleepOutcome outcome;
  if (policy.config().mode != PowerMode::BatteryOptIn) {
    outcome.result = PrepareSleepResult::NotEligible;
    outcome.snapshotResult = PowerSnapshotResult::Captured;
    outcome.snapshotPhase = PowerSnapshotPhase::None;
    outcome.decisionReason = SleepDecisionReason::AlwaysAwakeMode;
    return outcome;
  }
  PowerInputs initialInputs;
  outcome.snapshotResult = hooks.capturePowerInputsDetailed(&initialInputs);
  if (outcome.snapshotResult != PowerSnapshotResult::Captured) return outcome;
  outcome.snapshotPhase = PowerSnapshotPhase::None;
  const SleepDecision initialDecision = policy.evaluate(initialInputs);
  outcome.decisionReason = initialDecision.reason;
  if (!initialDecision.shouldSleep) {
    outcome.result = PrepareSleepResult::NotEligible;
    return outcome;
  }
  if (!hooks.finalizeTaskAndDisplay()) {
    outcome.result = PrepareSleepResult::TaskOrDisplayFinalizationFailed;
    return outcome;
  }
  if (!hooks.stopAudio()) {
    outcome.result = PrepareSleepResult::AudioQuiescenceFailed;
    return outcome;
  }
  if (!hooks.stopImageRgb()) {
    outcome.result = PrepareSleepResult::RgbQuiescenceFailed;
    return outcome;
  }
  if (!hooks.closeNetwork()) {
    outcome.result = PrepareSleepResult::NetworkQuiescenceFailed;
    return outcome;
  }

  PowerInputs finalInputs;
  outcome.snapshotResult = hooks.capturePowerInputsDetailed(&finalInputs);
  if (outcome.snapshotResult != PowerSnapshotResult::Captured) {
    outcome.result = PrepareSleepResult::SnapshotFailed;
    outcome.snapshotPhase = PowerSnapshotPhase::Final;
    return outcome;
  }
  outcome.snapshotPhase = PowerSnapshotPhase::None;
  const SleepDecision finalDecision = policy.evaluate(finalInputs);
  outcome.decisionReason = finalDecision.reason;
  if (!finalDecision.shouldSleep) {
    outcome.result = PrepareSleepResult::RecheckNotEligible;
    return outcome;
  }
  outcome.deepSleepResult = executeDeepSleepPlan(
      finalDecision, finalInputs.rtcNowEpochSeconds, platform);
  outcome.result = outcome.deepSleepResult == DeepSleepExecutionResult::Entered
      ? PrepareSleepResult::Entered
      : PrepareSleepResult::DeepSleepRejected;
  return outcome;
}

PrepareSleepResult prepareAndExecuteSleep(
    const PowerPolicy& policy,
    IPreSleepQuiescenceHooks& hooks,
    IDeepSleepPlatform& platform) {
  return prepareAndExecuteSleepDetailed(policy, hooks, platform).result;
}

SleepAttemptRuntime::SleepAttemptRuntime()
    : SleepAttemptRuntime(SleepAttemptRuntimeConfig()) {}

SleepAttemptRuntime::SleepAttemptRuntime(const SleepAttemptRuntimeConfig& config)
    : config_(config),
      configurationValid_(
          config.blockerRecheckMilliseconds > 0 &&
          config.blockerRecheckMilliseconds < 0x80000000UL &&
          config.minimumFailureRetryMilliseconds >= 5000U &&
          config.minimumFailureRetryMilliseconds <= 60000U &&
          config.maximumFailureRetryMilliseconds >=
              config.minimumFailureRetryMilliseconds &&
          config.maximumFailureRetryMilliseconds <= 60000U &&
          config.summaryIntervalMilliseconds >= 60000U &&
          config.summaryIntervalMilliseconds < 0x80000000UL),
      hasAttempt_(false),
      lastAttemptAtMilliseconds_(0),
      currentRetryMilliseconds_(config.minimumFailureRetryMilliseconds),
      hasOutcome_(false),
      lastOutcome_(),
      lastReportAtMilliseconds_(0),
      suppressedAttempts_(0) {}

bool SleepAttemptRuntime::sameOutcome(
    const PrepareSleepOutcome& left,
    const PrepareSleepOutcome& right) {
  return left.result == right.result &&
      left.snapshotResult == right.snapshotResult &&
      left.snapshotPhase == right.snapshotPhase &&
      left.decisionReason == right.decisionReason &&
      left.deepSleepResult == right.deepSleepResult;
}

bool SleepAttemptRuntime::operationalFailure(PrepareSleepResult result) {
  return result != PrepareSleepResult::Entered &&
      result != PrepareSleepResult::NotEligible;
}

SleepAttemptObservation SleepAttemptRuntime::poll(
    uint32_t nowMilliseconds,
    const PowerPolicy& policy,
    IPreSleepQuiescenceHooks& hooks,
    IDeepSleepPlatform& platform) {
  SleepAttemptObservation observation;
  if (!configurationValid_) return observation;
  if (hasAttempt_ && !elapsedAtLeast32(
          nowMilliseconds,
          lastAttemptAtMilliseconds_,
          currentRetryMilliseconds_)) {
    return observation;
  }

  observation.attempted = true;
  observation.outcome = prepareAndExecuteSleepDetailed(policy, hooks, platform);
  const bool repeated = hasOutcome_ && sameOutcome(lastOutcome_, observation.outcome);
  lastAttemptAtMilliseconds_ = nowMilliseconds;
  hasAttempt_ = true;

  if (operationalFailure(observation.outcome.result)) {
    if (repeated) {
      const uint32_t remaining = config_.maximumFailureRetryMilliseconds -
          currentRetryMilliseconds_;
      currentRetryMilliseconds_ +=
          currentRetryMilliseconds_ > remaining
              ? remaining
              : currentRetryMilliseconds_;
    } else {
      currentRetryMilliseconds_ = config_.minimumFailureRetryMilliseconds;
    }
  } else {
    currentRetryMilliseconds_ = config_.blockerRecheckMilliseconds;
  }

  if (!repeated) {
    observation.transition = true;
    lastReportAtMilliseconds_ = nowMilliseconds;
    suppressedAttempts_ = 0;
  } else {
    if (suppressedAttempts_ != std::numeric_limits<uint32_t>::max()) {
      ++suppressedAttempts_;
    }
    if (elapsedAtLeast32(
            nowMilliseconds,
            lastReportAtMilliseconds_,
            config_.summaryIntervalMilliseconds)) {
      observation.summary = true;
      observation.suppressedAttempts = suppressedAttempts_;
      suppressedAttempts_ = 0;
      lastReportAtMilliseconds_ = nowMilliseconds;
    }
  }
  lastOutcome_ = observation.outcome;
  hasOutcome_ = true;
  return observation;
}

WakeRecoveryRuntime::WakeRecoveryRuntime(
    IWakeRecoveryHooks& hooks,
    const WakeRecoveryConfig& config)
    : hooks_(hooks),
      state_(),
      config_(config),
      configurationValid_(
          config.releaseDebounceMilliseconds > 0 &&
          config.releaseDebounceMilliseconds < 0x80000000UL &&
          config.retryIntervalMilliseconds > 0 &&
          config.retryIntervalMilliseconds < 0x80000000UL &&
          config.maximumAttemptsPerStage > 0),
      releaseCandidateAtMilliseconds_(0),
      releaseCandidateActive_(false),
      lastNetworkAttemptAtMilliseconds_(0),
      networkAttempts_(0),
      hasNetworkAttempt_(false) {}

WakeRecoveryRuntime::WakeRecoveryRuntime(
    IWakeRecoveryHooks& hooks,
    uint32_t releaseDebounceMilliseconds)
    : WakeRecoveryRuntime(hooks, debounceOnlyConfig(releaseDebounceMilliseconds)) {}

bool WakeRecoveryRuntime::beginAfterHardwareReady(WakeReason reason) {
  releaseCandidateActive_ = false;
  networkAttempts_ = 0;
  hasNetworkAttempt_ = false;
  if (!configurationValid_) {
    state_.markFault();
    return false;
  }
  return state_.begin(reason) && state_.markHardwareReady();
}

bool WakeRecoveryRuntime::networkAttemptDue(uint32_t nowMilliseconds) const {
  return !hasNetworkAttempt_ || elapsedAtLeast32(
      nowMilliseconds,
      lastNetworkAttemptAtMilliseconds_,
      config_.retryIntervalMilliseconds);
}

void WakeRecoveryRuntime::recordNetworkAttempt(
    uint32_t nowMilliseconds,
    bool succeeded) {
  hasNetworkAttempt_ = true;
  lastNetworkAttemptAtMilliseconds_ = nowMilliseconds;
  ++networkAttempts_;
  if (succeeded) {
    networkAttempts_ = 0;
    hasNetworkAttempt_ = false;
  } else if (networkAttempts_ >= config_.maximumAttemptsPerStage) {
    state_.markFault();
  }
}

bool WakeRecoveryRuntime::poll(uint32_t nowMilliseconds) {
  switch (state_.stage()) {
    case ReconnectStage::ReconnectWifi:
      if (networkAttemptDue(nowMilliseconds)) {
        const bool succeeded = hooks_.reconnectWifi();
        recordNetworkAttempt(nowMilliseconds, succeeded);
        if (succeeded) state_.markWifiConnected();
      }
      break;
    case ReconnectStage::SyncInkloop:
      if (networkAttemptDue(nowMilliseconds)) {
        const bool succeeded = hooks_.syncInkloopSchedules();
        recordNetworkAttempt(nowMilliseconds, succeeded);
        if (succeeded) state_.markInkloopSynced();
      }
      break;
    case ReconnectStage::AwaitWakeButtonsReleased:
      if (!hooks_.allWakeButtonsReleased()) {
        releaseCandidateActive_ = false;
        break;
      }
      if (!releaseCandidateActive_) {
        releaseCandidateActive_ = true;
        releaseCandidateAtMilliseconds_ = nowMilliseconds;
        break;
      }
      if (elapsedAtLeast32(
              nowMilliseconds,
              releaseCandidateAtMilliseconds_,
              config_.releaseDebounceMilliseconds)) {
        state_.markWakeButtonsReleasedDebounced(true, true, true);
      }
      break;
    case ReconnectStage::ArmInput:
      if (hooks_.rearmButtonInput()) state_.markInputRearmed();
      break;
    case ReconnectStage::Ready:
      return true;
    case ReconnectStage::Fault:
    case ReconnectStage::NotStarted:
    case ReconnectStage::ReinitializeHardware:
      break;
  }
  return state_.readyForUserInput();
}

}  // namespace displaypower
}  // namespace inkloop
