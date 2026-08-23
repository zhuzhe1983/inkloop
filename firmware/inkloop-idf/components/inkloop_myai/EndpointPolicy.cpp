#include "EndpointPolicy.h"

#include <vector>

namespace inkloop {
namespace myai {
namespace {

std::string lowercase(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  }
  return value;
}

bool asciiControlOrSpace(const std::string& value) {
  for (unsigned char ch : value) {
    if (ch <= 0x20 || ch == 0x7f) return true;
  }
  return false;
}

bool decimalPort(const std::string& value, uint16_t& port) {
  if (value.empty() || value.size() > 5) return false;
  uint32_t parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10U + static_cast<uint32_t>(ch - '0');
  }
  if (parsed == 0 || parsed > 65535U) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}

bool parseIpv4(const std::string& value, std::array<uint8_t, 4>& output) {
  size_t offset = 0;
  for (size_t part = 0; part < output.size(); ++part) {
    if (offset >= value.size()) return false;
    size_t end = value.find('.', offset);
    if (part == output.size() - 1) {
      if (end != std::string::npos) return false;
      end = value.size();
    } else if (end == std::string::npos) {
      return false;
    }
    if (end == offset || end - offset > 3) return false;
    uint32_t octet = 0;
    for (size_t index = offset; index < end; ++index) {
      const char ch = value[index];
      if (ch < '0' || ch > '9') return false;
      octet = octet * 10U + static_cast<uint32_t>(ch - '0');
    }
    if (octet > 255U) return false;
    output[part] = static_cast<uint8_t>(octet);
    offset = end + 1;
  }
  return offset == value.size() + 1;
}

bool hexadecimalGroup(const std::string& value, uint16_t& output) {
  if (value.empty() || value.size() > 4) return false;
  uint32_t parsed = 0;
  for (char ch : value) {
    parsed <<= 4U;
    if (ch >= '0' && ch <= '9') parsed += static_cast<uint32_t>(ch - '0');
    else if (ch >= 'a' && ch <= 'f') parsed += static_cast<uint32_t>(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') parsed += static_cast<uint32_t>(ch - 'A' + 10);
    else return false;
  }
  output = static_cast<uint16_t>(parsed);
  return true;
}

bool parseIpv6Side(const std::string& value, bool finalSide,
                   std::vector<uint16_t>& groups) {
  if (value.empty()) return true;
  size_t offset = 0;
  while (offset <= value.size()) {
    const size_t end = value.find(':', offset);
    const size_t length = (end == std::string::npos ? value.size() : end) - offset;
    if (length == 0) return false;
    const std::string token = value.substr(offset, length);
    if (token.find('.') != std::string::npos) {
      if (!finalSide || end != std::string::npos) return false;
      std::array<uint8_t, 4> ipv4{};
      if (!parseIpv4(token, ipv4)) return false;
      groups.push_back(static_cast<uint16_t>((ipv4[0] << 8U) | ipv4[1]));
      groups.push_back(static_cast<uint16_t>((ipv4[2] << 8U) | ipv4[3]));
    } else {
      uint16_t group = 0;
      if (!hexadecimalGroup(token, group)) return false;
      groups.push_back(group);
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return true;
}

bool parseIpv6(const std::string& value, std::array<uint8_t, 16>& output) {
  if (value.empty() || value.find('%') != std::string::npos) return false;
  const size_t compressed = value.find("::");
  if (compressed != std::string::npos &&
      value.find("::", compressed + 2) != std::string::npos) {
    return false;
  }

  std::vector<uint16_t> left;
  std::vector<uint16_t> right;
  if (compressed == std::string::npos) {
    if (!parseIpv6Side(value, true, left) || left.size() != 8) return false;
  } else {
    if (!parseIpv6Side(value.substr(0, compressed), false, left) ||
        !parseIpv6Side(value.substr(compressed + 2), true, right) ||
        left.size() + right.size() >= 8) {
      return false;
    }
  }

  std::array<uint16_t, 8> groups{};
  for (size_t index = 0; index < left.size(); ++index) groups[index] = left[index];
  for (size_t index = 0; index < right.size(); ++index) {
    groups[groups.size() - right.size() + index] = right[index];
  }
  for (size_t index = 0; index < groups.size(); ++index) {
    output[index * 2] = static_cast<uint8_t>(groups[index] >> 8U);
    output[index * 2 + 1] = static_cast<uint8_t>(groups[index] & 0xffU);
  }
  return true;
}

bool reservedHostname(const std::string& host) {
  static const char* const suffixes[] = {
      "localhost", "local", "lan", "internal", "localdomain",
      "home.arpa", "test", "invalid", "example"};
  for (const char* suffix : suffixes) {
    const std::string value(suffix);
    if (host == value ||
        (host.size() > value.size() &&
         host.compare(host.size() - value.size(), value.size(), value) == 0 &&
         host[host.size() - value.size() - 1] == '.')) {
      return true;
    }
  }
  return false;
}

bool validDnsHostname(const std::string& host) {
  if (host.empty() || host.size() > 253 || reservedHostname(host)) return false;
  size_t offset = 0;
  while (offset < host.size()) {
    const size_t end = host.find('.', offset);
    const size_t length = (end == std::string::npos ? host.size() : end) - offset;
    if (length == 0 || length > 63) return false;
    for (size_t index = offset; index < offset + length; ++index) {
      const char ch = host[index];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) {
        return false;
      }
      if ((index == offset || index == offset + length - 1) && ch == '-') return false;
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return true;
}

}  // namespace

Status EndpointPolicy::parseHttpsUrl(const std::string& url,
                                     HttpsEndpoint& endpoint) {
  return parsePublicUrl(url, false, endpoint);
}

Status EndpointPolicy::parsePublicUrl(const std::string& url,
                                      bool allowPlaintextHttp,
                                      HttpsEndpoint& endpoint) {
  endpoint = HttpsEndpoint();
  size_t authorityOffset = 0;
  if (url.compare(0, 8, "https://") == 0) {
    endpoint.tls = true;
    endpoint.port = 443;
    authorityOffset = 8;
  } else if (allowPlaintextHttp && url.compare(0, 7, "http://") == 0) {
    endpoint.tls = false;
    endpoint.port = 80;
    authorityOffset = 7;
  }
  if (url.empty() || url.size() > kMaximumUrlBytes || asciiControlOrSpace(url) ||
      url.find('#') != std::string::npos || authorityOffset == 0) {
    return Status(ErrorCode::Security, 0, "invalid public HTTP endpoint");
  }
  const size_t authorityEnd = url.find_first_of("/?", authorityOffset);
  const std::string authority = url.substr(
      authorityOffset, authorityEnd == std::string::npos
                           ? std::string::npos
                           : authorityEnd - authorityOffset);
  if (authority.empty() || authority.find('@') != std::string::npos) {
    return Status(ErrorCode::Security, 0, "invalid public HTTP authority");
  }

  std::string host;
  std::string port;
  if (authority[0] == '[') {
    const size_t closing = authority.find(']');
    if (closing == std::string::npos || closing == 1) {
      return Status(ErrorCode::Security, 0, "invalid IPv6 HTTP authority");
    }
    host = authority.substr(1, closing - 1);
    if (closing + 1 < authority.size()) {
      if (authority[closing + 1] != ':') {
        return Status(ErrorCode::Security, 0, "invalid IPv6 HTTP port");
      }
      port = authority.substr(closing + 2);
    }
    std::array<uint8_t, 16> address{};
    if (!parseIpv6(host, address) || !isPublicIpv6(address)) {
      return Status(ErrorCode::Security, 0, "non-public IPv6 endpoint");
    }
  } else {
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
      if (authority.find(':', colon + 1) != std::string::npos) {
        return Status(ErrorCode::Security, 0, "unbracketed IPv6 endpoint");
      }
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    } else {
      host = authority;
    }
    host = lowercase(host);
    if (!host.empty() && host.back() == '.') host.pop_back();
    std::array<uint8_t, 4> address{};
    const bool looksIpv4 = host.find_first_not_of("0123456789.") == std::string::npos;
    if (looksIpv4) {
      if (!parseIpv4(host, address) || !isPublicIpv4(address)) {
        return Status(ErrorCode::Security, 0, "non-public IPv4 endpoint");
      }
    } else if (!validDnsHostname(host)) {
      return Status(ErrorCode::Security, 0, "invalid public HTTP hostname");
    }
  }

  if (!port.empty() && !decimalPort(port, endpoint.port)) {
    return Status(ErrorCode::Security, 0, "invalid public HTTP port");
  }
  endpoint.host = host;
  return Status::success();
}

bool EndpointPolicy::isPublicIpv4(const std::array<uint8_t, 4>& address) {
  const uint8_t a = address[0];
  const uint8_t b = address[1];
  const uint8_t c = address[2];
  if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
  if (a == 100 && (b & 0xc0U) == 0x40U) return false;
  if (a == 169 && b == 254) return false;
  if (a == 172 && b >= 16 && b <= 31) return false;
  if (a == 192 && (b == 168 || (b == 0 && c == 0) ||
                   (b == 0 && c == 2) || (b == 88 && c == 99))) return false;
  if (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) return false;
  if (a == 203 && b == 0 && c == 113) return false;
  return true;
}

bool EndpointPolicy::isPublicIpv6(const std::array<uint8_t, 16>& address) {
  // Only global-unicast 2000::/3 is eligible. This rejects unspecified,
  // loopback, IPv4-mapped, ULA, link-local and multicast in one fail-closed gate.
  if ((address[0] & 0xe0U) != 0x20U) return false;
  if (address[0] == 0x20 && address[1] == 0x01) {
    if (address[2] == 0x0d && address[3] == 0xb8) return false;  // documentation
    if (address[2] == 0x00 && address[3] == 0x02) return false;  // benchmark
    if (address[2] == 0x00 && (address[3] & 0xf0U) == 0x10U) return false;
    if (address[2] == 0x00 && (address[3] & 0xf0U) == 0x20U) return false;
  }
  if (address[0] == 0x20 && address[1] == 0x02) return false;  // 6to4
  return true;
}

}  // namespace myai
}  // namespace inkloop
