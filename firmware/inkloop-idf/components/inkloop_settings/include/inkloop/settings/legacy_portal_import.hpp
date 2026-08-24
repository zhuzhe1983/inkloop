#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "inkloop/settings/device_settings.hpp"

namespace inkloop {
namespace settings {

struct SettingsSnapshot;

inline constexpr std::size_t kMaximumLegacyPortalRecordBytes = 12288U;
inline constexpr std::uint8_t kLegacyPortalInitializedMarker = 0xA5U;

struct LegacyPortalJournalState {
  bool namespace_available = false;
  bool marker_present = false;
  bool marker_valid = false;
  bool head_present = false;
  std::uint8_t head = 0U;
  std::array<bool, 2> slot_present{{false, false}};
  std::array<std::string, 2> slot;
  // Earliest released firmware stored only a calibrated voice-pixel mapping
  // in `inkloop-v2/led-map`: 0=unknown, 1=voice on logical pixel 0,
  // 2=voice on logical pixel 1. The IDF adapter reads this key only; retired
  // feature flags in that namespace are deliberately outside this contract.
  bool early_led_map_present = false;
  std::uint8_t early_led_map = 0U;

  void clear();
};

class IReadOnlyLegacyPortalSource {
 public:
  virtual ~IReadOnlyLegacyPortalSource() = default;
  virtual SettingsStatus inspect(LegacyPortalJournalState& state) const = 0;
};

class ILegacySha256Verifier {
 public:
  virtual ~ILegacySha256Verifier() = default;
  virtual bool matches(const std::string& payload,
                       const std::string& expected_lower_hex) const = 0;
  virtual bool digest(const std::string& payload,
                      std::string& output_lower_hex) const = 0;
};

enum class LegacyImportState : std::uint8_t {
  Absent = 0,
  Candidate,
};

// A candidate is intentionally not written by this component. Product wiring
// must present/approve it and then call SettingsStoreCore::save explicitly.
// The Arduino namespace remains untouched, enabling rollback to old firmware.
struct LegacySettingsImport {
  LegacyImportState state = LegacyImportState::Absent;
  DeviceSettings values;
  std::uint16_t source_schema = 0U;
  std::uint64_t source_revision = 0U;
  bool used_fallback_slot = false;
  bool used_early_led_map = false;
  // SHA-256 of exactly the selected, verified migration inputs. This is the
  // stable identity consumed by the persistent migration marker; it never
  // contains the local password itself.
  std::string source_fingerprint;
};

SettingsStatus inspectLegacyPortalSettings(
    const IReadOnlyLegacyPortalSource& source,
    const ILegacySha256Verifier& verifier,
    const DeviceSettings& defaults_for_missing_legacy_fields,
    LegacySettingsImport& output);

// Beta27/beta29 could write an unmarked native journal directly from the
// Arduino portal snapshot. That importer copied only the original shared
// fields: it omitted the local password and early LED-role calibration, and
// persisted Arduino refresh enum 1 as the obsolete
// "experimental-six-color" identifier. This matcher recognizes only that
// complete historical projection. It deliberately returns false for an
// early-led-map-only source, a malformed candidate, any generation other than
// the old auto-importer's unique first save (generation one), a native schema-3
// record, or any target field changed by the user. The migration gate remains
// responsible for requiring the matching legacy fingerprint before a write.
bool matchesHistoricalIncompleteImport(
    const SettingsSnapshot& native_target,
    const LegacySettingsImport& verified_legacy_candidate);

}  // namespace settings
}  // namespace inkloop
