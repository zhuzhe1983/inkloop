#include "DisplayPowerAdapters.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_err.h>
#include <esp_sleep.h>

#include <algorithm>
#include <limits>

namespace inkloop {

namespace {

constexpr gpio_num_t kPaperColorWakePins[] = {
    GPIO_NUM_1,
    GPIO_NUM_9,
    GPIO_NUM_10,
};
constexpr uint32_t kLedAnimationTickMilliseconds = 50;

bool imageStateToFirmwareState(
    displaypower::ImageLedState state,
    LedState* mapped) {
  if (!mapped) return false;
  switch (state) {
    case displaypower::ImageLedState::Off:
      *mapped = LedState::Off;
      return true;
    case displaypower::ImageLedState::Generating:
      *mapped = LedState::Generating;
      return true;
    case displaypower::ImageLedState::Downloading:
      *mapped = LedState::Downloading;
      return true;
    case displaypower::ImageLedState::Caching:
      *mapped = LedState::Caching;
      return true;
    case displaypower::ImageLedState::Converting:
      *mapped = LedState::Listening;
      return true;
    case displaypower::ImageLedState::Writing:
      *mapped = LedState::Writing;
      return true;
    case displaypower::ImageLedState::Complete:
      *mapped = LedState::Complete;
      return true;
    case displaypower::ImageLedState::Error:
      *mapped = LedState::Error;
      return true;
  }
  return false;
}

uint8_t animationBrightness(const displaypower::LedOutput& output) {
  if (!output.illuminated) return 1;
  const uint8_t strongest = std::max(
      output.color.red,
      std::max(output.color.green, output.color.blue));
  const uint16_t scaled = static_cast<uint16_t>(strongest) * 64U;
  const uint8_t result = static_cast<uint8_t>((scaled + 254U) / 255U);
  return result == 0 ? 1 : result;
}

}  // namespace

uint32_t ArduinoDisplayPowerClock::nowMilliseconds() const {
  return millis();
}

FreeRtosDisplayPowerLock::~FreeRtosDisplayPowerLock() {
  if (mutex_) vSemaphoreDelete(mutex_);
}

bool FreeRtosDisplayPowerLock::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  return mutex_ != nullptr;
}

bool FreeRtosDisplayPowerLock::tryLock() {
  return mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE;
}

void FreeRtosDisplayPowerLock::unlock() {
  if (mutex_) xSemaphoreGive(mutex_);
}

M5PngPixelDecoder::M5PngPixelDecoder()
    : canvas_(&M5.Display), nextPixel_(0), ready_(false) {}

M5PngPixelDecoder::~M5PngPixelDecoder() {
  reset();
}

bool M5PngPixelDecoder::decode(
    const displaypower::ValidatedPng& png,
    const uint8_t* bytes,
    size_t length) {
  reset();
  if (!png.matchesExactBytes(bytes, length) ||
      png.width() != displaypower::kPaperColorWidth ||
      png.height() != displaypower::kPaperColorHeight) {
    return false;
  }
  canvas_.setColorDepth(24);
  if (!canvas_.createSprite(
          displaypower::kPaperColorWidth,
          displaypower::kPaperColorHeight)) {
    return false;
  }
  canvas_.fillSprite(WHITE);
  if (!canvas_.drawPng(bytes, length, 0, 0)) {
    reset();
    return false;
  }
  nextPixel_ = 0;
  ready_ = true;
  return true;
}

void M5PngPixelDecoder::reset() {
  canvas_.deleteSprite();
  nextPixel_ = 0;
  ready_ = false;
}

uint16_t M5PngPixelDecoder::width() const {
  return ready_ ? displaypower::kPaperColorWidth : 0;
}

uint16_t M5PngPixelDecoder::height() const {
  return ready_ ? displaypower::kPaperColorHeight : 0;
}

bool M5PngPixelDecoder::read(displaypower::RgbPixel* pixel) {
  const size_t pixelCount = static_cast<size_t>(displaypower::kPaperColorWidth) *
      displaypower::kPaperColorHeight;
  if (!ready_ || !pixel || nextPixel_ >= pixelCount) return false;
  const uint16_t x = static_cast<uint16_t>(nextPixel_ % displaypower::kPaperColorWidth);
  const uint16_t y = static_cast<uint16_t>(nextPixel_ / displaypower::kPaperColorWidth);
  const uint32_t color = canvas_.readPixel(x, y);
  *pixel = displaypower::RgbPixel(
      static_cast<uint8_t>(color >> 16U),
      static_cast<uint8_t>(color >> 8U),
      static_cast<uint8_t>(color));
  ++nextPixel_;
  return true;
}

