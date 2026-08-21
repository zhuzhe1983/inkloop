#include "DisplayController.h"

#include "CompactStatusLayoutPrimitives.h"
#include "PortalAccessDisplayPrimitives.h"

#include "Diagnostics.h"

namespace inkloop {

DisplayController::DisplayController() : canvas_(&M5.Display) {}

DisplayController::~DisplayController() {
  if (refreshMutex_) vSemaphoreDelete(refreshMutex_);
}

bool DisplayController::begin() {
  if (!refreshMutex_) refreshMutex_ = xSemaphoreCreateMutex();
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  return refreshMutex_ && resetCanvas(true);
}

bool DisplayController::acquire(const String& operation) {
  if (!refreshMutex_ || xSemaphoreTake(refreshMutex_, portMAX_DELAY) != pdTRUE) return false;
  busy_.store(true, std::memory_order_release);
  Diagnostics::event("DISPLAY_BEGIN", operation);
  return true;
}

void DisplayController::release(const String& operation, bool success) {
  busy_.store(false, std::memory_order_release);
  Diagnostics::event(success ? "DISPLAY_READY" : "DISPLAY_FAILED", operation);
  xSemaphoreGive(refreshMutex_);
}

bool DisplayController::resetCanvas(bool landscape) {
  canvas_.deleteSprite();
  M5.Display.setRotation(landscape ? 0 : 3);
  if (!canvas_.createSprite(M5.Display.width(), M5.Display.height())) return false;
  canvas_.fillSprite(WHITE);
  return true;
}

bool DisplayController::showStatus(
  const String& title,
  const String& detail,
  const String& value,
  uint16_t accent
) {
  if (!acquire(title)) return false;
  bool success = resetCanvas(true);
  if (success) {
    const int width = canvas_.width();
    const int height = canvas_.height();
    canvas_.fillRect(0, 0, width, 18, accent);
    canvas_.setTextDatum(middle_center);
    canvas_.setTextColor(BLACK);
    canvas_.setTextFont(4);
    canvas_.setTextSize(1);
    canvas_.drawString(title, width / 2, height / 2 - (value.length() ? 105 : 45));
    if (value.length()) {
      canvas_.setTextFont(7);
      canvas_.setTextSize(1);
      canvas_.drawString(value, width / 2, height / 2 - 15);
    }
    canvas_.setTextFont(2);
    canvas_.setTextSize(1);
    canvas_.setTextColor(BLACK);
    canvas_.drawString(detail, width / 2, height / 2 + (value.length() ? 70 : 25));
    canvas_.setTextColor(accent);
    canvas_.drawString("INKLOOP · M5 PAPERCOLOR", width / 2, height - 38);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    canvas_.pushSprite(0, 0);
  }
  release(title, success);
  return success;
}

bool DisplayController::showCompactStatus(
  const String& operation,
  const String& title,
  const String& detail,
  const String& value,
  size_t maximumCharactersPerLine,
  uint16_t accent
) {
  const CompactStatusValueLayout layout = makeCompactStatusValueLayout(
      std::string(value.c_str(), value.length()), maximumCharactersPerLine);
  if (!layout.valid || !acquire(operation)) return false;
  bool success = resetCanvas(true);
  if (success) {
    const int width = canvas_.width();
    const int height = canvas_.height();
    canvas_.fillRect(0, 0, width, 18, accent);
    canvas_.setTextDatum(middle_center);
    canvas_.setTextColor(BLACK);
    canvas_.setTextFont(4);
    canvas_.setTextSize(1);
    canvas_.drawString(title, width / 2, height / 2 - 112);
    canvas_.setTextFont(4);
    if (layout.secondLine.empty()) {
      canvas_.drawString(layout.firstLine.c_str(), width / 2, height / 2 - 12);
    } else {
      canvas_.drawString(layout.firstLine.c_str(), width / 2, height / 2 - 34);
      canvas_.drawString(layout.secondLine.c_str(), width / 2, height / 2 + 8);
    }
    canvas_.setTextFont(2);
    canvas_.drawString(detail, width / 2, height / 2 + 82);
    canvas_.setTextColor(accent);
    canvas_.drawString("INKLOOP · M5 PAPERCOLOR", width / 2, height - 38);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    canvas_.pushSprite(0, 0);
  }
  release(operation, success);
  return success;
}

bool DisplayController::showPortalAccess(
    const String& title, const String& detail, const String& accessCode,
    uint16_t accent) {
  return showCompactStatus(
      "PORTAL_ACCESS", title, detail, accessCode, 13, accent);
}

bool DisplayController::showSettingsPortal(
    const String& title, const String& detail, const String& accessPoint,
    const String& ipAddress, const String& accessCode, uint16_t accent) {
  const PortalAccessDisplayLayout layout = makePortalAccessDisplayLayout(
      std::string(accessPoint.c_str(), accessPoint.length()),
      std::string(ipAddress.c_str(), ipAddress.length()),
      std::string(accessCode.c_str(), accessCode.length()));
  if (!layout.valid || !acquire("SETTINGS_PORTAL_ACCESS")) return false;
  bool success = resetCanvas(true);
  if (success) {
    const int width = canvas_.width();
    const int height = canvas_.height();
    canvas_.fillRect(0, 0, width, 18, accent);
    canvas_.setTextDatum(middle_center);
    canvas_.setTextColor(BLACK);
    canvas_.setTextFont(4);
    canvas_.drawString(title, width / 2, 32);
    canvas_.setTextFont(2);
    canvas_.drawString(detail, width / 2, 62);
    canvas_.drawString((String("SSID: ") + layout.accessPoint.c_str()), width / 2, 92);
    canvas_.drawString(layout.ipUrl.c_str(), width / 2, 120);
    canvas_.setTextFont(4);
    canvas_.drawString(layout.localUrl.c_str(), width / 2, 154);
    canvas_.setTextFont(2);
    canvas_.drawString("Settings password (defaults to home Wi-Fi)", width / 2, 192);
    const int lineHeight = layout.passwordLines.size() <= 2 ? 36 : 27;
    canvas_.setTextFont(layout.passwordLines.size() <= 2 ? 4 : 2);
    for (size_t index = 0; index < layout.passwordLines.size(); ++index) {
      canvas_.drawString(
          layout.passwordLines[index].c_str(), width / 2,
          225 + static_cast<int>(index) * lineHeight);
    }
    canvas_.setTextFont(2);
    canvas_.drawString(
        "Same password for Settings Wi-Fi and local web login",
        width / 2, 322);
    canvas_.drawString("Not the six-digit MyAI binding code", width / 2, 347);
    canvas_.setTextColor(accent);
    canvas_.drawString("INKLOOP · M5 PAPERCOLOR", width / 2, height - 38);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    canvas_.pushSprite(0, 0);
  }
  release("SETTINGS_PORTAL_ACCESS", success);
  return success;
}

bool DisplayController::showWifiSetup(
    const String& title, const String& detail, const String& accessPoint,
    uint16_t accent) {
  return showCompactStatus(
      "WIFI_ACCESS_POINT", title, detail, accessPoint, 16, accent);
}

bool DisplayController::showPng(const uint8_t* bytes, size_t length, bool landscape) {
  if (!bytes || !length || !acquire("FRAME")) return false;
  bool success = resetCanvas(landscape);
  if (success) {
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    success = canvas_.drawPng(bytes, length, 0, 0);
  }
  if (success) canvas_.pushSprite(0, 0);
  release("FRAME", success);
  return success;
}

}  // namespace inkloop
