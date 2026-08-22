#include "inkloop/storage/esp_nvs_upgrade_inventory.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "inkloop/myai/CredentialPersistence.h"
#include "inkloop/myai/esp_nvs_credential_store.hpp"
#include "inkloop/storage/sha256.hpp"
#include "nvs.h"

namespace inkloop {
namespace storage {
namespace {

constexpr size_t kMaximumPortalRecordBytes = 12U * 1024U;

enum class NamespaceOpen : uint8_t { Missing, Open, Error };

NamespaceOpen openReadOnly(const char* name, nvs_handle_t& handle) {
  handle = 0;
  const esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
  if (result == ESP_OK) return NamespaceOpen::Open;
  if (result == ESP_ERR_NVS_NOT_FOUND) return NamespaceOpen::Missing;
  return NamespaceOpen::Error;
}

bool getU8(nvs_handle_t handle, const char* key, uint8_t& output,
           bool& present) {
  const esp_err_t result = nvs_get_u8(handle, key, &output);
  present = result == ESP_OK;
  return present || result == ESP_ERR_NVS_NOT_FOUND;
}

bool getU16(nvs_handle_t handle, const char* key, uint16_t& output,
            bool& present) {
  const esp_err_t result = nvs_get_u16(handle, key, &output);
  present = result == ESP_OK;
  return present || result == ESP_ERR_NVS_NOT_FOUND;
}

bool getU32(nvs_handle_t handle, const char* key, uint32_t& output,
            bool& present) {
  const esp_err_t result = nvs_get_u32(handle, key, &output);
  present = result == ESP_OK;
  return present || result == ESP_ERR_NVS_NOT_FOUND;
}

bool getU64(nvs_handle_t handle, const char* key, uint64_t& output,
            bool& present) {
  const esp_err_t result = nvs_get_u64(handle, key, &output);
  present = result == ESP_OK;
  return present || result == ESP_ERR_NVS_NOT_FOUND;
}

bool getString(nvs_handle_t handle, const char* key, size_t maximum,
               std::string& output, bool& present) {
  output.clear();
  present = false;
  size_t length = 0;
  esp_err_t result = nvs_get_str(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || length == 0 || length > maximum + 1U) return false;
  std::vector<char> buffer(length, '\0');
  result = nvs_get_str(handle, key, buffer.data(), &length);
  if (result != ESP_OK || length == 0 || buffer[length - 1U] != '\0')
    return false;
  output.assign(buffer.data(), length - 1U);
  std::fill(buffer.begin(), buffer.end(), '\0');
  present = true;
  return true;
}

bool lowerHex(const std::string& value, size_t exact) {
  if (value.size() != exact) return false;
  for (char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}

bool portalEnvelopeValid(const std::string& encoded) {
  if (encoded.empty() || encoded.size() > kMaximumPortalRecordBytes)
    return false;
  cJSON* envelope = cJSON_ParseWithLength(encoded.data(), encoded.size());
  if (!envelope || !cJSON_IsObject(envelope)) {
    cJSON_Delete(envelope);
    return false;
  }
  const cJSON* payload_item = cJSON_GetObjectItemCaseSensitive(envelope, "payload");
  const cJSON* checksum_item = cJSON_GetObjectItemCaseSensitive(envelope, "sha256");
  const bool shape = cJSON_IsString(payload_item) && payload_item->valuestring &&
                     cJSON_IsString(checksum_item) && checksum_item->valuestring;
  std::string payload = shape ? payload_item->valuestring : "";
  std::string checksum = shape ? checksum_item->valuestring : "";
  cJSON_Delete(envelope);
  if (!shape || payload.empty() || payload.size() > kMaximumPortalRecordBytes ||
      !lowerHex(checksum, 64U)) return false;
  Sha256 digest;
  std::string calculated;
  if (!digest.update(reinterpret_cast<const uint8_t*>(payload.data()),
                     payload.size()) ||
      !digest.finishHex(calculated) || calculated != checksum) return false;
  cJSON* document = cJSON_ParseWithLength(payload.data(), payload.size());
  if (!document || !cJSON_IsObject(document)) {
    cJSON_Delete(document);
    return false;
  }
  const cJSON* schema = cJSON_GetObjectItemCaseSensitive(document, "schema");
  const cJSON* fields = cJSON_GetObjectItemCaseSensitive(document, "fields");
  const cJSON* revision = cJSON_GetObjectItemCaseSensitive(document, "revision");
  const cJSON* onboarding = cJSON_GetObjectItemCaseSensitive(document, "onboarding");
  const cJSON* settings = cJSON_GetObjectItemCaseSensitive(document, "settings");
  const bool valid = cJSON_IsNumber(schema) &&
      (schema->valuedouble == 1.0 || schema->valuedouble == 2.0) &&
      cJSON_IsNumber(fields) && std::isfinite(fields->valuedouble) &&
      fields->valuedouble >= 0.0 &&
      cJSON_IsNumber(revision) && std::isfinite(revision->valuedouble) &&
      revision->valuedouble >= 1.0 && cJSON_IsObject(onboarding) &&
      cJSON_IsObject(settings);
  cJSON_Delete(document);
  payload.assign(payload.size(), '\0');
  checksum.assign(checksum.size(), '\0');
  calculated.assign(calculated.size(), '\0');
  return valid;
}

RecordProbe inspectSettings() {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly("inkloop-v2", handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  if (opened == NamespaceOpen::Error) return RecordProbe::IoError;
  uint16_t schema = 0;
  bool schema_present = false;
  bool ok = getU16(handle, "schema", schema, schema_present);
  const char* bool_keys[] = {"myai", "album", "render-exp", "sleep"};
  for (const char* key : bool_keys) {
    uint8_t value = 0;
    bool present = false;
    ok = ok && getU8(handle, key, value, present) && present && value <= 1U;
  }
  uint8_t led_map = 0;
  bool led_present = false;
  ok = ok && getU8(handle, "led-map", led_map, led_present) && led_present &&
       led_map <= 2U;
  nvs_close(handle);
  if (!ok || !schema_present) return RecordProbe::Invalid;
  if (schema == 1U) return RecordProbe::Recoverable;
  return schema == 2U ? RecordProbe::Valid : RecordProbe::Invalid;
}

RecordProbe inspectInkloopIdentity() {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly("inkloop", handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  if (opened == NamespaceOpen::Error) return RecordProbe::IoError;
  std::string secret;
  std::string device_id;
  bool secret_present = false;
  bool device_present = false;
  uint32_t revision = 0;
  bool revision_present = false;
  const bool ok = getString(handle, "secret", 64U, secret, secret_present) &&
      getString(handle, "device-id", 128U, device_id, device_present) &&
      getU32(handle, "revision", revision, revision_present);
  nvs_close(handle);
  const bool valid = ok && secret_present && lowerHex(secret, 64U) &&
                     revision_present && (!device_present || !device_id.empty());
  secret.assign(secret.size(), '\0');
  device_id.assign(device_id.size(), '\0');
  return valid ? RecordProbe::Valid : RecordProbe::Invalid;
}

RecordProbe inspectMyAi() {
  myai::EspNvsCredentialJournalStore journal;
  myai::CredentialJournalState state;
  myai::Status status = journal.inspect(state);
  if (!status.ok()) {
    state.redact();
    return RecordProbe::IoError;
  }
  const bool any = state.markerPresent || state.headPresent ||
                   state.slotPresent[0] || state.slotPresent[1];
  state.redact();
  if (!any) return RecordProbe::Missing;
  myai::JsonSha256CredentialCodec codec;
  myai::CredentialPersistenceCore persistence(journal, codec);
  myai::CredentialSnapshot snapshot;
  status = persistence.load(snapshot);
  snapshot.redact();
  return status.ok() ? RecordProbe::Valid : RecordProbe::Invalid;
}

RecordProbe inspectPortal() {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly("ink-portal", handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  if (opened == NamespaceOpen::Error) return RecordProbe::IoError;
  uint8_t marker = 0;
  uint8_t head = 0;
  bool marker_present = false;
  bool head_present = false;
  std::string slot_a;
  std::string slot_b;
  bool slot_a_present = false;
  bool slot_b_present = false;
  const bool ok = getU8(handle, "initialized", marker, marker_present) &&
      getU8(handle, "head", head, head_present) &&
      getString(handle, "snap-a", kMaximumPortalRecordBytes, slot_a,
                slot_a_present) &&
      getString(handle, "snap-b", kMaximumPortalRecordBytes, slot_b,
                slot_b_present);
  nvs_close(handle);
  const bool any = marker_present || head_present || slot_a_present ||
                   slot_b_present;
  if (!any) return RecordProbe::Missing;
  if (!ok || (marker_present && marker != 0xA5U) || !head_present ||
      (head != 1U && head != 2U)) return RecordProbe::Invalid;
  const bool selected = head == 1U
      ? (slot_a_present && portalEnvelopeValid(slot_a))
      : (slot_b_present && portalEnvelopeValid(slot_b));
  const bool fallback = head == 1U
      ? (slot_b_present && portalEnvelopeValid(slot_b))
      : (slot_a_present && portalEnvelopeValid(slot_a));
  slot_a.assign(slot_a.size(), '\0');
  slot_b.assign(slot_b.size(), '\0');
  if (selected) return RecordProbe::Valid;
  return fallback ? RecordProbe::Recoverable : RecordProbe::Invalid;
}

RecordProbe inspectAlbumMetadata() {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly("ink-album-meta", handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  if (opened == NamespaceOpen::Error) return RecordProbe::IoError;
  uint8_t head = 0;
  uint64_t a = 0;
  uint64_t b = 0;
  bool head_present = false;
  bool a_present = false;
  bool b_present = false;
  const bool ok = getU8(handle, "head", head, head_present) &&
      getU64(handle, "rev-a", a, a_present) &&
      getU64(handle, "rev-b", b, b_present);
  nvs_close(handle);
  if (!head_present && !a_present && !b_present) return RecordProbe::Missing;
  if (!ok || !head_present || (head != 1U && head != 2U))
    return RecordProbe::Invalid;
  const uint64_t selected = head == 1U ? a : b;
  const uint64_t fallback = head == 1U ? b : a;
  if (selected != 0U) return RecordProbe::Valid;
  return fallback != 0U ? RecordProbe::Recoverable : RecordProbe::Invalid;
}

RecordProbe inspectPairingUi() {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly("ink-pair-ui", handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  if (opened == NamespaceOpen::Error) return RecordProbe::IoError;
  uint8_t scrubbed = 0;
  bool present = false;
  const bool ok = getU8(handle, "scrubbed-v4", scrubbed, present);
  nvs_close(handle);
  if (!present && ok) return RecordProbe::Missing;
  return ok && scrubbed <= 1U ? RecordProbe::Valid : RecordProbe::Invalid;
}

RecordProbe inspectOpaqueSystemNamespace(const char* name) {
  nvs_handle_t handle = 0;
  const NamespaceOpen opened = openReadOnly(name, handle);
  if (opened == NamespaceOpen::Open) nvs_close(handle);
  if (opened == NamespaceOpen::Missing) return RecordProbe::Missing;
  return opened == NamespaceOpen::Open ? RecordProbe::Valid
                                       : RecordProbe::IoError;
}

}  // namespace

std::array<RecordProbe, kProtectedNvsNamespaces.size()>
EspNvsUpgradeInventory::inspect() const {
  return {{
      inspectSettings(), inspectInkloopIdentity(), inspectMyAi(),
      inspectPortal(), inspectAlbumMetadata(), inspectPairingUi(),
      inspectOpaqueSystemNamespace("nvs.net80211"),
      inspectOpaqueSystemNamespace("phy"),
      inspectOpaqueSystemNamespace("cal_data"),
  }};
}

}  // namespace storage
}  // namespace inkloop
