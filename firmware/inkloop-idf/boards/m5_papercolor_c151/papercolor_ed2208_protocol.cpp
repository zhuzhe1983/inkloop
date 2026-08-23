#include "inkloop/papercolor_ed2208_protocol.hpp"

#include <array>

namespace inkloop {
namespace {

constexpr std::array<uint8_t, 6> kCmdH{{0x49, 0x55, 0x20, 0x08, 0x09,
                                        0x18}};
constexpr std::array<uint8_t, 1> kPsr{{0x3F}};
constexpr std::array<uint8_t, 2> kPwr{{0x5F, 0x69}};
constexpr std::array<uint8_t, 4> kPof{{0x40, 0x1F, 0x1F, 0x2C}};
constexpr std::array<uint8_t, 4> kBtz{{0x6F, 0x1F, 0x1F, 0x22}};
constexpr std::array<uint8_t, 4> kBtst{{0x6F, 0x1F, 0x17, 0x17}};
constexpr std::array<uint8_t, 4> kPfs{{0x03, 0x54, 0x00, 0x44}};
constexpr std::array<uint8_t, 2> kTcon{{0x02, 0x00}};
constexpr std::array<uint8_t, 1> kPll{{0x08}};
constexpr std::array<uint8_t, 1> kCdi{{0x3F}};
constexpr std::array<uint8_t, 1> kPws{{0x2F}};
constexpr std::array<uint8_t, 1> kAgid{{0x01}};

constexpr std::array<Ed2208CommandView, 12> kInit{{
    {0xAA, kCmdH.data(), kCmdH.size()},
    {0x01, kPsr.data(), kPsr.size()},
    {0x00, kPwr.data(), kPwr.size()},
    {0x05, kPof.data(), kPof.size()},
    {0x08, kBtz.data(), kBtz.size()},
    {0x06, kBtst.data(), kBtst.size()},
    {0x03, kPfs.data(), kPfs.size()},
    {0x60, kTcon.data(), kTcon.size()},
    {0x30, kPll.data(), kPll.size()},
    {0x50, kCdi.data(), kCdi.size()},
    {0xE3, kPws.data(), kPws.size()},
    {0x84, kAgid.data(), kAgid.size()},
}};

}  // namespace

size_t ed2208InitCommandCount() { return kInit.size(); }

Ed2208CommandView ed2208InitCommand(size_t index) {
  return index < kInit.size() ? kInit[index] : Ed2208CommandView{};
}

bool ed2208PaletteIndexValid(uint8_t index) {
  return index < 16U &&
         (kPaperColorEd2208NativePaletteMask &
          static_cast<uint16_t>(1U << index)) != 0U;
}

bool ed2208FrameValid(const uint8_t* bytes, size_t length) {
  if (!bytes || length != kPaperColorEd2208FrameBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!ed2208PaletteIndexValid(static_cast<uint8_t>(bytes[i] >> 4U)) ||
        !ed2208PaletteIndexValid(static_cast<uint8_t>(bytes[i] & 0x0FU))) {
      return false;
    }
  }
  return true;
}

}  // namespace inkloop
