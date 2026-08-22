#include "inkloop/onboarding/pairing_frame.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace inkloop::onboarding {
namespace {

constexpr int kQuietModules = 4;
constexpr int kMaximumQrScale = 4;

constexpr std::array<std::array<uint8_t, 7>, 10> kDigits{{
    {{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {{0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {{0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {{0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}},
    {{0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}},
}};

bool allowedPaletteIndex(uint8_t index) {
  // This component owns a portable packed-nibble frame, not the ED2208 panel
  // contract. A board sink remains responsible for rejecting indices that its
  // controller reserves. Keeping the full nibble range here lets future 4-bpp
  // SKUs map black/white to any native palette entries.
  return index <= 0x0fU;
}

bool validPort(std::string_view port) {
  if (port.empty() || port.size() > 5U) return false;
  uint32_t value = 0;
  for (const char character : port) {
    if (character < '0' || character > '9') return false;
    value = value * 10U + static_cast<uint32_t>(character - '0');
  }
  return value > 0U && value <= 65535U;
}

bool validDnsHost(std::string_view host) {
  if (host.empty() || host.front() == '.' || host.back() == '.') return false;
  size_t label_start = 0;
  while (label_start < host.size()) {
    const size_t label_end = host.find('.', label_start);
    const size_t end = label_end == std::string_view::npos
        ? host.size() : label_end;
    const std::string_view label = host.substr(label_start, end - label_start);
    if (label.empty() || label.size() > 63U || label.front() == '-' ||
        label.back() == '-') {
      return false;
    }
    for (const char character : label) {
      const bool alpha_numeric =
          (character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9');
      if (!alpha_numeric && character != '-') return false;
    }
    if (label_end == std::string_view::npos) break;
    label_start = label_end + 1U;
  }
  return true;
}

bool validAuthority(std::string_view authority) {
  if (authority.empty() || authority.find('@') != std::string_view::npos)
    return false;
  if (authority.front() == '[') {
    const size_t bracket = authority.find(']');
    if (bracket == std::string_view::npos || bracket <= 1U) return false;
    const std::string_view address = authority.substr(1U, bracket - 1U);
    if (address.find(':') == std::string_view::npos) return false;
    for (const char character : address) {
      const bool hexadecimal =
          (character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F');
      if (!hexadecimal && character != ':' && character != '.') return false;
    }
    const std::string_view suffix = authority.substr(bracket + 1U);
    return suffix.empty() ||
        (suffix.front() == ':' && validPort(suffix.substr(1U)));
  }
  const size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) return validDnsHost(authority);
  if (authority.find(':') != colon) return false;
  return validDnsHost(authority.substr(0U, colon)) &&
      validPort(authority.substr(colon + 1U));
}

bool setPixel(uint8_t* frame, size_t frame_bytes, uint16_t width,
              uint16_t height, int x, int y, uint8_t index) {
  if (!frame || x < 0 || y < 0 || x >= width || y >= height) return false;
  const size_t pixel = static_cast<size_t>(y) * width +
      static_cast<size_t>(x);
  const size_t at = pixel >> 1U;
  if (at >= frame_bytes) return false;
  if ((pixel & 1U) == 0U) {
    frame[at] = static_cast<uint8_t>((index << 4U) | (frame[at] & 0x0fU));
  } else {
    frame[at] = static_cast<uint8_t>((frame[at] & 0xf0U) | index);
  }
  return true;
}

bool fillRect(uint8_t* frame, size_t frame_bytes, uint16_t width,
              uint16_t height, int x, int y, int rectangle_width,
              int rectangle_height, uint8_t index) {
  if (rectangle_width <= 0 || rectangle_height <= 0 || x < 0 || y < 0 ||
      x > static_cast<int>(width) - rectangle_width ||
      y > static_cast<int>(height) - rectangle_height) {
    return false;
  }
  for (int row = 0; row < rectangle_height; ++row) {
    for (int column = 0; column < rectangle_width; ++column) {
      if (!setPixel(frame, frame_bytes, width, height, x + column, y + row,
                    index)) {
        return false;
      }
    }
  }
  return true;
}

bool drawDigit(uint8_t* frame, size_t frame_bytes, const PairingFrameSpec& spec,
               char digit, int x, int y, int scale) {
  if (digit < '0' || digit > '9' || scale <= 0) return false;
  const auto& rows = kDigits[static_cast<size_t>(digit - '0')];
  for (int row = 0; row < 7; ++row) {
    for (int column = 0; column < 5; ++column) {
      if ((rows[static_cast<size_t>(row)] & (1U << (4 - column))) != 0U &&
          !fillRect(frame, frame_bytes, spec.width, spec.height,
                    x + column * scale, y + row * scale, scale, scale,
                    spec.black_index)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool validMyAiPairingInputs(std::string_view code,
                            std::string_view binding_url) {
  if (code.size() != 6U || binding_url.size() < 12U ||
      binding_url.size() > 256U ||
      binding_url.substr(0U, 8U) != "https://") {
    return false;
  }
  for (const char character : code) {
    if (character < '0' || character > '9') return false;
  }
  const size_t host_end = binding_url.find_first_of("/?#", 8U);
  const size_t authority_end = host_end == std::string_view::npos
      ? binding_url.size() : host_end;
  if (authority_end <= 8U ||
      !validAuthority(binding_url.substr(8U, authority_end - 8U))) {
    return false;
  }
  for (size_t index = 8U; index < binding_url.size(); ++index) {
    const auto character = static_cast<unsigned char>(binding_url[index]);
    if (character <= 0x20U || character >= 0x7fU || character == '\\' ||
        character == '"') {
      return false;
    }
  }
  return true;
}

PairingFrameResult renderPairingFrame4Bpp(
    const PairingFrameSpec& spec, std::string_view code,
    std::string_view binding_url, const IQrMatrix& matrix, uint8_t* frame,
    size_t frame_bytes, PairingFrameLayout* layout) {
  if (!validMyAiPairingInputs(code, binding_url))
    return PairingFrameResult::InvalidPairing;
  const int qr_side = matrix.side();
  if (qr_side < 21 || qr_side > 177 || ((qr_side - 21) % 4) != 0)
    return PairingFrameResult::InvalidMatrix;
  const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
  if (!frame || spec.width == 0U || spec.height == 0U ||
      (pixels & 1U) != 0U || frame_bytes != pixels / 2U ||
      !allowedPaletteIndex(spec.black_index) ||
      !allowedPaletteIndex(spec.white_index) ||
      spec.black_index == spec.white_index) {
    return PairingFrameResult::InvalidFrame;
  }

  const int horizontal_margin = std::max(16, static_cast<int>(spec.width) / 25);
  const int available_qr_width = static_cast<int>(spec.width) -
      horizontal_margin * 2;
  const int qr_modules = qr_side + kQuietModules * 2;
  const int qr_scale = std::min(kMaximumQrScale,
      available_qr_width / qr_modules);
  const int digit_scale = std::min(8,
      std::min(static_cast<int>(spec.width) / 50,
               static_cast<int>(spec.height) / 75));
  if (qr_scale <= 0 || digit_scale <= 0)
    return PairingFrameResult::FrameTooSmall;

  const int qr_pixels = qr_modules * qr_scale;
  const int code_width = 35 * digit_scale;
  const int code_height = 7 * digit_scale;
  const int gap = digit_scale * 4 + digit_scale / 2;
  const int stack_height = qr_pixels + gap + code_height;
  if (qr_pixels > spec.width || code_width > spec.width ||
      stack_height > spec.height) {
    return PairingFrameResult::FrameTooSmall;
  }
  const int qr_x = (static_cast<int>(spec.width) - qr_pixels) / 2;
  const int qr_y = (static_cast<int>(spec.height) - stack_height) / 2;
  const int code_x = (static_cast<int>(spec.width) - code_width) / 2;
  const int code_y = qr_y + qr_pixels + gap;

  const uint8_t packed_white = static_cast<uint8_t>(
      (spec.white_index << 4U) | spec.white_index);
  std::memset(frame, packed_white, frame_bytes);
  for (int y = 0; y < qr_side; ++y) {
    for (int x = 0; x < qr_side; ++x) {
      if (matrix.module(x, y) &&
          !fillRect(frame, frame_bytes, spec.width, spec.height,
                    qr_x + (x + kQuietModules) * qr_scale,
                    qr_y + (y + kQuietModules) * qr_scale,
                    qr_scale, qr_scale, spec.black_index)) {
        return PairingFrameResult::InvalidFrame;
      }
    }
  }
  for (size_t index = 0; index < code.size(); ++index) {
    if (!drawDigit(frame, frame_bytes, spec, code[index],
                   code_x + static_cast<int>(index) * 6 * digit_scale,
                   code_y, digit_scale)) {
      return PairingFrameResult::InvalidFrame;
    }
  }
  if (layout) {
    layout->qr_x = static_cast<uint16_t>(qr_x);
    layout->qr_y = static_cast<uint16_t>(qr_y);
    layout->qr_pixels = static_cast<uint16_t>(qr_pixels);
    layout->qr_scale = static_cast<uint16_t>(qr_scale);
    layout->code_x = static_cast<uint16_t>(code_x);
    layout->code_y = static_cast<uint16_t>(code_y);
    layout->digit_scale = static_cast<uint16_t>(digit_scale);
  }
  return PairingFrameResult::Ok;
}

}  // namespace inkloop::onboarding