bool M5FullScreenDisplayAdapter::claimSoleWriter(const void* writer) {
  if (!writer) return false;
  const void* expected = nullptr;
  return writer_.compare_exchange_strong(
      expected, writer, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool M5FullScreenDisplayAdapter::renderOfficialPng(
    const displaypower::PhysicalRefreshCapability& capability,
    const displaypower::ValidatedPng& png,
    const uint8_t* bytes,
    size_t length) {
  const void* writer = writer_.load(std::memory_order_acquire);
  const bool supportedDimensions =
      (png.width() == displaypower::kPaperColorWidth &&
       png.height() == displaypower::kPaperColorHeight) ||
      (png.width() == displaypower::kPaperColorHeight &&
       png.height() == displaypower::kPaperColorWidth);
  if (!capability.validFor(this, writer) ||
      !png.matchesExactBytes(bytes, length) || !supportedDimensions) {
    return false;
  }
  // DisplayController selects ED2208 epd_quality and performs one full refresh.
  return display_.showPng(
      bytes, length, png.width() == displaypower::kPaperColorWidth);
}

bool M5FullScreenDisplayAdapter::renderExperimentalPalette(
    const displaypower::PhysicalRefreshCapability& capability,
    const displaypower::PaletteFrame& frame) {
  const void* writer = writer_.load(std::memory_order_acquire);
  if (!capability.validFor(this, writer) || !frame.valid()) return false;
  const std::vector<displaypower::RgbPixel>& pixels = frame.pixels();
  if (pixels.size() != static_cast<size_t>(displaypower::kPaperColorWidth) *
      displaypower::kPaperColorHeight) {
    return false;
  }

  M5.Display.setRotation(0);
  if (M5.Display.width() != displaypower::kPaperColorWidth ||
      M5.Display.height() != displaypower::kPaperColorHeight) {
    return false;
  }
  // Pinned Panel_ED2208 maps epd_fastest to nearest-palette conversion with
  // `_dither_row_none`; the panel implementation still transfers the full frame.
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.startWrite();
  for (uint16_t y = 0; y < displaypower::kPaperColorHeight; ++y) {
    lgfx::rgb888_t row[displaypower::kPaperColorWidth];
    const size_t rowStart = static_cast<size_t>(y) * displaypower::kPaperColorWidth;
    for (uint16_t x = 0; x < displaypower::kPaperColorWidth; ++x) {
      const displaypower::RgbPixel& pixel = pixels[rowStart + x];
      row[x] = lgfx::rgb888_t(pixel.red, pixel.green, pixel.blue);
    }
    M5.Display.pushImage(0, y, displaypower::kPaperColorWidth, 1, row);
  }
  M5.Display.endWrite();
  M5.Display.waitDisplay();
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  return !M5.Display.displayBusy();
}

M5ImageLedAdapter::M5ImageLedAdapter(LedStatusController& leds)
    : leds_(leds),
      mutex_(nullptr),
      animationTask_(nullptr),
      running_(false),
      state_(displaypower::ImageLedState::Off),
      stateStartedAtMilliseconds_(0),
      lastTickAtMilliseconds_(0),
      hasTicked_(false) {}

M5ImageLedAdapter::~M5ImageLedAdapter() {
  running_.store(false, std::memory_order_release);
  if (animationTask_) {
    vTaskDelete(animationTask_);
    animationTask_ = nullptr;
  }
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

bool M5ImageLedAdapter::beginAnimation() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  if (animationTask_) return true;
  running_.store(true, std::memory_order_release);
  if (xTaskCreate(
          animationTaskEntry,
          "inkloop-image-led",
          3072,
          this,
          1,
          &animationTask_) != pdPASS) {
    running_.store(false, std::memory_order_release);
    animationTask_ = nullptr;
    return false;
  }
  return true;
}

bool M5ImageLedAdapter::setImageState(
    displaypower::ImageLedState state,
    uint32_t nowMilliseconds) {
  if (!mutex_ || !displaypower::imageLedOutput(state, nowMilliseconds, nowMilliseconds).valid ||
      xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  state_ = state;
  stateStartedAtMilliseconds_ = nowMilliseconds;
  hasTicked_ = false;
  xSemaphoreGive(mutex_);
  return tick(nowMilliseconds);
}

bool M5ImageLedAdapter::snapshot(
    displaypower::ImageLedState* state,
    uint32_t* stateStartedAt,
    uint32_t nowMilliseconds) {
  if (!mutex_ || !state || !stateStartedAt ||
      xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (hasTicked_ && !displaypower::elapsedAtLeast32(
          nowMilliseconds,
          lastTickAtMilliseconds_,
          kLedAnimationTickMilliseconds)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  *state = state_;
  *stateStartedAt = stateStartedAtMilliseconds_;
  lastTickAtMilliseconds_ = nowMilliseconds;
  hasTicked_ = true;
  xSemaphoreGive(mutex_);
  return true;
}

bool M5ImageLedAdapter::tick(uint32_t nowMilliseconds) {
  displaypower::ImageLedState state;
  uint32_t stateStartedAt = 0;
  if (!snapshot(&state, &stateStartedAt, nowMilliseconds)) {
    // A rate-limited tick is already healthy.
    return mutex_ != nullptr;
  }
  const displaypower::LedOutput output = displaypower::imageLedOutput(
      state, nowMilliseconds, stateStartedAt);
  if (!output.valid) return false;
  LedState mapped = LedState::Off;
  if (output.illuminated && !imageStateToFirmwareState(state, &mapped)) return false;
  return leds_.setRoleState(
      LedRole::Image,
      output.illuminated ? mapped : LedState::Off,
      animationBrightness(output));
}

bool M5ImageLedAdapter::quiesce(uint32_t nowMilliseconds) {
  return setImageState(displaypower::ImageLedState::Off, nowMilliseconds);
}

void M5ImageLedAdapter::animationTaskEntry(void* context) {
  M5ImageLedAdapter* self = static_cast<M5ImageLedAdapter*>(context);
  while (self && self->running_.load(std::memory_order_acquire)) {
    self->tick(millis());
    vTaskDelay(pdMS_TO_TICKS(kLedAnimationTickMilliseconds));
  }
  vTaskDelete(nullptr);
}

bool Esp32DevicePowerAdapter::resetWakeSources() {
  return esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL) == ESP_OK;
}

bool Esp32DevicePowerAdapter::enableTimerWakeAfterSeconds(uint64_t seconds) {
  if (seconds == 0 || seconds > std::numeric_limits<uint64_t>::max() / 1000000ULL) {
    return false;
  }
  return esp_sleep_enable_timer_wakeup(seconds * 1000000ULL) == ESP_OK;
}

bool Esp32DevicePowerAdapter::enableAnyLowWake(uint64_t gpioMask) {
  if (gpioMask != displaypower::PowerPolicy::paperColorExt1AnyLowMask()) return false;
  for (size_t index = 0;
       index < sizeof(kPaperColorWakePins) / sizeof(kPaperColorWakePins[0]);
       ++index) {
    const gpio_num_t pin = kPaperColorWakePins[index];
    if (!rtc_gpio_is_valid_gpio(pin) || rtc_gpio_pulldown_dis(pin) != ESP_OK ||
        rtc_gpio_pullup_en(pin) != ESP_OK) {
      return false;
    }
  }
  return esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ANY_LOW) == ESP_OK;
}

bool Esp32DevicePowerAdapter::enterDeepSleep() {
  Serial.flush();
  M5.Display.sleep();
  esp_deep_sleep_start();
  return false;
}

displaypower::WakeReason Esp32DevicePowerAdapter::detectedWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return displaypower::WakeReason::ColdBoot;
    case ESP_SLEEP_WAKEUP_TIMER:
      return displaypower::WakeReason::RtcTimer;
    case ESP_SLEEP_WAKEUP_EXT1:
      return displaypower::wakeReasonFromExt1Mask(esp_sleep_get_ext1_wakeup_status());
    default:
      return displaypower::WakeReason::Unknown;
  }
}

bool PaperColorPreSleepQuiescenceHooks::capturePowerInputs(
    displaypower::PowerInputs* inputs) {
  return capturePowerInputsDetailed(inputs) ==
      displaypower::PowerSnapshotResult::Captured;
}

displaypower::PowerSnapshotResult
PaperColorPreSleepQuiescenceHooks::capturePowerInputsDetailed(
    displaypower::PowerInputs* inputs) {
  if (!captureInputs_ || !inputs) {
    return displaypower::PowerSnapshotResult::InvalidTarget;
  }
  const displaypower::PowerSnapshotResult result =
      captureInputs_(context_, inputs);
  switch (result) {
    case displaypower::PowerSnapshotResult::Captured:
    case displaypower::PowerSnapshotResult::InvalidTarget:
    case displaypower::PowerSnapshotResult::ClockUnsynchronized:
    case displaypower::PowerSnapshotResult::TaskStoreUnavailable:
    case displaypower::PowerSnapshotResult::TaskScheduleInvalid:
    case displaypower::PowerSnapshotResult::UnknownFailure:
      return result;
  }
  return displaypower::PowerSnapshotResult::UnknownFailure;
}

bool PaperColorPreSleepQuiescenceHooks::finalizeTaskAndDisplay() {
  return finalizeTaskAndDisplay_ && finalizeTaskAndDisplay_(context_);
}

bool PaperColorPreSleepQuiescenceHooks::stopAudio() {
  return stopAudio_ && stopAudio_(context_);
}

bool PaperColorPreSleepQuiescenceHooks::stopImageRgb() {
  return imageLed_.quiesce(millis());
}

bool PaperColorPreSleepQuiescenceHooks::closeNetwork() {
  return closeNetwork_ && closeNetwork_(context_);
}

bool PaperColorWakeRecoveryHooks::reconnectWifi() {
  return reconnectWifi_ && reconnectWifi_(context_);
}

bool PaperColorWakeRecoveryHooks::syncInkloopSchedules() {
  return syncInkloopSchedules_ && syncInkloopSchedules_(context_);
}

bool PaperColorWakeRecoveryHooks::allWakeButtonsReleased() {
  M5.update();
  return !M5.BtnA.isPressed() && !M5.BtnB.isPressed() && !M5.BtnC.isPressed();
}

bool PaperColorWakeRecoveryHooks::rearmButtonInput() {
  if (!allWakeButtonsReleased()) return false;
  buttons_.suppressUntilRelease();
  buttons_.poll();
  return true;
}

}  // namespace inkloop
