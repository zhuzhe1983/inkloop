#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "inkloop/onboarding/pairing_frame.hpp"

namespace inkloop::onboarding {

enum class ProvisioningFrameResult : uint8_t {
  Ok,
  InvalidInput,
  InvalidFrame,
  FrameTooSmall,
};

// Settings AP information is deliberately rendered as text, never as a QR.
// The access value is shown exactly because it may be a user-supplied Wi-Fi
// password with case-sensitive punctuation.
ProvisioningFrameResult renderProvisioningFrame4Bpp(
    const PairingFrameSpec& spec, std::string_view ssid,
    std::string_view access_value, std::string_view local_host,
    std::string_view local_ip, uint8_t* frame, size_t frame_bytes);

}  // namespace inkloop::onboarding

