#include "LedStatusController.h"

#include <M5Unified.h>

#include "Diagnostics.h"

namespace inkloop {

namespace {
constexpr uint16_t kNominalMaximumBrightness = kLedDiagnosticBrightness;
}

LedStatusController::~LedStatusController() {
  if (mutex_) vSemaphoreDelete(mutex_);
}

bool LedStatusController::lock() const {
  return mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
}

void LedStatusController::unlock() const {
  xSemaphoreGive(mutex_);
}

bool LedStatusController::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  if (!lock()) {
    Diagnostics::event("ERROR", "LED_MUTEX_UNAVAILABLE");
    return false;
  }
  // PaperColor C151 has two WS2812 LEDs chained on GPIO21.  Use the
  // hardware-verified GRB/800kHz driver path from the PaperColor reference
  // implementation instead of M5Unified's generic strip abstraction.
  pixels_.begin();
  pixels_.setBrightness(scaledBrightness(80));
  pixels_.clear();
  pixels_.show();
  count_ = static_cast<uint8_t>(pixels_.numPixels());
  calibrated_ = settings_.current().ledMappingCalibrated;
  voiceLedIndex_ = settings_.current().voiceLedIndex > 0 ? 1 : 0;
  Diagnostics::event("LED_COUNT", String(count_));
  Diagnostics::event("LED_DRIVER", count_ == 2 ? "NEOPIXEL_GPIO21_GRB_READY"
                                                 : "NEOPIXEL_COUNT_ERROR");
  Diagnostics::event(
    "LED_MAPPING",
    calibrated_ ? String("VOICE_") + String(voiceLedIndex_) : "UNCALIBRATED"
  );
  unlock();
  return count_ == 2;
}

bool LedStatusController::setCombinedColor(
  uint8_t red,
  uint8_t green,
  uint8_t blue,
  uint8_t brightness
) {
  if (!lock()) return false;
  if (diagnosticActive_) {
    unlock();
    return false;
  }
  pixels_.setBrightness(scaledBrightness(brightness));
  for (uint8_t index = 0; index < count_; ++index)
    pixels_.setPixelColor(index, pixels_.Color(red, green, blue));
  pixels_.show();
  unlock();
  return true;
}

void LedStatusController::renderRolesLocked(uint8_t brightness) {
  pixels_.setBrightness(scaledBrightness(brightness));
  const LedFrame frame = resolveLedFrame(calibrated_, voiceLedIndex_, count_, voiceState_, imageState_);
  if (!calibrated_ || count_ < 2) {
    for (uint8_t index = 0; index < count_; ++index) {
      pixels_.setPixelColor(
          index, pixels_.Color(
              frame.pixels[0].red, frame.pixels[0].green,
              frame.pixels[0].blue));
    }
  } else {
    pixels_.setPixelColor(
        0, pixels_.Color(
               frame.pixels[0].red, frame.pixels[0].green,
               frame.pixels[0].blue));
    pixels_.setPixelColor(
        1, pixels_.Color(
               frame.pixels[1].red, frame.pixels[1].green,
               frame.pixels[1].blue));
  }
  pixels_.show();
}

uint8_t LedStatusController::scaledBrightness(
    uint8_t logicalBrightness) const {
  const uint16_t maximum = static_cast<uint16_t>(
      (static_cast<uint16_t>(maximumBrightnessPercent_) * 255U + 50U) /
      100U);
  const uint16_t scaled = static_cast<uint16_t>(
      (static_cast<uint32_t>(logicalBrightness) * maximum +
       kNominalMaximumBrightness / 2U) /
      kNominalMaximumBrightness);
  return static_cast<uint8_t>(scaled > maximum ? maximum : scaled);
}

bool LedStatusController::setMaximumBrightnessPercent(uint8_t percent) {
  if (percent < 1 || percent > 100 || !lock()) return false;
  maximumBrightnessPercent_ = percent;
  if (!diagnosticActive_) renderRolesLocked(desiredRoleBrightness_);
  Diagnostics::event("LED_MAX_BRIGHTNESS", String(percent));
  unlock();
  return true;
}

bool LedStatusController::setRoleState(LedRole role, LedState state, uint8_t brightness) {
  if (!lock()) return false;
  if (role == LedRole::Voice) voiceState_ = state;
  else imageState_ = state;
  desiredRoleBrightness_ = brightness;
  if (!diagnosticActive_) renderRolesLocked(desiredRoleBrightness_);
  unlock();
  return true;
}

