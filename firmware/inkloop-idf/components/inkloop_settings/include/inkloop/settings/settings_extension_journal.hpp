#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "inkloop/settings/settings_journal.hpp"

namespace inkloop {
namespace settings {

inline constexpr std::uint8_t kSettingsExtensionRecordSchema = 1U;
inline constexpr std::size_t kSettingsExtensionRecordBytes = 20U;

struct SettingsExtensionValues {
  bool led_roles_swapped = false;
  std::uint8_t aigc_steps = kDefaultAigcSteps;
};

bool operator==(const SettingsExtensionValues& left,
                const SettingsExtensionValues& right);
inline bool operator!=(const SettingsExtensionValues& left,
                       const SettingsExtensionValues& right) {
  return !(left == right);
}

struct SettingsExtensionSnapshot {
  std::uint32_t sequence = 0U;
  // Main generation at the time this extension became a candidate. It may be
  // lower than the current main generation after beta27 performed a main-only
  // save, but must never be higher than the currently selected main record.
  std::uint32_t settings_generation = 0U;
  SettingsExtensionValues values;
};

struct SettingsExtensionJournalState {
  bool namespace_available = false;
  bool head_present = false;
  std::uint32_t head_sequence = 0U;
  std::array<bool, 2> slot_present{{false, false}};
  std::array<std::vector<std::uint8_t>, 2> slot;

  void clear();
};

class ISettingsExtensionJournalStore {
 public:
  virtual ~ISettingsExtensionJournalStore() = default;
  virtual SettingsStatus inspect(SettingsExtensionJournalState& state) = 0;
  virtual SettingsStatus writeSlot(
      std::uint8_t slot, const std::vector<std::uint8_t>& encoded) = 0;
  // ext-head is the only commit selector. Implementations must perform one
  // bounded key write; no multi-key/nvs_commit atomicity is assumed.
  virtual SettingsStatus writeHead(std::uint32_t sequence) = 0;
};

SettingsStatus encodeSettingsExtensionRecord(
    const SettingsExtensionSnapshot& snapshot,
    std::vector<std::uint8_t>& output);
SettingsStatus decodeSettingsExtensionRecord(
    const std::vector<std::uint8_t>& input,
    SettingsExtensionSnapshot& output);

class SettingsExtensionStoreCore final {
 public:
  explicit SettingsExtensionStoreCore(
      ISettingsExtensionJournalStore& journal);

  SettingsStatus load(
      std::uint32_t selected_main_generation,
      const SettingsExtensionValues& legacy_fallback,
      SettingsExtensionSnapshot& snapshot);
  SettingsStatus prepare(
      std::uint32_t target_main_generation,
      const SettingsExtensionValues& values,
      SettingsExtensionSnapshot& prepared_snapshot);
  SettingsStatus publish(
      const SettingsExtensionSnapshot& prepared_snapshot,
      SettingsExtensionSnapshot& committed_snapshot);

 private:
  SettingsStatus inspectCommitted(
      SettingsExtensionJournalState& state,
      SettingsExtensionSnapshot& committed,
      bool& missing);

  ISettingsExtensionJournalStore& journal_;
};

SettingsExtensionValues settingsExtensionValues(
    const DeviceSettings& settings);
void applySettingsExtension(const SettingsExtensionSnapshot& extension,
                            DeviceSettings& settings);

SettingsStatus loadRollbackCompatibleSettings(
    SettingsStoreCore& main_store,
    SettingsExtensionStoreCore& extension_store,
    SettingsSnapshot& snapshot,
    SettingsExtensionSnapshot* selected_extension = nullptr);

// User/Portal mutations use semantic change detection:
// - extension-only: prepare + single ext-head write, main generation unchanged;
// - main-only: schema-2 main save, ext-head unchanged;
// - mixed: prepare extension, save/verify schema-2 main, then publish ext-head.
// If the last step fails, authoritative_snapshot is reloaded and may contain
// the new main fields with the old committed extension. The returned failure
// prevents callers from claiming the requested LED/steps were saved.
// Migration alone may force main and/or extension authority writes even when
// values equal their legacy fallbacks; ordinary Product mutations leave both
// force flags false.
SettingsStatus saveRollbackCompatibleSettings(
    SettingsStoreCore& main_store,
    SettingsExtensionStoreCore& extension_store,
    const DeviceSettings& values,
    std::uint32_t expected_generation,
    SettingsSnapshot& authoritative_snapshot,
    bool force_main_write = false,
    bool force_extension_write = false);

}  // namespace settings
}  // namespace inkloop
