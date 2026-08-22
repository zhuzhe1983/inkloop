#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "inkloop/settings/device_settings.hpp"

namespace inkloop {
namespace settings {

inline constexpr std::uint8_t kSettingsInitializedMarker = 0xB6U;
inline constexpr std::uint16_t kSettingsRecordSchema = 2U;

struct SettingsSnapshot {
  std::uint32_t generation = 0U;
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

// Two NVS transactions are intentional: the inactive slot is written and
// reread first, then head+marker are committed together. A reset before phase
// two leaves the previous head authoritative (or a fresh journal on first
// save), and a reset after phase two selects a complete verified record.
class SettingsStoreCore final {
 public:
  SettingsStoreCore(ISettingsJournalStore& journal,
                    const DeviceSettings& fresh_defaults);

  SettingsStatus load(SettingsSnapshot& snapshot);
  SettingsStatus save(const DeviceSettings& values,
                      std::uint32_t expected_generation,
                      SettingsSnapshot& committed_snapshot);

 private:
  ISettingsJournalStore& journal_;
  DeviceSettings fresh_defaults_;
};

}  // namespace settings
}  // namespace inkloop
