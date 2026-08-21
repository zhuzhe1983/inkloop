#pragma once

#include <map>
#include <string>

namespace inkloop {
namespace portal {

std::string htmlEscape(const std::string& value);
std::string jsonEscape(const std::string& value);
std::string urlEncode(const std::string& value);
bool parseFormBody(
    const std::string& body,
    size_t maximumBytes,
    std::map<std::string, std::string>* fields,
    std::string* error);
bool isSafeAssetId(const std::string& value);
bool isSensitiveDiagnosticLine(const std::string& line);
std::string sanitizedDiagnosticLine(const std::string& line, size_t maximumLength);

}  // namespace portal
}  // namespace inkloop
