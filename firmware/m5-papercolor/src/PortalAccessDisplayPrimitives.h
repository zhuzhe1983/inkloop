#pragma once

#include <stddef.h>

#include <string>
#include <vector>

#include "PortalSecurityPrimitives.h"

namespace inkloop {

struct PortalAccessDisplayLayout {
  bool valid = false;
  std::string accessPoint;
  std::string ipAddress;
  std::string ipUrl;
  std::string localUrl;
  std::vector<std::string> passwordLines;
};

inline bool safePortalLabel(
    const std::string& value, size_t minimum, size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch < 0x21 || ch > 0x7e) return false;
  }
  return true;
}

inline bool safePortalIpv4(const std::string& value) {
  if (value.size() < 7 || value.size() > 15) return false;
  size_t dots = 0;
  for (size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch == '.') ++dots;
    else if (ch < '0' || ch > '9') return false;
  }
  return dots == 3;
}

inline PortalAccessDisplayLayout makePortalAccessDisplayLayout(
    const std::string& accessPoint,
    const std::string& ipAddress,
    const std::string& localManagementPassword) {
  PortalAccessDisplayLayout layout;
  if (!safePortalLabel(accessPoint, 1, 32) || !safePortalIpv4(ipAddress) ||
      !portal::validLocalManagementPassword(localManagementPassword)) {
    return layout;
  }
  // The default firmware orientation is 400x600 (device bottom edge down), so
  // a 21-character line remains comfortably readable and keeps even a full
  // WPA2-length password within three rows.
  static const size_t kCharactersPerLine = 21;
  for (size_t offset = 0; offset < localManagementPassword.size();
       offset += kCharactersPerLine) {
    layout.passwordLines.push_back(localManagementPassword.substr(
        offset, kCharactersPerLine));
  }
  if (layout.passwordLines.empty() || layout.passwordLines.size() > 3) {
    layout.passwordLines.clear();
    return layout;
  }
  std::string reconstructed;
  for (size_t index = 0; index < layout.passwordLines.size(); ++index)
    reconstructed += layout.passwordLines[index];
  layout.accessPoint = accessPoint;
  layout.ipAddress = ipAddress;
  layout.ipUrl = "http://" + ipAddress + "/";
  layout.localUrl = "http://inkloop.local/";
  layout.valid = reconstructed == localManagementPassword;
  return layout;
}

}  // namespace inkloop
