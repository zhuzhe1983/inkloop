#pragma once

#include <stddef.h>

#include <string>

namespace inkloop {

struct CompactStatusValueLayout {
  bool valid = false;
  std::string firstLine;
  std::string secondLine;
};

inline CompactStatusValueLayout makeCompactStatusValueLayout(
    const std::string& value, size_t maximumCharactersPerLine) {
  CompactStatusValueLayout layout;
  if (value.empty() || maximumCharactersPerLine == 0 ||
      value.size() > maximumCharactersPerLine * 2U) {
    return layout;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch < 0x21 || ch > 0x7e) return layout;
  }
  const size_t split = value.size() > maximumCharactersPerLine
      ? maximumCharactersPerLine : value.size();
  layout.firstLine = value.substr(0, split);
  layout.secondLine = value.substr(split);
  layout.valid = layout.firstLine + layout.secondLine == value;
  return layout;
}

inline CompactStatusValueLayout layoutPortalAccessCode(
    const std::string& value) {
  // createNonce emits 24 random characters plus a two-character purpose
  // suffix. Two 13-character rows preserve all 26 characters and entropy.
  return makeCompactStatusValueLayout(value, 13);
}

inline CompactStatusValueLayout layoutWifiAccessPoint(
    const std::string& value) {
  return makeCompactStatusValueLayout(value, 16);
}

}  // namespace inkloop
