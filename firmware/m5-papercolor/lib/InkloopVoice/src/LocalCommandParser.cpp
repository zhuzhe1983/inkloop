#include "LocalCommandParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace inkloop {
namespace voice {
namespace {

bool asciiFirst(const std::string& value) {
  return !value.empty() && static_cast<unsigned char>(value[0]) < 0x80;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::string afterPrefix(const std::string& value, const std::string& prefix) {
  if (!startsWith(value, prefix)) return std::string();
  size_t begin = prefix.size();
  while (begin < value.size() && value[begin] == ' ') ++begin;
  return value.substr(begin);
}

bool parseInt(const std::string& value, int& output) {
  if (value.empty()) return false;
  char* end = NULL;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') return false;
  output = static_cast<int>(parsed);
  return true;
}

bool parseSize(const std::string& value) {
  const size_t split = value.find('x');
  if (split == std::string::npos || value.find('x', split + 1) != std::string::npos)
    return false;
  int width = 0, height = 0;
  return parseInt(value.substr(0, split), width) &&
         parseInt(value.substr(split + 1), height) && width >= 64 && width <= 2048 &&
         height >= 64 && height <= 2048;
}

std::string normalizeStorage(std::string value) {
  if (value == "tf" || value == "tf卡" || value == "sd卡") return "sd";
  if (value == "内置" || value == "内部" || value == "littlefs") return "internal";
  return value;
}

std::string normalizeResetTarget(std::string value) {
  if (value == "网络" || value == "wi-fi") return "wifi";
  if (value == "语音" || value == "my ai") return "myai";
  return value;
}

}  // namespace

std::string LocalCommandParser::normalize(const std::string& transcript) {
  std::string output;
  output.reserve(transcript.size());
  bool previousSpace = true;
  for (size_t index = 0; index < transcript.size(); ++index) {
    unsigned char ch = static_cast<unsigned char>(transcript[index]);
    if (ch < 0x80 && std::isspace(ch)) {
      if (!previousSpace) output.push_back(' ');
      previousSpace = true;
      continue;
    }
    previousSpace = false;
    output.push_back(ch < 0x80 ? static_cast<char>(std::tolower(ch))
                              : static_cast<char>(ch));
  }
  while (!output.empty() && output[output.size() - 1] == ' ') output.erase(output.size() - 1);
  // ASR commonly appends sentence punctuation. It is not part of a command.
  // Remove at most one ordinary sentence terminator. Repeated/trailing dots
  // are retained so a path-like target such as "a.." cannot normalize to the
  // different valid ID "a".
  if (!output.empty()) {
    const char tail = output[output.size() - 1];
    const bool punctuation = tail == '.' || tail == '!' || tail == '?' || tail == ',';
    const bool repeated = output.size() > 1 && output[output.size() - 2] == tail;
    if (punctuation && !repeated) output.erase(output.size() - 1);
  }
  const char* unicodePunctuation[] = {"。", "！", "？", "，", "；"};
  bool removed = true;
  while (removed) {
    removed = false;
    for (size_t index = 0;
         index < sizeof(unicodePunctuation) / sizeof(unicodePunctuation[0]); ++index) {
      const std::string suffix = unicodePunctuation[index];
      if (output.size() >= suffix.size() &&
          output.compare(output.size() - suffix.size(), suffix.size(), suffix) == 0) {
        output.erase(output.size() - suffix.size());
        removed = true;
        break;
      }
    }
  }
  return output;
}

ParsedCommand LocalCommandParser::parse(const std::string& transcript) const {
  const std::string text = normalize(transcript);
  ParsedCommand command;
  if (text == "剩余空间" || text == "查询剩余空间" || text == "还有多少空间" ||
      text == "free space" || text == "storage space" ||
      text == "query free space") {
    command.kind = CommandKind::QueryFreeSpace;
    command.english = asciiFirst(text);
    return command;
  }
  if (text == "列出图片" || text == "图片列表" || text == "列出所有图片" ||
      text == "list images" || text == "list all images") {
    command.kind = CommandKind::ListImages;
    command.english = asciiFirst(text);
    return command;
  }

  const struct PrefixCommand {
    const char* prefix;
    CommandKind kind;
    bool english;
  } prefixes[] = {
      {"显示图片 ", CommandKind::SelectImage, false},
      {"选择图片 ", CommandKind::SelectImage, false},
      {"show image ", CommandKind::SelectImage, true},
      {"select image ", CommandKind::SelectImage, true},
      {"删除图片 ", CommandKind::DeleteImage, false},
      {"delete image ", CommandKind::DeleteImage, true},
  };
  for (size_t index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
    const std::string target = afterPrefix(text, prefixes[index].prefix);
    if (!target.empty() && isValidStableId(target)) {
      command.kind = prefixes[index].kind;
      command.targetId = target;
      command.english = prefixes[index].english;
      return command;
    }
  }

  const struct Ordinal {
    const char* phrase;
    uint32_t ordinal;
  } ordinals[] = {{"第一张", 1}, {"第二张", 2}, {"第三张", 3},
                  {"first image", 1}, {"second image", 2}, {"third image", 3}};
  for (size_t index = 0; index < sizeof(ordinals) / sizeof(ordinals[0]); ++index) {
    if (text == ordinals[index].phrase) {
      command.kind = CommandKind::SelectImage;
      command.targetId = "@" + std::to_string(ordinals[index].ordinal);
      command.number = static_cast<int>(ordinals[index].ordinal);
      command.english = asciiFirst(text);
      return command;
    }
  }
  if (startsWith(text, "第") && text.size() > std::string("第张").size() &&
      text.compare(text.size() - std::string("张").size(), std::string("张").size(),
                   "张") == 0) {
    const std::string digits = text.substr(
        std::string("第").size(),
        text.size() - std::string("第").size() - std::string("张").size());
    int ordinal = 0;
    if (parseInt(digits, ordinal) && ordinal >= 1 && ordinal <= 999) {
      command.kind = CommandKind::SelectImage;
      command.targetId = "@" + std::to_string(ordinal);
      command.number = ordinal;
      return command;
    }
  }

  if (text == "清空所有图片" || text == "clear all images") {
    command.kind = CommandKind::ClearAllImages;
    command.english = asciiFirst(text);
    return command;
  }

  const char* volumePrefixes[] = {"设置音量 ", "音量 ", "set volume ", "volume "};
  for (size_t index = 0; index < sizeof(volumePrefixes) / sizeof(volumePrefixes[0]); ++index) {
    const std::string number = afterPrefix(text, volumePrefixes[index]);
    int value = 0;
    if (!number.empty() && parseInt(number, value) && value >= 0 && value <= 100) {
      command.kind = CommandKind::SetVolume;
      command.number = value;
      command.english = static_cast<unsigned char>(volumePrefixes[index][0]) < 0x80;
      return command;
    }
  }

  const char* formatPrefixes[] = {"格式化存储 ", "格式化 ", "format storage ", "format "};
  for (size_t index = 0; index < sizeof(formatPrefixes) / sizeof(formatPrefixes[0]); ++index) {
    std::string target = normalizeStorage(afterPrefix(text, formatPrefixes[index]));
    if (target == "sd" || target == "internal") {
      command.kind = CommandKind::FormatStorage;
      command.targetId = target;
      command.english = static_cast<unsigned char>(formatPrefixes[index][0]) < 0x80;
      return command;
    }
  }

  const char* promptPrefixes[] = {"设置智能体提示词 ", "智能体提示词 ",
                                  "set assistant prompt ", "assistant prompt "};
  for (size_t index = 0; index < sizeof(promptPrefixes) / sizeof(promptPrefixes[0]); ++index) {
    const std::string value = afterPrefix(text, promptPrefixes[index]);
    if (validTextValue(value, 256)) {
      command.kind = CommandKind::SetAssistantPrompt;
      command.value = value;
      command.english = static_cast<unsigned char>(promptPrefixes[index][0]) < 0x80;
      return command;
    }
  }

  const struct SettingPrefix {
    const char* prefix;
    const char* key;
    bool english;
  } settingPrefixes[] = {
      {"图片尺寸 ", "size", false}, {"image size ", "size", true},
      {"图片模型 ", "model", false}, {"image model ", "model", true},
      {"图片负面提示词 ", "negative_prompt", false},
      {"image negative prompt ", "negative_prompt", true},
  };
  for (size_t index = 0; index < sizeof(settingPrefixes) / sizeof(settingPrefixes[0]); ++index) {
    const std::string value = afterPrefix(text, settingPrefixes[index].prefix);
    const std::string key = settingPrefixes[index].key;
    const bool valid = key == "size" ? parseSize(value)
                     : key == "model" ? (value == "t2i" || value == "i2i")
                     : validTextValue(value, 256);
    if (valid) {
      command.kind = CommandKind::SetImageSetting;
      command.key = key;
      command.value = value;
      command.english = settingPrefixes[index].english;
      return command;
    }
  }

  const char* resetPrefixes[] = {"重置 ", "reset "};
  for (size_t index = 0; index < sizeof(resetPrefixes) / sizeof(resetPrefixes[0]); ++index) {
    const std::string target = normalizeResetTarget(afterPrefix(text, resetPrefixes[index]));
    if (target == "wifi" || target == "myai") {
      command.kind = CommandKind::ResetTarget;
      command.targetId = target;
      command.english = static_cast<unsigned char>(resetPrefixes[index][0]) < 0x80;
      return command;
    }
  }
  return command;
}

std::string LocalCommandParser::confirmationPhrase(const ParsedCommand& command) {
  switch (command.kind) {
    case CommandKind::DeleteImage:
      return command.english ? "confirm delete image " + command.targetId
                             : "确认删除图片 " + command.targetId;
    case CommandKind::ClearAllImages:
      return command.english ? "confirm clear all images" : "确认清空所有图片";
    case CommandKind::FormatStorage:
      return command.english ? "confirm format storage " + command.targetId
                             : "确认格式化存储 " + command.targetId;
    case CommandKind::ResetTarget:
      return command.english ? "confirm reset " + command.targetId
                             : "确认重置 " + command.targetId;
    default:
      return std::string();
  }
}

bool LocalCommandParser::needsSpokenConfirmation(CommandKind kind) {
  return kind == CommandKind::DeleteImage || kind == CommandKind::ClearAllImages ||
         kind == CommandKind::FormatStorage || kind == CommandKind::ResetTarget;
}

bool LocalCommandParser::needsPhysicalConfirmation(CommandKind kind) {
  return kind == CommandKind::ClearAllImages || kind == CommandKind::FormatStorage ||
         kind == CommandKind::ResetTarget;
}

bool LocalCommandParser::isValidStableId(const std::string& value) {
  if (value.empty() || value.size() > 64 || value[0] == '@') return false;
  if (value == "." || value == ".." || value[0] == '.' ||
      value[value.size() - 1] == '.' || value.find("..") != std::string::npos ||
      value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos || value.find("%2f") != std::string::npos ||
      value.find("%5c") != std::string::npos)
    return false;
  // Do not accept a drive-letter-shaped token even though ':' remains valid
  // for repository namespace IDs such as "sd:asset-42".
  if (value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) &&
      value[1] == ':')
    return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == ':' || ch == '.'))
      return false;
  }
  return true;
}

bool LocalCommandParser::validTextValue(const std::string& value, size_t maxBytes) {
  if (value.empty() || value.size() > maxBytes) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch < 0x20 && ch != '\t') return false;
  }
  return true;
}

}  // namespace voice
}  // namespace inkloop
