#include "PortalEncoding.h"

#include <ctype.h>
#include <stdint.h>

#include <iomanip>
#include <sstream>

namespace inkloop {
namespace portal {

std::string htmlEscape(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    switch (value[index]) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&#39;"; break;
      default: result.push_back(value[index]); break;
    }
  }
  return result;
}

std::string jsonEscape(const std::string& value) {
  std::ostringstream output;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    switch (character) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

std::string urlEncode(const std::string& value) {
  static const char hexadecimal[] = "0123456789ABCDEF";
  std::string output;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_' || character == '.' || character == '~') {
      output.push_back(static_cast<char>(character));
    } else {
      output.push_back('%');
      output.push_back(hexadecimal[(character >> 4) & 0x0f]);
      output.push_back(hexadecimal[character & 0x0f]);
    }
  }
  return output;
}

namespace {

int hexadecimalValue(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

bool decodeFormValue(const std::string& input, std::string* output) {
  output->clear();
  output->reserve(input.size());
  for (size_t index = 0; index < input.size(); ++index) {
    const char character = input[index];
    if (character == '+') {
      output->push_back(' ');
      continue;
    }
    if (character != '%') {
      output->push_back(character);
      continue;
    }
    if (index + 2 >= input.size()) return false;
    const int high = hexadecimalValue(input[index + 1]);
    const int low = hexadecimalValue(input[index + 2]);
    if (high < 0 || low < 0) return false;
    const char decoded = static_cast<char>((high << 4) | low);
    if (decoded == '\0') return false;
    output->push_back(decoded);
    index += 2;
  }
  return true;
}

std::string normalizedDiagnosticKeySpace(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (isalnum(character)) {
      result.push_back(static_cast<char>(tolower(character)));
    }
  }
  return result;
}

}  // namespace

bool parseFormBody(
    const std::string& body,
    size_t maximumBytes,
    std::map<std::string, std::string>* fields,
    std::string* error) {
  if (!fields) return false;
  fields->clear();
  if (body.size() > maximumBytes) {
    if (error) *error = "request_body_too_large";
    return false;
  }
  size_t start = 0;
  while (start <= body.size()) {
    const size_t end = body.find('&', start);
    const std::string pair = body.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!pair.empty()) {
      const size_t equals = pair.find('=');
      if (equals == std::string::npos) {
        if (error) *error = "invalid_form_field";
        return false;
      }
      std::string key;
      std::string value;
      if (!decodeFormValue(pair.substr(0, equals), &key) ||
          !decodeFormValue(pair.substr(equals + 1), &value) || key.empty() ||
          key.size() > 64 || fields->find(key) != fields->end()) {
        if (error) *error = "invalid_form_field";
        return false;
      }
      (*fields)[key] = value;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (error) error->clear();
  return true;
}

bool isSafeAssetId(const std::string& value) {
  if (value.empty() || value.size() > 96) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (!isalnum(character) && character != ':' && character != '-' &&
        character != '_') {
      return false;
    }
  }
  return true;
}

bool isSensitiveDiagnosticLine(const std::string& line) {
  // Removing separators makes camelCase, snake_case, kebab-case, header form,
  // and whitespace variants equivalent. This intentionally over-redacts: the
  // local diagnostic export is not a general-purpose log transport.
  const std::string normalized = normalizedDiagnosticKeySpace(line);
  static const char* terms[] = {
      "authorization", "bearer", "token", "pairing", "session", "password",
      "secret", "cookie", "apikey", "inkloopdevice"};
  for (size_t index = 0; index < sizeof(terms) / sizeof(terms[0]); ++index) {
    if (normalized.find(terms[index]) != std::string::npos) return true;
  }
  return false;
}

std::string sanitizedDiagnosticLine(const std::string& line, size_t maximumLength) {
  if (isSensitiveDiagnosticLine(line)) return "[REDACTED]";
  std::string output;
  const size_t length = line.size() < maximumLength ? line.size() : maximumLength;
  output.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    const unsigned char character = static_cast<unsigned char>(line[index]);
    output.push_back((character >= 0x20 || character == '\t') ? line[index] : ' ');
  }
  return output;
}

}  // namespace portal
}  // namespace inkloop
