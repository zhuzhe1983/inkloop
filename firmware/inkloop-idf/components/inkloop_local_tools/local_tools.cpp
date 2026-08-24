#include "inkloop/local_tools/local_tools.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

namespace inkloop {
namespace local_tools {
namespace {

bool validUtf8(std::string_view value) {
  size_t at = 0;
  while (at < value.size()) {
    const uint8_t first = static_cast<uint8_t>(value[at]);
    if (first < 0x80U) {
      if ((first < 0x20U && first != '\t' && first != '\n' &&
           first != '\r') || first == 0x7FU)
        return false;
      ++at;
      continue;
    }
    size_t trailing = 0;
    uint32_t scalar = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      trailing = 1U;
      scalar = first & 0x1FU;
      minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      trailing = 2U;
      scalar = first & 0x0FU;
      minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      trailing = 3U;
      scalar = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (trailing > value.size() - at - 1U) return false;
    for (size_t index = 1U; index <= trailing; ++index) {
      const uint8_t next = static_cast<uint8_t>(value[at + index]);
      if ((next & 0xC0U) != 0x80U) return false;
      scalar = (scalar << 6U) | (next & 0x3FU);
    }
    if (scalar < minimum || scalar > 0x10FFFFU ||
        (scalar >= 0xD800U && scalar <= 0xDFFFU) ||
        (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
        (scalar & 0xFFFFU) == 0xFFFEU ||
        (scalar & 0xFFFFU) == 0xFFFFU) {
      return false;
    }
    at += trailing + 1U;
  }
  return true;
}

std::string trimAscii(std::string_view value) {
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])))
    ++first;
  size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])))
    --last;
  return std::string(value.substr(first, last - first));
}

std::string normalizeAscii(std::string_view value) {
  std::string output = trimAscii(value);
  bool previous_space = false;
  size_t write = 0;
  for (size_t read = 0; read < output.size(); ++read) {
    const unsigned char ch = static_cast<unsigned char>(output[read]);
    if (ch < 0x80U && std::isspace(ch)) {
      if (!previous_space) output[write++] = ' ';
      previous_space = true;
    } else {
      output[write++] = ch < 0x80U
                            ? static_cast<char>(std::tolower(ch))
                            : static_cast<char>(ch);
      previous_space = false;
    }
  }
  output.resize(write);
  if (!output.empty() && output.back() == ' ') output.pop_back();
  return output;
}

bool blankAudioArtifact(std::string_view value) {
  std::string normalized = normalizeAscii(value);
  if (normalized.size() >= 2U && normalized.front() == '[' &&
      normalized.back() == ']')
    normalized = normalized.substr(1, normalized.size() - 2U);
  std::string compact;
  for (char ch : normalized) {
    if (ch == ' ' || ch == '-') compact.push_back('_');
    else compact.push_back(ch);
  }
  return compact == "blank_audio";
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string withoutSentenceTerminator(std::string value) {
  static constexpr std::array<std::string_view, 9> kTerminators{
      "。", "！", "？", "；", "，", ".", "!", "?", ","};
  for (std::string_view terminator : kTerminators) {
    if (endsWith(value, terminator)) {
      value.erase(value.size() - terminator.size());
      break;
    }
  }
  return trimAscii(value);
}

bool oneOf(std::string_view value,
           std::initializer_list<std::string_view> choices) {
  for (std::string_view choice : choices) {
    if (value == choice) return true;
  }
  return false;
}

int chineseDigit(std::string_view value) {
  if (value == "零" || value == "〇") return 0;
  if (value == "一") return 1;
  if (value == "二" || value == "两") return 2;
  if (value == "三") return 3;
  if (value == "四") return 4;
  if (value == "五") return 5;
  if (value == "六") return 6;
  if (value == "七") return 7;
  if (value == "八") return 8;
  if (value == "九") return 9;
  return -1;
}

bool parseChineseNumber(std::string_view value, uint32_t& output) {
  if (value.empty() || value.size() % 3U != 0U) return false;
  const size_t count = value.size() / 3U;
  const auto token = [&](size_t index) { return value.substr(index * 3U, 3U); };
  if (count == 1U) {
    const int digit = chineseDigit(token(0));
    if (digit >= 0) {
      output = static_cast<uint32_t>(digit);
      return true;
    }
    if (token(0) == "十") {
      output = 10U;
      return true;
    }
    return false;
  }
  if (count == 2U) {
    const int first = chineseDigit(token(0));
    const int second = chineseDigit(token(1));
    if (first >= 1 && token(1) == "十") {
      output = static_cast<uint32_t>(first * 10);
      return true;
    }
    if (token(0) == "十" && second >= 1) {
      output = static_cast<uint32_t>(10 + second);
      return true;
    }
    if (first == 1 && token(1) == "百") {
      output = 100U;
      return true;
    }
    return false;
  }
  if (count == 3U) {
    const int first = chineseDigit(token(0));
    const int third = chineseDigit(token(2));
    if (first >= 1 && token(1) == "十" && third >= 1) {
      output = static_cast<uint32_t>(first * 10 + third);
      return true;
    }
  }
  return false;
}

bool parseUnsigned(std::string_view value, uint32_t maximum, uint32_t& output) {
  if (value.empty()) return false;
  uint32_t parsed = 0;
  bool ascii = true;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      ascii = false;
      break;
    }
    const uint32_t digit = static_cast<uint32_t>(ch - '0');
    if (parsed > (maximum - std::min(maximum, digit)) / 10U) return false;
    parsed = parsed * 10U + digit;
    if (parsed > maximum) return false;
  }
  if (ascii) {
    output = parsed;
    return true;
  }
  return parseChineseNumber(value, output) && output <= maximum;
}

