#pragma once

#include <stdint.h>

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <M5Unified.h>

#include "ButtonRouter.h"
#include "DisplayController.h"
#include "DisplayPowerRuntime.h"
#include "LedStatusController.h"

namespace inkloop {

class ArduinoDisplayPowerClock final
    : public displaypower::IDisplayPowerClock {
 public:
  uint32_t nowMilliseconds() const override;
};

class FreeRtosDisplayPowerLock final
    : public displaypower::IDisplayPowerLock {
 public:
  FreeRtosDisplayPowerLock() : mutex_(nullptr) {}
  ~FreeRtosDisplayPowerLock();

  bool begin();
  bool tryLock() override;
  void unlock() override;

 private:
  SemaphoreHandle_t mutex_;
};

class M5PngPixelDecoder final : public displaypower::IPngPixelDecoder {
 public:
  M5PngPixelDecoder();
  ~M5PngPixelDecoder();

  bool decode(
      const displaypower::ValidatedPng& png,
      const uint8_t* bytes,
      size_t length) override;
  void reset() override;
  uint16_t width() const override;
  uint16_t height() const override;
  bool read(displaypower::RgbPixel* pixel) override;

 private:
  M5Canvas canvas_;
  size_t nextPixel_;
  bool ready_;
};

class M5FullScreenDisplayAdapter final
    : public displaypower::IFullScreenDisplay {
 public:
  explicit M5FullScreenDisplayAdapter(DisplayController& display)
      : display_(display), writer_(nullptr) {}

  bool claimSoleWriter(const void* writer) override;
  bool renderOfficialPng(
      const displaypower::PhysicalRefreshCapability& capability,
      const displaypower::ValidatedPng& png,
      const uint8_t* bytes,
      size_t length) override;
  bool renderExperimentalPalette(
      const displaypower::PhysicalRefreshCapability& capability,
      const displaypower::PaletteFrame& frame) override;

 private:
  DisplayController& display_;
  std::atomic<const void*> writer_;
};

class M5ImageLedAdapter final : public displaypower::IImageLedSink {
 public:
  explicit M5ImageLedAdapter(LedStatusController& leds);
  ~M5ImageLedAdapter();

  bool beginAnimation();
  bool setImageState(
      displaypower::ImageLedState state,
      uint32_t nowMilliseconds) override;
  bool tick(uint32_t nowMilliseconds) override;
  bool quiesce(uint32_t nowMilliseconds) override;

 private:
  static void animationTaskEntry(void* context);
  bool snapshot(
      displaypower::ImageLedState* state,
      uint32_t* stateStartedAt,
      uint32_t nowMilliseconds);

  LedStatusController& leds_;
  SemaphoreHandle_t mutex_;
  TaskHandle_t animationTask_;
  std::atomic<bool> running_;
  displaypower::ImageLedState state_;
  uint32_t stateStartedAtMilliseconds_;
  uint32_t lastTickAtMilliseconds_;
  bool hasTicked_;
};

class Esp32DevicePowerAdapter final
    : public displaypower::IDeepSleepPlatform {
 public:
  bool resetWakeSources() override;
  bool enableTimerWakeAfterSeconds(uint64_t seconds) override;
  bool enableAnyLowWake(uint64_t gpioMask) override;
  bool enterDeepSleep() override;

  static displaypower::WakeReason detectedWakeReason();
};

using DisplayPowerHook = bool (*)(void* context);
using CapturePowerInputsHook = displaypower::PowerSnapshotResult (*)(
    void* context,
    displaypower::PowerInputs* inputs);

class PaperColorPreSleepQuiescenceHooks final
    : public displaypower::IPreSleepQuiescenceHooks {
 public:
  PaperColorPreSleepQuiescenceHooks(
      M5ImageLedAdapter& imageLed,
      CapturePowerInputsHook captureInputs,
      DisplayPowerHook finalizeTaskAndDisplay,
      DisplayPowerHook stopAudio,
      DisplayPowerHook closeNetwork,
      void* context)
      : imageLed_(imageLed),
        captureInputs_(captureInputs),
        finalizeTaskAndDisplay_(finalizeTaskAndDisplay),
        stopAudio_(stopAudio),
        closeNetwork_(closeNetwork),
        context_(context) {}

  bool capturePowerInputs(displaypower::PowerInputs* inputs) override;
  displaypower::PowerSnapshotResult capturePowerInputsDetailed(
      displaypower::PowerInputs* inputs) override;
  bool finalizeTaskAndDisplay() override;
  bool stopAudio() override;
  bool stopImageRgb() override;
  bool closeNetwork() override;

 private:
  M5ImageLedAdapter& imageLed_;
  CapturePowerInputsHook captureInputs_;
  DisplayPowerHook finalizeTaskAndDisplay_;
  DisplayPowerHook stopAudio_;
  DisplayPowerHook closeNetwork_;
  void* context_;
};

class PaperColorWakeRecoveryHooks final
    : public displaypower::IWakeRecoveryHooks {
 public:
  PaperColorWakeRecoveryHooks(
      ButtonRouter& buttons,
      DisplayPowerHook reconnectWifi,
      DisplayPowerHook syncInkloopSchedules,
      void* context)
      : buttons_(buttons),
        reconnectWifi_(reconnectWifi),
        syncInkloopSchedules_(syncInkloopSchedules),
        context_(context) {}

  bool reconnectWifi() override;
  bool syncInkloopSchedules() override;
  bool allWakeButtonsReleased() override;
  bool rearmButtonInput() override;

 private:
  ButtonRouter& buttons_;
  DisplayPowerHook reconnectWifi_;
  DisplayPowerHook syncInkloopSchedules_;
  void* context_;
};

}  // namespace inkloop
