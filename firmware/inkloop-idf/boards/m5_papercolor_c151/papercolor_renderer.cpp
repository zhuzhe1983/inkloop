#include "inkloop/papercolor_renderer.hpp"

#include <cstring>
#include "inkloop/onboarding/esp_pairing_frame.hpp"
#include "inkloop/onboarding/provisioning_frame.hpp"
#include "inkloop/papercolor_ed2208_protocol.hpp"
#include "inkloop/render/ImageProcessing.h"

namespace inkloop {
namespace {

constexpr size_t kPixelCount =
    static_cast<size_t>(kPaperColorEd2208Width) * kPaperColorEd2208Height;
constexpr BoardRenderStrategyCatalog kRenderStrategyCatalog{
    {{{"official-quality", "官方高质量"},
      {"classic-six-color", "经典六色抖动"},
      {"reflectance-photo", "照片优化"},
      {"solid-clean", "纯色 / 文字"}}},
    4U};
static_assert(kRenderStrategyCatalog.valid(),
              "PaperColor strategy catalog must remain bounded and valid");

bool parseStrategy(std::string_view value,
                   displaypower::RenderStrategy& output) {
  if (value == "official-quality") {
    output = displaypower::RenderStrategy::OfficialQuality;
    return true;
  }
  if (value == "classic-six-color") {
    output = displaypower::RenderStrategy::ExperimentalSixColor;
    return true;
  }
  if (value == "reflectance-photo") {
    output = displaypower::RenderStrategy::ReflectancePhoto;
    return true;
  }
  if (value == "solid-clean") {
    output = displaypower::RenderStrategy::SolidClean;
    return true;
  }
  return false;
}

class RgbSource final : public displaypower::IPixelSource {
 public:
  explicit RgbSource(const BoardRgbFrameView& view) : view_(view) {}

  uint16_t width() const override { return view_.width; }
  uint16_t height() const override { return view_.height; }

  bool read(displaypower::RgbPixel* pixel) override {
    if (!pixel || !view_.bytes || at_ >= kPixelCount) return false;
    const size_t y = at_ / view_.width;
    const size_t x = at_ % view_.width;
    const size_t offset = y * view_.row_bytes + x * 3U;
    if (offset > view_.length || view_.length - offset < 3U) return false;
    *pixel = displaypower::RgbPixel(
        view_.bytes[offset], view_.bytes[offset + 1U],
        view_.bytes[offset + 2U]);
    ++at_;
    return true;
  }

 private:
  const BoardRgbFrameView& view_;
  size_t at_ = 0;
};

uint8_t panelIndex(const displaypower::RgbPixel& source) {
  const displaypower::RgbPixel color =
      displaypower::nearestPaperColorColor(source);
  if (color.red == 0U && color.green == 0U && color.blue == 0U) return 0U;
  if (color.red == 255U && color.green == 255U && color.blue == 255U)
    return 1U;
  if (color.red == 255U && color.green == 243U && color.blue == 56U)
    return 2U;
  if (color.red == 191U && color.green == 0U && color.blue == 0U) return 3U;
  if (color.red == 100U && color.green == 64U && color.blue == 255U)
    return 5U;
  return 6U;
}

class FrameSink final : public displaypower::IPixelSink {
 public:
  FrameSink(uint8_t* frame, size_t frame_bytes)
      : frame_(frame), frame_bytes_(frame_bytes) {
    if (frame_ && frame_bytes_ == kPaperColorEd2208FrameBytes)
      std::memset(frame_, 0x11, frame_bytes_);
  }

  bool write(const displaypower::RgbPixel& pixel) override {
    if (!frame_ || frame_bytes_ != kPaperColorEd2208FrameBytes ||
        at_ >= kPixelCount) {
      return false;
    }
    const uint8_t index = panelIndex(pixel);
    uint8_t& packed = frame_[at_ >> 1U];
    if ((at_ & 1U) == 0U)
      packed = static_cast<uint8_t>((index << 4U) | (packed & 0x0fU));
    else
      packed = static_cast<uint8_t>((packed & 0xf0U) | index);
    ++at_;
    return true;
  }

  bool finish() override {
    return frame_ && frame_bytes_ == kPaperColorEd2208FrameBytes &&
        at_ == kPixelCount;
  }

 private:
  uint8_t* frame_ = nullptr;
  size_t frame_bytes_ = 0;
  size_t at_ = 0;
};

}  // namespace

BoardRenderStrategyCatalog PaperColorRenderer::renderStrategyCatalog() const {
  return kRenderStrategyCatalog;
}

bool PaperColorRenderer::supportsRenderStrategy(
    std::string_view strategy) const {
  return kRenderStrategyCatalog.contains(strategy);
}

esp_err_t PaperColorRenderer::renderRgbFullFrame(
    const BoardRgbFrameView& rgb, std::string_view strategy,
    uint8_t* output, size_t output_bytes) {
  if (!rgb.bytes || !output || rgb.width != kPaperColorEd2208Width ||
      rgb.height != kPaperColorEd2208Height ||
      rgb.row_bytes != static_cast<size_t>(rgb.width) * 3U ||
      rgb.length != rgb.row_bytes * rgb.height ||
      output_bytes != kPaperColorEd2208FrameBytes || strategy.empty() ||
      strategy.size() > 64U) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!supportsRenderStrategy(strategy)) return ESP_ERR_NOT_SUPPORTED;
  displaypower::RenderStrategy selected;
  if (!parseStrategy(strategy, selected))
    return ESP_ERR_NOT_SUPPORTED;  // Defensive: support check and parser agree.
  RgbSource source(rgb);
  FrameSink sink(output, output_bytes);
  std::string error;
  return displaypower::streamRenderPixels(
             source, sink, selected, &error, nullptr)
             ? ESP_OK
             : ESP_FAIL;
}

esp_err_t PaperColorRenderer::renderProvisioningFrame(
    std::string_view ssid, std::string_view access_value,
    std::string_view local_host, std::string_view local_ip,
    uint8_t* output, size_t output_bytes) {
  const onboarding::ProvisioningFrameResult result =
      onboarding::renderProvisioningFrame4Bpp(
          onboarding::PairingFrameSpec{}, ssid, access_value, local_host,
          local_ip, output, output_bytes);
  switch (result) {
    case onboarding::ProvisioningFrameResult::Ok:
      return ESP_OK;
    case onboarding::ProvisioningFrameResult::InvalidInput:
    case onboarding::ProvisioningFrameResult::InvalidFrame:
    case onboarding::ProvisioningFrameResult::FrameTooSmall:
      return ESP_ERR_INVALID_ARG;
  }
  return ESP_FAIL;
}

esp_err_t PaperColorRenderer::renderMyAiPairingFrame(
    std::string_view six_digit_code, std::string_view binding_url,
    uint8_t* output, size_t output_bytes) {
  if (six_digit_code.size() > 6U || binding_url.size() > 256U)
    return ESP_ERR_INVALID_ARG;
  const std::string code(six_digit_code);
  const std::string url(binding_url);
  return onboarding::renderEspMyAiPairingFrame4Bpp(
      code.c_str(), url.c_str(), onboarding::PairingFrameSpec{}, output,
      output_bytes);
}

}  // namespace inkloop
