#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace inkloop::onboarding {

// The QR encoder remains platform-specific. Keeping this tiny matrix contract
// portable lets every future Inkloop board reuse the same validated layout and
// direct-to-panel renderer without depending on Arduino or a display library.
class IQrMatrix {
 public:
  virtual ~IQrMatrix() = default;
  virtual int side() const = 0;
  virtual bool module(int x, int y) const = 0;
};

struct PairingFrameSpec {
  uint16_t width = 400;
  uint16_t height = 600;
  uint8_t black_index = 0;
  uint8_t white_index = 1;
};

struct PairingFrameLayout {
  uint16_t qr_x = 0;
  uint16_t qr_y = 0;
  uint16_t qr_pixels = 0;
  uint16_t qr_scale = 0;
  uint16_t code_x = 0;
  uint16_t code_y = 0;
  uint16_t digit_scale = 0;
};

enum class PairingFrameResult : uint8_t {
  Ok,
  InvalidPairing,
  InvalidMatrix,
  InvalidFrame,
  FrameTooSmall,
};

bool validMyAiPairingInputs(std::string_view six_digit_code,
                            std::string_view binding_url);

// Renders one complete packed 4-bpp frame (high nibble first). Only black and
// white native palette indices are used, so the e-paper driver can perform one
// stable full refresh without PNG decode, dithering, or visible intermediate
// screens. QR and code are horizontally centered; their combined stack is
// vertically centered. For the validated 400x600/version-8 layout this exactly
// preserves qr_y=140 and code_y=404 from the Arduino firmware.
PairingFrameResult renderPairingFrame4Bpp(
    const PairingFrameSpec& spec, std::string_view six_digit_code,
    std::string_view binding_url, const IQrMatrix& matrix, uint8_t* frame,
    size_t frame_bytes, PairingFrameLayout* layout = nullptr);

}  // namespace inkloop::onboarding

