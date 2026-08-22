#include "inkloop/storage/esp_nvs_upgrade_marker_journal.hpp"

#include <cstddef>
#include <cstdint>

#include "nvs.h"

namespace inkloop {
namespace storage {
namespace {

constexpr char kNamespace[] = "ink-migrate-v1";
constexpr char kInitializedKey[] = "initialized";
constexpr char kHeadKey[] = "head";
constexpr char kSlot0Key[] = "slot0";
constexpr char kSlot1Key[] = "slot1";

MigrationJournalStoreCode readSlot(nvs_handle_t handle, const char* key,
                                   RawMigrationJournalSlot& slot) {
  slot = RawMigrationJournalSlot{};
  std::size_t length = 0U;
  esp_err_t result = nvs_get_blob(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return MigrationJournalStoreCode::Ok;
  if (result != ESP_OK) return MigrationJournalStoreCode::IoError;
  slot.present = true;
  slot.length = length;
  if (length != slot.bytes.size()) return MigrationJournalStoreCode::Ok;
  result = nvs_get_blob(handle, key, slot.bytes.data(), &length);
  if (result != ESP_OK || length != slot.bytes.size()) {
    slot = RawMigrationJournalSlot{};
    return MigrationJournalStoreCode::IoError;
  }
  return MigrationJournalStoreCode::Ok;
}

}  // namespace

MigrationJournalStoreCode EspNvsMigrationMarkerJournalStore::inspectRaw(
    RawMigrationMarkerJournal& state) const {
  state = RawMigrationMarkerJournal{};
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) {
    state.namespace_available = true;
    return MigrationJournalStoreCode::Ok;
  }
  if (opened != ESP_OK) return MigrationJournalStoreCode::IoError;
  state.namespace_available = true;

  std::uint8_t initialized = 0U;
  esp_err_t result = nvs_get_u8(handle, kInitializedKey, &initialized);
  state.initialized_present = result == ESP_OK;
  state.initialized = state.initialized_present ? initialized : 0U;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    state = RawMigrationMarkerJournal{};
    return MigrationJournalStoreCode::IoError;
  }

  std::uint64_t head = 0U;
  result = nvs_get_u64(handle, kHeadKey, &head);
  state.head_present = result == ESP_OK;
  state.head_sequence = state.head_present ? head : 0U;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    state = RawMigrationMarkerJournal{};
    return MigrationJournalStoreCode::IoError;
  }

  MigrationJournalStoreCode code =
      readSlot(handle, kSlot0Key, state.slots[0]);
  if (code == MigrationJournalStoreCode::Ok)
    code = readSlot(handle, kSlot1Key, state.slots[1]);
  nvs_close(handle);
  if (code != MigrationJournalStoreCode::Ok)
    state = RawMigrationMarkerJournal{};
  return code;
}

MigrationJournalStoreCode
EspNvsMigrationMarkerJournalStore::writeSlotAndCommit(
    std::uint8_t slot,
    const EncodedMigrationJournalSlot& encoded) {
  if (slot > 1U) return MigrationJournalStoreCode::InvalidArgument;
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return MigrationJournalStoreCode::IoError;
  const char* key = slot == 0U ? kSlot0Key : kSlot1Key;
  esp_err_t result =
      nvs_set_blob(handle, key, encoded.data(), encoded.size());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? MigrationJournalStoreCode::Ok
                          : MigrationJournalStoreCode::IoError;
}

MigrationJournalStoreCode
EspNvsMigrationMarkerJournalStore::writeHeadAndMarkerAndCommit(
    std::uint64_t sequence) {
  if (sequence == 0U) return MigrationJournalStoreCode::InvalidArgument;
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return MigrationJournalStoreCode::IoError;
  esp_err_t result = nvs_set_u64(handle, kHeadKey, sequence);
  if (result == ESP_OK)
    result = nvs_set_u8(handle, kInitializedKey,
                        kMigrationJournalInitializedMarker);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? MigrationJournalStoreCode::Ok
                          : MigrationJournalStoreCode::IoError;
}

}  // namespace storage
}  // namespace inkloop
