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
      if (first < 0x20U && first != '\t' && first != '\n' && first != '\r')
        return false;
      ++at;
      continue;
    }
    size_t length = 0;
    if (first >= 0xC2U && first <= 0xDFU) length = 2;
    else if (first >= 0xE0U && first <= 0xEFU) length = 3;
    else if (first >= 0xF0U && first <= 0xF4U) length = 4;
    else return false;
    if (length > value.size() - at) return false;
    for (size_t index = 1; index < length; ++index) {
      if ((static_cast<uint8_t>(value[at + index]) & 0xC0U) != 0x80U)
        return false;
    }
    const uint8_t second = static_cast<uint8_t>(value[at + 1]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second >= 0xA0U) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second >= 0x90U))
      return false;
    at += length;
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
  const std::array<std::array<std::string_view, 3>, 7> families{{
      {{"空间", "容量", "存储"}},
      {{"删除", "清空", "相册"}},
      {{"音量", "声音", "扬声器"}},
      {{"格式化", "tf卡", "tf 卡"}},
      {{"智能体提示词", "助手提示词", "系统提示词"}},
      {{"aigc图片提示词", "aigc 图片提示词", "图片生成提示词"}},
      {{"led", "灯光", "亮度"}},
  }};
  size_t count = 0;
  for (const auto& family : families) {
    bool present = false;
    for (std::string_view keyword : family) {
      if (text.find(keyword) != std::string_view::npos) present = true;
    }
    if (present) ++count;
  }
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
                   "剩余空间是多少"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Remaining;
    return matched(std::move(command));
  }
  if (oneOf(text, {"查询总空间", "查看总空间", "总空间是多少",
                   "查询总容量", "总容量是多少", "tf卡总容量是多少",
                   "tf 卡总容量是多少"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Total;
    return matched(std::move(command));
  }
  if (oneOf(text, {"查询总空间和剩余空间", "查询剩余空间和总空间",
                   "查询存储空间", "查看存储空间"})) {
    command.kind = CommandKind::QueryStorage;
    command.storage_metric = StorageMetric::Both;
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

  if (oneOf(text, {"清空相册", "清空所有图片", "删除所有图片"})) {
    command.kind = CommandKind::ClearAlbum;
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询音量", "查看音量", "当前音量", "当前音量是多少",
                   "音量是多少"})) {
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

  if (oneOf(text, {"格式化tf卡", "格式化 tf卡", "格式化tf 卡", "格式化 tf 卡"})) {
    command.kind = CommandKind::FormatTfCard;
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询智能体提示词", "查看智能体提示词",
                   "当前智能体提示词", "智能体提示词是什么"})) {
    command.kind = CommandKind::QueryAssistantPrompt;
    return matched(std::move(command));
  }
  bool recognized = false;
  std::string prompt = promptAfter(
      original, text,
      {"设置智能体提示词为", "把智能体提示词设置为", "设置智能体提示词：",
       "设置智能体提示词:"},
      recognized);
  if (recognized) {
    if (!validPrompt(prompt)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetAssistantPrompt;
    command.text = std::move(prompt);
    return matched(std::move(command));
  }

  if (oneOf(text, {"查询aigc图片提示词", "查看aigc图片提示词",
                   "当前aigc图片提示词", "aigc图片提示词是什么",
                   "查询图片生成提示词"})) {
    command.kind = CommandKind::QueryAigcPrompt;
    return matched(std::move(command));
  }
  prompt = promptAfter(
      original, text,
      {"设置aigc图片提示词为", "把aigc图片提示词设置为",
       "设置aigc图片提示词：", "设置aigc图片提示词:",
       "设置图片生成提示词为", "设置图片生成提示词：",
       "设置图片生成提示词:"},
      recognized);
  if (recognized) {
    if (!validPrompt(prompt)) return failed(ParseCode::InvalidValue);
    command.kind = CommandKind::SetAigcPrompt;
    command.text = std::move(prompt);
    return matched(std::move(command));
  }

  for (std::string_view prefix : {
           std::string_view("设置led最大亮度为"),
           std::string_view("把led最大亮度设置为"),
           std::string_view("设置led最大亮度")}) {
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
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == ':' || ch == '.'))
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
