#include "inkloop/storage/local_chat_log.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace inkloop {
namespace storage {
namespace {

size_t utf8SequenceLength(const std::string& value, size_t at) {
  if (at >= value.size()) return 0;
  const uint8_t first = static_cast<uint8_t>(value[at]);
  if (first < 0x80U) return 1;
  size_t length = 0;
  if (first >= 0xC2U && first <= 0xDFU) length = 2;
  else if (first >= 0xE0U && first <= 0xEFU) length = 3;
  else if (first >= 0xF0U && first <= 0xF4U) length = 4;
  else return 0;
  if (length > value.size() - at) return 0;
  for (size_t index = 1; index < length; ++index) {
    if ((static_cast<uint8_t>(value[at + index]) & 0xC0U) != 0x80U) return 0;
  }
  const uint8_t second = static_cast<uint8_t>(value[at + 1]);
  if ((first == 0xE0U && second < 0xA0U) ||
      (first == 0xEDU && second >= 0xA0U) ||
      (first == 0xF0U && second < 0x90U) ||
      (first == 0xF4U && second >= 0x90U)) return 0;
  return length;
}

std::string boundedUtf8(const std::string& input, size_t maximum_bytes) {
  std::string output;
  output.reserve(std::min(input.size(), maximum_bytes));
  size_t at = 0;
  while (at < input.size()) {
    const size_t length = utf8SequenceLength(input, at);
    if (length == 0) {
      ++at;
      continue;
    }
    const uint8_t first = static_cast<uint8_t>(input[at]);
    if (first < 0x20U && first != '\n' && first != '\t') {
      if (output.size() < maximum_bytes) output.push_back(' ');
      ++at;
      continue;
    }
    if (length > maximum_bytes - output.size()) break;
    output.append(input, at, length);
    at += length;
  }
  size_t first = 0;
  while (first < output.size() &&
         (output[first] == ' ' || output[first] == '\n' ||
          output[first] == '\t')) ++first;
  size_t last = output.size();
  while (last > first &&
         (output[last - 1] == ' ' || output[last - 1] == '\n' ||
          output[last - 1] == '\t')) --last;
  return output.substr(first, last - first);
}

void appendHexEscape(uint8_t value, std::string& output) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.append("\\u00");
  output.push_back(kHex[(value >> 4U) & 0xFU]);
  output.push_back(kHex[value & 0xFU]);
}

bool appendJsonString(const std::string& input, std::string& output,
                      size_t maximum_line_bytes) {
  output.push_back('"');
  for (unsigned char ch : input) {
    switch (ch) {
      case '"': output.append("\\\""); break;
      case '\\': output.append("\\\\"); break;
      case '\n': output.append("\\n"); break;
      case '\t': output.append("\\t"); break;
      default:
        if (ch < 0x20U) appendHexEscape(ch, output);
        else output.push_back(static_cast<char>(ch));
        break;
    }
    if (output.size() >= maximum_line_bytes) return false;
  }
  output.push_back('"');
  return output.size() <= maximum_line_bytes;
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

void appendCodepoint(uint32_t codepoint, std::string& output) {
  if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

class JsonCursor {
 public:
  JsonCursor(const char* data, size_t length) : data_(data), length_(length) {}

  bool objectStart() { skipWhitespace(); return consume('{'); }
  bool objectEnd() { skipWhitespace(); return consume('}'); }
  bool comma() { skipWhitespace(); return consume(','); }
  bool colon() { skipWhitespace(); return consume(':'); }
  bool atEnd() { skipWhitespace(); return at_ == length_; }
  bool nextIsObjectEnd() { skipWhitespace(); return at_ < length_ && data_[at_] == '}'; }

  bool string(std::string& output, size_t maximum_bytes) {
    skipWhitespace();
    if (!consume('"')) return false;
    output.clear();
    while (at_ < length_) {
      const uint8_t ch = static_cast<uint8_t>(data_[at_++]);
      if (ch == '"') return output.size() <= maximum_bytes;
      if (ch < 0x20U) return false;
      if (ch != '\\') {
        output.push_back(static_cast<char>(ch));
      } else {
        if (at_ >= length_) return false;
        const char escaped = data_[at_++];
        switch (escaped) {
          case '"': output.push_back('"'); break;
          case '\\': output.push_back('\\'); break;
          case '/': output.push_back('/'); break;
          case 'b': output.push_back('\b'); break;
          case 'f': output.push_back('\f'); break;
          case 'n': output.push_back('\n'); break;
          case 'r': output.push_back('\r'); break;
          case 't': output.push_back('\t'); break;
          case 'u': {
            uint32_t first = 0;
            if (!hexQuad(first)) return false;
            uint32_t codepoint = first;
            if (first >= 0xD800U && first <= 0xDBFFU) {
              if (at_ + 2 > length_ || data_[at_] != '\\' ||
                  data_[at_ + 1] != 'u') return false;
              at_ += 2;
              uint32_t second = 0;
              if (!hexQuad(second) || second < 0xDC00U || second > 0xDFFFU)
                return false;
              codepoint = 0x10000U + ((first - 0xD800U) << 10U) +
                          (second - 0xDC00U);
            } else if (first >= 0xDC00U && first <= 0xDFFFU) {
              return false;
            }
            appendCodepoint(codepoint, output);
            break;
          }
          default: return false;
        }
      }
      if (output.size() > maximum_bytes) return false;
    }
    return false;
  }

  bool unsignedInteger(uint64_t& output) {
    skipWhitespace();
    if (at_ >= length_ || data_[at_] < '0' || data_[at_] > '9') return false;
    if (data_[at_] == '0' && at_ + 1 < length_ &&
        data_[at_ + 1] >= '0' && data_[at_ + 1] <= '9') return false;
    uint64_t value = 0;
    while (at_ < length_ && data_[at_] >= '0' && data_[at_] <= '9') {
      const uint8_t digit = static_cast<uint8_t>(data_[at_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
        return false;
      value = value * 10U + digit;
      ++at_;
    }
    output = value;
    return true;
  }

  bool skipScalar() {
    skipWhitespace();
    if (at_ >= length_) return false;
    if (data_[at_] == '"') {
      std::string ignored;
      return string(ignored, kMaximumChatLineBytes);
    }
    uint64_t ignored_number = 0;
    if (data_[at_] >= '0' && data_[at_] <= '9')
      return unsignedInteger(ignored_number);
    static constexpr const char* kLiterals[] = {"true", "false", "null"};
    for (const char* literal : kLiterals) {
      size_t index = 0;
      while (literal[index] != '\0' && at_ + index < length_ &&
             data_[at_ + index] == literal[index]) ++index;
      if (literal[index] == '\0') {
        at_ += index;
        return true;
      }
    }
    return false;
  }

 private:
  bool consume(char expected) {
    if (at_ >= length_ || data_[at_] != expected) return false;
    ++at_;
    return true;
  }

  void skipWhitespace() {
    while (at_ < length_ && (data_[at_] == ' ' || data_[at_] == '\t' ||
                             data_[at_] == '\r' || data_[at_] == '\n')) ++at_;
  }

  bool hexQuad(uint32_t& output) {
    if (at_ + 4 > length_) return false;
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
      const int digit = hexValue(data_[at_++]);
      if (digit < 0) return false;
      value = (value << 4U) | static_cast<uint32_t>(digit);
    }
    output = value;
    return true;
  }

  const char* data_;
  size_t length_;
  size_t at_ = 0;
};

bool parseKind(const std::string& role, const std::string& kind,
               ChatRecordKind& output) {
  if (kind.empty()) {
    if (role == "user") output = ChatRecordKind::AsrFinal;
    else if (role == "assistant") output = ChatRecordKind::AssistantFinal;
    else if (role == "tool") output = ChatRecordKind::ToolState;
    else return false;
    return true;
  }
  if (kind == "asr.final" && role == "user") output = ChatRecordKind::AsrFinal;
  else if (kind == "assistant.final" && role == "assistant")
    output = ChatRecordKind::AssistantFinal;
  else if (kind == "tool.state" && role == "tool")
    output = ChatRecordKind::ToolState;
  else if (kind == "aigc.state" && role == "tool")
    output = ChatRecordKind::AigcState;
  else return false;
  return true;
}

enum class ParseRecordResult : uint8_t {
  Valid,
  IgnoredArtifact,
  Malformed,
};

ParseRecordResult parseRecord(const char* line, size_t length,
                              ChatRecord& output) {
  if (!line || length == 0 || length > kMaximumChatLineBytes)
    return ParseRecordResult::Malformed;
  JsonCursor cursor(line, length);
  if (!cursor.objectStart()) return ParseRecordResult::Malformed;
  bool have_sequence = false;
  bool have_role = false;
  bool have_text = false;
  bool have_kind = false;
  bool have_time = false;
  bool have_version = false;
  uint64_t version = 1;
  uint64_t sequence = 0;
  std::string role;
  std::string kind;
  std::string utc;
  std::string text;
  bool first_field = true;
  while (!cursor.nextIsObjectEnd()) {
    if (!first_field && !cursor.comma()) return ParseRecordResult::Malformed;
    first_field = false;
    std::string key;
    if (!cursor.string(key, 32) || !cursor.colon())
      return ParseRecordResult::Malformed;
    if (key == "v") {
      if (have_version || !cursor.unsignedInteger(version))
        return ParseRecordResult::Malformed;
      have_version = true;
    } else if (key == "sequence") {
      if (have_sequence || !cursor.unsignedInteger(sequence))
        return ParseRecordResult::Malformed;
      have_sequence = true;
    } else if (key == "role") {
      if (have_role || !cursor.string(role, 16))
        return ParseRecordResult::Malformed;
      have_role = true;
    } else if (key == "kind") {
      if (have_kind || !cursor.string(kind, 24))
        return ParseRecordResult::Malformed;
      have_kind = true;
    } else if (key == "time") {
      if (have_time || !cursor.string(utc, 64))
        return ParseRecordResult::Malformed;
      have_time = true;
    } else if (key == "text") {
      if (have_text || !cursor.string(text, kMaximumChatTextBytes))
        return ParseRecordResult::Malformed;
      have_text = true;
    } else if (!cursor.skipScalar()) {
      return ParseRecordResult::Malformed;
    }
  }
  if (!cursor.objectEnd() || !cursor.atEnd() || !have_sequence ||
      !have_role || !have_text || version != 1 || sequence == 0 ||
      sequence == std::numeric_limits<uint64_t>::max())
    return ParseRecordResult::Malformed;
  output.sequence = sequence;
  output.utc = boundedUtf8(utc, 64);
  output.text = boundedUtf8(text, kMaximumChatTextBytes);
  if (!parseKind(role, kind, output.kind)) return ParseRecordResult::Malformed;
  if (output.text.empty()) return ParseRecordResult::IgnoredArtifact;
  if (output.kind == ChatRecordKind::AsrFinal &&
      LocalChatLog::isBlankAudioArtifact(output.text))
    return ParseRecordResult::IgnoredArtifact;
  return ParseRecordResult::Valid;
}

std::string encodeRecord(const ChatRecord& record) {
  std::string output = "{\"v\":1,\"sequence\":" +
                       std::to_string(record.sequence) + ",\"time\":";
  if (!appendJsonString(record.utc, output, kMaximumChatLineBytes)) return {};
  output.append(",\"role\":\"");
  output.append(LocalChatLog::roleName(record.kind));
  output.append("\",\"kind\":\"");
  output.append(LocalChatLog::kindName(record.kind));
  output.append("\",\"text\":");
  if (!appendJsonString(record.text, output, kMaximumChatLineBytes)) return {};
  output.push_back('}');
  return output.size() <= kMaximumChatLineBytes ? output : std::string();
}

class RecoveryVisitor final : public IChatLineVisitor {
 public:
  bool onLine(const char* line, size_t length) override {
    ChatRecord record;
    const ParseRecordResult parsed = parseRecord(line, length, record);
    if (parsed == ParseRecordResult::Malformed ||
        record.sequence <= last_sequence_) {
      corruption = true;
      return true;
    }
    last_sequence_ = record.sequence;
    if (parsed == ParseRecordResult::Valid) ++valid_records;
    return true;
  }
  bool onMalformedLine() override { corruption = true; return true; }

  uint64_t last_sequence_ = 0;
  size_t valid_records = 0;
  bool corruption = false;
};

class PageVisitor final : public IChatLineVisitor {
 public:
  PageVisitor(uint64_t after, size_t limit, ChatPage& target)
      : after_(after), limit_(limit), target_(target) {}

  bool onLine(const char* line, size_t length) override {
    ChatRecord record;
    const ParseRecordResult parsed = parseRecord(line, length, record);
    if (parsed == ParseRecordResult::Malformed || record.sequence <= last_seen_) {
      target_.corruption_observed = true;
      return true;
    }
    last_seen_ = record.sequence;
    if (parsed == ParseRecordResult::IgnoredArtifact) return true;
    if (record.sequence <= after_) return true;
    if (target_.records.size() >= limit_) {
      target_.has_more = true;
      return false;
    }
    if (aggregate_text_ + record.text.size() > kMaximumChatPageTextBytes) {
      target_.has_more = true;
      return false;
    }
    aggregate_text_ += record.text.size();
    target_.next_cursor = record.sequence;
    target_.records.push_back(std::move(record));
    return true;
  }

  bool onMalformedLine() override {
    target_.corruption_observed = true;
    return true;
  }

 private:
  uint64_t after_;
  size_t limit_;
  ChatPage& target_;
  uint64_t last_seen_ = 0;
  size_t aggregate_text_ = 0;
};

}  // namespace

LocalChatLog::LocalChatLog(IChatLineStore& store, size_t rotate_at_bytes)
    : store_(store), rotate_at_bytes_(rotate_at_bytes) {}

ChatLogResult LocalChatLog::recover(ChatRecovery& recovery) {
  recovery = ChatRecovery();
  if (rotate_at_bytes_ < 64 || rotate_at_bytes_ > kDefaultChatLogBytes)
    return {ChatLogCode::InvalidArgument};
  RecoveryVisitor visitor;
  const ChatLogResult result = store_.scan(visitor);
  if (!result.ok()) return result;
  if (visitor.last_sequence_ == std::numeric_limits<uint64_t>::max() - 1U)
    return {ChatLogCode::SequenceExhausted};
  next_sequence_ = visitor.last_sequence_ + 1U;
  corruption_observed_ = visitor.corruption;
  ready_ = true;
  recovery.next_sequence = next_sequence_;
  recovery.valid_records = visitor.valid_records;
  recovery.corruption_observed = corruption_observed_;
  recovery.rotated_history_present = store_.rotatedHistoryPresent();
  return {};
}

ChatLogResult LocalChatLog::appendAsr(const std::string& text, bool final,
                                      const std::string& utc) {
  if (!final) return {ChatLogCode::IgnoredPartial};
  const std::string bounded = boundedUtf8(text, kMaximumChatTextBytes);
  if (bounded.empty()) return {ChatLogCode::IgnoredEmpty};
  if (isBlankAudioArtifact(bounded)) return {ChatLogCode::IgnoredBlankAudio};
  return append(ChatRecordKind::AsrFinal, bounded, utc);
}

ChatLogResult LocalChatLog::appendAssistant(const std::string& text, bool final,
                                            const std::string& utc) {
  if (!final) return {ChatLogCode::IgnoredPartial};
  return append(ChatRecordKind::AssistantFinal, text, utc);
}

ChatLogResult LocalChatLog::appendToolState(const std::string& text,
                                            const std::string& utc) {
  return append(ChatRecordKind::ToolState, text, utc);
}

ChatLogResult LocalChatLog::appendAigcState(const std::string& text,
                                            const std::string& utc) {
  return append(ChatRecordKind::AigcState, text, utc);
}

ChatLogResult LocalChatLog::append(ChatRecordKind kind,
                                   const std::string& text,
                                   const std::string& utc) {
  if (!ready_) return {ChatLogCode::NotReady};
  if (next_sequence_ == 0 ||
      next_sequence_ == std::numeric_limits<uint64_t>::max())
    return {ChatLogCode::SequenceExhausted};
  ChatRecord record;
  record.sequence = next_sequence_;
  record.kind = kind;
  record.utc = boundedUtf8(utc, 64);
  if (record.utc.empty()) record.utc = "unknown";
  record.text = boundedUtf8(text, kMaximumChatTextBytes);
  if (record.text.empty()) return {ChatLogCode::IgnoredEmpty};
  const std::string line = encodeRecord(record);
  if (line.empty()) return {ChatLogCode::TooLarge};
  const ChatLogResult result = store_.appendLine(line, rotate_at_bytes_);
  if (!result.ok()) return result;
  ++next_sequence_;
  return {};
}

ChatLogResult LocalChatLog::readPage(uint64_t after_sequence, size_t limit,
                                     ChatPage& page) const {
  page = ChatPage();
  page.next_cursor = after_sequence;
  page.corruption_observed = corruption_observed_;
  if (!ready_) return {ChatLogCode::NotReady};
  if (limit == 0 || limit > kMaximumChatPageItems)
    return {ChatLogCode::InvalidArgument};
  PageVisitor visitor(after_sequence, limit, page);
  return store_.scan(visitor);
}

ChatLogResult LocalChatLog::clear() {
  if (!ready_) return {ChatLogCode::NotReady};
  const ChatLogResult result = store_.clear();
  if (!result.ok()) return result;
  next_sequence_ = 1;
  corruption_observed_ = false;
  return {};
}

const char* LocalChatLog::roleName(ChatRecordKind kind) {
  switch (kind) {
    case ChatRecordKind::AsrFinal: return "user";
    case ChatRecordKind::AssistantFinal: return "assistant";
    case ChatRecordKind::ToolState:
    case ChatRecordKind::AigcState: return "tool";
  }
  return "tool";
}

const char* LocalChatLog::kindName(ChatRecordKind kind) {
  switch (kind) {
    case ChatRecordKind::AsrFinal: return "asr.final";
    case ChatRecordKind::AssistantFinal: return "assistant.final";
    case ChatRecordKind::ToolState: return "tool.state";
    case ChatRecordKind::AigcState: return "aigc.state";
  }
  return "tool.state";
}

bool LocalChatLog::isBlankAudioArtifact(const std::string& text) {
  std::string normalized = boundedUtf8(text, 64);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch + 32U);
                   if (ch == ' ' || ch == '-') return '_';
                   return static_cast<char>(ch);
                 });
  if (normalized.size() >= 2 && normalized.front() == '[' &&
      normalized.back() == ']')
    normalized = normalized.substr(1, normalized.size() - 2);
  return normalized == "blank_audio";
}

const char* chatLogCodeName(ChatLogCode code) {
  switch (code) {
    case ChatLogCode::Ok: return "OK";
    case ChatLogCode::IgnoredPartial: return "IGNORED_PARTIAL";
    case ChatLogCode::IgnoredEmpty: return "IGNORED_EMPTY";
    case ChatLogCode::IgnoredBlankAudio: return "IGNORED_BLANK_AUDIO";
    case ChatLogCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ChatLogCode::NotReady: return "NOT_READY";
    case ChatLogCode::TooLarge: return "TOO_LARGE";
    case ChatLogCode::IoError: return "IO_ERROR";
    case ChatLogCode::SequenceExhausted: return "SEQUENCE_EXHAUSTED";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
