#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace inkloop {

class DisplayController {
 public:
  DisplayController();
  ~DisplayController();

  bool begin();
  bool showStatus(const String& title, const String& detail, const String& value = "", uint16_t accent = BLUE);
  bool showPortalAccess(const String& title, const String& detail,
                        const String& accessCode, uint16_t accent = BLUE);
  bool showSettingsPortal(const String& title, const String& detail,
                          const String& accessPoint, const String& ipAddress,
                          const String& accessCode,
                          uint16_t accent = BLUE);
  bool showWifiSetup(const String& title, const String& detail,
                     const String& accessPoint, uint16_t accent = YELLOW);
  bool showPng(const uint8_t* bytes, size_t length, bool landscape);
  bool busy() const { return busy_.load(std::memory_order_acquire); }

 private:
  bool acquire(const String& operation);
  void release(const String& operation, bool success);
  bool resetCanvas(bool landscape);
  bool showCompactStatus(const String& operation, const String& title,
                         const String& detail, const String& value,
                         size_t maximumCharactersPerLine, uint16_t accent);

  M5Canvas canvas_;
  SemaphoreHandle_t refreshMutex_ = nullptr;
  std::atomic<bool> busy_{false};
};

}  // namespace inkloop
