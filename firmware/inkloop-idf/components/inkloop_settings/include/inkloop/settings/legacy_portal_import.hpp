#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "inkloop/settings/device_settings.hpp"

namespace inkloop {
namespace settings {

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
};

SettingsStatus inspectLegacyPortalSettings(
    const IReadOnlyLegacyPortalSource& source,
    const ILegacySha256Verifier& verifier,
    const DeviceSettings& defaults_for_missing_legacy_fields,
    LegacySettingsImport& output);

}  // namespace settings
}  // namespace inkloop

