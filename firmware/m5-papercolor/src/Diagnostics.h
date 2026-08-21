#pragma once

#include <Arduino.h>

namespace inkloop {

struct DiagnosticSnapshot {
  String buildVersion;
  String protocolVersion;
  String hardwareId;
  int board = 0;
  bool pm1Ready = false;
  bool wifiConnected = false;
  String ip;
  String deviceId;
  bool paired = false;
  String pairingCode;
  uint32_t revision = 0;
  bool displayBusy = false;
  bool littleFsReady = false;
  String bootPhase;
  bool wifiProvisioning = false;
  bool wifiPortalActive = false;
  bool internalMounted = false;
  bool internalRecoveryRequired = false;
  bool taskStoreReady = false;
  String assetBackend;
  bool dataPreserved = true;
  uint8_t ledCount = 0;
  bool ledMappingCalibrated = false;
  uint8_t voiceLedIndex = 0;
};

class Diagnostics {
 public:
  static void event(const char* name, const String& value = "");
  static void status(const DiagnosticSnapshot& snapshot);
};

}  // namespace inkloop
