#pragma once

#include <Arduino.h>
#include <M5PM1.h>
#include <M5Unified.h>

namespace inkloop {

class PaperColorBoardSupport {
 public:
  bool begin();
  bool prepareSdCard();
  bool sdCardInserted();
  void playTone(uint16_t frequency = 1047, uint32_t duration = 120);
  bool pm1Ready() const { return pm1Ready_; }
  int boardId() const { return static_cast<int>(M5.getBoard()); }

 private:
  M5PM1 pm1_;
  bool pm1Ready_ = false;
  bool sdPowerReady_ = false;
};

}  // namespace inkloop