bool parsePercent(std::string value, uint32_t& output) {
  value = trimAscii(value);
  if (startsWith(value, "百分之")) value.erase(0, std::string("百分之").size());
  if (endsWith(value, "%")) value.erase(value.size() - 1U);
  value = trimAscii(value);
  return parseUnsigned(value, 100U, output);
}

bool parseChineseImageOrdinal(std::string_view value, uint32_t& output) {
  static constexpr std::array<std::string_view, 6> kPrefixes{
      "第", "显示第", "选择第", "上屏第", "刷新第", "打开第"};
  for (std::string_view prefix : kPrefixes) {
    if (!startsWith(value, prefix)) continue;
    value.remove_prefix(prefix.size());
    if (endsWith(value, "张图片"))
      value.remove_suffix(std::string_view("张图片").size());
    else if (endsWith(value, "张"))
      value.remove_suffix(std::string_view("张").size());
    else
      return false;
    return parseUnsigned(value, kMaximumImageOrdinal, output) && output != 0U;
  }
  return false;
}

bool parseEnglishOrdinalToken(std::string value, uint32_t& output) {
  value = trimAscii(value);
  if (startsWith(value, "number ")) value.erase(0, 7U);
  if (value.size() > 2U) {
    const std::string suffix = value.substr(value.size() - 2U);
    if (oneOf(suffix, {"st", "nd", "rd", "th"}))
      value.erase(value.size() - 2U);
  }
  return parseUnsigned(value, kMaximumImageOrdinal, output) && output != 0U;
}

bool parseEnglishImageOrdinal(std::string_view value, uint32_t& output) {
  static constexpr std::array<std::string_view, 5> kLeadingPrefixes{
      "show image ", "select image ", "display image ", "open image ",
      "image "};
  for (std::string_view prefix : kLeadingPrefixes) {
    if (!startsWith(value, prefix)) continue;
    return parseEnglishOrdinalToken(std::string(value.substr(prefix.size())),
                                    output);
  }
  if (startsWith(value, "show the ") || startsWith(value, "select the ") ||
      startsWith(value, "display the ") || startsWith(value, "open the ")) {
    const size_t prefix = value.find("the ") + 4U;
    if (!endsWith(value, " image")) return false;
    return parseEnglishOrdinalToken(
        std::string(value.substr(prefix, value.size() - prefix - 6U)), output);
  }
  return false;
}

bool intentPresent(std::string_view text,
                   std::initializer_list<std::string_view> keywords) {
  for (std::string_view keyword : keywords) {
    if (text.find(keyword) != std::string_view::npos) return true;
  }
  return false;
}

std::string promptAfter(const std::string& original,
                        const std::string& normalized,
                        std::initializer_list<std::string_view> prefixes,
                        bool& recognized) {
  for (std::string_view prefix : prefixes) {
    if (!startsWith(normalized, prefix)) continue;
    recognized = true;
    return trimAscii(std::string_view(original).substr(prefix.size()));
  }
  recognized = false;
  return std::string();
}

ParseResult matched(Command command) {
  ParseResult result;
  result.code = ParseCode::Matched;
  result.command = std::move(command);
  return result;
}

ParseResult failed(ParseCode code) {
  ParseResult result;
  result.code = code;
  return result;
}

