#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "FirmwarePrimitives.h"
#include "LedRoleDiagnosticPrimitives.h"
#include "SettingsStore.h"

namespace inkloop {

class LedStatusController {
 public:
  explicit LedStatusController(SettingsStore& settings)
      : settings_(settings), pixels_(2, 21, NEO_GRB + NEO_KHZ800) {}
  ~LedStatusController();

  bool begin();
  bool setCombinedColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness = 32);
  bool setRoleState(LedRole role, LedState state, uint8_t brightness = 32);
  bool setMaximumBrightnessPercent(uint8_t percent);
  bool setMapping(bool calibrated, uint8_t voiceLedIndex);
  bool runPixelDiagnostic();
  void pollPixelDiagnostic(uint32_t nowMilliseconds);
  bool pixelDiagnosticActive() const;

  uint8_t count() const;
  bool mappingCalibrated() const;
  uint8_t voiceLedIndex() const;
  uint8_t maximumBrightnessPercent() const;

 private:
  bool lock() const;
  void unlock() const;
  uint8_t scaledBrightness(uint8_t logicalBrightness) const;
  void renderRolesLocked(uint8_t brightness);
  void renderDiagnosticFrameLocked(const LedRoleDiagnosticFrame& frame);

  SettingsStore& settings_;
  Adafruit_NeoPixel pixels_;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  uint8_t count_ = 0;
  bool calibrated_ = false;
  uint8_t voiceLedIndex_ = 0;
  LedState voiceState_ = LedState::Off;
  LedState imageState_ = LedState::Off;
  uint8_t desiredRoleBrightness_ = 32;
  uint8_t maximumBrightnessPercent_ = 60;
  bool diagnosticActive_ = false;
  uint32_t diagnosticStartedAt_ = 0;
  uint8_t diagnosticPhase_ = 8;
};

}  // namespace inkloop
