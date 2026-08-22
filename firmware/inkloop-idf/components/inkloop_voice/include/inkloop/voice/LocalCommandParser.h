#pragma once

#include "VoiceTypes.h"

namespace inkloop {
namespace voice {

class LocalCommandParser {
 public:
  ParsedCommand parse(const std::string& transcript) const;
  static std::string normalize(const std::string& transcript);
  static std::string confirmationPhrase(const ParsedCommand& command);
  static bool needsSpokenConfirmation(CommandKind kind);
  static bool needsPhysicalConfirmation(CommandKind kind);
  static bool isValidStableId(const std::string& value);

 private:
  static bool validTextValue(const std::string& value, size_t maxBytes);
};

}  // namespace voice
}  // namespace inkloop