size_t intentFamilyCount(std::string_view text) {
  size_t count = 0;
  count += intentPresent(text, {"空间", "容量", "存储", "space", "storage"});
  count += intentPresent(
      text, {"删除", "清空", "相册", "上屏", "图片列表", "delete image",
             "clear album", "clear all images", "list images", "list album",
             "show image", "select image", "display image"});
  count += intentPresent(
      text, {"音量", "扬声器", "volume", "speaker volume"});
  count += intentPresent(text, {"格式化", "format tf card"});
  count += intentPresent(
      text, {"智能体提示词", "助手提示词", "系统提示词",
             "assistant prompt", "system prompt"});
  count += intentPresent(
      text, {"aigc图片提示词", "aigc 图片提示词", "图片生成提示词",
             "aigc prompt", "image prompt template"});
  count += intentPresent(
      text, {"aigc图片步数", "aigc 图片步数", "图片生成步数",
             "image generation steps", "image steps", "aigc steps"});
  count += intentPresent(
      text, {"负面提示词", "反向提示词", "negative prompt"});
  count += intentPresent(
      text, {"渲染方式", "渲染策略", "render strategy", "render policy"});
  count += intentPresent(text, {"led", "灯光", "亮度", "brightness"});
  return count;
}

}  // namespace

ParseResult LocalCommandParser::parseFinalAsr(
    std::string_view transcript) const {
  if (transcript.size() > kMaximumTranscriptBytes)
    return failed(ParseCode::TooLong);
  if (!validUtf8(transcript)) return failed(ParseCode::InvalidEncoding);
  const std::string original = trimAscii(transcript);
  if (original.empty()) return failed(ParseCode::IgnoredEmpty);
  if (blankAudioArtifact(original)) return failed(ParseCode::IgnoredBlankAudio);
  const std::string text = withoutSentenceTerminator(normalizeAscii(original));
  const std::string control_original = withoutSentenceTerminator(original);

  Command command;
  if (oneOf(text, {"查询剩余空间", "查看剩余空间", "还有多少剩余空间",
                   "还有多少可用空间", "tf卡还剩多少空间", "tf 卡还剩多少空间",
                   "剩余空间是多少", "query free space", "check free space",
                   "free space", "available space", "how much free space",
                   "how much storage is left"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Remaining;
    return matched(std::move(command));
  }
  if (oneOf(text, {"查询总空间", "查看总空间", "总空间是多少",
                   "查询总容量", "总容量是多少", "tf卡总容量是多少",
                   "tf 卡总容量是多少", "query total space",
                   "check total space", "total space", "total storage"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Total;
    return matched(std::move(command));
  }
  if (oneOf(text, {"查询总空间和剩余空间", "查询剩余空间和总空间",
                   "查询存储空间", "查看存储空间", "query storage",
                   "check storage", "storage status", "query storage space"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Both;
    return matched(std::move(command));
  }

  if (oneOf(text, {"列出相册", "列出图片", "列出所有图片", "图片列表",
                   "查看相册", "相册有多少张", "相册里有几张", "list album",
                   "list images", "list all images", "show album summary",
                   "how many images"})) {
    command.kind = CommandKind::ListAlbum;
    return matched(std::move(command));
  }

  uint32_t image_ordinal = 0U;
  if (parseChineseImageOrdinal(text, image_ordinal) ||
      parseEnglishImageOrdinal(text, image_ordinal)) {
    command.kind = CommandKind::SelectImageOrdinal;
    command.number = image_ordinal;
    return matched(std::move(command));
  }

  if (startsWith(text, "删除第")) {
    std::string_view tail(text);
    tail.remove_prefix(std::string_view("删除第").size());
    std::string_view suffix;
    if (endsWith(tail, "张图片")) suffix = "张图片";
    else if (endsWith(tail, "张")) suffix = "张";
    else return failed(ParseCode::InvalidValue);
    tail.remove_suffix(suffix.size());
    uint32_t ordinal = 0;
    if (!parseUnsigned(tail, kMaximumImageOrdinal, ordinal) || ordinal == 0U)
      return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::DeleteImageOrdinal;
    command.number = ordinal;
    return matched(std::move(command));
  }

  for (std::string_view prefix : {
           std::string_view("delete image "),
           std::string_view("delete image number ")}) {
    if (!startsWith(text, prefix)) continue;
    const std::string target = trimAscii(text.substr(prefix.size()));
    uint32_t ordinal = 0U;
    if (parseEnglishOrdinalToken(target, ordinal)) {
      command.kind = CommandKind::DeleteImageOrdinal;
      command.number = ordinal;
      return matched(std::move(command));
    }
    const std::string id = trimAscii(
        std::string_view(control_original).substr(prefix.size()));
    if (!validImageId(id)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::DeleteImageId;
    command.text = id;
    return matched(std::move(command));
  }

  if (startsWith(text, "delete the ") && endsWith(text, " image")) {
    const std::string ordinal_text = text.substr(
        std::string_view("delete the ").size(),
        text.size() - std::string_view("delete the ").size() -
            std::string_view(" image").size());
    uint32_t ordinal = 0U;
    if (!parseEnglishOrdinalToken(ordinal_text, ordinal))
      return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::DeleteImageOrdinal;
    command.number = ordinal;
    return matched(std::move(command));
  }

  for (std::string_view prefix : {std::string_view("删除指定图片 "),
                                  std::string_view("删除图片 ")}) {
    if (!startsWith(text, prefix)) continue;
    const std::string id = trimAscii(
        std::string_view(control_original).substr(prefix.size()));
    if (!validImageId(id)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::DeleteImageId;
    command.text = id;
    return matched(std::move(command));
  }

  if (oneOf(text, {"清空相册", "清空所有图片", "删除所有图片",
                   "clear album", "clear all images", "delete all images"})) {
    command.kind = CommandKind::ClearAlbum;
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询音量", "查看音量", "当前音量", "当前音量是多少",
                   "音量是多少", "query volume", "check volume",
                   "current volume", "what is the volume"})) {
    command.kind = CommandKind::QueryVolume;
    return matched(std::move(command));
  }
  for (std::string_view prefix : {
           std::string_view("把音量设置为"), std::string_view("设置音量为"),
           std::string_view("设置音量"), std::string_view("把音量调到"),
           std::string_view("音量调到"), std::string_view("音量设置为"),
           std::string_view("把音量调整为"), std::string_view("调整音量为")}) {
    if (!startsWith(text, prefix)) continue;
    uint32_t percent = 0;
    if (!parsePercent(text.substr(prefix.size()), percent))
      return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetVolume;
    command.number = percent;
    return matched(std::move(command));
  }
  for (std::string_view prefix : {
           std::string_view("set volume to "),
           std::string_view("set volume "),
           std::string_view("volume to "), std::string_view("volume ")}) {
    if (!startsWith(text, prefix)) continue;
    uint32_t percent = 0U;
    if (!parsePercent(std::string(text.substr(prefix.size())), percent))
      return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetVolume;
    command.number = percent;
    return matched(std::move(command));
  }

  if (oneOf(text, {"格式化tf卡", "格式化 tf卡", "格式化tf 卡", "格式化 tf 卡",
                   "format tf card", "format the tf card"})) {
    command.kind = CommandKind::FormatTfCard;
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询智能体提示词", "查看智能体提示词",
                   "当前智能体提示词", "智能体提示词是什么",
                   "query assistant prompt", "show assistant prompt",
                   "current assistant prompt", "what is the assistant prompt",
                   "query system prompt"})) {
    command.kind = CommandKind::QueryAssistantPrompt;
    return matched(std::move(command));
  }
  bool recognized = false;
  std::string prompt = promptAfter(
      original, text,
      {"设置智能体提示词为", "把智能体提示词设置为", "设置智能体提示词：",
       "设置智能体提示词:", "set assistant prompt to ",
       "set assistant prompt ", "set system prompt to ",
       "set system prompt "},
      recognized);
  if (recognized) {
    if (!validPrompt(prompt)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetAssistantPrompt;
    command.text = std::move(prompt);
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询aigc图片提示词", "查看aigc图片提示词",
                   "当前aigc图片提示词", "aigc图片提示词是什么",
                   "查询图片生成提示词", "query aigc prompt",
                   "show aigc prompt", "current aigc prompt",
                   "query image prompt template",
                   "show image prompt template"})) {
    command.kind = CommandKind::QueryAigcPrompt;
    return matched(std::move(command));
  }
  prompt = promptAfter(
      original, text,
      {"设置aigc图片提示词为", "把aigc图片提示词设置为",
       "设置aigc图片提示词：", "设置aigc图片提示词:",
       "设置图片生成提示词为", "设置图片生成提示词：",
       "设置图片生成提示词:", "set aigc prompt to ",
       "set aigc prompt ", "set image prompt template to ",
       "set image prompt template "},
      recognized);
  if (recognized) {
    if (!validPrompt(prompt)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetAigcPrompt;
    command.text = std::move(prompt);
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询aigc图片步数", "查看aigc图片步数",
                   "当前aigc图片步数", "aigc图片步数是多少",
                   "查询图片生成步数", "查看图片生成步数",
                   "当前图片生成步数", "图片生成步数是多少",
                   "query image generation steps", "show image steps",
                   "current image steps", "what are the image steps",
                   "query aigc steps"})) {
    command.kind = CommandKind::QueryAigcSteps;
    return matched(std::move(command));
  }
  for (std::string_view prefix : {
           std::string_view("设置aigc图片步数为"),
           std::string_view("把aigc图片步数设置为"),
           std::string_view("设置图片生成步数为"),
           std::string_view("把图片生成步数设置为"),
           std::string_view("set image generation steps to "),
           std::string_view("set image steps to "),
           std::string_view("set image steps "),
           std::string_view("set aigc steps to "),
           std::string_view("image steps ")}) {
    if (!startsWith(text, prefix)) continue;
    uint32_t steps = 0U;
    if (!parseUnsigned(text.substr(prefix.size()), kMaximumAigcSteps, steps) ||
        steps < kMinimumAigcSteps) {
      return failed(ParseCode::InvalidValue);
    }
    command.kind = CommandKind::SetAigcSteps;
    command.number = steps;
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询aigc负面提示词", "查看aigc负面提示词",
                   "当前aigc负面提示词", "aigc负面提示词是什么",
                   "查询图片负面提示词", "查询反向提示词",
                   "query aigc negative prompt", "show aigc negative prompt",
                   "current aigc negative prompt", "query negative prompt",
                   "show negative prompt"})) {
    command.kind = CommandKind::QueryAigcNegativePrompt;
    return matched(std::move(command));
  }
  if (oneOf(text, {"清空aigc负面提示词", "清除aigc负面提示词",
                   "清空图片负面提示词", "clear aigc negative prompt",
                   "clear negative prompt"})) {
    command.kind = CommandKind::SetAigcNegativePrompt;
    command.text.clear();
    return matched(std::move(command));
  }
  prompt = promptAfter(
      original, text,
      {"设置aigc负面提示词为", "把aigc负面提示词设置为",
       "设置aigc负面提示词：", "设置aigc负面提示词:",
       "设置图片负面提示词为", "设置反向提示词为",
       "set aigc negative prompt to ", "set aigc negative prompt ",
       "set negative prompt to ", "set negative prompt "},
      recognized);
  if (recognized) {
    if (!validNegativePrompt(prompt)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetAigcNegativePrompt;
    command.text = std::move(prompt);
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询默认渲染方式", "查看默认渲染方式",
                   "当前默认渲染方式", "默认渲染方式是什么",
                   "查询默认渲染策略", "当前默认渲染策略",
                   "query default render strategy",
                   "show default render strategy",
                   "current default render strategy",
                   "what is the default render strategy",
                   "query default render policy"})) {
    command.kind = CommandKind::QueryDefaultRenderStrategy;
    return matched(std::move(command));
  }
  std::string strategy = promptAfter(
      original, text,
      {"设置默认渲染方式为", "把默认渲染方式设置为",
       "设置默认渲染策略为", "把默认渲染策略设置为",
       "set default render strategy to ", "set default render strategy ",
       "set default render policy to ", "set default render policy "},
      recognized);
  if (recognized) {
    strategy = withoutSentenceTerminator(normalizeAscii(strategy));
    if (!validRenderStrategyId(strategy))
      return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetDefaultRenderStrategy;
    command.text = std::move(strategy);
    return matched(std::move(command));
  }

  for (std::string_view prefix : {
           std::string_view("设置led最大亮度为"),
           std::string_view("把led最大亮度设置为"),
           std::string_view("设置led最大亮度"),
           std::string_view("set led maximum brightness to "),
           std::string_view("set led maximum brightness ")}) {
    if (!startsWith(text, prefix)) continue;
    uint32_t percent = 0;
    if (!parsePercent(text.substr(prefix.size()), percent))
      return failed(ParseCode::InvalidValue);
    if (percent == 0U) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetLedMaximumBrightness;
    command.number = percent;
    return matched(std::move(command));
  }

  if (intentFamilyCount(text) > 1U) return failed(ParseCode::Ambiguous);
  return failed(ParseCode::NoMatch);
}

