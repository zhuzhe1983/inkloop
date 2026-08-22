#include "inkloop/storage/album_index.hpp"

#include <algorithm>
#include <limits>

namespace inkloop {
namespace storage {
namespace {

class JsonCursor {
 public:
  explicit JsonCursor(const std::string& input) : input_(input) {}

  bool consume(char expected) {
    whitespace();
    if (at_ >= input_.size() || input_[at_] != expected) return false;
    ++at_;
    return true;
  }
  bool next(char expected) {
    whitespace();
    return at_ < input_.size() && input_[at_] == expected;
  }
  bool end() {
    whitespace();
    return at_ == input_.size();
  }
  bool boolean(bool& output) {
    whitespace();
    if (literal("true")) {
      output = true;
      return true;
    }
    if (literal("false")) {
      output = false;
      return true;
    }
    return false;
  }
  bool unsignedInteger(uint64_t& output) {
    whitespace();
    if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9')
      return false;
    if (input_[at_] == '0' && at_ + 1U < input_.size() &&
        input_[at_ + 1U] >= '0' && input_[at_ + 1U] <= '9') return false;
    uint64_t value = 0;
    while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') {
      const uint8_t digit = static_cast<uint8_t>(input_[at_] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
        return false;
      value = value * 10U + digit;
      ++at_;
    }
    output = value;
    return true;
  }
  bool string(std::string& output, size_t maximum) {
    whitespace();
    if (at_ >= input_.size() || input_[at_++] != '"') return false;
    output.clear();
    while (at_ < input_.size()) {
      const uint8_t ch = static_cast<uint8_t>(input_[at_++]);
      if (ch == '"') return output.size() <= maximum;
      if (ch < 0x20U) return false;
      if (ch != '\\') output.push_back(static_cast<char>(ch));
      else {
        if (at_ >= input_.size()) return false;
        const char escaped = input_[at_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/')
          output.push_back(escaped);
        else if (escaped == 'b') output.push_back('\b');
        else if (escaped == 'f') output.push_back('\f');
        else if (escaped == 'n') output.push_back('\n');
        else if (escaped == 'r') output.push_back('\r');
        else if (escaped == 't') output.push_back('\t');
        else if (escaped == 'u') {
          uint32_t first = 0;
          if (!hexQuad(first)) return false;
          uint32_t codepoint = first;
          if (first >= 0xd800U && first <= 0xdbffU) {
            if (at_ + 2U > input_.size() || input_[at_] != '\\' ||
                input_[at_ + 1U] != 'u') return false;
            at_ += 2U;
            uint32_t second = 0;
            if (!hexQuad(second) || second < 0xdc00U || second > 0xdfffU)
              return false;
            codepoint = 0x10000U + ((first - 0xd800U) << 10U) +
                        (second - 0xdc00U);
          } else if (first >= 0xdc00U && first <= 0xdfffU) return false;
          appendCodepoint(codepoint, output);
        } else return false;
      }
      if (output.size() > maximum) return false;
    }
    return false;
  }

 private:
  void whitespace() {
    while (at_ < input_.size() &&
           (input_[at_] == ' ' || input_[at_] == '\t' ||
            input_[at_] == '\r' || input_[at_] == '\n')) ++at_;
  }
  bool literal(const char* value) {
    size_t index = 0;
    while (value[index] && at_ + index < input_.size() &&
           input_[at_ + index] == value[index]) ++index;
    if (value[index]) return false;
    at_ += index;
    return true;
  }
  static int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
  }
  bool hexQuad(uint32_t& output) {
    if (at_ + 4U > input_.size()) return false;
    uint32_t value = 0;
    for (size_t index = 0; index < 4U; ++index) {
      const int digit = hexValue(input_[at_++]);
      if (digit < 0) return false;
      value = (value << 4U) | static_cast<uint32_t>(digit);
    }
    output = value;
    return true;
  }
  static void appendCodepoint(uint32_t value, std::string& output) {
    if (value <= 0x7fU) output.push_back(static_cast<char>(value));
    else if (value <= 0x7ffU) {
      output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
      output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
      output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
  }

  const std::string& input_;
  size_t at_ = 0;
};

bool validScopedPath(const std::string& path, const std::string& content_sha) {
  return path == std::string("/inkloop-album/") + content_sha + ".png";
}

bool validAsset(const AlbumIndexAsset& asset) {
  return validAlbumAssetId(asset.id) &&
         validAlbumAssetId(asset.content_sha256) &&
         validScopedPath(asset.path, asset.content_sha256) &&
         asset.bytes >= 45U && asset.bytes <= kMaximumAlbumAssetBytes &&
         asset.task_id.size() <= 256U &&
         validRenderStrategy(asset.render_strategy);
}

bool parseAsset(JsonCursor& cursor, AlbumIndexAsset& asset) {
  if (!cursor.consume('{')) return false;
  bool id = false;
  bool path = false;
  bool content = false;
  bool bytes = false;
  bool landscape = false;
  bool created = false;
  bool task = false;
  bool render = false;
  while (!cursor.next('}')) {
    std::string key;
    if (!cursor.string(key, 32U) || !cursor.consume(':')) return false;
    bool field_ok = false;
    if (key == "id" && !id) field_ok = id = cursor.string(asset.id, 64U);
    else if (key == "path" && !path)
      field_ok = path = cursor.string(asset.path, 96U);
    else if (key == "contentSha256" && !content)
      field_ok = content = cursor.string(asset.content_sha256, 64U);
    else if (key == "bytes" && !bytes) {
      uint64_t value = 0;
      bytes = cursor.unsignedInteger(value) &&
              value <= std::numeric_limits<size_t>::max();
      if (bytes) asset.bytes = static_cast<size_t>(value);
      field_ok = bytes;
    } else if (key == "landscape" && !landscape)
      field_ok = landscape = cursor.boolean(asset.landscape);
    else if (key == "created" && !created) {
      uint64_t value = 0;
      created = cursor.unsignedInteger(value) && value <= 0xffffffffULL;
      if (created) asset.created = static_cast<uint32_t>(value);
      field_ok = created;
    } else if (key == "taskId" && !task)
      field_ok = task = cursor.string(asset.task_id, 256U);
    else if (key == "renderStrategy" && !render)
      field_ok = render = cursor.string(asset.render_strategy, 64U);
    else return false;
    if (!field_ok) return false;
    if (!cursor.next('}')) {
      if (!cursor.consume(',')) return false;
    }
  }
  if (!cursor.consume('}')) return false;
  if (!content && id && path && validScopedPath(asset.path, asset.id)) {
    asset.content_sha256 = asset.id;
    content = true;
  }
  return id && path && content && bytes && landscape && created && task &&
         validAsset(asset);
}

void appendEscaped(const std::string& value, std::string& output) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.push_back('"');
  for (unsigned char ch : value) {
    if (ch == '"') output.append("\\\"");
    else if (ch == '\\') output.append("\\\\");
    else if (ch == '\n') output.append("\\n");
    else if (ch == '\r') output.append("\\r");
    else if (ch == '\t') output.append("\\t");
    else if (ch < 0x20U) {
      output.append("\\u00");
      output.push_back(kHex[ch >> 4U]);
      output.push_back(kHex[ch & 0x0fU]);
    } else output.push_back(static_cast<char>(ch));
  }
  output.push_back('"');
}

}  // namespace

bool validAlbumAssetId(const std::string& value) {
  if (value.size() != 64U) return false;
  for (char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}

bool validRenderStrategy(const std::string& value) {
  return value == "official-quality" || value == "classic-six-color" ||
         value == "reflectance-photo" || value == "solid-clean";
}

AlbumIndexCode parseAlbumIndex(const std::string& json, AlbumIndex& output) {
  output = AlbumIndex();
  if (json.empty()) return AlbumIndexCode::InvalidArgument;
  if (json.size() > kMaximumAlbumIndexBytes) return AlbumIndexCode::TooLarge;
  JsonCursor cursor(json);
  if (!cursor.consume('{')) return AlbumIndexCode::InvalidJson;
  bool schema = false;
  bool current = false;
  bool current_render = false;
  bool assets = false;
  while (!cursor.next('}')) {
    std::string key;
    if (!cursor.string(key, 32U) || !cursor.consume(':'))
      return AlbumIndexCode::InvalidJson;
    if (key == "schema" && !schema) {
      uint64_t value = 0;
      schema = cursor.unsignedInteger(value) && value == 1U;
      if (!schema) return AlbumIndexCode::InvalidSchema;
    } else if (key == "current" && !current) {
      current = cursor.string(output.current, 64U);
      if (!current) return AlbumIndexCode::InvalidCurrent;
    } else if (key == "currentRenderStrategy" && !current_render) {
      current_render = cursor.string(output.current_render_strategy, 64U);
      if (!current_render ||
          (!output.current_render_strategy.empty() &&
           !validRenderStrategy(output.current_render_strategy)))
        return AlbumIndexCode::InvalidCurrent;
    } else if (key == "assets" && !assets) {
      if (!cursor.consume('[')) return AlbumIndexCode::InvalidJson;
      assets = true;
      while (!cursor.next(']')) {
        if (output.assets.size() >= kMaximumAlbumEntries)
          return AlbumIndexCode::TooLarge;
        AlbumIndexAsset asset;
        if (!parseAsset(cursor, asset)) return AlbumIndexCode::InvalidAsset;
        for (const AlbumIndexAsset& existing : output.assets)
          if (existing.id == asset.id) return AlbumIndexCode::DuplicateAsset;
        output.assets.push_back(std::move(asset));
        if (!cursor.next(']') && !cursor.consume(','))
          return AlbumIndexCode::InvalidJson;
      }
      if (!cursor.consume(']')) return AlbumIndexCode::InvalidJson;
    } else return AlbumIndexCode::InvalidJson;
    if (!cursor.next('}') && !cursor.consume(','))
      return AlbumIndexCode::InvalidJson;
  }
  if (!cursor.consume('}') || !cursor.end() || !schema || !current || !assets)
    return AlbumIndexCode::InvalidJson;
  if (!output.current.empty()) {
    bool found = false;
    for (const AlbumIndexAsset& asset : output.assets)
      if (asset.id == output.current) found = true;
    if (!found) return AlbumIndexCode::InvalidCurrent;
  } else if (!output.current_render_strategy.empty()) {
    return AlbumIndexCode::InvalidCurrent;
  }
  return AlbumIndexCode::Ok;
}

AlbumIndexCode encodeAlbumIndex(const AlbumIndex& input, std::string& json) {
  json.clear();
  if (input.assets.size() > kMaximumAlbumEntries)
    return AlbumIndexCode::TooLarge;
  if (!input.current.empty()) {
    bool found = false;
    for (const AlbumIndexAsset& asset : input.assets)
      if (asset.id == input.current) found = true;
    if (!found) return AlbumIndexCode::InvalidCurrent;
    if (!input.current_render_strategy.empty() &&
        !validRenderStrategy(input.current_render_strategy))
      return AlbumIndexCode::InvalidCurrent;
  } else if (!input.current_render_strategy.empty()) {
    return AlbumIndexCode::InvalidCurrent;
  }
  json.append("{\"schema\":1,\"current\":");
  appendEscaped(input.current, json);
  json.append(",\"currentRenderStrategy\":");
  appendEscaped(input.current_render_strategy, json);
  json.append(",\"assets\":[");
  for (size_t index = 0; index < input.assets.size(); ++index) {
    const AlbumIndexAsset& asset = input.assets[index];
    if (!validAsset(asset)) return AlbumIndexCode::InvalidAsset;
    for (size_t prior = 0; prior < index; ++prior)
      if (input.assets[prior].id == asset.id)
        return AlbumIndexCode::DuplicateAsset;
    if (index) json.push_back(',');
    json.append("{\"id\":");
    appendEscaped(asset.id, json);
    json.append(",\"path\":");
    appendEscaped(asset.path, json);
    json.append(",\"contentSha256\":");
    appendEscaped(asset.content_sha256, json);
    json.append(",\"bytes\":");
    json.append(std::to_string(asset.bytes));
    json.append(",\"landscape\":");
    json.append(asset.landscape ? "true" : "false");
    json.append(",\"created\":");
    json.append(std::to_string(asset.created));
    json.append(",\"taskId\":");
    appendEscaped(asset.task_id, json);
    json.append(",\"renderStrategy\":");
    appendEscaped(asset.render_strategy, json);
    json.push_back('}');
    if (json.size() > kMaximumAlbumIndexBytes) {
      json.clear();
      return AlbumIndexCode::TooLarge;
    }
  }
  json.append("]}");
  if (json.size() > kMaximumAlbumIndexBytes) {
    json.clear();
    return AlbumIndexCode::TooLarge;
  }
  return AlbumIndexCode::Ok;
}

const char* albumIndexCodeName(AlbumIndexCode code) {
  switch (code) {
    case AlbumIndexCode::Ok: return "ok";
    case AlbumIndexCode::InvalidArgument: return "invalid_argument";
    case AlbumIndexCode::TooLarge: return "too_large";
    case AlbumIndexCode::InvalidJson: return "invalid_json";
    case AlbumIndexCode::InvalidSchema: return "invalid_schema";
    case AlbumIndexCode::InvalidAsset: return "invalid_asset";
    case AlbumIndexCode::DuplicateAsset: return "duplicate_asset";
    case AlbumIndexCode::InvalidCurrent: return "invalid_current";
  }
  return "unknown";
}

}  // namespace storage
}  // namespace inkloop
