#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "AppConfig.h"

namespace inkloop {

struct PersistentSettings {
  static constexpr uint16_t kSchemaVersion = 2;

  uint16_t schemaVersion = kSchemaVersion;
  FeatureConfig features{};
  bool ledMappingCalibrated = false;
  uint8_t voiceLedIndex = 0;
};

class SettingsStore {
 public:
  bool begin();
  const PersistentSettings& current() const { return settings_; }
  bool setMyAiEnabled(bool enabled);
  bool setLedMapping(bool calibrated, uint8_t voiceLedIndex);
  bool ready() const { return ready_; }

 private:
  bool writeDefaults(uint8_t encodedLedMap = 0);

  Preferences preferences_;
  PersistentSettings settings_;
  bool ready_ = false;
};

}  // namespace inkloop
