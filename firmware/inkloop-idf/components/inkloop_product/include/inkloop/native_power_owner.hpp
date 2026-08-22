#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "inkloop/board.hpp"
#include "inkloop/button_input.hpp"
#include "inkloop/esp_board_wake_capabilities.hpp"
#include "inkloop/esp_deep_sleep_adapter.hpp"
#include "inkloop/esp_wifi_station.hpp"
#include "inkloop/native_display_service.hpp"
#include "inkloop/native_inkloop_service.hpp"
#include "inkloop/native_portal_owner.hpp"
#include "inkloop/native_voice_service.hpp"
#include "inkloop/power_runtime.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/status_led_owner.hpp"

namespace inkloop {

// Product composition for the portable power policy. It executes only from
// the low-priority Portal lane, while cross-lane button activity is copied
// through one critical-section-protected timestamp.
class NativePowerOwner final : public ISleepPreparation,
                               public IWakeRecoverySeam {
 public:
  NativePowerOwner(IBoardAdapter& board, RuntimeSupervisor& supervisor,
                   EspButtonInputOwner& buttons, EspStatusLedOwner& leds,
                   EspWifiStationOwner& wifi, NativeDisplayService& display,
                   NativeVoiceService& voice, NativeInkloopService& inkloop,
                   NativePortalOwner& portal);

  esp_err_t initialize(uint32_t now_ms);
  esp_err_t afterSupervisorStarted(uint32_t now_ms);
  // State-only rollback; button and network resources are released by their
  // owning components in the composition root.
  void shutdown();
  void noteButtonActivity(uint32_t now_ms);
  void tick(uint32_t now_ms);
  bool recovering() const;
  bool deferBackgroundPanel(uint32_t now_ms) const;
  WakeCause bootWakeCause() const { return boot_wake_cause_; }

  PowerSnapshotResult capturePowerInputs(PowerInputs& inputs) override;
  bool settleTasksAndSync() override;
  bool quiesceDisplay() override;
  bool quiesceVoiceAndAudio() override;
  bool quiesceNetwork() override;
  bool restoreAwakeServices() override;

  bool requestAwakeIndicator() override;
  bool restorePeripheralsPreservingPanel() override;
  bool reconnectNetwork() override;
  bool syncMetadataPreservingPanel() override;
  bool allWakeButtonsReleased() override;
  bool rearmButtonInput() override;
  void requestDeviceRestoredPrompt() override;

 private:
  void refreshBlockers(uint32_t now_ms);
  bool postVoiceLed(VoiceLedMode mode);
  uint64_t nextRequestId();
  static bool due(uint32_t now_ms, uint32_t deadline_ms);

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  EspButtonInputOwner& buttons_;
  EspStatusLedOwner& leds_;
  EspWifiStationOwner& wifi_;
  NativeDisplayService& display_;
  NativeVoiceService& voice_;
  NativeInkloopService& inkloop_;
  NativePortalOwner& portal_;
  EspBoardWakeCapabilities wake_capabilities_;
  EspDeepSleepAdapter deep_sleep_;
  SleepPolicy policy_;
  SleepAttemptRuntime attempts_{};
  WakeRecoveryRuntime recovery_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  PowerActivityState activity_{};
  uint64_t request_sequence_ = 0;
  uint64_t next_heartbeat_epoch_ = 0;
  uint32_t capture_now_ms_ = 0;
  uint32_t preserve_panel_until_ms_ = 0;
  WakeCause boot_wake_cause_ = WakeCause::Unknown;
  bool recovery_active_ = false;
  bool network_quiesced_ = false;
  bool initialized_ = false;
};

}  // namespace inkloop
