#include "inkloop/recovery/recovery_network_config.hpp"

#include <array>
#include <cstdio>
#include <utility>

namespace inkloop {
namespace recovery {
namespace {

bool parseOctet(std::string_view value, size_t& offset, uint8_t& output) {
  if (offset >= value.size()) return false;
  const size_t begin = offset;
  unsigned int parsed = 0U;
  while (offset < value.size() && value[offset] >= '0' &&
         value[offset] <= '9') {
    if (offset - begin >= 3U) return false;
    parsed = parsed * 10U + static_cast<unsigned int>(value[offset] - '0');
    if (parsed > 255U) return false;
    ++offset;
  }
  if (offset == begin || (offset - begin > 1U && value[begin] == '0'))
    return false;
  output = static_cast<uint8_t>(parsed);
  return true;
}

bool tokenText(const std::string& value) {
  if (value.size() < 16U || value.size() > kMaximumRecoveryTokenBytes)
    return false;
  for (const unsigned char ch : value) {
    const bool alpha = (ch >= 'a' && ch <= 'z') ||
                       (ch >= 'A' && ch <= 'Z');
    if (!alpha && !(ch >= '0' && ch <= '9') && ch != '-' && ch != '_')
      return false;
  }
  return true;
}

bool accessText(const std::array<char, 64>& value, std::string& output) {
  size_t length = 0U;
  while (length < value.size() && value[length] != '\0') ++length;
  if (length < 4U || length > kMaximumRecoveryAccessCodeBytes ||
      length == value.size()) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch < 0x20U || ch > 0x7eU) return false;
  }
  output.assign(value.data(), length);
  return true;
}

}  // namespace

bool validRecoveryLocalIpv4(std::string_view ipv4) {
  if (ipv4.empty() || ipv4.size() > 15U) return false;
  std::array<uint8_t, 4> octets{};
  size_t offset = 0U;
  for (size_t index = 0; index < octets.size(); ++index) {
    if (!parseOctet(ipv4, offset, octets[index])) return false;
    if (index + 1U < octets.size()) {
      if (offset >= ipv4.size() || ipv4[offset] != '.') return false;
      ++offset;
    }
  }
  if (offset != ipv4.size()) return false;
  return octets[0] == 10U ||
         (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
         (octets[0] == 192U && octets[1] == 168U) ||
         (octets[0] == 169U && octets[1] == 254U);
}

bool buildRecoveryAccessConfig(
    const std::array<char, 64>& local_access_code, std::string_view actual_ipv4,
    const std::string& session_token, const std::string& csrf_token,
    RecoveryAccessConfig& output, RecoveryEndpointGuidance& guidance) {
  std::string access;
  if (!accessText(local_access_code, access) ||
      !validRecoveryLocalIpv4(actual_ipv4) || !tokenText(session_token) ||
      !tokenText(csrf_token) || session_token == csrf_token) {
    return false;
  }
  const std::string port = std::to_string(kRecoveryHttpPort);
  const std::string ip(actual_ipv4);
  const std::string mdns_host = std::string(kRecoveryMdnsHost) + ":" + port;
  const std::string ip_host = ip + ":" + port;
  const std::string mdns_origin = "http://" + mdns_host;
  const std::string ip_origin = "http://" + ip_host;
  if (mdns_origin.size() + 2U > guidance.mdns_url.size() ||
      ip_origin.size() + 2U > guidance.local_ip_url.size()) {
    return false;
  }

  RecoveryAccessConfig next;
  next.access_code = std::move(access);
  next.session_id = session_token;
  next.csrf_token = csrf_token;
  next.allowed_hosts[0] = mdns_host;
  next.allowed_hosts[1] = ip_host;
  next.allowed_host_count = 2U;
  next.allowed_origins[0] = mdns_origin;
  next.allowed_origins[1] = ip_origin;
  next.allowed_origin_count = 2U;
  next.session_lifetime_seconds = 900U;

  RecoveryEndpointGuidance next_guidance;
  std::snprintf(next_guidance.mdns_url.data(), next_guidance.mdns_url.size(),
                "%s/", mdns_origin.c_str());
  std::snprintf(next_guidance.local_ip_url.data(),
                next_guidance.local_ip_url.size(), "%s/", ip_origin.c_str());
  output = std::move(next);
  guidance = next_guidance;
  return true;
}

}  // namespace recovery
}  // namespace inkloop