bool LocalCommandParser::validImageId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumImageIdBytes ||
      value.front() == '.' || value.back() == '.' || value == ".." ||
      value.find("..") != std::string_view::npos ||
      value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos)
    return false;
  if (value.size() == 2U &&
      std::isalpha(static_cast<unsigned char>(value.front())) &&
      value.back() == ':')
    return false;
  for (unsigned char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == ':' ||
          ch == '.'))
      return false;
  }
  return true;
}

bool LocalCommandParser::validPrompt(std::string_view value) {
  if (value.empty() || value.size() > kMaximumPromptBytes || !validUtf8(value))
    return false;
  for (unsigned char ch : value) {
    if (ch < 0x20U && ch != '\t' && ch != '\n') return false;
  }
  return true;
}

bool LocalCommandParser::validStoredPrompt(std::string_view value) {
  if (value.empty() || value.size() > kMaximumStoredPromptBytes ||
      !validUtf8(value))
    return false;
  for (unsigned char ch : value) {
    if (ch < 0x20U && ch != '\t' && ch != '\n') return false;
  }
  return true;
}

bool LocalCommandParser::validNegativePrompt(std::string_view value,
                                             bool empty_allowed) {
  if (value.empty()) return empty_allowed;
  if (value.size() > kMaximumNegativePromptBytes || !validUtf8(value))
    return false;
  for (unsigned char ch : value) {
    if (ch < 0x20U && ch != '\t' && ch != '\n') return false;
  }
  return true;
}

