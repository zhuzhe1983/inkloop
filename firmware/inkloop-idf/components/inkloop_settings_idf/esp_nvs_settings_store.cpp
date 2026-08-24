#include "inkloop/settings/esp_nvs_settings_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "nvs.h"
#include "psa/crypto.h"

namespace inkloop {
namespace settings {
namespace {

constexpr char kSettingsNamespace[] = "ink-settings-v1";
constexpr char kSettingsMarkerKey[] = "initialized";
constexpr char kSettingsHeadKey[] = "head";
constexpr char kSettingsSlot0Key[] = "slot0";
constexpr char kSettingsSlot1Key[] = "slot1";

constexpr char kLegacyNamespace[] = "ink-portal";
constexpr char kLegacyMarkerKey[] = "initialized";
constexpr char kLegacyHeadKey[] = "head";
constexpr char kLegacySlotAKey[] = "snap-a";
constexpr char kLegacySlotBKey[] = "snap-b";
constexpr char kEarlySettingsNamespace[] = "inkloop-v2";
constexpr char kEarlyLedMapKey[] = "led-map";

SettingsStatus storage(const char* detail) {
  return {SettingsError::Storage, detail};
}

SettingsStatus readBlob(nvs_handle_t handle, const char* key, bool& present,
                        std::vector<std::uint8_t>& output) {
  present = false;
  output.clear();
  std::size_t length = 0U;
  esp_err_t result = nvs_get_blob(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return SettingsStatus::success();
  if (result != ESP_OK || length == 0U ||
      length > kMaximumSettingsRecordBytes)
    return storage("settings slot length invalid");
  output.resize(length);
  result = nvs_get_blob(handle, key, output.data(), &length);
  if (result != ESP_OK || length != output.size()) {
    std::fill(output.begin(), output.end(), 0U);
    output.clear();
    return storage("settings slot read failed");
  }
  present = true;
  return SettingsStatus::success();
}

SettingsStatus readLegacyString(nvs_handle_t handle, const char* key,
                                bool& present, std::string& output) {
  present = false;
  output.clear();
  std::size_t length = 0U;
  esp_err_t result = nvs_get_str(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return SettingsStatus::success();
  if (result != ESP_OK || length == 0U ||
      length > kMaximumLegacyPortalRecordBytes + 1U)
    return storage("legacy portal slot length invalid");
  std::vector<char> buffer(length, '\0');
  result = nvs_get_str(handle, key, buffer.data(), &length);
  if (result != ESP_OK || length == 0U || buffer[length - 1U] != '\0') {
    std::fill(buffer.begin(), buffer.end(), '\0');
    return storage("legacy portal slot read failed");
  }
  output.assign(buffer.data(), length - 1U);
  std::fill(buffer.begin(), buffer.end(), '\0');
  present = true;
  return SettingsStatus::success();
}

bool validLowerSha256(const std::string& value) {
  return value.size() == 64U &&
      std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      });
}

}  // namespace

SettingsStatus EspNvsSettingsJournalStore::inspect(
    SettingsJournalState& state) {
  state.clear();
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kSettingsNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) {
    state.namespace_available = true;
    return SettingsStatus::success();
  }
  if (opened != ESP_OK) return storage("settings NVS open failed");
  state.namespace_available = true;

  std::uint8_t marker = 0U;
  esp_err_t result = nvs_get_u8(handle, kSettingsMarkerKey, &marker);
  state.marker_present = result == ESP_OK;
  state.marker_valid = state.marker_present &&
      marker == kSettingsInitializedMarker;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    state.clear();
    return storage("settings marker read failed");
  }

  std::uint32_t head = 0U;
  result = nvs_get_u32(handle, kSettingsHeadKey, &head);
  state.head_present = result == ESP_OK;
  state.head_generation = state.head_present ? head : 0U;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    state.clear();
    return storage("settings head read failed");
  }

  SettingsStatus status = readBlob(
      handle, kSettingsSlot0Key, state.slot_present[0], state.slot[0]);
  if (status.ok())
    status = readBlob(
        handle, kSettingsSlot1Key, state.slot_present[1], state.slot[1]);
  nvs_close(handle);
  if (!status.ok()) state.clear();
  return status;
}

SettingsStatus EspNvsSettingsJournalStore::writeSlotAndCommit(
    std::uint8_t slot, const std::vector<std::uint8_t>& encoded) {
  if (slot > 1U || encoded.empty() ||
      encoded.size() > kMaximumSettingsRecordBytes)
    return {SettingsError::InvalidArgument, "settings slot invalid"};
  nvs_handle_t handle = 0;
  if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storage("settings NVS open failed");
  const char* key = slot == 0U ? kSettingsSlot0Key : kSettingsSlot1Key;
  esp_err_t result = nvs_set_blob(handle, key, encoded.data(), encoded.size());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? SettingsStatus::success()
                          : storage("settings slot write failed");
}

