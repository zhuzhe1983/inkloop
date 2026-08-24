#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "inkloop/settings/device_settings.hpp"

namespace inkloop {
namespace settings {

inline constexpr std::uint8_t kSettingsInitializedMarker = 0xB6U;
// The retained beta27 rollback image accepts only schemas 1/2, the voice flag,
// and a zero reserved byte. Every new main-journal write therefore remains
// schema 2. Schema 3 is read-only compatibility for never-released development
// records; LED role swap and AIGC steps live in an independently selected
// sidecar whose committed record may name an older compatible main generation.
inline constexpr std::uint16_t kSettingsRecordSchema = 2U;
inline constexpr std::uint16_t kMaximumReadableSettingsRecordSchema = 3U;

struct SettingsSnapshot {
  std::uint32_t generation = 0U;
  // Observed wire schema of a decoded committed record. Fresh defaults use
  // zero. Encoding always writes schema 2; callers must not use this field to
  // request schema 3.
  std::uint16_t decoded_record_schema = 0U;
  DeviceSettings values;
};

struct SettingsJournalState {
  bool namespace_available = false;
  bool marker_present = false;
  bool marker_valid = false;
  bool head_present = false;
  std::uint32_t head_generation = 0U;
  std::array<bool, 2> slot_present{{false, false}};
  std::array<std::vector<std::uint8_t>, 2> slot;

  void clear();
};

class ISettingsJournalStore {
 public:
  virtual ~ISettingsJournalStore() = default;
  virtual SettingsStatus inspect(SettingsJournalState& state) = 0;
  virtual SettingsStatus writeSlotAndCommit(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) = 0;
  virtual SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) = 0;
};

SettingsStatus encodeSettingsRecord(const SettingsSnapshot& snapshot,
                                    std::vector<std::uint8_t>& output);
SettingsStatus decodeSettingsRecord(const std::vector<std::uint8_t>& input,
                                    SettingsSnapshot& output);

// The inactive slot is written and reread first. The generation head is the
// authority selector; the initialized marker is advisory compatibility state
// written by the same adapter operation, but no multi-key NVS atomicity is
// assumed. After an uncertain selector write, readback decides which complete
// record is authoritative.
class SettingsStoreCore final {
 public:
  SettingsStoreCore(ISettingsJournalStore& journal,
                    const DeviceSettings& fresh_defaults);

  SettingsStatus load(SettingsSnapshot& snapshot);
  SettingsStatus prepare(const DeviceSettings& values,
                         std::uint32_t expected_generation,
                         SettingsSnapshot& prepared_snapshot);
  SettingsStatus commitPrepared(
      const SettingsSnapshot& prepared_snapshot,
      SettingsSnapshot& committed_snapshot);
  SettingsStatus save(const DeviceSettings& values,
                      std::uint32_t expected_generation,
                      SettingsSnapshot& committed_snapshot);

 private:
  SettingsStatus commitPreparedInternal(
      const SettingsSnapshot& prepared_snapshot,
      SettingsSnapshot& committed_snapshot);
  ISettingsJournalStore& journal_;
  DeviceSettings fresh_defaults_;
};

}  // namespace settings
}  // namespace inkloop
