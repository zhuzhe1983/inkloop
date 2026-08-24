#include "inkloop/settings/legacy_portal_import.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "inkloop/settings/settings_journal.hpp"

namespace inkloop {
namespace settings {
namespace {

SettingsStatus corrupt(const char* detail) {
  return {SettingsError::Corrupt, detail};
}

class JsonCursor {
 public:
  explicit JsonCursor(const std::string& input) : input_(input) {}

  bool consume(char expected) {
    whitespace();
    if (at_ >= input_.size() || input_[at_] != expected) return false;
    ++at_;
    return true;
  }

  bool string(std::string& output, std::size_t maximum) {
    whitespace();
    if (at_ >= input_.size() || input_[at_++] != '"') return false;
    output.clear();
    while (at_ < input_.size()) {
      const std::uint8_t ch = static_cast<std::uint8_t>(input_[at_++]);
      if (ch == '"') return validUtf8Text(output, maximum, true);
      if (ch < 0x20U) return false;
      if (ch != '\\') {
        output.push_back(static_cast<char>(ch));
      } else {
        if (at_ >= input_.size()) return false;
        const char escaped = input_[at_++];
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
            std::uint32_t first = 0U;
            if (!hexQuad(first)) return false;
            std::uint32_t scalar = first;
            if (first >= 0xD800U && first <= 0xDBFFU) {
              if (at_ + 2U > input_.size() || input_[at_] != '\\' ||
                  input_[at_ + 1U] != 'u')
                return false;
              at_ += 2U;
              std::uint32_t second = 0U;
              if (!hexQuad(second) || second < 0xDC00U || second > 0xDFFFU)
                return false;
              scalar = 0x10000U + ((first - 0xD800U) << 10U) +
                  (second - 0xDC00U);
            } else if (first >= 0xDC00U && first <= 0xDFFFU) {
              return false;
            }
            appendUtf8(scalar, output);
            break;
          }
          default: return false;
        }
      }
      if (output.size() > maximum) return false;
    }
    return false;
  }

