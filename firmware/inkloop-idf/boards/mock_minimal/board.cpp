#include "inkloop/mock_board.hpp"

#include <cstring>

namespace inkloop {
namespace {

constexpr gpio_num_t kNextButton = static_cast<gpio_num_t>(27);
constexpr spi_host_device_t kNoStorageSpiHost =
    static_cast<spi_host_device_t>(-1);
constexpr BoardDescriptor kDescriptor{
    "mock-minimal",
    128,
    296,
    2,
    false,
    false,
    false,
    false,
    0,
    boardButtonMask(BoardButton::Next)};
static_assert(kDescriptor.valid(), "mock board descriptor must remain valid");
constexpr BoardRenderStrategyCatalog kRenderStrategyCatalog{
    {{{"official-quality", "官方高质量"},
      {"classic-six-color", "经典六色抖动"},
      {"solid-clean", "纯色 / 文字"},
      {}}},
    3U};
static_assert(kRenderStrategyCatalog.valid(),
              "mock strategy catalog must remain bounded and valid");

class MockDisplay final : public IBoardDisplay {
 public:
  explicit MockDisplay(MockBoardObservations& observations)
      : observations_(observations) {}

  esp_err_t writeFullFrame(const BoardFrameView& frame) override {
    ++observations_.frame_writes;
    if (sleeping_) return ESP_ERR_INVALID_STATE;
    if (!frame.bytes || frame.width != kDescriptor.width ||
        frame.height != kDescriptor.height ||
        frame.length != kDescriptor.packed4BppFrameBytes() ||
        frame.format != BoardFrameFormat::Palette4Bpp) {
      return ESP_ERR_INVALID_ARG;
    }
    for (size_t at = 0; at < frame.length; ++at) {
      const uint8_t high = static_cast<uint8_t>(frame.bytes[at] >> 4U);
      const uint8_t low = static_cast<uint8_t>(frame.bytes[at] & 0x0fU);
      if (high >= kDescriptor.palette_colors ||
          low >= kDescriptor.palette_colors) {
        return ESP_ERR_INVALID_ARG;
      }
    }
    return ESP_OK;
  }

  esp_err_t sleep() override {
    sleeping_ = true;
    return ESP_OK;
  }

  bool busy() const override { return false; }

  void reset() { sleeping_ = false; }

 private:
  MockBoardObservations& observations_;
  bool sleeping_ = false;
};

class MockRenderer final : public IBoardRenderer {
 public:
  explicit MockRenderer(MockBoardObservations& observations)
      : observations_(observations) {}

  BoardRenderStrategyCatalog renderStrategyCatalog() const override {
    return kRenderStrategyCatalog;
  }

  bool supportsRenderStrategy(std::string_view strategy) const override {
    return kRenderStrategyCatalog.contains(strategy);
  }

  esp_err_t renderRgbFullFrame(
      const BoardRgbFrameView& rgb, std::string_view strategy,
      uint8_t* output, size_t output_bytes) override {
    ++observations_.rgb_frame_renders;
    const size_t row_bytes = static_cast<size_t>(kDescriptor.width) * 3U;
    if (!supportsRenderStrategy(strategy) || !rgb.bytes || !output ||
        rgb.width != kDescriptor.width || rgb.height != kDescriptor.height ||
        rgb.row_bytes != row_bytes ||
        rgb.length != row_bytes * kDescriptor.height ||
        output_bytes != kDescriptor.packed4BppFrameBytes()) {
      return ESP_ERR_INVALID_ARG;
    }
    std::memset(output, 0x11, output_bytes);
    const size_t pixels =
        static_cast<size_t>(kDescriptor.width) * kDescriptor.height;
    for (size_t at = 0; at < pixels; ++at) {
      const size_t offset = at * 3U;
      const uint32_t luminance = 54U * rgb.bytes[offset] +
          183U * rgb.bytes[offset + 1U] + 19U * rgb.bytes[offset + 2U];
      const uint8_t index = luminance < 32768U ? 0U : 1U;
      uint8_t& packed = output[at >> 1U];
      if ((at & 1U) == 0U)
        packed = static_cast<uint8_t>((index << 4U) | (packed & 0x0fU));
      else
        packed = static_cast<uint8_t>((packed & 0xf0U) | index);
    }
    return ESP_OK;
  }

  bool supportsOnboardingFrames() const override { return false; }

  esp_err_t renderProvisioningFrame(
      std::string_view, std::string_view, std::string_view, std::string_view,
      uint8_t*, size_t) override {
    return ESP_ERR_NOT_SUPPORTED;
  }

  esp_err_t renderMyAiPairingFrame(
      std::string_view, std::string_view, uint8_t*, size_t) override {
    return ESP_ERR_NOT_SUPPORTED;
  }

 private:
  MockBoardObservations& observations_;
};

class MockBoardAdapter final : public IBoardAdapter {
 public:
  MockBoardAdapter() : display_(observations_), renderer_(observations_) {}

  const BoardDescriptor& descriptor() const override { return kDescriptor; }

  esp_err_t initialize() override {
    if (initialized_) return ESP_OK;
    ++observations_.initializations;
    initialized_ = true;
    display_.reset();
    return ESP_OK;
  }

  void shutdown() override {
    ++observations_.shutdowns;
    initialized_ = false;
  }

  IBoardDisplay* display() override {
    ++observations_.display_accesses;
    return initialized_ ? &display_ : nullptr;
  }

  IBoardRenderer* renderer() override {
    ++observations_.renderer_accesses;
    return initialized_ ? &renderer_ : nullptr;
  }

  IAudioCodecControl* audioCodec() override {
    ++observations_.audio_codec_accesses;
    return nullptr;
  }

  EspI2sAudioConfig audioConfig() const override { return {}; }

  i2c_master_bus_handle_t internalI2cBus() const override { return nullptr; }

  spi_host_device_t sharedStorageSpiHost() const override {
    return kNoStorageSpiHost;
  }

  gpio_num_t buttonGpio(BoardButton button) const override {
    ++observations_.button_gpio_reads;
    return kDescriptor.supportsButton(button) ? kNextButton : GPIO_NUM_NC;
  }

  bool buttonPressed(BoardButton button) const override {
    ++observations_.button_state_reads;
    return initialized_ && kDescriptor.supportsButton(button) &&
           next_pressed_;
  }

  esp_err_t setRgb(const BoardRgbPixel*, size_t) override {
    ++observations_.rgb_writes;
    return ESP_ERR_NOT_SUPPORTED;
  }

  esp_err_t prepareSdCard() override {
    ++observations_.sd_preparations;
    return ESP_ERR_NOT_SUPPORTED;
  }

  bool sdCardInserted() const override { return false; }

  MockBoardObservations observations() const { return observations_; }

  void reset() {
    initialized_ = false;
    next_pressed_ = false;
    observations_ = {};
    display_.reset();
  }

  void setNextPressed(bool pressed) { next_pressed_ = pressed; }

 private:
  mutable MockBoardObservations observations_{};
  MockDisplay display_;
  MockRenderer renderer_;
  bool initialized_ = false;
  bool next_pressed_ = false;
};

MockBoardAdapter adapter;

}  // namespace

IBoardAdapter& board_adapter() { return adapter; }

MockBoardObservations mock_board_observations() {
  return adapter.observations();
}

void mock_board_reset() { adapter.reset(); }

void mock_board_set_next_pressed(bool pressed) {
  adapter.setNextPressed(pressed);
}

}  // namespace inkloop
