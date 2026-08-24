#pragma once

#include "inkloop/settings/legacy_portal_import.hpp"
#include "inkloop/settings/settings_extension_journal.hpp"
#include "inkloop/settings/settings_journal.hpp"

namespace inkloop {
namespace settings {

// Native journal owner. This namespace is independent of the Arduino portal
// namespace so upgrades remain rollback-safe.
class EspNvsSettingsJournalStore final : public ISettingsJournalStore {
 public:
  SettingsStatus inspect(SettingsJournalState& state) override;
  SettingsStatus writeSlotAndCommit(
      std::uint8_t slot,
      const std::vector<std::uint8_t>& encoded) override;
  SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) override;
};

// Rollback-safe extensions use keys unknown to beta27 in the existing settings
// namespace. beta27 reads/writes only its known tuple and never enumerates or
// erases the extension keys. ext-head is the sole extension commit selector.
class EspNvsSettingsExtensionJournalStore final
    : public ISettingsExtensionJournalStore {
 public:
  SettingsStatus inspect(SettingsExtensionJournalState& state) override;
  SettingsStatus writeSlot(
      std::uint8_t slot,
      const std::vector<std::uint8_t>& encoded) override;
  SettingsStatus writeHead(std::uint32_t sequence) override;
};

// Compatibility adapter for released Arduino images. It only opens
// `ink-portal` with NVS_READONLY and exposes bounded raw records for verified
// candidate decoding. It has deliberately no write/erase/repair API.
class EspNvsReadOnlyLegacyPortalSource final
    : public IReadOnlyLegacyPortalSource {
 public:
  SettingsStatus inspect(LegacyPortalJournalState& state) const override;
};

class EspPsaLegacySha256Verifier final : public ILegacySha256Verifier {
 public:
  bool matches(const std::string& payload,
               const std::string& expected_lower_hex) const override;
  bool digest(const std::string& payload,
              std::string& output_lower_hex) const override;
};

}  // namespace settings
}  // namespace inkloop
