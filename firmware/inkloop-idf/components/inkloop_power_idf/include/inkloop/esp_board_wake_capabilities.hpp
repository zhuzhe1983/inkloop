#pragma once

#include "inkloop/board.hpp"
#include "inkloop/esp_deep_sleep_adapter.hpp"

namespace inkloop {

class EspBoardWakeCapabilities final : public IWakePinCapabilities {
 public:
  explicit EspBoardWakeCapabilities(IBoardAdapter& board) : board_(board) {}

  bool supportsWakeButton(WakeButton button) const override;
  int wakePin(WakeButton button) const override;
  bool wakeButtonPressed(WakeButton button) const override;

 private:
  static BoardButton boardButton(WakeButton button);
  IBoardAdapter& board_;
};

}  // namespace inkloop