bool LocalCommandParser::validRenderStrategyId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumRenderStrategyBytes ||
      value.front() < 'a' || value.front() > 'z' || value.back() == '-') {
    return false;
  }
  bool previous_hyphen = false;
  for (const unsigned char ch : value) {
    const bool hyphen = ch == '-';
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || hyphen) ||
        (hyphen && previous_hyphen)) {
      return false;
    }
    previous_hyphen = hyphen;
  }
  return true;
}

bool LocalCommandParser::destructive(CommandKind kind) {
  return kind == CommandKind::DeleteImageOrdinal ||
      kind == CommandKind::DeleteImageId || kind == CommandKind::ClearAlbum ||
      kind == CommandKind::FormatTfCard;
}

ToolOutcome LocalToolsSession::handleFinalAsr(
    std::string_view transcript, uint64_t now_ms, ILocalToolsAdapter& adapter,
    IConfirmationTokenSource& tokens) {
  const ParseResult parsed = parser_.parseFinalAsr(transcript);
  ToolOutcome outcome;
  outcome.parse_code = parsed.code;
  outcome.command = parsed.command.kind;
  if (parsed.ignored()) {
    outcome.code = ExecutionCode::Ignored;
    return outcome;
  }
  if (!parsed.matched()) {
    outcome.code = ExecutionCode::Rejected;
    return outcome;
  }

  if (LocalCommandParser::destructive(parsed.command.kind)) {
    // Replacing one pending destructive request must actively erase the old
    // token before issuing a new one; std::string assignment alone may retain
    // the previous secret in capacity storage.
    cancelConfirmation();
    std::string token;
    if (!tokens.issue(token) || !validToken(token)) {
      cancelConfirmation();
      outcome.code = ExecutionCode::TokenUnavailable;
      return outcome;
    }
    pending_command_ = parsed.command;
    pending_token_ = token;
    pending_issued_ms_ = now_ms;
    pending_ = true;
    outcome.code = ExecutionCode::ConfirmationRequired;
    outcome.confirmation_token = std::move(token);
    return outcome;
  }

  cancelConfirmation();
  outcome = execute(parsed.command, adapter);
  outcome.parse_code = parsed.code;
  return outcome;
}

