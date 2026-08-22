#include "inkloop/cloud/esp_nvs_identity_store.hpp"

#include <array>
#include <cstring>
#include <string>

#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

namespace inkloop {
namespace cloud {
namespace {

constexpr char kArduinoNamespace[] = "inkloop";
constexpr char kDeviceIdKey[] = "device-id";
constexpr char kSecretKey[] = "secret";
constexpr char kRevisionKey[] = "revision";
constexpr size_t kMaximumDeviceIdBytes = 80U;
constexpr size_t kSecretBytes = 64U;

InkloopCloudStatus storageFailure(const char* detail) {
  InkloopCloudStatus result;
  result.code = InkloopCloudCode::Storage;
  result.detail = detail ? detail : "";
  return result;
}

InkloopCloudStatus invalid(const char* detail) {
  InkloopCloudStatus result;
  result.code = InkloopCloudCode::InvalidArgument;
  result.detail = detail ? detail : "";
  return result;
}

bool lowerHexSecret(const std::string& value) {
  if (value.size() != kSecretBytes) return false;
  for (const char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  }
  return true;
}

bool validDeviceId(const std::string& value) {
  if (value.size() < 20U || value.size() > kMaximumDeviceIdBytes) return false;
  for (const char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-')) return false;
  }
  return true;
}

InkloopCloudStatus readString(nvs_handle_t handle, const char* key,
                              size_t maximum, bool& present,
                              std::string& output) {
  present = false;
  output.clear();
  size_t required = 0;
  const esp_err_t measured = nvs_get_str(handle, key, nullptr, &required);
  if (measured == ESP_ERR_NVS_NOT_FOUND) return InkloopCloudStatus::success();
  if (measured != ESP_OK || required == 0U || required > maximum + 1U)
    return storageFailure("legacy_identity_string_invalid");
  std::array<char, kMaximumDeviceIdBytes + 1U> buffer{};
  size_t readable = required;
  if (nvs_get_str(handle, key, buffer.data(), &readable) != ESP_OK ||
      readable != required || buffer[required - 1U] != '\0') {
    return storageFailure("legacy_identity_string_read_failed");
  }
  output.assign(buffer.data(), required - 1U);
  if (output.size() + 1U != required)
    return storageFailure("legacy_identity_string_contains_nul");
  present = true;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus commitResult(esp_err_t result, const char* detail) {
  return result == ESP_OK ? InkloopCloudStatus::success()
                          : storageFailure(detail);
}

}  // namespace

InkloopCloudStatus EspArduinoInkloopIdentityNvs::read(
    ArduinoInkloopIdentityRecord& record) {
  record = ArduinoInkloopIdentityRecord();
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kArduinoNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return InkloopCloudStatus::success();
  if (opened != ESP_OK) return storageFailure("legacy_identity_nvs_open_failed");

  InkloopCloudStatus status = readString(
      handle, kDeviceIdKey, kMaximumDeviceIdBytes,
      record.device_id_present, record.device_id);
  if (status.ok()) {
    status = readString(handle, kSecretKey, kSecretBytes,
                        record.secret_present, record.secret);
  }
  if (status.ok()) {
    const esp_err_t revision = nvs_get_u32(handle, kRevisionKey,
                                            &record.revision);
    if (revision == ESP_OK) {
      record.revision_present = true;
    } else if (revision != ESP_ERR_NVS_NOT_FOUND) {
      status = storageFailure("legacy_identity_revision_read_failed");
    }
  }
  nvs_close(handle);
  if (!status.ok()) record = ArduinoInkloopIdentityRecord();
  return status;
}

InkloopCloudStatus EspArduinoInkloopIdentityNvs::commitFreshIdentity(
    const std::string& secret) {
  if (!lowerHexSecret(secret)) return invalid("fresh_secret_invalid");
  nvs_handle_t handle = 0;
  if (nvs_open(kArduinoNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storageFailure("legacy_identity_nvs_open_failed");
  // One NVS commit mirrors the Arduino reset semantics without namespace/key
  // erase: old device binding, revision, and secret remain durable if it fails.
  esp_err_t result = nvs_set_str(handle, kDeviceIdKey, "");
  if (result == ESP_OK) result = nvs_set_u32(handle, kRevisionKey, 0U);
  if (result == ESP_OK)
    result = nvs_set_str(handle, kSecretKey, secret.c_str());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return commitResult(result, "fresh_identity_commit_failed");
}

InkloopCloudStatus EspArduinoInkloopIdentityNvs::commitDeviceId(
    const std::string& device_id) {
  if (!validDeviceId(device_id)) return invalid("device_id_invalid");
  nvs_handle_t handle = 0;
  if (nvs_open(kArduinoNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storageFailure("legacy_identity_nvs_open_failed");
  esp_err_t result = nvs_set_str(handle, kDeviceIdKey, device_id.c_str());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return commitResult(result, "device_id_commit_failed");
}

InkloopCloudStatus EspArduinoInkloopIdentityNvs::commitRevision(
    uint32_t revision) {
  nvs_handle_t handle = 0;
  if (nvs_open(kArduinoNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storageFailure("legacy_identity_nvs_open_failed");
  esp_err_t result = nvs_set_u32(handle, kRevisionKey, revision);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return commitResult(result, "revision_commit_failed");
}

bool EspArduinoInkloopIdentityPlatform::readLegacyEfuseMac(uint64_t& mac) {
  mac = 0;
  return esp_efuse_mac_get_default(reinterpret_cast<uint8_t*>(&mac)) == ESP_OK;
}

bool EspArduinoInkloopIdentityPlatform::fillRandom(uint8_t* bytes,
                                                   size_t length) {
  if (!bytes || length == 0U) return false;
  esp_fill_random(bytes, length);
  return true;
}

EspNvsInkloopIdentityStore::EspNvsInkloopIdentityStore()
    : store_(nvs_, platform_) {}

InkloopCloudStatus EspNvsInkloopIdentityStore::loadOrCreate(
    InkloopIdentitySnapshot& snapshot) {
  return store_.loadOrCreate(snapshot);
}

InkloopCloudStatus EspNvsInkloopIdentityStore::saveDeviceId(
    const std::string& device_id) {
  return store_.saveDeviceId(device_id);
}

InkloopCloudStatus EspNvsInkloopIdentityStore::saveAppliedRevision(
    uint32_t revision) {
  return store_.saveAppliedRevision(revision);
}

}  // namespace cloud
}  // namespace inkloop
