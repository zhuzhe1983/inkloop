#include "inkloop/diagnostics/diagnostic_detail.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace inkloop::diagnostics {
namespace {

bool asciiControl(unsigned char value) {
  return value < 0x20U || value == 0x7fU;
}

size_t utf8SequenceBytes(const std::string& value, size_t at) {
  if (at >= value.size()) return 0U;
  const unsigned char first = static_cast<unsigned char>(value[at]);
  if (first < 0x80U) return asciiControl(first) ? 0U : 1U;
  size_t bytes = 0U;
  if (first >= 0xc2U && first <= 0xdfU) bytes = 2U;
  else if (first >= 0xe0U && first <= 0xefU) bytes = 3U;
  else if (first >= 0xf0U && first <= 0xf4U) bytes = 4U;
  else return 0U;
  if (bytes > value.size() - at) return 0U;
  for (size_t index = 1U; index < bytes; ++index) {
    if ((static_cast<unsigned char>(value[at + index]) & 0xc0U) != 0x80U)
      return 0U;
  }
  const unsigned char second = static_cast<unsigned char>(value[at + 1U]);
  if ((first == 0xe0U && second < 0xa0U) ||
      (first == 0xedU && second >= 0xa0U) ||
      (first == 0xf0U && second < 0x90U) ||
      (first == 0xf4U && second >= 0x90U)) {
    return 0U;
  }
  return bytes;
}

std::string lowercaseAscii(const std::string& value) {
  std::string output = value;
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(
                       ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
                 });
  return output;
}

bool containsCredentialMaterial(const std::string& lowercase) {
  static constexpr std::string_view kAlwaysSensitive[] = {
      "bearer ", "http://", "https://", "ws://", "wss://", "ags_",
      "api_key", "api-key", "apikey", "set-cookie", "cookie:"};
  for (const std::string_view marker : kAlwaysSensitive) {
    if (lowercase.find(marker) != std::string::npos) return true;
  }

  // Stable phrases such as "authorization rejected" and contract codes such
  // as "invalid_pairing_token" are useful and contain no credential value.
  // Reject a label only when it introduces a value, including JSON syntax.
  static constexpr std::string_view kValueLabels[] = {
      "token", "authorization", "credential", "password", "secret"};
  const auto safeStatusContinuation = [](std::string_view label,
                                         std::string_view suffix) {
    static constexpr std::string_view kTokenStatuses[] = {
        "expired", "invalid", "revoked", "missing", "unavailable",
        "required"};
    static constexpr std::string_view kAuthorizationStatuses[] = {
        "rejected", "failed", "denied", "required", "unavailable"};
    const auto matches = [suffix](std::string_view status) {
      return suffix.size() >= status.size() &&
             suffix.substr(0U, status.size()) == status &&
             (suffix.size() == status.size() || suffix[status.size()] == ' ');
    };
    if (label == "token") {
      for (const std::string_view status : kTokenStatuses)
        if (matches(status)) return true;
    } else if (label == "authorization") {
      for (const std::string_view status : kAuthorizationStatuses)
        if (matches(status)) return true;
    }
    return false;
  };
  for (const std::string_view label : kValueLabels) {
    size_t at = 0U;
    while ((at = lowercase.find(label, at)) != std::string::npos) {
      const size_t label_end = at + label.size();
      size_t after = label_end;
      while (after < lowercase.size() && lowercase[after] == ' ') ++after;
      if (after > label_end && after < lowercase.size()) {
        if (!safeStatusContinuation(label,
                                    std::string_view(lowercase).substr(after))) {
          return true;
        }
        at = after;
        continue;
      }
      if (after < lowercase.size() &&
          (lowercase[after] == ':' || lowercase[after] == '=' ||
           lowercase[after] == '"' || lowercase[after] == '\'')) {
        return true;
      }
      at += label.size();
    }
  }
  return false;
}

bool opaqueCredentialLikeWord(const std::string& value) {
  size_t begin = 0U;
  while (begin < value.size()) {
    while (begin < value.size() && value[begin] == ' ') ++begin;
    size_t end = begin;
    bool has_upper = false;
    bool has_lower = false;
    bool has_digit = false;
    bool opaque = true;
    while (end < value.size() && value[end] != ' ') {
      const unsigned char ch = static_cast<unsigned char>(value[end]);
      has_upper = has_upper || (ch >= 'A' && ch <= 'Z');
      has_lower = has_lower || (ch >= 'a' && ch <= 'z');
      has_digit = has_digit || (ch >= '0' && ch <= '9');
      opaque = opaque && (std::isalnum(ch) != 0 || ch == '_' || ch == '-' ||
                          ch == '.' || ch == '/' || ch == '+');
      ++end;
    }
    const size_t bytes = end - begin;
    if (opaque && bytes >= 40U && has_digit && (has_upper || has_lower))
      return true;
    begin = end;
  }
  return false;
}

}  // namespace

std::string sanitizeDiagnosticDetail(const std::string& input,
                                     size_t maximum_bytes) {
  if (input.empty() || maximum_bytes == 0U ||
      maximum_bytes > kMaximumDiagnosticDetailBytes) {
    return std::string();
  }
  std::string normalized;
  normalized.reserve(std::min(input.size(), maximum_bytes));
  bool previous_space = false;
  for (size_t at = 0U; at < input.size();) {
    const unsigned char byte = static_cast<unsigned char>(input[at]);
    if (byte == ' ' || byte == '\r' || byte == '\n' || byte == '\t') {
      if (!previous_space && !normalized.empty() &&
          normalized.size() < maximum_bytes) {
        normalized.push_back(' ');
      }
      previous_space = true;
      ++at;
      continue;
    }
    const size_t sequence = utf8SequenceBytes(input, at);
    if (sequence == 0U) return std::string();
    if (sequence > maximum_bytes - normalized.size()) break;
    normalized.append(input, at, sequence);
    previous_space = sequence == 1U && input[at] == ' ';
    at += sequence;
  }
  while (!normalized.empty() && normalized.back() == ' ')
    normalized.pop_back();
  if (normalized.empty()) return std::string();
  const std::string lowercase = lowercaseAscii(normalized);
  if (containsCredentialMaterial(lowercase) ||
      opaqueCredentialLikeWord(normalized)) {
    return std::string();
  }
  return normalized;
}

bool isCanonicalDiagnosticDetail(const std::string& value,
                                 size_t maximum_bytes) {
  return !value.empty() &&
         sanitizeDiagnosticDetail(value, maximum_bytes) == value;
}

}  // namespace inkloop::diagnostics
