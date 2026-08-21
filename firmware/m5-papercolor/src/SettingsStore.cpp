#include "SettingsStore.h"

namespace inkloop {

namespace {
constexpr char kNamespace[] = "inkloop-v2";
constexpr char kSchemaKey[] = "schema";
constexpr char kMyAiKey[] = "myai";
constexpr char kAlbumKey[] = "album";
constexpr char kRenderKey[] = "render-exp";
constexpr char kSleepKey[] = "sleep";
constexpr char kLedMapKey[] = "led-map";
#ifndef INKLOOP_MYAI_FEATURE_DEFAULT
#define INKLOOP_MYAI_FEATURE_DEFAULT 0
#endif
}  // namespace

bool SettingsStore::begin() {
  ready_ = false;
  preferences_.end();
  if (!preferences_.begin(kNamespace, false)) return false;
  const uint16_t schema = preferences_.getUShort(kSchemaKey, 0);
  if (schema != PersistentSettings::kSchemaVersion) {
    const uint8_t previousLedMap = schema == 1 ? preferences_.getUChar(kLedMapKey, 0) : 0;
    ready_ = writeDefaults(previousLedMap <= 2 ? previousLedMap : 0);
    if (ready_) {
      settings_.ledMappingCalibrated = previousLedMap == 1 || previousLedMap == 2;
      settings_.voiceLedIndex = previousLedMap == 2 ? 1 : 0;
    }
    if (!ready_) preferences_.end();
    return ready_;
  }

  settings_.schemaVersion = schema;
  settings_.features.myAiEnabled = preferences_.getBool(kMyAiKey, false);
  settings_.features.albumEnabled = preferences_.getBool(kAlbumKey, true);
  settings_.features.experimentalRenderEnabled = preferences_.getBool(kRenderKey, false);
  settings_.features.deepSleepEnabled = preferences_.getBool(kSleepKey, false);
  const uint8_t encodedLedMap = preferences_.getUChar(kLedMapKey, 0);
  settings_.ledMappingCalibrated = encodedLedMap == 1 || encodedLedMap == 2;
  settings_.voiceLedIndex = encodedLedMap == 2 ? 1 : 0;
  ready_ = true;
  return ready_;
}

bool SettingsStore::writeDefaults(uint8_t encodedLedMap) {
  settings_ = PersistentSettings{};
  settings_.features.myAiEnabled = INKLOOP_MYAI_FEATURE_DEFAULT != 0;
  return preferences_.putBool(kMyAiKey, settings_.features.myAiEnabled) &&
    preferences_.putBool(kAlbumKey, true) &&
    preferences_.putBool(kRenderKey, false) &&
    preferences_.putBool(kSleepKey, false) &&
    preferences_.putUChar(kLedMapKey, encodedLedMap) == sizeof(uint8_t) &&
    preferences_.putUShort(kSchemaKey, settings_.schemaVersion) == sizeof(uint16_t);
}

bool SettingsStore::setMyAiEnabled(bool enabled) {
  if (!ready_ || !preferences_.putBool(kMyAiKey, enabled)) return false;
  settings_.features.myAiEnabled = enabled;
  return true;
}

bool SettingsStore::setLedMapping(bool calibrated, uint8_t voiceLedIndex) {
  if (!ready_) return false;
  const uint8_t normalizedIndex = voiceLedIndex > 0 ? 1 : 0;
  const uint8_t encodedLedMap = calibrated ? normalizedIndex + 1 : 0;
  if (preferences_.putUChar(kLedMapKey, encodedLedMap) != sizeof(uint8_t)) return false;
  settings_.ledMappingCalibrated = calibrated;
  settings_.voiceLedIndex = normalizedIndex;
  return true;
}

}  // namespace inkloop
