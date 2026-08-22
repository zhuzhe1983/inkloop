#pragma once

#include "inkloop/board.hpp"

namespace inkloop {

class PaperColorRenderer final : public IBoardRenderer {
 public:
  BoardRenderStrategyCatalog renderStrategyCatalog() const override;
  bool supportsRenderStrategy(std::string_view strategy) const override;
  esp_err_t renderRgbFullFrame(
      const BoardRgbFrameView& rgb, std::string_view strategy,
      uint8_t* output, size_t output_bytes) override;
  bool supportsOnboardingFrames() const override { return true; }
  esp_err_t renderProvisioningFrame(
      std::string_view ssid, std::string_view access_value,
      std::string_view local_host, std::string_view local_ip,
      uint8_t* output, size_t output_bytes) override;
  esp_err_t renderMyAiPairingFrame(
      std::string_view six_digit_code, std::string_view binding_url,
      uint8_t* output, size_t output_bytes) override;
};

}  // namespace inkloop
