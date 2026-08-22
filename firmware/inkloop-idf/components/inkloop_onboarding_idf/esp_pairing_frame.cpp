#include "inkloop/onboarding/esp_pairing_frame.hpp"

#include <array>

#include "qrcode.h"

// qrcode 0.2.0 links Nayuki's encoder but does not publish qrcodegen.h as a
// component include. Keep only the stable C ABI declarations needed to force
// the already-confirmed Arduino version-8 composition. Calling the high-level
// esp_qrcode_generate() wrapper is intentionally forbidden because it both
// auto-selects a smaller version and logs the complete URL at INFO.
extern "C" {
enum qrcodegen_Ecc {
  qrcodegen_Ecc_LOW = 0,
  qrcodegen_Ecc_MEDIUM = 1,
  qrcodegen_Ecc_QUARTILE = 2,
  qrcodegen_Ecc_HIGH = 3,
};

enum qrcodegen_Mask {
  qrcodegen_Mask_AUTO = -1,
  qrcodegen_Mask_0 = 0,
  qrcodegen_Mask_1 = 1,
  qrcodegen_Mask_2 = 2,
  qrcodegen_Mask_3 = 3,
  qrcodegen_Mask_4 = 4,
  qrcodegen_Mask_5 = 5,
  qrcodegen_Mask_6 = 6,
  qrcodegen_Mask_7 = 7,
};

bool qrcodegen_encodeText(const char* text, uint8_t temp_buffer[],
                          uint8_t qrcode[], enum qrcodegen_Ecc error_level,
                          int min_version, int max_version,
                          enum qrcodegen_Mask mask, bool boost_error_level);
}

namespace inkloop::onboarding {
namespace {

constexpr int kConfirmedQrVersion = 8;
constexpr size_t kVersionEightBufferBytes = 302;

class EspQrMatrix final : public IQrMatrix {
 public:
  explicit EspQrMatrix(esp_qrcode_handle_t handle) : handle_(handle) {}
  int side() const override { return esp_qrcode_get_size(handle_); }
  bool module(int x, int y) const override {
    return esp_qrcode_get_module(handle_, x, y);
  }

 private:
  esp_qrcode_handle_t handle_ = nullptr;
};

esp_err_t resultToEspError(PairingFrameResult result) {
  switch (result) {
    case PairingFrameResult::Ok:
      return ESP_OK;
    case PairingFrameResult::InvalidPairing:
    case PairingFrameResult::InvalidFrame:
      return ESP_ERR_INVALID_ARG;
    case PairingFrameResult::InvalidMatrix:
      return ESP_ERR_INVALID_RESPONSE;
    case PairingFrameResult::FrameTooSmall:
      return ESP_ERR_NOT_SUPPORTED;
  }
  return ESP_FAIL;
}

}  // namespace

esp_err_t renderEspMyAiPairingFrame4Bpp(
    const char* code, const char* binding_url, const PairingFrameSpec& spec,
    uint8_t* frame, size_t frame_bytes, PairingFrameLayout* layout) {
  if (!code || !binding_url ||
      !validMyAiPairingInputs(code, binding_url)) {
    return ESP_ERR_INVALID_ARG;
  }
  std::array<uint8_t, kVersionEightBufferBytes> temporary{};
  std::array<uint8_t, kVersionEightBufferBytes> qrcode{};
  if (!qrcodegen_encodeText(
          binding_url, temporary.data(), qrcode.data(),
          qrcodegen_Ecc_MEDIUM, kConfirmedQrVersion, kConfirmedQrVersion,
          qrcodegen_Mask_AUTO, true)) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  const EspQrMatrix matrix(qrcode.data());
  return resultToEspError(renderPairingFrame4Bpp(
      spec, code, binding_url, matrix, frame, frame_bytes, layout));
}

}  // namespace inkloop::onboarding
