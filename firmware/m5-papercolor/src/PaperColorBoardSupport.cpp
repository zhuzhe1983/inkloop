#include "PaperColorBoardSupport.h"

#include "Diagnostics.h"

namespace inkloop {

bool PaperColorBoardSupport::begin() {
  auto config = M5.config();
  config.clear_display = false;
  M5.begin(config);

  Diagnostics::event("BOARD", String(boardId()));
  if (M5.getBoard() != m5::board_t::board_M5PaperColor) {
    Diagnostics::event("WARN", "PAPERCOLOR_NOT_DETECTED");
  }

  const m5pm1_err_t status = pm1_.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
  pm1Ready_ = status == M5PM1_OK;
  Diagnostics::event("PM1", pm1Ready_ ? "READY" : String("ERROR_") + String(static_cast<int>(status)));
  if (pm1Ready_) {
    const m5pm1_err_t ldoStatus = pm1_.setLdoEnable(true);
    if (ldoStatus != M5PM1_OK) {
      pm1Ready_ = false;
      Diagnostics::event(
          "RGB_POWER", String("LDO_ENABLE_FAILED_") +
              String(static_cast<int>(ldoStatus)));
    } else {
      Diagnostics::event("RGB_POWER", "LDO_ENABLED");
    }
  }
  if (pm1Ready_) {
    pm1_.setI2cConfig(0);
    pm1_.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
    pm1_.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);
    pm1_.setChargeEnable(true);
    pm1_.setBoostEnable(true);
  }

  if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
  M5.Speaker.setVolume(72);
  const bool ready =
      M5.getBoard() == m5::board_t::board_M5PaperColor && pm1Ready_;
  Diagnostics::event(
      "HARDWARE_READY", ready ? "READY" : "ERROR_BOARD_PM1_OR_RGB_POWER");
  return ready;
}

void PaperColorBoardSupport::playTone(uint16_t frequency, uint32_t duration) {
  if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
  M5.Speaker.setVolume(72);
  M5.Speaker.tone(frequency, duration);
}

bool PaperColorBoardSupport::prepareSdCard() {
  if (!pm1Ready_) return false;
  // Official PaperColor user-demo PM1 setup: GPIO3 powers TF, GPIO4 enables
  // card detection, and GPIO1 is the active-low card-detect input.
  pm1_.pinMode(M5PM1_GPIO_NUM_3, OUTPUT);
  pm1_.digitalWrite(M5PM1_GPIO_NUM_3, HIGH);
  pm1_.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);
  pm1_.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);
  pm1_.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);
  sdPowerReady_ = true;
  delay(20);
  Diagnostics::event("SD_POWER", "READY");
  return true;
}

bool PaperColorBoardSupport::sdCardInserted() {
  return sdPowerReady_ && pm1_.digitalRead(M5PM1_GPIO_NUM_1) == LOW;
}

}  // namespace inkloop