bool LedStatusController::setMapping(bool calibrated, uint8_t voiceLedIndex) {
  if (!lock()) return false;
  if (diagnosticActive_) {
    unlock();
    return false;
  }
  const uint8_t normalizedIndex = voiceLedIndex > 0 ? 1 : 0;
  if (!settings_.setLedMapping(calibrated, normalizedIndex)) {
    unlock();
    Diagnostics::event("ERROR", "LED_MAPPING_PERSIST_FAILED");
    return false;
  }
  calibrated_ = calibrated;
  voiceLedIndex_ = normalizedIndex;
  Diagnostics::event(
    "LED_MAPPING",
    calibrated_ ? String("VOICE_") + String(voiceLedIndex_) : "UNCALIBRATED"
  );
  renderRolesLocked(desiredRoleBrightness_);
  unlock();
  return true;
}

void LedStatusController::renderDiagnosticFrameLocked(
    const LedRoleDiagnosticFrame& frame) {
  pixels_.setBrightness(scaledBrightness(kLedDiagnosticBrightness));
  pixels_.clear();
  if (frame.illuminated) {
    if (frame.role == LedDiagnosticRole::PowerProof) {
      // Mirrors the official PaperColor demo sequence first: prove the PM1
      // RGB rail and both pixels together before any per-index role mapping.
      for (uint8_t index = 0; index < count_; ++index)
        pixels_.setPixelColor(
            index, pixels_.Color(frame.red, frame.green, frame.blue));
      Diagnostics::event("LED_ROLE_DIAGNOSTIC", "ROLE:POWER:ALL");
      Diagnostics::event("LED_ROLE_DIAGNOSTIC", "CYCLE:POWER:1");
      pixels_.show();
      return;
    }
    const bool voice = frame.role == LedDiagnosticRole::Voice;
    const uint8_t index = voice ? voiceLedIndex_
                                : static_cast<uint8_t>(voiceLedIndex_ == 0 ? 1 : 0);
    pixels_.setPixelColor(
        index, pixels_.Color(frame.red, frame.green, frame.blue));
    Diagnostics::event(
        "LED_ROLE_DIAGNOSTIC",
        String("ROLE:") + (voice ? "VOICE:" : "IMAGE:") + String(index));
    Diagnostics::event(
        "LED_ROLE_DIAGNOSTIC",
        String("CYCLE:") + (voice ? "VOICE:" : "IMAGE:") +
            String(frame.cycle + 1));
  }
  pixels_.show();
}

bool LedStatusController::runPixelDiagnostic() {
  if (!lock()) return false;
  if (diagnosticActive_ || count_ != 2 || !calibrated_) {
    Diagnostics::event(
        "LED_ROLE_DIAGNOSTIC",
        diagnosticActive_ ? "BUSY" : (count_ != 2 ? "COUNT_REJECTED" : "ROLE_UNCALIBRATED"));
    unlock();
    return false;
  }
  diagnosticActive_ = true;
  diagnosticStartedAt_ = millis();
  diagnosticPhase_ = 0;
  Diagnostics::event("LED_ROLE_DIAGNOSTIC", "START");
  renderDiagnosticFrameLocked(ledRoleDiagnosticFrame(0));
  unlock();
  return true;
}

void LedStatusController::pollPixelDiagnostic(uint32_t nowMilliseconds) {
  if (!lock()) return;
  if (!diagnosticActive_) {
    unlock();
    return;
  }
  const LedRoleDiagnosticFrame frame = ledRoleDiagnosticFrame(
      nowMilliseconds - diagnosticStartedAt_);
  if (frame.complete) {
    diagnosticActive_ = false;
    diagnosticPhase_ = frame.phase;
    renderRolesLocked(desiredRoleBrightness_);
    Diagnostics::event("LED_ROLE_DIAGNOSTIC", "COMPLETE");
  } else if (frame.phase != diagnosticPhase_) {
    diagnosticPhase_ = frame.phase;
    renderDiagnosticFrameLocked(frame);
  }
  unlock();
}

bool LedStatusController::pixelDiagnosticActive() const {
  if (!lock()) return false;
  const bool active = diagnosticActive_;
  unlock();
  return active;
}

uint8_t LedStatusController::count() const {
  if (!lock()) return 0;
  const uint8_t result = count_;
  unlock();
  return result;
}

bool LedStatusController::mappingCalibrated() const {
  if (!lock()) return false;
  const bool result = calibrated_;
  unlock();
  return result;
}

uint8_t LedStatusController::voiceLedIndex() const {
  if (!lock()) return 0;
  const uint8_t result = voiceLedIndex_;
  unlock();
  return result;
}

uint8_t LedStatusController::maximumBrightnessPercent() const {
  if (!lock()) return 0;
  const uint8_t result = maximumBrightnessPercent_;
  unlock();
  return result;
}

}  // namespace inkloop
