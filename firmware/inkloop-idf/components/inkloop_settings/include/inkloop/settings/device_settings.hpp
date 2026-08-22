#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {
namespace settings {

// End-to-end limits intentionally match the existing Portal contract. MyAI
// accepts up to 2048 bytes, so a value accepted here cannot later be truncated
// or rejected by either the WebUI or MyAI runtime. The 256-byte ASR command
// limit is an input-transport bound, not a persisted-settings bound.
inline constexpr std::size_t kMaximumAssistantPromptBytes = 512U;
inline constexpr std::size_t kMaximumAigcPromptTemplateBytes = 512U;
inline constexpr std::size_t kMaximumNegativePromptBytes = 384U;
inline constexpr std::size_t kMaximumRenderStrategyBytes = 64U;
inline constexpr std::size_t kMaximumLocalManagementPasswordBytes = 63U;
inline constexpr std::size_t kMaximumSettingsRecordBytes = 3072U;

enum class SettingsError : std::uint8_t {
  None = 0,
  InvalidArgument,
  InvalidState,
  Storage,
  Corrupt,
  Conflict,
  Exhausted,
  TooLarge,
};

struct SettingsStatus {
  SettingsError code = SettingsError::None;
  const char* detail = "";

  constexpr bool ok() const { return code == SettingsError::None; }
  static constexpr SettingsStatus success() { return {}; }
};

enum class AssetStoragePreference : std::uint8_t {
  Automatic = 0,
  Internal = 1,
  Removable = 2,
};

// This is deliberately a SKU-neutral value object. Render strategy is an
// adapter-owned stable identifier rather than a PaperColor enum so another
// panel family can publish its own supported strategy catalog.
struct DeviceSettings {
  std::uint8_t volume_percent = 60U;
  std::uint8_t led_maximum_brightness_percent = 60U;
  bool voice_assistance_enabled = true;
  std::string assistant_prompt;
  std::string aigc_prompt_template;
  std::string negative_prompt;
  AssetStoragePreference asset_storage_preference =
      AssetStoragePreference::Automatic;
  std::string default_render_strategy = "official-quality";
  // Empty means "reuse the saved home Wi-Fi password". A non-empty override
  // is applied to both the Settings AP and local Portal on the next boot.
  std::string local_management_password_override;
};

bool operator==(const DeviceSettings& left, const DeviceSettings& right);
inline bool operator!=(const DeviceSettings& left, const DeviceSettings& right) {
  return !(left == right);
}

bool validUtf8Text(const std::string& value, std::size_t maximum_bytes,
                   bool empty_allowed);
bool validRenderStrategyId(const std::string& value);
bool validLocalManagementPasswordOverride(const std::string& value);
bool validDeviceSettings(const DeviceSettings& value);

// Portable conservative defaults. Product/board composition may replace
// these without changing journal or validation behavior.
DeviceSettings makeGenericDeviceDefaults();

// A caller-supplied profile creates useful PaperColor defaults without
// coupling the storage core to M5, C151, a fixed resolution, or a fixed panel.
SettingsStatus makePaperColorDefaults(std::uint16_t width,
                                      std::uint16_t height,
                                      const std::string& panel_description,
                                      DeviceSettings& output);

const char* settingsErrorName(SettingsError value);
const char* assetStoragePreferenceName(AssetStoragePreference value);

}  // namespace settings
}  // namespace inkloop
