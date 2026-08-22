#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "inkloop/esp_i2s_audio.hpp"

namespace inkloop {

enum class BoardButton : uint8_t {
  Previous,
  Next,
  Voice,
};

constexpr uint8_t boardButtonMask(BoardButton button) {
  return static_cast<uint8_t>(1U << static_cast<uint8_t>(button));
}

struct BoardDescriptor {
  const char* id;
  uint16_t width;
  uint16_t height;
  uint8_t palette_colors;
  bool has_psram;
  bool has_sd;
  bool has_microphone;
  bool has_speaker;
  uint8_t rgb_pixels;
  uint8_t button_mask;

  constexpr bool supportsButton(BoardButton button) const {
    return (button_mask & boardButtonMask(button)) != 0U;
  }

  constexpr size_t packed4BppFrameBytes() const {
    return (static_cast<size_t>(width) * static_cast<size_t>(height) + 1U) /
           2U;
  }
};

struct BoardRgbPixel {
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
};

enum class BoardFrameFormat : uint8_t {
  // Board-native palette indices, two pixels per byte, high nibble first.
  Palette4Bpp,
};

struct BoardFrameView {
  const uint8_t* bytes = nullptr;
  size_t length = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  BoardFrameFormat format = BoardFrameFormat::Palette4Bpp;
};

struct BoardRgbFrameView {
  const uint8_t* bytes = nullptr;
  size_t length = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  size_t row_bytes = 0;
};

inline constexpr size_t kMaximumBoardRenderStrategies = 4U;
inline constexpr std::string_view kOfficialQualityRenderStrategy =
    "official-quality";

struct BoardRenderStrategy {
  std::string_view id;
  std::string_view display_name;
};

// A fixed-size view copied by value keeps the SKU catalog bounded and stable.
// Boards own the IDs and labels; product code only enumerates this catalog.
struct BoardRenderStrategyCatalog {
  std::array<BoardRenderStrategy, kMaximumBoardRenderStrategies> entries{};
  uint8_t count = 0U;

  constexpr bool contains(std::string_view strategy) const {
    if (count > entries.size()) return false;
    for (size_t index = 0; index < count; ++index) {
      if (entries[index].id == strategy) return true;
    }
    return false;
  }

  constexpr bool valid() const {
    if (count == 0U || count > entries.size() ||
        !contains(kOfficialQualityRenderStrategy)) {
      return false;
    }
    for (size_t index = 0; index < entries.size(); ++index) {
      const BoardRenderStrategy& entry = entries[index];
      if (index >= count) {
        if (!entry.id.empty() || !entry.display_name.empty()) return false;
        continue;
      }
      if (entry.id.empty() || entry.id.size() > 32U ||
          entry.display_name.empty() || entry.display_name.size() > 64U) {
        return false;
      }
      bool previous_hyphen = false;
      for (const char ch : entry.id) {
        const bool hyphen = ch == '-';
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              hyphen) ||
            (hyphen && previous_hyphen)) {
          return false;
        }
        previous_hyphen = hyphen;
      }
      if (entry.id.front() == '-' || entry.id.back() == '-') return false;
      for (size_t previous = 0; previous < index; ++previous) {
        if (entries[previous].id == entry.id) return false;
      }
    }
    return true;
  }
};

// SKU-specific image and onboarding policy. Product services only normalize a
// PNG into logical scanline RGB and allocate the board-native frame; palette
// codes, dithering algorithms, controller quirks and onboarding layout remain
// behind the selected boards/<sku> adapter.
class IBoardRenderer {
 public:
  virtual ~IBoardRenderer() = default;
  // Storage and cloud contracts carry stable strategy IDs, but support is a
  // board capability. Product code must ask before spending memory/time on a
  // decode and must never silently substitute a different visual policy.
  virtual BoardRenderStrategyCatalog renderStrategyCatalog() const = 0;
  virtual bool supportsRenderStrategy(std::string_view strategy) const = 0;
  virtual esp_err_t renderRgbFullFrame(
      const BoardRgbFrameView& rgb, std::string_view strategy,
      uint8_t* output, size_t output_bytes) = 0;
  virtual bool supportsOnboardingFrames() const = 0;
  virtual esp_err_t renderProvisioningFrame(
      std::string_view ssid, std::string_view access_value,
      std::string_view local_host, std::string_view local_ip,
      uint8_t* output, size_t output_bytes) = 0;
  virtual esp_err_t renderMyAiPairingFrame(
      std::string_view six_digit_code, std::string_view binding_url,
      uint8_t* output, size_t output_bytes) = 0;
};

class IBoardDisplay {
 public:
  virtual ~IBoardDisplay() = default;
  virtual esp_err_t writeFullFrame(const BoardFrameView& frame) = 0;
  virtual esp_err_t sleep() = 0;
  virtual bool busy() const = 0;
};

// Hardware adapters for future Inkloop SKUs implement this contract. Portable
// components depend on declared capabilities, never board-specific globals.
class IBoardAdapter {
 public:
  virtual ~IBoardAdapter() = default;
  virtual const BoardDescriptor& descriptor() const = 0;
  virtual esp_err_t initialize() = 0;
  virtual void shutdown() = 0;
  virtual IBoardDisplay* display() = 0;
  virtual IBoardRenderer* renderer() = 0;
  virtual IAudioCodecControl* audioCodec() = 0;
  virtual EspI2sAudioConfig audioConfig() const = 0;
  virtual i2c_master_bus_handle_t internalI2cBus() const = 0;
  virtual spi_host_device_t sharedStorageSpiHost() const = 0;
  virtual gpio_num_t buttonGpio(BoardButton button) const = 0;
  virtual bool buttonPressed(BoardButton button) const = 0;
  virtual esp_err_t setRgb(const BoardRgbPixel* pixels, size_t count) = 0;
  virtual esp_err_t prepareSdCard() = 0;
  virtual bool sdCardInserted() const = 0;
};

// Supplied by the selected INKLOOP_BOARD_COMPONENT.
IBoardAdapter& board_adapter();

inline BoardDescriptor board_descriptor() {
  return board_adapter().descriptor();
}

inline esp_err_t board_initialize() {
  return board_adapter().initialize();
}

}  // namespace inkloop
