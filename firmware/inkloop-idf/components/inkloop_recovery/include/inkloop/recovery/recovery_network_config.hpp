#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "inkloop/recovery/recovery_portal.hpp"

namespace inkloop {
namespace recovery {

inline constexpr uint16_t kRecoveryHttpPort = 8080U;
inline constexpr char kRecoveryMdnsHost[] = "inkloop.local";

struct RecoveryEndpointGuidance {
  std::array<char, 64> mdns_url{};
  std::array<char, 64> local_ip_url{};
};

bool validRecoveryLocalIpv4(std::string_view ipv4);

// Builds an exact two-host allow-list: inkloop.local:<port> and the actual
// station/AP IPv4:<port>. It never accepts wildcard or public addresses.
bool buildRecoveryAccessConfig(
    const std::array<char, 64>& local_access_code, std::string_view actual_ipv4,
    const std::string& session_token, const std::string& csrf_token,
    RecoveryAccessConfig& output, RecoveryEndpointGuidance& guidance);

}  // namespace recovery
}  // namespace inkloop