ToolOutcome LocalToolsSession::confirm(std::string_view token, uint64_t now_ms,
                                       ILocalToolsAdapter& adapter) {
  ToolOutcome outcome;
  if (!pending_) {
    outcome.code = ExecutionCode::ConfirmationMissing;
    return outcome;
  }
  outcome.command = pending_command_.kind;
  if (now_ms < pending_issued_ms_ ||
      now_ms - pending_issued_ms_ > kConfirmationLifetimeMs) {
    cancelConfirmation();
    outcome.code = ExecutionCode::ConfirmationExpired;
    return outcome;
  }
  if (!validToken(token) || !tokensEqual(token, pending_token_)) {
    outcome.code = ExecutionCode::ConfirmationMismatch;
    return outcome;
  }
  const Command command = pending_command_;
  cancelConfirmation();
  outcome = execute(command, adapter);
  outcome.parse_code = ParseCode::Matched;
  return outcome;
}

void LocalToolsSession::cancelConfirmation() {
  std::fill(pending_token_.begin(), pending_token_.end(), '\0');
  pending_token_.clear();
  pending_command_ = Command();
  pending_issued_ms_ = 0;
  pending_ = false;
}

ToolOutcome LocalToolsSession::execute(const Command& command,
                                       ILocalToolsAdapter& adapter) {
  ToolOutcome outcome;
  outcome.command = command.kind;
  outcome.storage_metric = command.storage_metric;
  AdapterResult result;
  switch (command.kind) {
    case CommandKind::QueryStorage:
      result = adapter.queryStorage(outcome.storage);
      if (result.ok() && outcome.storage.remaining_bytes > outcome.storage.total_bytes) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        return outcome;
      }
      break;
    case CommandKind::ListAlbum:
      result = adapter.queryAlbumSummary(outcome.album_summary);
      if (result.ok() &&
          (outcome.album_summary.count > kMaximumImageOrdinal ||
           outcome.album_summary.current_ordinal >
               outcome.album_summary.count)) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        return outcome;
      }
      break;
    case CommandKind::SelectImageOrdinal:
      result = adapter.resolveImageByOrdinal(
          command.number, outcome.album_selection);
      if (result.ok() &&
          (outcome.album_selection.ordinal != command.number ||
           outcome.album_selection.ordinal == 0U ||
           outcome.album_selection.zero_based_index + 1U !=
               outcome.album_selection.ordinal ||
           outcome.album_selection.total == 0U ||
           outcome.album_selection.total > kMaximumImageOrdinal ||
           outcome.album_selection.ordinal > outcome.album_selection.total ||
           !LocalCommandParser::validImageId(
               outcome.album_selection.asset_id))) {
        outcome.album_selection = AlbumSelection{};
        outcome.code = ExecutionCode::AdapterContractViolation;
        return outcome;
      }
      break;
    case CommandKind::DeleteImageOrdinal:
      result = adapter.deleteImageByOrdinal(command.number);
      break;
    case CommandKind::DeleteImageId:
      result = adapter.deleteImageById(command.text);
      break;
    case CommandKind::ClearAlbum:
      result = adapter.clearAlbum();
      break;
    case CommandKind::QueryVolume:
      result = adapter.queryVolume(outcome.percent);
      if (result.ok() && outcome.percent > 100U) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        return outcome;
      }
      break;
    case CommandKind::SetVolume:
      result = adapter.setVolume(static_cast<uint8_t>(command.number));
      outcome.percent = static_cast<uint8_t>(command.number);
      break;
    case CommandKind::FormatTfCard:
      result = adapter.formatTfCard();
      break;
    case CommandKind::QueryAssistantPrompt:
      result = adapter.queryAssistantPrompt(outcome.text);
      if (result.ok() && !outcome.text.empty() &&
          !LocalCommandParser::validStoredPrompt(outcome.text)) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        outcome.text.clear();
        return outcome;
      }
      break;
    case CommandKind::SetAssistantPrompt:
      result = adapter.setAssistantPrompt(command.text);
      outcome.text = command.text;
      break;
    case CommandKind::QueryAigcPrompt:
      result = adapter.queryAigcPrompt(outcome.text);
      if (result.ok() && !outcome.text.empty() &&
          !LocalCommandParser::validStoredPrompt(outcome.text)) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        outcome.text.clear();
        return outcome;
      }
      break;
    case CommandKind::SetAigcPrompt:
      result = adapter.setAigcPrompt(command.text);
      outcome.text = command.text;
      break;
    case CommandKind::QueryAigcSteps:
      result = adapter.queryAigcSteps(outcome.steps);
      if (result.ok() &&
          (outcome.steps < kMinimumAigcSteps ||
           outcome.steps > kMaximumAigcSteps)) {
        outcome.steps = 0U;
        outcome.code = ExecutionCode::AdapterContractViolation;
        return outcome;
      }
      break;
    case CommandKind::SetAigcSteps:
      result = adapter.setAigcSteps(static_cast<uint8_t>(command.number));
      outcome.steps = static_cast<uint8_t>(command.number);
      break;
    case CommandKind::QueryAigcNegativePrompt:
      result = adapter.queryAigcNegativePrompt(outcome.text);
      if (result.ok() && !LocalCommandParser::validNegativePrompt(
                             outcome.text, true)) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        outcome.text.clear();
        return outcome;
      }
      break;
    case CommandKind::SetAigcNegativePrompt:
      result = adapter.setAigcNegativePrompt(command.text);
      outcome.text = command.text;
      break;
    case CommandKind::QueryDefaultRenderStrategy:
      result = adapter.queryDefaultRenderStrategy(outcome.text);
      if (result.ok() &&
          !LocalCommandParser::validRenderStrategyId(outcome.text)) {
        outcome.code = ExecutionCode::AdapterContractViolation;
        outcome.text.clear();
        return outcome;
      }
      break;
    case CommandKind::SetDefaultRenderStrategy:
      result = adapter.setDefaultRenderStrategy(command.text);
      outcome.text = command.text;
      break;
    case CommandKind::SetLedMaximumBrightness:
      result = adapter.setLedMaximumBrightness(
          static_cast<uint8_t>(command.number));
      outcome.percent = static_cast<uint8_t>(command.number);
      break;
    case CommandKind::None:
      outcome.code = ExecutionCode::Rejected;
      return outcome;
  }
  outcome.adapter_code = result.code;
  outcome.code = result.ok() ? ExecutionCode::Executed
                             : ExecutionCode::AdapterFailure;
  return outcome;
}

