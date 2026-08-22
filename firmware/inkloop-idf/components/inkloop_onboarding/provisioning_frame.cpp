#include "inkloop/onboarding/provisioning_frame.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "inkloop/onboarding/font8x8_basic.hpp"

namespace inkloop::onboarding {
namespace {

struct TextLine {
  std::string text;
  uint8_t scale = 1U;
  uint8_t color = 0U;
  uint8_t gap_after = 8U;
};

bool printable(std::string_view value, size_t minimum, size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (const unsigned char character : value) {
    if (character < 0x20U || character > 0x7eU) return false;
  }
  return true;
}

bool setPixel(uint8_t* frame, size_t bytes, uint16_t width, uint16_t height,
              int x, int y, uint8_t color) {
  if (!frame || x < 0 || y < 0 || x >= width || y >= height) return false;
  const size_t pixel = static_cast<size_t>(y) * width + x;
  const size_t at = pixel >> 1U;
  if (at >= bytes) return false;
  if ((pixel & 1U) == 0U)
    frame[at] = static_cast<uint8_t>((color << 4U) | (frame[at] & 0x0fU));
  else
    frame[at] = static_cast<uint8_t>((frame[at] & 0xf0U) | color);
  return true;
}

bool fillRect(uint8_t* frame, size_t bytes, const PairingFrameSpec& spec,
              int x, int y, int width, int height, uint8_t color) {
  if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x > static_cast<int>(spec.width) - width ||
      y > static_cast<int>(spec.height) - height) return false;
  for (int row = 0; row < height; ++row)
    for (int column = 0; column < width; ++column)
      if (!setPixel(frame, bytes, spec.width, spec.height,
                    x + column, y + row, color)) return false;
  return true;
}

bool drawText(uint8_t* frame, size_t bytes, const PairingFrameSpec& spec,
              std::string_view text, int y, int scale, uint8_t color) {
  const int advance = 9 * scale;
  const int width = text.empty() ? 0 :
      static_cast<int>(text.size()) * advance - scale;
  if (scale <= 0 || width > spec.width) return false;
  int x = (static_cast<int>(spec.width) - width) / 2;
  for (const char character : text) {
    const auto* glyph = font8x8Glyph(character);
    if (!glyph) return false;
    for (int row = 0; row < 8; ++row) {
      for (int column = 0; column < 8; ++column) {
        if (((*glyph)[static_cast<size_t>(row)] & (1U << column)) != 0U &&
            !fillRect(frame, bytes, spec, x + column * scale,
                      y + row * scale, scale, scale, color)) return false;
      }
    }
    x += advance;
  }
  return true;
}

bool appendWrapped(std::string_view value, size_t per_line, uint8_t scale,
                   uint8_t color, uint8_t gap,
                   std::vector<TextLine>& lines) {
  if (per_line == 0U) return false;
  for (size_t at = 0; at < value.size(); at += per_line) {
    TextLine line;
    line.text = std::string(value.substr(at, per_line));
    line.scale = scale;
    line.color = color;
    line.gap_after = gap;
    lines.push_back(std::move(line));
  }
  return true;
}

}  // namespace

ProvisioningFrameResult renderProvisioningFrame4Bpp(
    const PairingFrameSpec& spec, std::string_view ssid,
    std::string_view access_value, std::string_view local_host,
    std::string_view local_ip, uint8_t* frame, size_t frame_bytes) {
  if (!printable(ssid, 1U, 32U) ||
      !printable(access_value, 8U, 63U) ||
      !printable(local_host, 1U, 64U) ||
      !printable(local_ip, 1U, 64U)) {
    return ProvisioningFrameResult::InvalidInput;
  }
  const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
  if (!frame || spec.width < 300U || spec.height < 480U ||
      (pixels & 1U) != 0U || frame_bytes != pixels / 2U ||
      spec.black_index > 6U || spec.white_index > 6U ||
      spec.black_index == 4U || spec.white_index == 4U ||
      spec.black_index == spec.white_index) {
    return ProvisioningFrameResult::InvalidFrame;
  }

  const uint8_t white = static_cast<uint8_t>(
      (spec.white_index << 4U) | spec.white_index);
  std::memset(frame, white, frame_bytes);
  const size_t detail_per_line =
      (static_cast<size_t>(spec.width) - 32U) / 18U;
  std::vector<TextLine> lines;
  lines.reserve(16U);
  lines.push_back({"INKLOOP SETTINGS", 2U, spec.black_index, 18U});
  lines.push_back({"CONNECT WI-FI", 1U, 5U, 8U});
  lines.push_back({"SSID", 1U, spec.black_index, 4U});
  appendWrapped(ssid, detail_per_line, 2U, spec.black_index, 6U, lines);
  lines.push_back({"PASSWORD", 1U, spec.black_index, 4U});
  appendWrapped(access_value, detail_per_line, 2U, spec.black_index, 6U,
                lines);
  lines.push_back({"OPEN AFTER CONNECTING", 1U, 5U, 4U});
  appendWrapped(local_host, detail_per_line, 2U, spec.black_index, 4U, lines);
  appendWrapped(local_ip, detail_per_line, 2U, spec.black_index, 4U, lines);

  int content_height = 0;
  for (const TextLine& line : lines)
    content_height += static_cast<int>(line.scale) * 8 + line.gap_after;
  if (content_height > spec.height - 48) {
    return ProvisioningFrameResult::FrameTooSmall;
  }
  int y = (static_cast<int>(spec.height) - content_height) / 2;
  for (const TextLine& line : lines) {
    if (!drawText(frame, frame_bytes, spec, line.text, y, line.scale,
                  line.color)) {
      return ProvisioningFrameResult::InvalidFrame;
    }
    y += static_cast<int>(line.scale) * 8 + line.gap_after;
  }
  // Small non-text accents make the page state recognizable at a glance while
  // preserving all password/SSID pixels as black-on-white text.
  if (!fillRect(frame, frame_bytes, spec, 40, y + 4,
                spec.width - 80, 6, 2U) ||
      !fillRect(frame, frame_bytes, spec, 80, y + 24,
                spec.width - 160, 4, 5U)) {
    return ProvisioningFrameResult::InvalidFrame;
  }
  return ProvisioningFrameResult::Ok;
}

}  // namespace inkloop::onboarding

