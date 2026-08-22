#include "inkloop/cloud/arduino_compatible_identity_store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace inkloop {
namespace cloud {
namespace {

InkloopCloudStatus failure(InkloopCloudCode code, const char* detail) {
  InkloopCloudStatus result;
  result.code = code;
  result.detail = detail ? detail : "";
  return result;
}

bool lowerHex(const std::string& value, size_t exact) {
  if (value.size() != exact) return false;
  for (const char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  }
  return true;
}

bool validDeviceId(const std::string& value) {
  if (value.empty() || value.size() < 20U || value.size() > 80U) return false;
  for (const char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-')) return false;
  }
  return true;
}

std::string hexSecret(const std::array<uint8_t, 32>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(64U, '0');
  for (size_t index = 0; index < bytes.size(); ++index) {
    output[index * 2U] = kHex[bytes[index] >> 4U];
    output[index * 2U + 1U] = kHex[bytes[index] & 0x0fU];
  }
  return output;
}

std::string legacyHardwareId(uint64_t mac) {
  char output[24]{};
  const int length = std::snprintf(
      output, sizeof(output), "M5PC-%012llX",
      static_cast<unsigned long long>(mac & 0xffffffffffffULL));
  if (length != 17) return {};
  return output;
}

}  // namespace

ArduinoCompatibleInkloopIdentityStore::ArduinoCompatibleInkloopIdentityStore(
    IArduinoInkloopIdentityNvs& nvs,
    IArduinoInkloopIdentityPlatform& platform)
    : nvs_(nvs), platform_(platform) {}

InkloopCloudStatus ArduinoCompatibleInkloopIdentityStore::loadOrCreate(
    InkloopIdentitySnapshot& snapshot) {
  snapshot = InkloopIdentitySnapshot();
  uint64_t mac = 0;
  if (!platform_.readLegacyEfuseMac(mac))
    return failure(InkloopCloudCode::Storage, "efuse_mac_unavailable");
  snapshot.hardware_id = legacyHardwareId(mac);
  if (snapshot.hardware_id.empty())
    return failure(InkloopCloudCode::Storage, "hardware_id_encode_failed");

  ArduinoInkloopIdentityRecord record;
  InkloopCloudStatus status = nvs_.read(record);
  if (!status.ok()) {
    snapshot = InkloopIdentitySnapshot();
    return status;
  }

  const bool secret_missing = !record.secret_present || record.secret.empty();
  if (!secret_missing) {
    if (!lowerHex(record.secret, 64U) ||
        (record.device_id_present && !record.device_id.empty() &&
         !validDeviceId(record.device_id))) {
      snapshot = InkloopIdentitySnapshot();
      return failure(InkloopCloudCode::Storage, "legacy_identity_invalid");
    }
    snapshot.device_id = record.device_id_present ? record.device_id : "";
    snapshot.secret = record.secret;
    snapshot.applied_revision = record.revision_present ? record.revision : 0U;
    return InkloopCloudStatus::success();
  }

  std::array<uint8_t, 32> random{};
  if (!platform_.fillRandom(random.data(), random.size())) {
    snapshot = InkloopIdentitySnapshot();
    return failure(InkloopCloudCode::Storage, "secret_entropy_unavailable");
  }
  const std::string secret = hexSecret(random);
  std::fill(random.begin(), random.end(), 0U);
  status = nvs_.commitFreshIdentity(secret);
  if (!status.ok()) {
    snapshot = InkloopIdentitySnapshot();
    return status;
  }
  // Matches the released Arduino recovery behavior without erasing NVS: a new
  // secret invalidates the prior server binding and restarts its revision.
  snapshot.hardware_id = legacyHardwareId(mac);
  snapshot.secret = secret;
  snapshot.device_id.clear();
  snapshot.applied_revision = 0;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus ArduinoCompatibleInkloopIdentityStore::saveDeviceId(
    const std::string& device_id) {
  if (!validDeviceId(device_id))
    return failure(InkloopCloudCode::InvalidArgument, "device_id_invalid");
  return nvs_.commitDeviceId(device_id);
}

InkloopCloudStatus ArduinoCompatibleInkloopIdentityStore::saveAppliedRevision(
    uint32_t revision) {
  return nvs_.commitRevision(revision);
}

}  // namespace cloud
}  // namespace inkloop
