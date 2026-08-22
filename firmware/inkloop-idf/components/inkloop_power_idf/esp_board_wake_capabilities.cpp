#include "inkloop/esp_board_wake_capabilities.hpp"

namespace inkloop {

BoardButton EspBoardWakeCapabilities::boardButton(WakeButton button) {
  switch (button) {
    case WakeButton::Voice:
      return BoardButton::Voice;
    case WakeButton::Previous:
      return BoardButton::Previous;
    case WakeButton::Next:
      return BoardButton::Next;
  }
  return BoardButton::Voice;
}

bool EspBoardWakeCapabilities::supportsWakeButton(WakeButton button) const {
  return board_.descriptor().supportsButton(boardButton(button));
}

int EspBoardWakeCapabilities::wakePin(WakeButton button) const {
  if (!supportsWakeButton(button)) return static_cast<int>(GPIO_NUM_NC);
  return static_cast<int>(board_.buttonGpio(boardButton(button)));
}

bool EspBoardWakeCapabilities::wakeButtonPressed(WakeButton button) const {
  if (!supportsWakeButton(button)) return false;
  return board_.buttonPressed(boardButton(button));
}

}  // namespace inkloop