bool LocalToolsSession::validToken(std::string_view token) {
  if (token.size() < 16U || token.size() > kMaximumConfirmationTokenBytes)
    return false;
  for (unsigned char ch : token) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_')) return false;
  }
  return true;
}

bool LocalToolsSession::tokensEqual(std::string_view left,
                                    std::string_view right) {
  size_t difference = left.size() ^ right.size();
  const size_t common = std::min(left.size(), right.size());
  for (size_t index = 0; index < common; ++index)
    difference |= static_cast<unsigned char>(left[index]) ^
                  static_cast<unsigned char>(right[index]);
  return difference == 0U;
}

const char* commandName(CommandKind kind) {
  switch (kind) {
    case CommandKind::QueryStorage: return "storage.query";
    case CommandKind::ListAlbum: return "album.list";
    case CommandKind::SelectImageOrdinal: return "album.select_ordinal";
    case CommandKind::DeleteImageOrdinal: return "album.delete_ordinal";
    case CommandKind::DeleteImageId: return "album.delete_id";
    case CommandKind::ClearAlbum: return "album.clear";
    case CommandKind::QueryVolume: return "volume.query";
    case CommandKind::SetVolume: return "volume.set";
    case CommandKind::FormatTfCard: return "storage.format_tf";
    case CommandKind::QueryAssistantPrompt: return "prompt.assistant.query";
    case CommandKind::SetAssistantPrompt: return "prompt.assistant.set";
    case CommandKind::QueryAigcPrompt: return "prompt.aigc.query";
    case CommandKind::SetAigcPrompt: return "prompt.aigc.set";
    case CommandKind::QueryAigcSteps: return "image.steps.query";
    case CommandKind::SetAigcSteps: return "image.steps.set";
    case CommandKind::QueryAigcNegativePrompt:
      return "prompt.aigc_negative.query";
    case CommandKind::SetAigcNegativePrompt:
      return "prompt.aigc_negative.set";
    case CommandKind::QueryDefaultRenderStrategy:
      return "render.default.query";
    case CommandKind::SetDefaultRenderStrategy:
      return "render.default.set";
    case CommandKind::SetLedMaximumBrightness: return "led.maximum.set";
    case CommandKind::None: return "none";
  }
  return "none";
}

