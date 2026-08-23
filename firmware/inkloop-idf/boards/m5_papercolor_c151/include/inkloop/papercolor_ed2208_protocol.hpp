#pragma once

#include <cstddef>
#include <cstdint>

namespace inkloop {

constexpr uint16_t kPaperColorEd2208Width = 400;
constexpr uint16_t kPaperColorEd2208Height = 600;
constexpr size_t kPaperColorEd2208FrameBytes =
    static_cast<size_t>(kPaperColorEd2208Width) *
    kPaperColorEd2208Height / 2U;
// ED2208 native codes are sparse: black, white, yellow, red, blue and green
// use 0, 1, 2, 3, 5 and 6. Codes 4 and 7..15 are not panel colors.
inline constexpr uint16_t kPaperColorEd2208NativePaletteMask = 0x006FU;

struct Ed2208CommandView {
  uint8_t command = 0;
  const uint8_t* data = nullptr;
  size_t length = 0;
};

size_t ed2208InitCommandCount();
Ed2208CommandView ed2208InitCommand(size_t index);
bool ed2208PaletteIndexValid(uint8_t index);
bool ed2208FrameValid(const uint8_t* bytes, size_t length);

}  // namespace inkloop
