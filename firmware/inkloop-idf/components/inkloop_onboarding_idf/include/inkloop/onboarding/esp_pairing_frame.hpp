#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "inkloop/onboarding/pairing_frame.hpp"

namespace inkloop::onboarding {

// Encodes the authoritative MyAI HTTPS binding URL and renders directly into
// a caller-owned native 4-bpp frame. It never accepts a Settings-AP URL or a
// locally invented replacement code.
esp_err_t renderEspMyAiPairingFrame4Bpp(
    const char* six_digit_code, const char* binding_url,
    const PairingFrameSpec& spec, uint8_t* frame, size_t frame_bytes,
    PairingFrameLayout* layout = nullptr);

}  // namespace inkloop::onboarding

