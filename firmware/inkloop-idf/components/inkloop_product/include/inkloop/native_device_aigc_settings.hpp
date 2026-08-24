#pragma once

#include <cstdint>

#include "inkloop/portal/portal_core.hpp"
#include "inkloop/settings/device_settings.hpp"

namespace inkloop {

// Portable projection shared by NativeDeviceStateOwner and its host tests.
// Persistence still belongs to NativeDeviceStateOwner's SettingsStoreCore;
// this seam performs no validation, I/O, allocation, or alternate storage.
inline void applyNativeDeviceAigcSettingsPatch(
    const portal::PortalSettingsPatch& patch,
    settings::DeviceSettings& next) {
  if (patch.has_image_generation_steps)
    next.aigc_steps = patch.image_generation_steps;
}

inline std::uint8_t nativeDeviceAigcSteps(
    const settings::DeviceSettings& values) {
  return values.aigc_steps;
}

}  // namespace inkloop