SettingsStatus EspNvsSettingsJournalStore::writeHeadAndMarkerAndCommit(
    std::uint32_t generation) {
  if (generation == 0U)
    return {SettingsError::InvalidArgument, "settings head invalid"};
  nvs_handle_t handle = 0;
  if (nvs_open(kSettingsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storage("settings NVS open failed");
  esp_err_t result = nvs_set_u32(handle, kSettingsHeadKey, generation);
  if (result == ESP_OK)
    result = nvs_set_u8(
        handle, kSettingsMarkerKey, kSettingsInitializedMarker);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? SettingsStatus::success()
                          : storage("settings head write failed");
}

SettingsStatus EspNvsReadOnlyLegacyPortalSource::inspect(
    LegacyPortalJournalState& state) const {
  state.clear();
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kLegacyNamespace, NVS_READONLY, &handle);
  if (opened != ESP_OK && opened != ESP_ERR_NVS_NOT_FOUND)
    return storage("legacy portal NVS open failed");
  state.namespace_available = true;

  if (opened == ESP_OK) {
    std::uint8_t marker = 0U;
    esp_err_t result = nvs_get_u8(handle, kLegacyMarkerKey, &marker);
    state.marker_present = result == ESP_OK;
    state.marker_valid = state.marker_present &&
        marker == kLegacyPortalInitializedMarker;
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      state.clear();
      return storage("legacy portal marker read failed");
    }

    std::uint8_t head = 0U;
    result = nvs_get_u8(handle, kLegacyHeadKey, &head);
    state.head_present = result == ESP_OK;
    state.head = state.head_present ? head : 0U;
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(handle);
      state.clear();
      return storage("legacy portal head read failed");
    }

    SettingsStatus status = readLegacyString(
        handle, kLegacySlotAKey, state.slot_present[0], state.slot[0]);
    if (status.ok())
      status = readLegacyString(
          handle, kLegacySlotBKey, state.slot_present[1], state.slot[1]);
    nvs_close(handle);
    if (!status.ok()) {
      state.clear();
      return status;
    }
  }

  // This compatibility source is deliberately narrower than the retired
  // SettingsStore. It never reads `myai`, `album`, `render-exp`, or `sleep`,
  // so obsolete feature flags cannot disable required native behavior.
  handle = 0;
  const esp_err_t early_opened =
      nvs_open(kEarlySettingsNamespace, NVS_READONLY, &handle);
  if (early_opened == ESP_ERR_NVS_NOT_FOUND)
    return SettingsStatus::success();
  if (early_opened != ESP_OK) {
    state.clear();
    return storage("early LED map NVS open failed");
  }
  std::uint8_t encoded = 0U;
  const esp_err_t map_result = nvs_get_u8(handle, kEarlyLedMapKey, &encoded);
  nvs_close(handle);
  if (map_result != ESP_OK && map_result != ESP_ERR_NVS_NOT_FOUND) {
    state.clear();
    return storage("early LED map read failed");
  }
  state.early_led_map_present = map_result == ESP_OK;
  state.early_led_map = state.early_led_map_present ? encoded : 0U;
  return SettingsStatus::success();
}

bool EspPsaLegacySha256Verifier::matches(
    const std::string& payload,
    const std::string& expected_lower_hex) const {
  if (!validLowerSha256(expected_lower_hex)) return false;
  std::string actual;
  return digest(payload, actual) && actual == expected_lower_hex;
}

bool EspPsaLegacySha256Verifier::digest(
    const std::string& payload, std::string& output_lower_hex) const {
  output_lower_hex.clear();
  std::array<std::uint8_t, 32> digest{};
  std::size_t digest_length = 0U;
  if (psa_crypto_init() != PSA_SUCCESS ||
      psa_hash_compute(
          PSA_ALG_SHA_256,
          reinterpret_cast<const std::uint8_t*>(payload.data()),
          payload.size(), digest.data(), digest.size(), &digest_length) !=
          PSA_SUCCESS || digest_length != digest.size())
    return false;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string actual(64U, '0');
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    actual[index * 2U] = kHex[digest[index] >> 4U];
    actual[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
  }
  output_lower_hex = std::move(actual);
  return true;
}

}  // namespace settings
}  // namespace inkloop
