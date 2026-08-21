#pragma once

#include <stddef.h>

#include <string>

namespace inkloop {
namespace portal {

static const size_t kMinimumLocalManagementPasswordBytes = 8;
static const size_t kMaximumLocalManagementPasswordBytes = 63;
static const size_t kRecommendedLocalManagementPasswordBytes = 12;

// The same value is used by WPA2 and the local HTTP bootstrap. It defaults to
// the password the owner just supplied for the home Wi-Fi network, so accept
// the complete printable-ASCII WPA2 passphrase range instead of imposing a
// second, Inkloop-specific complexity policy. Encoding and HTML/JSON escaping
// are enforced at their respective boundaries.
inline bool validLocalManagementPassword(const std::string& value) {
  if (value.size() < kMinimumLocalManagementPasswordBytes ||
      value.size() > kMaximumLocalManagementPasswordBytes) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch < 0x20 || ch > 0x7e) return false;
  }
  return true;
}

}  // namespace portal
}  // namespace inkloop