  bool unsigned64(std::uint64_t& output) {
    whitespace();
    if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9')
      return false;
    if (input_[at_] == '0' && at_ + 1U < input_.size() &&
        input_[at_ + 1U] >= '0' && input_[at_ + 1U] <= '9')
      return false;
    std::uint64_t value = 0U;
    while (at_ < input_.size() && input_[at_] >= '0' &&
           input_[at_] <= '9') {
      const std::uint8_t digit =
          static_cast<std::uint8_t>(input_[at_] - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
        return false;
      value = value * 10U + digit;
      ++at_;
    }
    output = value;
    return true;
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

  bool skipValue(std::uint8_t depth = 0U) {
    if (depth > 12U) return false;
    whitespace();
    if (at_ >= input_.size()) return false;
    if (input_[at_] == '"') {
      std::string ignored;
      return string(ignored, kMaximumLegacyPortalRecordBytes);
    }
    if (input_[at_] == '{') {
      ++at_;
      whitespace();
      if (at_ < input_.size() && input_[at_] == '}') {
        ++at_;
        return true;
      }
      while (true) {
        std::string key;
        if (!string(key, 128U) || !consume(':') || !skipValue(depth + 1U))
          return false;
        whitespace();
        if (at_ < input_.size() && input_[at_] == '}') {
          ++at_;
          return true;
        }
        if (at_ >= input_.size() || input_[at_++] != ',') return false;
      }
    }
    if (input_[at_] == '[') {
      ++at_;
      whitespace();
      if (at_ < input_.size() && input_[at_] == ']') {
        ++at_;
        return true;
      }
      while (true) {
        if (!skipValue(depth + 1U)) return false;
        whitespace();
        if (at_ < input_.size() && input_[at_] == ']') {
          ++at_;
          return true;
        }
        if (at_ >= input_.size() || input_[at_++] != ',') return false;
      }
    }
    bool ignored_bool = false;
    if (input_.compare(at_, 4U, "true") == 0 ||
        input_.compare(at_, 5U, "false") == 0)
      return boolean(ignored_bool);
    if (literal("null")) return true;
    if (input_[at_] == '-') ++at_;
    const std::size_t number_start = at_;
    while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9')
      ++at_;
    if (at_ == number_start) return false;
    if (at_ < input_.size() && input_[at_] == '.') {
      ++at_;
      const std::size_t fraction_start = at_;
      while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9')
        ++at_;
      if (at_ == fraction_start) return false;
    }
    if (at_ < input_.size() &&
        (input_[at_] == 'e' || input_[at_] == 'E')) {
      ++at_;
      if (at_ < input_.size() &&
          (input_[at_] == '+' || input_[at_] == '-'))
        ++at_;
      const std::size_t exponent_start = at_;
      while (at_ < input_.size() && input_[at_] >= '0' &&
             input_[at_] <= '9')
        ++at_;
      if (at_ == exponent_start) return false;
    }
    return true;
  }

  bool end() {
    whitespace();
    return at_ == input_.size();
  }

 private:
  void whitespace() {
    while (at_ < input_.size() &&
           (input_[at_] == ' ' || input_[at_] == '\t' ||
            input_[at_] == '\r' || input_[at_] == '\n'))
      ++at_;
  }

  bool literal(const char* expected) {
    const std::string text(expected);
    if (text.size() > input_.size() - at_ ||
        input_.compare(at_, text.size(), text) != 0)
      return false;
    at_ += text.size();
    return true;
  }

  static int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
  }

  bool hexQuad(std::uint32_t& output) {
    if (at_ + 4U > input_.size()) return false;
    std::uint32_t value = 0U;
    for (std::uint8_t index = 0U; index < 4U; ++index) {
      const int digit = hexValue(input_[at_++]);
      if (digit < 0) return false;
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    output = value;
    return true;
  }

  static void appendUtf8(std::uint32_t scalar, std::string& output) {
    if (scalar <= 0x7FU) {
      output.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7FFU) {
      output.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else if (scalar <= 0xFFFFU) {
      output.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
      output.push_back(
          static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else {
      output.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
      output.push_back(
          static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
      output.push_back(
          static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    }
  }

  const std::string& input_;
  std::size_t at_ = 0U;
};

bool validLowerSha256(const std::string& value) {
  return value.size() == 64U &&
      std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      });
}

bool parseEnvelope(const std::string& input, std::string& payload,
                   std::string& checksum) {
  JsonCursor cursor(input);
  if (!cursor.consume('{')) return false;
  bool have_payload = false;
  bool have_checksum = false;
  bool first = true;
  while (true) {
    if (cursor.consume('}')) break;
    if (!first && !cursor.consume(',')) return false;
    first = false;
    std::string key;
    if (!cursor.string(key, 32U) || !cursor.consume(':')) return false;
    if (key == "payload") {
      if (have_payload || !cursor.string(payload,
                                          kMaximumLegacyPortalRecordBytes))
        return false;
      have_payload = true;
    } else if (key == "sha256") {
      if (have_checksum || !cursor.string(checksum, 64U)) return false;
      have_checksum = true;
    } else if (!cursor.skipValue()) {
      return false;
    }
  }
  return cursor.end() && have_payload && have_checksum &&
      validLowerSha256(checksum);
}

struct LegacyParsedSettings {
  std::uint16_t schema = 0U;
  std::uint64_t revision = 0U;
  DeviceSettings values;
  bool portal_led_swap_present = false;
  bool portal_local_password_present = false;
  std::string source_fingerprint;
};

bool parseLegacySettingsObject(JsonCursor& cursor, DeviceSettings& value,
                               bool& led_swap_present,
                               bool& local_password_present) {
  if (!cursor.consume('{')) return false;
  bool first = true;
  bool have_storage = false;
  bool have_volume = false;
  bool have_assistant = false;
  bool have_negative = false;
  bool have_refresh = false;
  bool voice_seen = false;
  bool aigc_seen = false;
  bool brightness_seen = false;
  bool led_swap_seen = false;
  bool local_password_seen = false;
  while (true) {
    if (cursor.consume('}')) break;
    if (!first && !cursor.consume(',')) return false;
    first = false;
    std::string key;
    if (!cursor.string(key, 64U) || !cursor.consume(':')) return false;
    if (key == "storage" || key == "volume" || key == "refresh" ||
        key == "led_brightness") {
      std::uint64_t number = 0U;
      if (!cursor.unsigned64(number)) return false;
      if (key == "storage") {
        if (have_storage || number > 2U) return false;
        value.asset_storage_preference =
            static_cast<AssetStoragePreference>(number);
        have_storage = true;
      } else if (key == "volume") {
        if (have_volume || number > 100U) return false;
        value.volume_percent = static_cast<std::uint8_t>(number);
        have_volume = true;
      } else if (key == "led_brightness") {
        if (brightness_seen || number > 100U) return false;
        value.led_maximum_brightness_percent =
            static_cast<std::uint8_t>(number);
        brightness_seen = true;
      } else {
        if (have_refresh || number > 3U) return false;
        static const char* const kStrategies[] = {
            "official-quality", "classic-six-color",
            "reflectance-photo", "solid-clean"};
        value.default_render_strategy = kStrategies[number];
        have_refresh = true;
      }
    } else if (key == "voice_assistance") {
      if (voice_seen || !cursor.boolean(value.voice_assistance_enabled))
        return false;
      voice_seen = true;
    } else if (key == "prompt") {
      if (have_assistant || !cursor.string(
              value.assistant_prompt, kMaximumAssistantPromptBytes))
        return false;
      have_assistant = true;
    } else if (key == "image_prompt") {
      if (aigc_seen || !cursor.string(
              value.aigc_prompt_template, kMaximumAigcPromptTemplateBytes))
        return false;
      aigc_seen = true;
    } else if (key == "negative") {
      if (have_negative || !cursor.string(
              value.negative_prompt, kMaximumNegativePromptBytes))
        return false;
      have_negative = true;
    } else if (key == "led_swap") {
      if (led_swap_seen || !cursor.boolean(value.led_roles_swapped))
        return false;
      led_swap_seen = true;
    } else if (key == "local_password") {
      if (local_password_seen ||
          !cursor.string(value.local_management_password_override,
                         kMaximumLocalManagementPasswordBytes)) {
        return false;
      }
      local_password_seen = true;
    } else if (!cursor.skipValue()) {
      return false;
    }
  }
  led_swap_present = led_swap_seen;
  local_password_present = local_password_seen;
  return have_storage && have_volume && have_assistant && have_negative &&
      have_refresh && validDeviceSettings(value);
}

bool parseLegacyPayload(const std::string& input,
                        const DeviceSettings& defaults,
                        LegacyParsedSettings& output) {
  JsonCursor cursor(input);
  if (!cursor.consume('{')) return false;
  bool first = true;
  bool have_schema = false;
  bool have_revision = false;
  bool have_settings = false;
  LegacyParsedSettings parsed;
  parsed.values = defaults;
  while (true) {
    if (cursor.consume('}')) break;
    if (!first && !cursor.consume(',')) return false;
    first = false;
    std::string key;
    if (!cursor.string(key, 64U) || !cursor.consume(':')) return false;
    if (key == "schema") {
      std::uint64_t schema = 0U;
      if (have_schema || !cursor.unsigned64(schema) || schema < 1U ||
          schema > 2U)
        return false;
      parsed.schema = static_cast<std::uint16_t>(schema);
      have_schema = true;
    } else if (key == "revision") {
      if (have_revision || !cursor.unsigned64(parsed.revision)) return false;
      have_revision = true;
    } else if (key == "settings") {
      if (have_settings ||
          !parseLegacySettingsObject(
              cursor, parsed.values, parsed.portal_led_swap_present,
              parsed.portal_local_password_present))
        return false;
      have_settings = true;
    } else if (!cursor.skipValue()) {
      return false;
    }
  }
  if (!cursor.end() || !have_schema || !have_revision || !have_settings ||
      (parsed.schema >= 2U && !parsed.portal_local_password_present) ||
      !validDeviceSettings(parsed.values))
    return false;
  output = std::move(parsed);
  return true;
}

bool decodeLegacyRecord(const std::string& raw,
                        const ILegacySha256Verifier& verifier,
                        const DeviceSettings& defaults,
                        LegacyParsedSettings& output) {
  if (raw.empty() || raw.size() > kMaximumLegacyPortalRecordBytes)
    return false;
  std::string payload;
  std::string checksum;
  if (!parseEnvelope(raw, payload, checksum) ||
      !verifier.matches(payload, checksum))
    return false;
  if (!parseLegacyPayload(payload, defaults, output)) return false;
  output.source_fingerprint = checksum;
  return true;
}

bool ledMapFingerprint(const ILegacySha256Verifier& verifier,
                       const std::string& portal_fingerprint,
                       std::uint8_t encoded,
                       std::string& output) {
  if (encoded > 2U) return false;
  const std::string canonical = portal_fingerprint.empty()
      ? "inkloop-v2/led-map=" + std::to_string(encoded)
      : "ink-portal/sha256=" + portal_fingerprint +
          ";inkloop-v2/led-map=" + std::to_string(encoded);
  return verifier.digest(canonical, output) && validLowerSha256(output);
}

}  // namespace

void LegacyPortalJournalState::clear() {
  for (std::string& value : slot) {
    value.assign(value.size(), '\0');
    value.clear();
  }
  *this = LegacyPortalJournalState{};
}

SettingsStatus inspectLegacyPortalSettings(
    const IReadOnlyLegacyPortalSource& source,
    const ILegacySha256Verifier& verifier,
    const DeviceSettings& defaults_for_missing_legacy_fields,
    LegacySettingsImport& output) {
  output = LegacySettingsImport{};
  if (!validDeviceSettings(defaults_for_missing_legacy_fields))
    return {SettingsError::InvalidArgument, "legacy defaults invalid"};
  LegacyPortalJournalState state;
  SettingsStatus status = source.inspect(state);
  if (!status.ok()) {
    state.clear();
    return {SettingsError::Storage, "legacy portal inspection failed"};
  }
  if (!state.namespace_available) {
    state.clear();
    return {SettingsError::Storage, "legacy portal namespace unavailable"};
  }
  const bool any_material = state.marker_present || state.head_present ||
      state.slot_present[0] || state.slot_present[1];
  if (!any_material) {
    output.values = defaults_for_missing_legacy_fields;
    if (state.early_led_map_present) {
      if (state.early_led_map > 2U) {
        state.clear();
        return corrupt("early LED role map invalid");
      }
      output.state = LegacyImportState::Candidate;
      output.used_early_led_map = true;
      if (state.early_led_map == 1U || state.early_led_map == 2U) {
        output.values.led_roles_swapped = state.early_led_map == 2U;
      }
      if (!ledMapFingerprint(verifier, "", state.early_led_map,
                             output.source_fingerprint)) {
        state.clear();
        output = LegacySettingsImport{};
        return {SettingsError::Storage,
                "early LED role map fingerprint failed"};
      }
    }
    state.clear();
    return SettingsStatus::success();
  }
  if ((state.marker_present && !state.marker_valid) || !state.head_present ||
      state.head < 1U || state.head > 2U) {
    state.clear();
    return corrupt("legacy portal metadata invalid");
  }
  const std::uint8_t preferred = static_cast<std::uint8_t>(state.head - 1U);
  const std::uint8_t fallback = static_cast<std::uint8_t>(preferred ^ 1U);
  LegacyParsedSettings parsed;
  bool used_fallback = false;
  bool decoded = state.slot_present[preferred] &&
      decodeLegacyRecord(state.slot[preferred], verifier,
                         defaults_for_missing_legacy_fields, parsed);
  if (!decoded && state.slot_present[fallback]) {
    decoded = decodeLegacyRecord(state.slot[fallback], verifier,
                                 defaults_for_missing_legacy_fields, parsed);
    used_fallback = decoded;
  }
  if (!decoded) {
    state.clear();
    return corrupt("legacy portal committed snapshot invalid");
  }
  if (!parsed.portal_led_swap_present && state.early_led_map_present) {
    if (state.early_led_map > 2U) {
      state.clear();
      return corrupt("early LED role map invalid");
    }
    output.used_early_led_map = true;
    if (state.early_led_map == 1U || state.early_led_map == 2U) {
      parsed.values.led_roles_swapped = state.early_led_map == 2U;
    }
    std::string combined;
    if (!ledMapFingerprint(verifier, parsed.source_fingerprint,
                           state.early_led_map, combined)) {
      state.clear();
      return {SettingsError::Storage,
              "legacy settings fingerprint failed"};
    }
    parsed.source_fingerprint = std::move(combined);
  }
  state.clear();
  output.state = LegacyImportState::Candidate;
  output.values = std::move(parsed.values);
  output.source_schema = parsed.schema;
  output.source_revision = parsed.revision;
  output.used_fallback_slot = used_fallback;
  output.source_fingerprint = std::move(parsed.source_fingerprint);
  return SettingsStatus::success();
}

bool matchesHistoricalIncompleteImport(
    const SettingsSnapshot& native_target,
    const LegacySettingsImport& verified_legacy_candidate) {
  // The old auto-importer never consumed an inkloop-v2-only calibration. A
  // portal record (schema 1 or 2) and its stable fingerprint must both exist.
  if (native_target.generation != 1U ||
      (native_target.decoded_record_schema != 1U &&
       native_target.decoded_record_schema != 2U) ||
      verified_legacy_candidate.state != LegacyImportState::Candidate ||
      verified_legacy_candidate.source_schema < 1U ||
      verified_legacy_candidate.source_schema > 2U ||
      !validLowerSha256(verified_legacy_candidate.source_fingerprint) ||
      !validDeviceSettings(native_target.values) ||
      !validDeviceSettings(verified_legacy_candidate.values)) {
    return false;
  }

  DeviceSettings historical = verified_legacy_candidate.values;
  historical.local_management_password_override.clear();
  historical.led_roles_swapped = false;
  if (historical.default_render_strategy == "classic-six-color") {
    historical.default_render_strategy = "experimental-six-color";
  }
  return validDeviceSettings(historical) && native_target.values == historical;
}

}  // namespace settings
}  // namespace inkloop