const char* parseCodeName(ParseCode code) {
  switch (code) {
    case ParseCode::Matched: return "MATCHED";
    case ParseCode::IgnoredEmpty: return "IGNORED_EMPTY";
    case ParseCode::IgnoredBlankAudio: return "IGNORED_BLANK_AUDIO";
    case ParseCode::NoMatch: return "NO_MATCH";
    case ParseCode::Ambiguous: return "AMBIGUOUS";
    case ParseCode::InvalidEncoding: return "INVALID_ENCODING";
    case ParseCode::TooLong: return "TOO_LONG";
    case ParseCode::InvalidValue: return "INVALID_VALUE";
  }
  return "NO_MATCH";
}

const char* executionCodeName(ExecutionCode code) {
  switch (code) {
    case ExecutionCode::Ignored: return "IGNORED";
    case ExecutionCode::Rejected: return "REJECTED";
    case ExecutionCode::ConfirmationRequired: return "CONFIRMATION_REQUIRED";
    case ExecutionCode::Executed: return "EXECUTED";
    case ExecutionCode::AdapterFailure: return "ADAPTER_FAILURE";
    case ExecutionCode::TokenUnavailable: return "TOKEN_UNAVAILABLE";
    case ExecutionCode::ConfirmationMissing: return "CONFIRMATION_MISSING";
    case ExecutionCode::ConfirmationMismatch: return "CONFIRMATION_MISMATCH";
    case ExecutionCode::ConfirmationExpired: return "CONFIRMATION_EXPIRED";
    case ExecutionCode::AdapterContractViolation:
      return "ADAPTER_CONTRACT_VIOLATION";
  }
  return "REJECTED";
}

}  // namespace local_tools
}  // namespace inkloop
