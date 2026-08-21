#include "Diagnostics.h"

#include <ArduinoJson.h>
#include <M5Unified.h>

namespace inkloop {

void Diagnostics::event(const char* name, const String& value) {
  Serial.print("INKLOOP_");
  Serial.print(name);
  if (value.length()) {
    Serial.print(':');
    Serial.print(value);
  }
  Serial.println();
}

void Diagnostics::status(const DiagnosticSnapshot& snapshot) {
  JsonDocument status;
  status["firmware"] = snapshot.buildVersion;
  status["protocolFirmware"] = snapshot.protocolVersion;
  status["hardwareId"] = snapshot.hardwareId;
  status["board"] = snapshot.board;
  status["pm1"] = snapshot.pm1Ready;
  status["wifi"] = snapshot.wifiConnected;
  status["ip"] = snapshot.ip;
  status["deviceId"] = snapshot.deviceId;
  status["paired"] = snapshot.paired;
  status["pairingCode"] = snapshot.pairingCode;
  status["revision"] = snapshot.revision;
  status["displayBusy"] = snapshot.displayBusy;
  status["littleFs"] = snapshot.littleFsReady;
  status["bootPhase"] = snapshot.bootPhase;
  status["wifiProvisioning"] = snapshot.wifiProvisioning;
  status["wifiPortalActive"] = snapshot.wifiPortalActive;
  status["internalMounted"] = snapshot.internalMounted;
  status["internalRecoveryRequired"] = snapshot.internalRecoveryRequired;
  status["taskStoreReady"] = snapshot.taskStoreReady;
  status["assetBackend"] = snapshot.assetBackend;
  status["dataPreserved"] = snapshot.dataPreserved;
  status["ledCount"] = snapshot.ledCount;
  status["ledMappingCalibrated"] = snapshot.ledMappingCalibrated;
  status["voiceLedIndex"] = snapshot.voiceLedIndex;
  status["freeHeap"] = ESP.getFreeHeap();
  status["freePsram"] = ESP.getFreePsram();
  String payload;
  serializeJson(status, payload);
  event("STATUS", payload);
}

}  // namespace inkloop
