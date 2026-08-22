#include "inkloop/portal/portal_core.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace inkloop {
namespace portal {
namespace {

constexpr char kSessionCookieName[] = "Inkloop-Portal";
constexpr size_t kMaximumPathBytes = 512U;
constexpr size_t kMaximumHostBytes = 128U;
constexpr size_t kMaximumOriginBytes = 160U;
constexpr size_t kMaximumCookieBytes = 512U;
constexpr size_t kMaximumBindingUrlBytes = 320U;
constexpr size_t kMaximumStateLabelBytes = 96U;
constexpr size_t kMaximumAlbumOriginBytes = 64U;
constexpr size_t kMaximumRenderStrategyBytes = 32U;
constexpr size_t kMaximumRenderStrategyDisplayNameBytes = 64U;
constexpr uint32_t kMaximumRuntimeQueueCapacity = 1024U;

bool appendBounded(std::string& output, const std::string& value,
                   size_t maximum) {
  if (value.size() > maximum || output.size() > maximum - value.size())
    return false;
  output.append(value);
  return true;
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
  const size_t maximum = std::max(left.size(), right.size());
  size_t difference = left.size() ^ right.size();
  for (size_t index = 0; index < maximum; ++index) {
    const unsigned char a = index < left.size()
                                ? static_cast<unsigned char>(left[index])
                                : 0U;
    const unsigned char b = index < right.size()
                                ? static_cast<unsigned char>(right[index])
                                : 0U;
    difference |= static_cast<size_t>(a ^ b);
  }
  return difference == 0U;
}

size_t utf8Length(const std::string& value, size_t at) {
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
    if ((static_cast<uint8_t>(value[at + index]) & 0xC0U) != 0x80U)
      return 0;
  }
  const uint8_t second = static_cast<uint8_t>(value[at + 1]);
  if ((first == 0xE0U && second < 0xA0U) ||
      (first == 0xEDU && second >= 0xA0U) ||
      (first == 0xF0U && second < 0x90U) ||
      (first == 0xF4U && second >= 0x90U)) {
    return 0;
  }
  return length;
}

bool validText(const std::string& value, size_t maximum, bool allow_empty,
               bool multiline = true) {
  if (value.size() > maximum || (!allow_empty && value.empty())) return false;
  for (size_t at = 0; at < value.size();) {
    const size_t length = utf8Length(value, at);
    if (length == 0) return false;
    if (length == 1) {
      const uint8_t ch = static_cast<uint8_t>(value[at]);
      if (ch == 0U || ch == 0x7FU ||
          (ch < 0x20U && !(multiline && (ch == '\n' || ch == '\t')))) {
        return false;
      }
    }
    at += length;
  }
  return true;
}

bool safeToken(const std::string& value, size_t minimum, size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
          ch == '~')) {
      return false;
    }
  }
  return true;
}

bool safePassword(const std::string& value) {
  if (value.size() < 8U || value.size() > 63U) return false;
  for (unsigned char ch : value) {
    if (ch < 0x20U || ch > 0x7EU) return false;
  }
  return true;
}

bool safeHost(const std::string& value) {
  if (value.empty() || value.size() > kMaximumHostBytes) return false;
  for (unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == ':' ||
          ch == '[' || ch == ']')) {
      return false;
    }
  }
  return true;
}

bool safeOrigin(const std::string& value) {
  if (value.size() > kMaximumOriginBytes ||
      !(value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0)) {
    return false;
  }
  for (unsigned char ch : value) {
    if (ch <= 0x20U || ch >= 0x7FU) return false;
  }
  return true;
}

bool safeIdentifier(const std::string& value, size_t maximum) {
  if (value.empty() || value.size() > maximum) return false;
  for (unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.'))
      return false;
  }
  return value != "." && value != ".." && value.find("..") == std::string::npos;
}

bool safeCursor(const std::string& value) {
  if (value.size() > kMaximumAlbumCursorBytes) return false;
  for (unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
          ch == '~')) {
      return false;
    }
  }
  return true;
}

bool validRenderStrategyId(const std::string& value) {
  if (value.empty() || value.size() > kMaximumRenderStrategyBytes ||
      value.front() == '-' || value.back() == '-') {
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

bool validCapabilities(const PortalBoardCapabilities& capabilities) {
  if (capabilities.render_strategy_count == 0U ||
      capabilities.render_strategy_count >
          capabilities.render_strategies.size() ||
      capabilities.rgb_pixels > kMaximumPortalRgbPixels) {
    return false;
  }
  for (size_t index = 0; index < capabilities.render_strategies.size();
       ++index) {
    const PortalRenderStrategyCapability& entry =
        capabilities.render_strategies[index];
    if (index >= capabilities.render_strategy_count) {
      if (!entry.id.empty() || !entry.display_name.empty()) return false;
      continue;
    }
    if (!validRenderStrategyId(entry.id) ||
        !validText(entry.display_name,
                   kMaximumRenderStrategyDisplayNameBytes, false, false)) {
      return false;
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (capabilities.render_strategies[previous].id == entry.id)
        return false;
    }
  }
  return true;
}

PortalResult readCapabilities(const IPortalReadCache& cache,
                              PortalBoardCapabilities& output) {
  PortalStateSnapshot state;
  const PortalResult result = cache.readState(state);
  if (result != PortalResult::Ok) return result;
  if (!validCapabilities(state.capabilities)) return PortalResult::InvalidData;
  output = state.capabilities;
  return PortalResult::Ok;
}

bool validPairingCode(const std::string& value) {
  if (value.empty()) return true;
  if (value.size() != 6U) return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool validBindingUrl(const std::string& value) {
  if (value.empty()) return true;
  if (value.size() > kMaximumBindingUrlBytes ||
      value.rfind("https://", 0) != 0) {
    return false;
  }
  for (unsigned char ch : value) {
    if (ch <= 0x20U || ch >= 0x7FU) return false;
  }
  return true;
}

std::string jsonEscape(const std::string& value) {
  std::string output;
  output.reserve(value.size() + 8U);
  static constexpr char kHex[] = "0123456789abcdef";
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': output.append("\\\""); break;
      case '\\': output.append("\\\\"); break;
      case '\n': output.append("\\n"); break;
      case '\r': output.append("\\r"); break;
      case '\t': output.append("\\t"); break;
      default:
        if (ch < 0x20U) {
          output.append("\\u00");
          output.push_back(kHex[(ch >> 4U) & 0xFU]);
          output.push_back(kHex[ch & 0xFU]);
        } else {
          output.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return output;
}

PortalResponse response(int status, const std::string& body,
                        const char* content_type =
                            "application/json; charset=utf-8") {
  PortalResponse output;
  output.status = status;
  output.content_type = content_type;
  output.body = body;
  return output;
}

PortalResponse errorResponse(int status, const char* code) {
  return response(status, std::string("{\"ok\":false,\"error\":\"") +
                              code + "\"}");
}

PortalResponse resultError(PortalResult result, const char* invalid_code) {
  switch (result) {
    case PortalResult::Busy: {
      PortalResponse output = errorResponse(409, "device_busy");
      output.retry_after_seconds = 1U;
      return output;
    }
    case PortalResult::TooLarge: return errorResponse(413, "response_too_large");
    case PortalResult::InvalidData: return errorResponse(422, invalid_code);
    case PortalResult::Unauthorized: return errorResponse(401, "unauthorized");
    case PortalResult::Forbidden: return errorResponse(403, "forbidden");
    case PortalResult::InvalidRequest: return errorResponse(400, invalid_code);
    case PortalResult::Unavailable: return errorResponse(503, "portal_source_unavailable");
    case PortalResult::InvalidConfiguration:
      return errorResponse(503, "portal_invalid_configuration");
    case PortalResult::Ok: break;
  }
  return errorResponse(500, "portal_internal_error");
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  return -1;
}

bool urlDecode(const std::string& input, bool plus_is_space,
               size_t maximum, std::string& output) {
  output.clear();
  output.reserve(std::min(input.size(), maximum));
  for (size_t index = 0; index < input.size(); ++index) {
    unsigned char value = static_cast<unsigned char>(input[index]);
    if (value == '%' && index + 2U < input.size()) {
      const int high = hexDigit(input[index + 1U]);
      const int low = hexDigit(input[index + 2U]);
      if (high < 0 || low < 0) return false;
      value = static_cast<unsigned char>((high << 4) | low);
      index += 2U;
    } else if (value == '%') {
      return false;
    } else if (value == '+' && plus_is_space) {
      value = ' ';
    }
    if (value == 0U || output.size() >= maximum) return false;
    output.push_back(static_cast<char>(value));
  }
  return true;
}

using Fields = std::vector<std::pair<std::string, std::string>>;

bool parseFields(const std::string& encoded, bool plus_is_space,
                 size_t maximum_fields, Fields& output) {
  output.clear();
  if (encoded.empty()) return true;
  size_t start = 0;
  while (start <= encoded.size()) {
    const size_t separator = encoded.find('&', start);
    const size_t end = separator == std::string::npos ? encoded.size() : separator;
    if (end == start || output.size() >= maximum_fields) return false;
    const size_t equal = encoded.find('=', start);
    if (equal == std::string::npos || equal >= end) return false;
    std::string key;
    std::string value;
    if (!urlDecode(encoded.substr(start, equal - start), plus_is_space, 64U, key) ||
        !urlDecode(encoded.substr(equal + 1U, end - equal - 1U),
                   plus_is_space, kMaximumPortalRequestBodyBytes, value) ||
        key.empty()) {
      return false;
    }
    for (const auto& existing : output) {
      if (existing.first == key) return false;
    }
    output.emplace_back(std::move(key), std::move(value));
    if (separator == std::string::npos) break;
    start = separator + 1U;
  }
  return true;
}

const std::string* field(const Fields& fields, const char* name) {
  for (const auto& item : fields) {
    if (item.first == name) return &item.second;
  }
  return nullptr;
}

bool onlyFields(const Fields& fields,
                const std::vector<std::string>& allowed) {
  for (const auto& item : fields) {
    if (std::find(allowed.begin(), allowed.end(), item.first) == allowed.end())
      return false;
  }
  return true;
}

bool formContentType(const std::string& value) {
  static const std::string expected = "application/x-www-form-urlencoded";
  return value == expected || value.rfind(expected + ";", 0) == 0;
}

bool pngContentType(const std::string& value) {
  return value == "image/png" || value.rfind("image/png;", 0) == 0;
}

bool parseUnsigned(const std::string& value, uint64_t& output) {
  if (value.empty()) return false;
  uint64_t parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
    const uint8_t digit = static_cast<uint8_t>(ch - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
      return false;
    parsed = parsed * 10U + digit;
  }
  output = parsed;
  return true;
}

bool parseBytePercent(const std::string& value, uint8_t minimum,
                      uint8_t& output) {
  uint64_t parsed = 0;
  if (!parseUnsigned(value, parsed) || parsed < minimum || parsed > 100U)
    return false;
  output = static_cast<uint8_t>(parsed);
  return true;
}

bool parseBoolean(const std::string& value, bool& output) {
  if (value == "1" || value == "true" || value == "on") {
    output = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off") {
    output = false;
    return true;
  }
  return false;
}

bool splitPath(const std::string& input, std::string& path,
               std::string& query) {
  for (unsigned char ch : input) {
    if (ch <= 0x20U || ch >= 0x7FU) return false;
  }
  const size_t question = input.find('?');
  path = input.substr(0, question);
  query = question == std::string::npos ? std::string() : input.substr(question + 1U);
  return !path.empty() && path[0] == '/' && path.find('#') == std::string::npos;
}

const char* myAiStateName(MyAiPortalState state) {
  switch (state) {
    case MyAiPortalState::Unconfigured: return "unconfigured";
    case MyAiPortalState::Pairing: return "pairing";
    case MyAiPortalState::Bound: return "bound";
    case MyAiPortalState::Active: return "active";
    case MyAiPortalState::RecoveryRequired: return "recovery_required";
    case MyAiPortalState::Unavailable: return "unavailable";
  }
  return "unavailable";
}

const char* chatRoleName(ChatRole role) {
  switch (role) {
    case ChatRole::User: return "user";
    case ChatRole::Assistant: return "assistant";
    case ChatRole::Tool: return "tool";
  }
  return "tool";
}

const char* firmwareUpdatePhaseName(PortalFirmwareUpdatePhase phase) {
  switch (phase) {
    case PortalFirmwareUpdatePhase::Unavailable: return "unavailable";
    case PortalFirmwareUpdatePhase::Ready: return "ready";
    case PortalFirmwareUpdatePhase::AcceptedOffline:
      return "accepted_offline";
  }
  return "unavailable";
}

const char* firmwareUpdateCodeName(PortalFirmwareUpdateCode code) {
  switch (code) {
    case PortalFirmwareUpdateCode::None: return "none";
    case PortalFirmwareUpdateCode::UpToDate: return "up_to_date";
    case PortalFirmwareUpdateCode::ConfigurationInvalid:
      return "configuration_invalid";
    case PortalFirmwareUpdateCode::NetworkUnavailable:
      return "network_unavailable";
    case PortalFirmwareUpdateCode::TimedOut: return "timed_out";
    case PortalFirmwareUpdateCode::ManifestRejected:
      return "manifest_rejected";
    case PortalFirmwareUpdateCode::ImageRejected: return "image_rejected";
    case PortalFirmwareUpdateCode::VerificationFailed:
      return "verification_failed";
    case PortalFirmwareUpdateCode::StagingFailed: return "staging_failed";
    case PortalFirmwareUpdateCode::InternalError: return "internal_error";
    case PortalFirmwareUpdateCode::UpdateConfirmed: return "update_confirmed";
    case PortalFirmwareUpdateCode::UpdateRolledBack:
      return "update_rolled_back";
  }
  return "internal_error";
}

bool validFirmwareUpdateState(const PortalFirmwareUpdateSnapshot& update) {
  if (static_cast<uint8_t>(update.phase) >
          static_cast<uint8_t>(PortalFirmwareUpdatePhase::AcceptedOffline) ||
      static_cast<uint8_t>(update.code) >
          static_cast<uint8_t>(PortalFirmwareUpdateCode::UpdateRolledBack)) {
    return false;
  }
  if (!update.configured) {
    return !update.accepted_offline &&
           update.phase == PortalFirmwareUpdatePhase::Unavailable;
  }
  if (update.accepted_offline) {
    return update.phase == PortalFirmwareUpdatePhase::AcceptedOffline &&
           update.code == PortalFirmwareUpdateCode::None;
  }
  return update.phase == PortalFirmwareUpdatePhase::Ready &&
         update.code != PortalFirmwareUpdateCode::ConfigurationInvalid;
}

bool blankAudioArtifact(const std::string& input) {
  std::string normalized;
  normalized.reserve(input.size());
  size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])))
    ++first;
  size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1U])))
    --last;
  if (last > first + 1U && input[first] == '[' && input[last - 1U] == ']') {
    ++first;
    --last;
  }
  for (size_t index = first; index < last; ++index) {
    const unsigned char ch = static_cast<unsigned char>(input[index]);
    if (ch == '_' || ch == '-' || std::isspace(ch)) continue;
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized == "blankaudio";
}

bool validRuntimeTelemetry(const PortalRuntimeTelemetry& runtime) {
  if (!runtime.available) {
    return runtime.lane_count == 0U && runtime.sequence == 0U &&
           runtime.last_managed_update_ms == 0U &&
           runtime.internal_heap_min_free_bytes == 0U &&
           runtime.psram_min_free_bytes == 0U &&
           runtime.resource_sample_count == 0U &&
           !runtime.internal_heap_sampled && !runtime.psram_available;
  }
  if (static_cast<size_t>(runtime.lane_count) != runtime.lanes.size() ||
      static_cast<size_t>(runtime.lane_count) != kPortalRuntimeLaneCount ||
      (!runtime.psram_available && runtime.psram_min_free_bytes != 0U) ||
      (!runtime.internal_heap_sampled &&
       runtime.internal_heap_min_free_bytes != 0U)) {
    return false;
  }
  for (const PortalRuntimeLaneTelemetry& lane : runtime.lanes) {
    if (lane.queue_capacity == 0U ||
        lane.queue_capacity > kMaximumRuntimeQueueCapacity ||
        lane.queue_depth > lane.queue_high_water ||
        lane.queue_high_water > lane.queue_capacity ||
        lane.configured_core < 0 || lane.configured_core > 1 ||
        lane.observed_core < -1 || lane.observed_core > 1 ||
        lane.configured_priority == 0U ||
        (!lane.stack_sampled && lane.stack_low_water_bytes != 0U)) {
      return false;
    }
  }
  return true;
}

bool validState(const PortalStateSnapshot& state) {
  return validText(state.firmware_version, 64U, false, false) &&
         validFirmwareUpdateState(state.firmware_update) &&
         validText(state.device_name, kMaximumStateLabelBytes, true, false) &&
         state.display_width > 0U && state.display_width <= 8192U &&
         state.display_height > 0U && state.display_height <= 8192U &&
         static_cast<uint8_t>(state.myai_state) <=
             static_cast<uint8_t>(MyAiPortalState::Unavailable) &&
         validPairingCode(state.pairing_code) &&
         validBindingUrl(state.binding_url) &&
         validCapabilities(state.capabilities) &&
         validRuntimeTelemetry(state.runtime) &&
         state.settings.volume <= 100U &&
         state.storage_free_bytes <= state.storage_total_bytes &&
         state.settings.led_maximum_brightness_percent >= 1U &&
         state.settings.led_maximum_brightness_percent <= 100U &&
         validText(state.settings.assistant_prompt,
                   kMaximumAssistantPromptBytes, true) &&
         validText(state.settings.image_prompt_template,
                   kMaximumImagePromptBytes, false) &&
         validText(state.settings.negative_prompt,
                   kMaximumNegativePromptBytes, true) &&
         (state.settings.asset_storage_preference == "automatic" ||
          state.settings.asset_storage_preference == "internal" ||
          state.settings.asset_storage_preference == "removable") &&
         state.capabilities.supportsRenderStrategy(
             state.settings.default_render_strategy);
}

bool validAlbumPage(const AlbumPageQuery& query, const AlbumPage& page) {
  if (page.items.size() > query.limit ||
      page.total_items > kMaximumAlbumTotalItems ||
      page.items.size() > page.total_items || !safeCursor(page.next_cursor) ||
      (!page.next_cursor.empty() && page.next_cursor == query.cursor)) {
    return false;
  }
  size_t aggregate = 0;
  for (const AlbumItem& item : page.items) {
    if (!safeIdentifier(item.id, kMaximumAlbumIdBytes) ||
        !validText(item.title, kMaximumAlbumTitleBytes, true, false) ||
        !validText(item.origin, kMaximumAlbumOriginBytes, true, false) ||
        item.render_strategy.size() > kMaximumRenderStrategyBytes ||
        !validRenderStrategyId(item.render_strategy)) {
      return false;
    }
    const size_t fields = item.id.size() + item.title.size() + item.origin.size() +
                          item.render_strategy.size();
    if (fields > kMaximumAlbumPageFieldBytes ||
        aggregate > kMaximumAlbumPageFieldBytes - fields) {
      return false;
    }
    aggregate += fields;
  }
  return true;
}

}  // namespace

PortalCore::PortalCore(const PortalAccessConfig& access,
                       const IPortalReadCache& cache,
                       IPortalCommandQueue& commands)
    : access_(access), cache_(cache), commands_(commands) {
  ready_ = validateConfiguration();
}

bool PortalCore::validateConfiguration() const {
  if (!safePassword(access_.access_code) ||
      !safeToken(access_.session_id, 24U, 64U) ||
      !safeToken(access_.csrf_token, 24U, 64U) ||
      access_.session_lifetime_seconds < 60U ||
      access_.session_lifetime_seconds > 86400U ||
      access_.allowed_hosts.empty() || access_.allowed_hosts.size() > 8U ||
      access_.allowed_origins.empty() || access_.allowed_origins.size() > 8U) {
    return false;
  }
  for (const std::string& host : access_.allowed_hosts) {
    if (!safeHost(host)) return false;
  }
  for (const std::string& origin : access_.allowed_origins) {
    if (!safeOrigin(origin)) return false;
  }
  return true;
}

bool PortalCore::hostAllowed(const std::string& host) const {
  if (!safeHost(host)) return false;
  return std::find(access_.allowed_hosts.begin(), access_.allowed_hosts.end(),
                   host) != access_.allowed_hosts.end();
}

bool PortalCore::originAllowed(const std::string& origin) const {
  if (!safeOrigin(origin)) return false;
  return std::find(access_.allowed_origins.begin(),
                   access_.allowed_origins.end(), origin) !=
         access_.allowed_origins.end();
}

bool PortalCore::sessionAuthorized(const PortalRequest& request) const {
  if (!session_issued_ || request.now_seconds >= session_expires_at_seconds_ ||
      request.cookie.empty() || request.cookie.size() > kMaximumCookieBytes) {
    return false;
  }
  size_t start = 0;
  while (start < request.cookie.size()) {
    while (start < request.cookie.size() &&
           (request.cookie[start] == ' ' || request.cookie[start] == ';'))
      ++start;
    const size_t end = request.cookie.find(';', start);
    const size_t stop = end == std::string::npos ? request.cookie.size() : end;
    const size_t equal = request.cookie.find('=', start);
    if (equal != std::string::npos && equal < stop) {
      const std::string name = request.cookie.substr(start, equal - start);
      const std::string value = request.cookie.substr(equal + 1U, stop - equal - 1U);
      if (name == kSessionCookieName &&
          constantTimeEquals(value, access_.session_id)) {
        return true;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1U;
  }
  return false;
}

bool PortalCore::mutationAuthorized(const PortalRequest& request) const {
  return sessionAuthorized(request) && originAllowed(request.origin) &&
         constantTimeEquals(request.csrf_token, access_.csrf_token);
}

PortalResponse PortalCore::handle(const PortalRequest& request) {
  if (!ready_) return errorResponse(503, "portal_invalid_configuration");
  if (!request.peer_is_local) return errorResponse(403, "local_peer_required");
  if (request.method != "GET" && request.method != "POST")
    return errorResponse(405, "method_not_allowed");
  if (request.path.empty() || request.path.size() > kMaximumPathBytes ||
      request.host.size() > kMaximumHostBytes ||
      request.origin.size() > kMaximumOriginBytes ||
      request.cookie.size() > kMaximumCookieBytes ||
      request.csrf_token.size() > 128U ||
      request.content_type.size() > 128U ||
      request.body.size() > kMaximumPortalRequestBodyBytes ||
      !hostAllowed(request.host)) {
    return errorResponse(400, "invalid_request_metadata");
  }
  std::string path;
  std::string query;
  if (!splitPath(request.path, path, query))
    return errorResponse(400, "invalid_path");
  if (path == "/health" && request.method == "GET" && query.empty())
    return response(200, "{\"ok\":true,\"service\":\"inkloop-portal\"}");
  if (path == "/" && request.method == "GET" && query.empty())
    return response(200, dashboardHtml(), "text/html; charset=utf-8");
  if (path == "/api/session" && query.empty()) return handleSession(request);
  if (!sessionAuthorized(request)) return errorResponse(401, "session_required");
  if (request.method == "POST" && !mutationAuthorized(request))
    return errorResponse(403, "origin_or_csrf_rejected");
  return handleAuthenticated(request);
}

PortalResponse PortalCore::handleSession(const PortalRequest& request) {
  if (request.method != "POST") return errorResponse(405, "method_not_allowed");
  if (!originAllowed(request.origin) || !formContentType(request.content_type))
    return errorResponse(403, "origin_rejected");
  Fields fields;
  if (!parseFields(request.body, true, 1U, fields) || fields.size() != 1U ||
      (fields[0].first != "nonce" && fields[0].first != "access_code")) {
    return errorResponse(400, "invalid_session_request");
  }
  if (!constantTimeEquals(fields[0].second, access_.access_code))
    return errorResponse(401, "invalid_access_code");
  if (request.now_seconds >
      std::numeric_limits<uint64_t>::max() - access_.session_lifetime_seconds) {
    return errorResponse(503, "clock_invalid");
  }
  session_issued_ = true;
  session_expires_at_seconds_ =
      request.now_seconds + access_.session_lifetime_seconds;
  PortalResponse output = response(
      200, std::string("{\"ok\":true,\"csrfToken\":\"") +
               jsonEscape(access_.csrf_token) + "\",\"expiresAt\":" +
               std::to_string(session_expires_at_seconds_) + "}");
  output.set_cookie = std::string(kSessionCookieName) + "=" +
                      access_.session_id +
                      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                      std::to_string(access_.session_lifetime_seconds);
  return output;
}

PortalResponse PortalCore::handleAuthenticated(const PortalRequest& request) {
  std::string path;
  std::string query;
  if (!splitPath(request.path, path, query))
    return errorResponse(400, "invalid_path");
  const bool query_allowed =
      (request.method == "GET" &&
       (path == "/api/album" || path == "/api/album/preview" ||
        path == "/api/chat" || path == "/api/myai/chat")) ||
      (request.method == "POST" && path == "/api/album/upload");
  if (!query.empty() && !query_allowed)
    return errorResponse(400, "unexpected_query");
  if (request.method == "GET" && path == "/api/state") return renderState();
  if (request.method == "GET" && path == "/api/album/preview") {
    Fields fields;
    if (!parseFields(query, false, 1U, fields) || fields.size() != 1U ||
        fields[0].first != "asset_id" ||
        !safeIdentifier(fields[0].second, kMaximumAlbumIdBytes)) {
      return errorResponse(422, "invalid_asset_id");
    }
    PortalResponse output = response(200, std::string(), "image/png");
    output.disposition = ResponseDisposition::StreamAlbumPreview;
    output.stream.asset_id = fields[0].second;
    return output;
  }
  if (request.method == "GET" && path == "/api/album")
    return renderAlbum(request);
  if (request.method == "GET" &&
      (path == "/api/chat" || path == "/api/myai/chat")) {
    return renderChat(request);
  }
  if (request.method != "POST") return errorResponse(404, "route_not_found");
  if (path == "/api/settings") return updateSettings(request);
  if (path == "/api/system/update") return requestFirmwareUpdate(request);
  if (path == "/api/audio/preview") {
    if (!formContentType(request.content_type))
      return errorResponse(415, "form_content_type_required");
    Fields fields;
    uint8_t volume = 0;
    if (!parseFields(request.body, true, 1U, fields) || fields.size() != 1U ||
        fields[0].first != "volume" ||
        !parseBytePercent(fields[0].second, 0U, volume)) {
      return errorResponse(400, "invalid_volume");
    }
    PortalBoardCapabilities capabilities;
    const PortalResult capability_result =
        readCapabilities(cache_, capabilities);
    if (capability_result != PortalResult::Ok)
      return resultError(capability_result, "capabilities_unavailable");
    if (!capabilities.hasDuplexAudio())
      return errorResponse(422, "audio_not_supported");
    PortalCommand command;
    command.type = PortalCommandType::PreviewVolume;
    command.volume = volume;
    return enqueueCommand(std::move(command));
  }
  if (path == "/api/onboarding/myai/start")
    return enqueueSimple(request, PortalCommandType::StartMyAiPairing);
  if (path == "/api/onboarding/myai/rebind")
    return enqueueSimple(request, PortalCommandType::RebindMyAi);
  if (path == "/api/myai/chat/clear" || path == "/api/chat/clear")
    return enqueueSimple(request, PortalCommandType::ClearLocalChat);
  if (path == "/api/album/display")
    return enqueueAsset(request, PortalCommandType::DisplayAlbumItem);
  if (path == "/api/album/delete")
    return enqueueAsset(request, PortalCommandType::DeleteAlbumItem);
  if (path == "/api/album/render") {
    if (!formContentType(request.content_type))
      return errorResponse(415, "form_content_type_required");
    Fields fields;
    if (!parseFields(request.body, true, 2U, fields) || fields.size() != 2U ||
        !onlyFields(fields, {"asset_id", "render_strategy"})) {
      return errorResponse(400, "invalid_render_request");
    }
    const std::string* asset = field(fields, "asset_id");
    const std::string* strategy = field(fields, "render_strategy");
    if (!asset || !strategy || !safeIdentifier(*asset, kMaximumAlbumIdBytes) ||
        !validRenderStrategyId(*strategy)) {
      return errorResponse(422, "invalid_render_request");
    }
    PortalBoardCapabilities capabilities;
    const PortalResult capability_result =
        readCapabilities(cache_, capabilities);
    if (capability_result != PortalResult::Ok)
      return resultError(capability_result, "capabilities_unavailable");
    if (!capabilities.supportsRenderStrategy(*strategy))
      return errorResponse(422, "render_strategy_not_supported");
    PortalCommand command;
    command.type = PortalCommandType::SetAlbumRenderStrategy;
    command.asset_id = *asset;
    command.render_strategy = *strategy;
    return enqueueCommand(std::move(command));
  }
  if (path == "/api/aigc/generate") {
    if (!formContentType(request.content_type))
      return errorResponse(415, "form_content_type_required");
    Fields fields;
    if (!parseFields(request.body, true, 1U, fields) || fields.size() != 1U ||
        fields[0].first != "prompt" ||
        !validText(fields[0].second, kMaximumGeneratePromptBytes, false)) {
      return errorResponse(422, "invalid_aigc_prompt");
    }
    PortalCommand command;
    command.type = PortalCommandType::GenerateImage;
    command.prompt = fields[0].second;
    return enqueueCommand(std::move(command));
  }
  if (path == "/api/album/upload") {
    if (!pngContentType(request.content_type) || request.content_length == 0U ||
        request.content_length > kMaximumAlbumUploadBytes ||
        request.content_length > std::numeric_limits<size_t>::max()) {
      return errorResponse(413, "invalid_upload_size_or_type");
    }
    Fields fields;
    if (!parseFields(query, false, 1U, fields) || fields.size() != 1U ||
        fields[0].first != "title" ||
        !validText(fields[0].second, kMaximumAlbumTitleBytes, false, false)) {
      return errorResponse(422, "invalid_upload_title");
    }
    PortalResponse output = response(202, std::string());
    output.disposition = ResponseDisposition::StreamAlbumUpload;
    output.stream.request_id = nextRequestId();
    output.stream.upload_title = fields[0].second;
    output.stream.content_length = static_cast<size_t>(request.content_length);
    return output;
  }
  return errorResponse(404, "route_not_found");
}

PortalResponse PortalCore::renderState() {
  PortalStateSnapshot state;
  const PortalResult result = cache_.readState(state);
  if (result != PortalResult::Ok) return resultError(result, "state_invalid");
  if (!validState(state)) return errorResponse(422, "state_invalid");
  std::ostringstream json;
  json << "{\"ok\":true,\"tabs\":[\"device\",\"album\",\"myai\",\"settings\"],"
       << "\"state\":{\"firmwareVersion\":\"" << jsonEscape(state.firmware_version)
       << "\",\"firmwareUpdate\":{\"configured\":"
       << (state.firmware_update.configured ? "true" : "false")
       << ",\"acceptedOffline\":"
       << (state.firmware_update.accepted_offline ? "true" : "false")
       << ",\"currentVersion\":\"" << jsonEscape(state.firmware_version)
       << "\",\"status\":\""
       << firmwareUpdatePhaseName(state.firmware_update.phase)
       << "\",\"code\":\""
       << firmwareUpdateCodeName(state.firmware_update.code) << "\"}"
       << ",\"deviceName\":\"" << jsonEscape(state.device_name)
       << "\",\"displayWidth\":" << state.display_width
       << ",\"displayHeight\":" << state.display_height
       << ",\"wifiOnline\":" << (state.wifi_online ? "true" : "false")
       << ",\"storageReady\":" << (state.storage_ready ? "true" : "false")
       << ",\"storageFreeBytes\":" << state.storage_free_bytes
       << ",\"storageTotalBytes\":" << state.storage_total_bytes
       << ",\"displayBusy\":" << (state.display_busy ? "true" : "false")
       << ",\"capabilities\":{\"microphone\":"
       << (state.capabilities.has_microphone ? "true" : "false")
       << ",\"speaker\":"
       << (state.capabilities.has_speaker ? "true" : "false")
       << ",\"duplexAudio\":"
       << (state.capabilities.hasDuplexAudio() ? "true" : "false")
       << ",\"rgbPixels\":"
       << static_cast<unsigned int>(state.capabilities.rgb_pixels)
       << ",\"removableStorage\":"
       << (state.capabilities.has_removable_storage ? "true" : "false")
       << ",\"renderStrategies\":[";
  for (size_t index = 0;
       index < state.capabilities.render_strategy_count; ++index) {
    if (index != 0U) json << ',';
    const PortalRenderStrategyCapability& strategy =
        state.capabilities.render_strategies[index];
    json << "{\"id\":\"" << jsonEscape(strategy.id)
         << "\",\"displayName\":\""
         << jsonEscape(strategy.display_name) << "\"}";
  }
  json << "]}"
       << ",\"displayTiming\":{\"completedRefreshes\":"
       << state.display_completed_refreshes
       << ",\"loadDecodeMs\":" << state.display_load_decode_ms
       << ",\"conversionMs\":" << state.display_conversion_ms
       << ",\"panelRefreshMs\":" << state.display_panel_refresh_ms
       << ",\"totalMs\":" << state.display_total_ms << "}"
       << ",\"runtimeTelemetry\":{\"available\":"
       << (state.runtime.available ? "true" : "false")
       << ",\"sequence\":" << state.runtime.sequence
       << ",\"lastManagedUpdateMs\":"
       << state.runtime.last_managed_update_ms
       << ",\"internalHeapSampled\":"
       << (state.runtime.internal_heap_sampled ? "true" : "false")
       << ",\"internalHeapMinFreeBytes\":"
       << state.runtime.internal_heap_min_free_bytes
       << ",\"psramAvailable\":"
       << (state.runtime.psram_available ? "true" : "false")
       << ",\"psramMinFreeBytes\":"
       << state.runtime.psram_min_free_bytes
       << ",\"resourceSampleCount\":"
       << state.runtime.resource_sample_count
       << ",\"laneCount\":"
       << static_cast<unsigned int>(state.runtime.lane_count)
       << ",\"lanes\":[";
  if (state.runtime.available) {
    for (size_t index = 0;
         index < static_cast<size_t>(state.runtime.lane_count); ++index) {
      const PortalRuntimeLaneTelemetry& lane = state.runtime.lanes[index];
      if (index != 0U) json << ',';
      json << "{\"index\":" << index
           << ",\"queueCapacity\":" << lane.queue_capacity
           << ",\"queueDepth\":" << lane.queue_depth
           << ",\"queueHighWater\":" << lane.queue_high_water
           << ",\"stackSampled\":"
           << (lane.stack_sampled ? "true" : "false")
           << ",\"stackLowWaterBytes\":"
           << lane.stack_low_water_bytes
           << ",\"handlerCount\":" << lane.handler_count
           << ",\"handlerMaxUs\":" << lane.handler_max_us
           << ",\"tickCount\":" << lane.tick_count
           << ",\"tickMaxUs\":" << lane.tick_max_us
           << ",\"tickLateCount\":" << lane.tick_late_count
           << ",\"tickMissed\":" << lane.tick_missed
           << ",\"tickLateMaxUs\":" << lane.tick_late_max_us
           << ",\"lastProgressMs\":" << lane.last_progress_ms
           << ",\"configuredCore\":"
           << static_cast<int>(lane.configured_core)
           << ",\"observedCore\":"
           << static_cast<int>(lane.observed_core)
           << ",\"configuredPriority\":"
           << static_cast<unsigned int>(lane.configured_priority)
           << ",\"observedPriority\":"
           << static_cast<unsigned int>(lane.observed_priority)
           << ",\"running\":" << (lane.task_running ? "true" : "false")
           << '}';
    }
  }
  json << "]}"
       << ",\"myAi\":{\"state\":\"" << myAiStateName(state.myai_state)
       << "\",\"pairingCode\":\"" << jsonEscape(state.pairing_code)
       << "\",\"bindingUrl\":\"" << jsonEscape(state.binding_url)
       << "\"},\"settings\":{\"volume\":"
       << static_cast<unsigned int>(state.settings.volume)
       << ",\"voiceAssistanceEnabled\":"
       << (state.settings.voice_assistance_enabled ? "true" : "false")
       << ",\"assistantPrompt\":\"" << jsonEscape(state.settings.assistant_prompt)
       << "\",\"imagePromptTemplate\":\""
       << jsonEscape(state.settings.image_prompt_template)
       << "\",\"negativePrompt\":\"" << jsonEscape(state.settings.negative_prompt)
       << "\",\"assetStoragePreference\":\""
       << jsonEscape(state.settings.asset_storage_preference)
       << "\",\"defaultRenderStrategy\":\""
       << jsonEscape(state.settings.default_render_strategy)
       << "\",\"localManagementPasswordOverridden\":"
       << (state.settings.local_management_password_overridden
               ? "true" : "false")
       << ",\"ledMaximumBrightness\":"
       << static_cast<unsigned int>(state.settings.led_maximum_brightness_percent)
       << "}}}";
  const std::string body = json.str();
  if (body.size() > kMaximumPortalResponseBytes)
    return errorResponse(413, "state_response_too_large");
  return response(200, body);
}

PortalResponse PortalCore::renderAlbum(const PortalRequest& request) {
  std::string path;
  std::string query;
  if (!splitPath(request.path, path, query))
    return errorResponse(400, "invalid_path");
  Fields fields;
  if (!parseFields(query, false, 2U, fields) ||
      !onlyFields(fields, {"cursor", "limit"})) {
    return errorResponse(422, "invalid_album_query");
  }
  AlbumPageQuery page_query;
  if (const std::string* cursor = field(fields, "cursor")) {
    if (!safeCursor(*cursor)) return errorResponse(422, "invalid_album_cursor");
    page_query.cursor = *cursor;
  }
  if (const std::string* limit = field(fields, "limit")) {
    uint64_t parsed = 0;
    if (!parseUnsigned(*limit, parsed) || parsed == 0U ||
        parsed > kMaximumAlbumPageItems) {
      return errorResponse(422, "invalid_album_limit");
    }
    page_query.limit = static_cast<size_t>(parsed);
  }
  AlbumPage page;
  const PortalResult result = cache_.readAlbumPage(page_query, page);
  if (result != PortalResult::Ok) return resultError(result, "album_page_invalid");
  if (!validAlbumPage(page_query, page))
    return errorResponse(422, "album_page_invalid");
  std::ostringstream header;
  header << "{\"ok\":true,\"retention\":{\"maximumItems\":"
         << kMaximumAlbumTotalItems << "},\"page\":{\"cursor\":\""
         << jsonEscape(page_query.cursor) << "\",\"nextCursor\":\""
         << jsonEscape(page.next_cursor) << "\",\"limit\":" << page_query.limit
         << ",\"totalItems\":" << page.total_items << ",\"revision\":"
         << page.revision << "},\"items\":[";
  std::string body;
  body.reserve(12288U);
  if (!appendBounded(body, header.str(), kMaximumPortalResponseBytes))
    return errorResponse(413, "album_response_too_large");
  for (size_t index = 0; index < page.items.size(); ++index) {
    const AlbumItem& item = page.items[index];
    std::ostringstream json;
    if (index != 0U) json << ',';
    json << "{\"id\":\"" << jsonEscape(item.id) << "\",\"title\":\""
         << jsonEscape(item.title) << "\",\"origin\":\""
         << jsonEscape(item.origin) << "\",\"bytes\":" << item.bytes
         << ",\"current\":" << (item.current ? "true" : "false")
         << ",\"factoryAsset\":" << (item.factory_asset ? "true" : "false")
         << ",\"renderStrategy\":\"" << jsonEscape(item.render_strategy)
         << "\"}";
    if (!appendBounded(body, json.str(), kMaximumPortalResponseBytes))
      return errorResponse(413, "album_response_too_large");
  }
  if (!appendBounded(body, "]}", kMaximumPortalResponseBytes))
    return errorResponse(413, "album_response_too_large");
  return response(200, body);
}

PortalResponse PortalCore::renderChat(const PortalRequest& request) {
  std::string path;
  std::string query;
  if (!splitPath(request.path, path, query))
    return errorResponse(400, "invalid_path");
  Fields fields;
  if (!parseFields(query, false, 2U, fields) ||
      !onlyFields(fields, {"after", "limit"})) {
    return errorResponse(422, "invalid_chat_query");
  }
  ChatPageQuery page_query;
  if (const std::string* after = field(fields, "after")) {
    if (!parseUnsigned(*after, page_query.after_sequence))
      return errorResponse(422, "invalid_chat_cursor");
  }
  if (const std::string* limit = field(fields, "limit")) {
    uint64_t parsed = 0;
    if (!parseUnsigned(*limit, parsed) || parsed == 0U ||
        parsed > kMaximumChatPageItems) {
      return errorResponse(422, "invalid_chat_limit");
    }
    page_query.limit = static_cast<size_t>(parsed);
  }
  ChatPage page;
  const PortalResult result = cache_.readLocalChatPage(page_query, page);
  if (result != PortalResult::Ok) return resultError(result, "chat_page_invalid");
  if (page.items.size() > page_query.limit ||
      page.total_items > kMaximumChatTotalItems ||
      page.items.size() > page.total_items ||
      page.next_after_sequence < page_query.after_sequence ||
      (page.has_more && page.next_after_sequence == page_query.after_sequence)) {
    return errorResponse(422, "chat_page_invalid");
  }
  std::string body = "{\"ok\":true,\"retention\":\"local\",\"audioIncluded\":false,";
  body += "\"page\":{\"after\":" + std::to_string(page_query.after_sequence) +
          ",\"nextAfter\":" + std::to_string(page.next_after_sequence) +
          ",\"hasMore\":" + (page.has_more ? "true" : "false") +
          ",\"limit\":" + std::to_string(page_query.limit) +
          ",\"totalItems\":" + std::to_string(page.total_items) +
          ",\"maximumItems\":" + std::to_string(kMaximumChatTotalItems) +
          ",\"corruptionObserved\":" +
          (page.corruption_observed ? "true" : "false") + "},\"messages\":[";
  uint64_t previous = page_query.after_sequence;
  size_t aggregate = 0;
  size_t emitted = 0;
  for (const ChatItem& item : page.items) {
    if (item.sequence <= previous ||
        static_cast<uint8_t>(item.role) >
            static_cast<uint8_t>(ChatRole::Tool) ||
        !validText(item.text, kMaximumChatTextBytes, false)) {
      return errorResponse(422, "chat_page_invalid");
    }
    previous = item.sequence;
    if (blankAudioArtifact(item.text)) continue;
    if (aggregate > kMaximumChatPageTextBytes - item.text.size())
      return errorResponse(413, "chat_page_too_large");
    aggregate += item.text.size();
    std::ostringstream json;
    if (emitted++ != 0U) json << ',';
    json << "{\"sequence\":" << item.sequence << ",\"role\":\""
         << chatRoleName(item.role) << "\",\"text\":\""
         << jsonEscape(item.text) << "\"}";
    if (!appendBounded(body, json.str(), kMaximumPortalResponseBytes))
      return errorResponse(413, "chat_response_too_large");
  }
  if (page.next_after_sequence < previous)
    return errorResponse(422, "chat_page_invalid");
  if (!appendBounded(body, "]}", kMaximumPortalResponseBytes))
    return errorResponse(413, "chat_response_too_large");
  return response(200, body);
}

PortalResponse PortalCore::updateSettings(const PortalRequest& request) {
  if (!formContentType(request.content_type))
    return errorResponse(415, "form_content_type_required");
  PortalBoardCapabilities capabilities;
  const PortalResult capability_result =
      readCapabilities(cache_, capabilities);
  if (capability_result != PortalResult::Ok)
    return resultError(capability_result, "capabilities_unavailable");
  Fields fields;
  const std::vector<std::string> allowed = {
      "volume", "led_brightness", "voice_assistance", "assistant_prompt",
      "image_prompt_template", "negative_prompt", "storage_preference",
      "default_render_strategy", "local_password",
      "local_password_confirm", "reset_local_password"};
  if (!parseFields(request.body, true, allowed.size(), fields) || fields.empty() ||
      !onlyFields(fields, allowed)) {
    return errorResponse(400, "invalid_settings_request");
  }
  PortalCommand command;
  command.type = PortalCommandType::UpdateSettings;
  PortalSettingsPatch& patch = command.settings;
  if (const std::string* value = field(fields, "volume")) {
    if (!capabilities.hasDuplexAudio())
      return errorResponse(422, "audio_not_supported");
    patch.has_volume = true;
    if (!parseBytePercent(*value, 0U, patch.volume))
      return errorResponse(422, "invalid_volume");
  }
  if (const std::string* value = field(fields, "led_brightness")) {
    if (capabilities.rgb_pixels == 0U)
      return errorResponse(422, "rgb_not_supported");
    patch.has_led_maximum_brightness = true;
    if (!parseBytePercent(*value, 0U, patch.led_maximum_brightness_percent))
      return errorResponse(422, "invalid_led_brightness");
  }
  if (const std::string* value = field(fields, "voice_assistance")) {
    if (!capabilities.hasDuplexAudio())
      return errorResponse(422, "audio_not_supported");
    patch.has_voice_assistance_enabled = true;
    if (!parseBoolean(*value, patch.voice_assistance_enabled))
      return errorResponse(422, "invalid_voice_assistance");
  }
  if (const std::string* value = field(fields, "assistant_prompt")) {
    patch.has_assistant_prompt = true;
    if (!validText(*value, kMaximumAssistantPromptBytes, true))
      return errorResponse(422, "invalid_assistant_prompt");
    patch.assistant_prompt = *value;
  }
  if (const std::string* value = field(fields, "image_prompt_template")) {
    patch.has_image_prompt_template = true;
    if (!validText(*value, kMaximumImagePromptBytes, false))
      return errorResponse(422, "invalid_image_prompt_template");
    patch.image_prompt_template = *value;
  }
  if (const std::string* value = field(fields, "negative_prompt")) {
    patch.has_negative_prompt = true;
    if (!validText(*value, kMaximumNegativePromptBytes, true))
      return errorResponse(422, "invalid_negative_prompt");
    patch.negative_prompt = *value;
  }
  if (const std::string* value = field(fields, "storage_preference")) {
    if (*value != "automatic" && *value != "internal" &&
        *value != "removable") {
      return errorResponse(422, "invalid_storage_preference");
    }
    if (*value == "removable" && !capabilities.has_removable_storage)
      return errorResponse(422, "removable_storage_not_supported");
    patch.has_asset_storage_preference = true;
    patch.asset_storage_preference = *value;
  }
  if (const std::string* value = field(fields, "default_render_strategy")) {
    if (!validRenderStrategyId(*value))
      return errorResponse(422, "invalid_default_render_strategy");
    if (!capabilities.supportsRenderStrategy(*value))
      return errorResponse(422, "render_strategy_not_supported");
    patch.has_default_render_strategy = true;
    patch.default_render_strategy = *value;
  }
  bool reset_password = false;
  if (const std::string* value = field(fields, "reset_local_password")) {
    if (!parseBoolean(*value, reset_password))
      return errorResponse(422, "invalid_reset_local_password");
  }
  const std::string* password = field(fields, "local_password");
  const std::string* confirmation = field(fields, "local_password_confirm");
  const bool custom_password_requested =
      (password && !password->empty()) ||
      (confirmation && !confirmation->empty());
  if (reset_password && custom_password_requested)
    return errorResponse(422, "local_password_mode_conflict");
  if (custom_password_requested) {
    if (!password || !confirmation || *password != *confirmation)
      return errorResponse(422, "local_password_mismatch");
    if (!safePassword(*password))
      return errorResponse(422, "invalid_local_management_password");
    patch.has_local_management_password_override = true;
    patch.local_management_password_override = *password;
  } else if (reset_password) {
    patch.has_local_management_password_override = true;
    patch.local_management_password_override.clear();
  }
  return enqueueCommand(std::move(command));
}

PortalResponse PortalCore::requestFirmwareUpdate(
    const PortalRequest& request) {
  if (!formContentType(request.content_type))
    return errorResponse(415, "form_content_type_required");
  Fields fields;
  if (!parseFields(request.body, true, 1U, fields) || fields.size() != 1U ||
      fields[0].first != "confirm") {
    return errorResponse(400, "invalid_firmware_update_request");
  }
  if (!constantTimeEquals(fields[0].second, "install-signed-firmware")) {
    return errorResponse(422, "firmware_update_confirmation_required");
  }
  PortalStateSnapshot state;
  const PortalResult state_result = cache_.readState(state);
  if (state_result != PortalResult::Ok)
    return resultError(state_result, "firmware_update_state_invalid");
  if (!validState(state))
    return errorResponse(422, "firmware_update_state_invalid");
  if (!state.firmware_update.configured)
    return errorResponse(503, "firmware_update_unavailable");
  if (state.firmware_update.accepted_offline) {
    PortalResponse output = errorResponse(409, "firmware_update_busy");
    output.retry_after_seconds = 5U;
    return output;
  }
  PortalCommand command;
  command.type = PortalCommandType::RequestFirmwareUpdate;
  return enqueueCommand(std::move(command));
}

PortalResponse PortalCore::enqueueSimple(const PortalRequest& request,
                                         PortalCommandType type) {
  if (!request.body.empty()) return errorResponse(400, "unexpected_request_body");
  PortalCommand command;
  command.type = type;
  return enqueueCommand(std::move(command));
}

PortalResponse PortalCore::enqueueAsset(const PortalRequest& request,
                                        PortalCommandType type) {
  if (!formContentType(request.content_type))
    return errorResponse(415, "form_content_type_required");
  Fields fields;
  if (!parseFields(request.body, true, 1U, fields) || fields.size() != 1U ||
      fields[0].first != "asset_id" ||
      !safeIdentifier(fields[0].second, kMaximumAlbumIdBytes)) {
    return errorResponse(422, "invalid_asset_id");
  }
  PortalCommand command;
  command.type = type;
  command.asset_id = fields[0].second;
  return enqueueCommand(std::move(command));
}

PortalResponse PortalCore::enqueueCommand(PortalCommand command) {
  command.request_id = nextRequestId();
  const PortalResult result = commands_.tryEnqueue(command);
  if (result != PortalResult::Ok) return resultError(result, "command_rejected");
  if (command.type == PortalCommandType::RequestFirmwareUpdate) {
    return response(
        202,
        std::string("{\"ok\":true,\"state\":\"accepted\",\"requestId\":") +
            std::to_string(command.request_id) +
            ",\"command\":\"REQUEST_FIRMWARE_UPDATE\"," +
            "\"offlineAfterAcceptance\":true," +
            "\"resultAfterReboot\":\"when_recorded\"}");
  }
  return response(202,
                  std::string("{\"ok\":true,\"state\":\"queued\",\"requestId\":") +
                      std::to_string(command.request_id) +
                      ",\"command\":\"" +
                      portalCommandTypeName(command.type) + "\"}");
}

uint64_t PortalCore::nextRequestId() {
  const uint64_t output = next_request_id_;
  if (next_request_id_ == std::numeric_limits<uint64_t>::max())
    next_request_id_ = 1U;
  else
    ++next_request_id_;
  return output;
}

const char* PortalCore::dashboardHtml() {
  static constexpr char kHtml[] = R"INKLOOP(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Inkloop 本地管理</title><style>
:root{color-scheme:light;--ink:#18201c;--paper:#f5f0df;--line:#c9c1a8;--accent:#0b6b55;--warn:#9b3b25}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.5 system-ui,sans-serif}main{max-width:980px;margin:auto;padding:20px}h1{margin:.2rem 0}.card{border:1px solid var(--line);border-radius:12px;background:#fffdf5;padding:16px;margin:14px 0}.tabs{display:flex;gap:8px;overflow:auto}.tabs button,.actions button,button{border:1px solid var(--line);border-radius:8px;background:white;padding:9px 13px}.tabs button.active{background:var(--ink);color:white}.panel{display:none}.panel.active{display:block}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}.album{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px}.album article{border:1px solid var(--line);padding:10px;border-radius:10px}.album img,.upload-preview{display:block;width:100%;height:auto;aspect-ratio:2/3;object-fit:contain;background:#eee}.album-controls{display:grid;grid-template-columns:1fr 1fr;gap:6px}.album-controls button{width:100%;padding:7px}.album select{margin:6px 0}label{display:block;margin:8px 0}input,textarea,select{width:100%;padding:8px;margin-top:4px}textarea{min-height:90px}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row input[type=checkbox]{width:auto}.muted{color:#606760}.error{color:var(--warn)}#status{position:sticky;bottom:8px;background:#18201cee;color:white;padding:10px;border-radius:8px}ol{padding-left:24px}.chat-role{font-weight:700;margin-right:8px}.diag-summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:10px 0}.diag-summary div{border:1px solid var(--line);border-radius:8px;padding:8px}.diag-summary dt{color:#606760;font-size:.8rem}.diag-summary dd{margin:2px 0 0;font-variant-numeric:tabular-nums;font-weight:650}.diag-scroll{overflow-x:auto;border:1px solid var(--line);border-radius:8px}.diag-table{width:100%;border-collapse:collapse;font-size:.82rem;font-variant-numeric:tabular-nums}.diag-table caption{text-align:left;padding:8px;color:#606760}.diag-table th,.diag-table td{padding:7px 8px;border-top:1px solid var(--line);text-align:right;white-space:nowrap}.diag-table th:first-child,.diag-table td:first-child{text-align:left}.diag-table tbody th{font-weight:650}.diag-table .empty{text-align:center;color:#606760}@media(max-width:560px){main{padding:12px}.diag-summary{grid-template-columns:1fr 1fr}}
</style></head><body><main><h1>Inkloop 本地管理</h1><p class="muted">设备端仅提供本地缓存与排队操作；上屏、生成、MyAI 和存储工作在 owner task 中执行。</p>
<section id="login-card" class="card"><form id="login"><label>本地管理密码<input name="nonce" type="password" minlength="8" maxlength="63" required></label><button>进入</button></form></section>
<section id="portal" hidden><nav class="tabs" aria-label="设置分类"><button data-tab="device">设备</button><button data-tab="album">相册</button><button data-tab="myai">MyAI</button><button data-tab="settings">设置</button></nav>
<div class="panel card" data-panel="device"><h2>设备状态</h2><dl id="device-state"></dl><section aria-labelledby="firmware-title"><h3 id="firmware-title">系统与固件</h3><p id="firmware-update-state" class="muted" role="status">等待固件状态</p><p id="firmware-update-detail" class="muted"></p><button id="firmware-update" type="button" disabled>检查并安装签名更新</button><p class="muted">请求获接受后，本地 Portal 会主动离线；设备将验证并安装与本机型号匹配的签名固件，然后重启。请保持供电；若设备成功记录终态，重连后可查看该结果。</p></section><section aria-labelledby="runtime-title"><h3 id="runtime-title">运行诊断</h3><p class="muted">用于排查按键与语音延迟；仅随当前可见的本地管理页面状态更新。</p><dl id="runtime-summary" class="diag-summary"><div><dt>采样状态</dt><dd>等待数据</dd></div></dl><div class="diag-scroll" role="region" aria-label="运行通道诊断表" tabindex="0"><table class="diag-table"><caption>队列、栈与调度累计值</caption><thead><tr><th scope="col">通道</th><th scope="col">核心 / 优先级</th><th scope="col">队列 当前 / 峰值 / 容量</th><th scope="col">栈最低余量</th><th scope="col">Handler 最大</th><th scope="col">Tick 最大</th><th scope="col">延迟 / 漏拍</th></tr></thead><tbody id="runtime-diagnostics"><tr><td class="empty" colspan="7">等待运行时采样</td></tr></tbody></table></div></section></div>
<div class="panel card" data-panel="album"><h2>相册</h2><form id="upload"><label>图片标题<input name="title" maxlength="64" required></label><label>选择图片<input name="image" type="file" accept="image/*" required></label><canvas id="upload-preview" class="upload-preview" width="400" height="600"></canvas><button>按预览上传</button></form><div id="album" class="album"></div><div class="actions"><button id="album-more">下一页</button></div></div>
<script>(()=>{'use strict';const previous=window.fetch.bind(window),canvas=document.querySelector('#upload-preview');function apply(state){if(!state||!canvas)return;const width=Number(state.displayWidth),height=Number(state.displayHeight);if(!Number.isInteger(width)||!Number.isInteger(height)||width<1||height<1||width>8192||height>8192)return;if(canvas.width!==width)canvas.width=width;if(canvas.height!==height)canvas.height=height;canvas.style.aspectRatio=width+'/'+height}window.fetch=async(...args)=>{const response=await previous(...args);if(String(args[0])==='/api/state'&&response.ok)response.clone().json().then(v=>apply(v.state)).catch(()=>{});return response}})();</script>
<div class="panel card" data-panel="myai"><h2>MyAI</h2><p id="myai-state"></p><p><a id="binding-link" rel="noreferrer" target="_blank" hidden>打开绑定页面</a></p><div class="row"><button id="pair">申请绑定码</button><button id="rebind">恢复 / 重新绑定</button></div><h3>本地聊天</h3><ol id="chat"></ol><div class="row"><button id="chat-more">下一页</button><button id="chat-clear">清空本地聊天</button></div><h3>内容生成</h3><form id="generate"><label>图片主题<textarea name="prompt" maxlength="1024" required></textarea></label><button>排队生成</button></form></div>
<div class="panel card" data-panel="settings"><h2>设置</h2><form id="settings"><fieldset><legend>声音与状态灯</legend><div class="grid"><label id="audio-volume-setting">音量 <output id="volume-out"></output><input name="volume" type="range" min="0" max="100"></label><label id="voice-assistance-setting" class="row"><input name="voice_assistance" type="checkbox">启用语音辅助</label><label id="led-brightness-setting">LED 最大亮度 <output id="led-out"></output><input name="led_brightness" type="range" min="0" max="100"></label></div><button id="preview-volume" type="button">试听当前音量</button></fieldset><fieldset><legend>画面、保存与刷新</legend><div class="grid"><label>相册存储<select name="storage_preference"></select></label><label>默认渲染方式<select name="default_render_strategy"></select></label></div><p>存储位置在下次重启后统一应用；单张图片仍可在相册中单独选择渲染方式。</p></fieldset><fieldset><legend>MyAI 与提示词</legend><label>智能体提示词<textarea name="assistant_prompt" maxlength="512" required></textarea></label><label>图片提示词模板<textarea name="image_prompt_template" maxlength="512" required></textarea></label><label>图片负面提示词<textarea name="negative_prompt" maxlength="384"></textarea></label></fieldset><fieldset><legend>本地访问</legend><p id="password-mode">当前默认使用已保存的家庭 Wi-Fi 密码。</p><div class="grid"><label>新本地管理密码<input name="local_password" type="password" minlength="8" maxlength="63" autocomplete="new-password"></label><label>再次输入<input name="local_password_confirm" type="password" minlength="8" maxlength="63" autocomplete="new-password"></label></div><label class="row"><input name="reset_local_password" type="checkbox">恢复为家庭 Wi-Fi 密码</label><p>修改后重启生效，并同时用于 Settings Wi-Fi 与网页登录。</p></fieldset><button>保存设置</button></form></div></section><p id="status" role="status">请输入本地管理密码。</p></main>
<script>(()=>{'use strict';const originalFetch=window.fetch.bind(window);window.fetch=async(...args)=>{const response=await originalFetch(...args);const url=String(args[0]);if(url==='/api/session'&&response.ok){try{const value=await response.clone().json();if(value.csrfToken)window.__inkloopCsrf=value.csrfToken}catch(_){}}return response};const post=(path,data)=>originalFetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Inkloop-CSRF':window.__inkloopCsrf||''},body:new URLSearchParams(data)});function enhanceAlbum(){const catalog=window.__inkloopCapabilities?.renderStrategies;if(!Array.isArray(catalog)||!catalog.length||catalog.length>4)return;document.querySelectorAll('#album article:not([data-enhanced])').forEach(card=>{const img=card.querySelector('img'),button=card.querySelector('button');if(!img||!button)return;const id=new URL(img.src,location.href).searchParams.get('asset_id');if(!id)return;card.dataset.enhanced='1';const select=document.createElement('select');for(const entry of catalog){const option=document.createElement('option');option.value=entry.id;option.textContent=entry.displayName;select.append(option)}select.value=catalog.some(x=>x.id===card.dataset.renderStrategy)?card.dataset.renderStrategy:catalog[0].id;select.setAttribute('aria-label','渲染方式');select.onchange=async()=>{const r=await post('/api/album/render',{asset_id:id,render_strategy:select.value});document.querySelector('#status').textContent=r.ok?'渲染方式已排队。':'修改渲染方式失败。'};const actions=document.createElement('div');actions.className='album-controls';button.remove();actions.append(button);const remove=document.createElement('button');remove.textContent='删除';remove.onclick=async()=>{if(!confirm('删除这张本地图片？'))return;const r=await post('/api/album/delete',{asset_id:id});document.querySelector('#status').textContent=r.ok?'删除已排队。':'删除失败或当前版本暂不支持。'};actions.append(remove);card.append(select,actions)})}new MutationObserver(enhanceAlbum).observe(document.querySelector('#album'),{childList:true});document.querySelector('[data-tab="device"]').addEventListener('click',async()=>{try{const r=await originalFetch('/api/state'),v=await r.json(),d=document.querySelector('#device-state');if(!v.ok||!v.state||d.querySelector('[data-storage-capacity]'))return;const f=x=>{for(const u of ['B','KiB','MiB','GiB']){if(x<1024||u==='GiB')return(u==='B'?x:x.toFixed(1))+' '+u;x/=1024}};for(const [name,value] of [['剩余空间',f(v.state.storageFreeBytes)],['总空间',f(v.state.storageTotalBytes)]]){const dt=document.createElement('dt'),dd=document.createElement('dd');dt.dataset.storageCapacity='1';dd.dataset.storageCapacity='1';dt.textContent=name;dd.textContent=value;d.append(dt,dd)}}catch(_){}})})();</script>
<script>(()=>{'use strict';let csrf='',state=null,albumCursor='',chatAfter=0,uploadBlob=null;const q=s=>document.querySelector(s),status=q('#status');function note(x,bad=false){status.textContent=x;status.classList.toggle('error',bad)}async function json(path,options={}){options.headers=new Headers(options.headers||{});options.headers.set('Accept','application/json');if(options.method==='POST'&&csrf)options.headers.set('X-Inkloop-CSRF',csrf);const r=await fetch(path,options),t=await r.text();if(t.length>32768)throw Error('response_too_large');let v;try{v=JSON.parse(t)}catch(_){throw Error('invalid_response')}if(!r.ok||v.ok===false)throw Error(v.error||('http_'+r.status));return v}function form(path,data){return json(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)})}function applyCapabilities(c,f){const list=c?.renderStrategies,seen=new Set;if(!Array.isArray(list)||list.length<1||list.length>4||typeof c.microphone!=='boolean'||typeof c.speaker!=='boolean'||typeof c.duplexAudio!=='boolean'||c.duplexAudio!==(c.microphone&&c.speaker)||!Number.isInteger(c.rgbPixels)||c.rgbPixels<0||c.rgbPixels>8||typeof c.removableStorage!=='boolean')throw Error('invalid_capabilities');for(const x of list){if(!x||typeof x.id!=='string'||typeof x.displayName!=='string'||x.id.length>32||x.displayName.length<1||x.displayName.length>64||!/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(x.id)||seen.has(x.id))throw Error('invalid_capabilities');seen.add(x.id)}window.__inkloopCapabilities=c;const audio=c.duplexAudio,rgb=c.rgbPixels>0;q('#audio-volume-setting').hidden=!audio;q('#voice-assistance-setting').hidden=!audio;q('#preview-volume').hidden=!audio;f.volume.disabled=!audio;f.voice_assistance.disabled=!audio;q('#preview-volume').disabled=!audio;q('#led-brightness-setting').hidden=!rgb;f.led_brightness.disabled=!rgb;const storage=[['automatic','自动选择'],['internal','设备内置存储']];if(c.removableStorage)storage.push(['removable','TF 卡']);f.storage_preference.replaceChildren(...storage.map(([value,label])=>{const o=document.createElement('option');o.value=value;o.textContent=label;return o}));f.default_render_strategy.replaceChildren(...list.map(x=>{const o=document.createElement('option');o.value=x.id;o.textContent=x.displayName;return o}))}function tab(name){document.querySelectorAll('[data-tab]').forEach(x=>x.classList.toggle('active',x.dataset.tab===name));document.querySelectorAll('[data-panel]').forEach(x=>x.classList.toggle('active',x.dataset.panel===name));if(name==='album')loadAlbum(albumCursor);if(name==='myai')loadChat(chatAfter)}document.querySelectorAll('[data-tab]').forEach(x=>x.onclick=()=>tab(x.dataset.tab));q('#login').onsubmit=async e=>{e.preventDefault();try{const v=await form('/api/session',{nonce:e.target.nonce.value});csrf=v.csrfToken;q('#login-card').hidden=true;q('#portal').hidden=false;tab('device');await loadState();note('已连接本地 Portal。')}catch(x){note('登录失败：'+x.message,true)}};async function loadState(){const v=await json('/api/state');state=v.state;const d=q('#device-state');d.replaceChildren();for(const [k,x] of Object.entries({固件:state.firmwareVersion,设备:state.deviceName,网络:state.wifiOnline?'在线':'离线',存储:state.storageReady?'就绪':'不可用',屏幕:state.displayBusy?'忙':'就绪'})){const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=k;dd.textContent=x;d.append(dt,dd)}const m=state.myAi;q('#myai-state').textContent='状态：'+m.state+(m.pairingCode?' · 绑定码：'+m.pairingCode:'');const a=q('#binding-link');a.hidden=!m.bindingUrl;if(m.bindingUrl)a.href=m.bindingUrl;const s=state.settings,f=q('#settings');applyCapabilities(state.capabilities,f);f.volume.value=s.volume;f.voice_assistance.checked=s.voiceAssistanceEnabled;f.led_brightness.value=s.ledMaximumBrightness;f.assistant_prompt.value=s.assistantPrompt;f.image_prompt_template.value=s.imagePromptTemplate;f.negative_prompt.value=s.negativePrompt;f.storage_preference.value=s.assetStoragePreference;if(!f.storage_preference.value)f.storage_preference.value='automatic';f.default_render_strategy.value=s.defaultRenderStrategy;if(!f.default_render_strategy.value)throw Error('invalid_default_render_strategy');q('#password-mode').textContent=s.localManagementPasswordOverridden?'当前使用自定义本地管理密码。':'当前使用已保存的家庭 Wi-Fi 密码。';syncOutputs()}function syncOutputs(){q('#volume-out').textContent=q('#settings').volume.value+'%';q('#led-out').textContent=q('#settings').led_brightness.value+'%'}q('#settings').oninput=syncOutputs;q('#settings').onsubmit=async e=>{e.preventDefault();const f=e.target,data={assistant_prompt:f.assistant_prompt.value,image_prompt_template:f.image_prompt_template.value,negative_prompt:f.negative_prompt.value,storage_preference:f.storage_preference.value,default_render_strategy:f.default_render_strategy.value,local_password:f.local_password.value,local_password_confirm:f.local_password_confirm.value,reset_local_password:f.reset_local_password.checked?'1':'0'};if(!f.volume.disabled){data.volume=f.volume.value;data.voice_assistance=f.voice_assistance.checked?'1':'0'}if(!f.led_brightness.disabled)data.led_brightness=f.led_brightness.value;try{await form('/api/settings',data);f.local_password.value='';f.local_password_confirm.value='';f.reset_local_password.checked=false;note('设置已排队保存；存储位置或本地密码变更将在重启后生效。')}catch(x){note('保存失败：'+x.message,true)}};q('#preview-volume').onclick=async()=>{try{await form('/api/audio/preview',{volume:q('#settings').volume.value});note('音量试听已排队。')}catch(x){note('试听失败：'+x.message,true)}};q('#pair').onclick=async()=>{try{await form('/api/onboarding/myai/start',{});note('MyAI 绑定已排队。')}catch(x){note(x.message,true)}};q('#rebind').onclick=async()=>{if(!confirm('清除旧 MyAI 设备凭据并重新绑定？'))return;try{await form('/api/onboarding/myai/rebind',{});note('MyAI 重新绑定已排队。')}catch(x){note(x.message,true)}};q('#generate').onsubmit=async e=>{e.preventDefault();try{await form('/api/aigc/generate',{prompt:e.target.prompt.value});note('内容生成已排队。')}catch(x){note(x.message,true)}};async function loadAlbum(cursor=''){try{const v=await json('/api/album?limit=16&cursor='+encodeURIComponent(cursor));const root=q('#album');root.replaceChildren();for(const item of v.items){const card=document.createElement('article'),img=document.createElement('img'),h=document.createElement('h3'),p=document.createElement('p'),button=document.createElement('button');card.dataset.renderStrategy=item.renderStrategy;img.loading='lazy';img.src='/api/album/preview?asset_id='+encodeURIComponent(item.id);h.textContent=item.title||item.id;p.textContent=item.origin+' · '+item.bytes+' B';button.textContent=item.current?'当前图片':'排队上屏';button.disabled=item.current;button.onclick=async()=>{try{await form('/api/album/display',{asset_id:item.id});note('上屏已排队。')}catch(x){note(x.message,true)}};card.append(img,h,p,button);root.append(card)}albumCursor=v.page.nextCursor;q('#album-more').disabled=!albumCursor}catch(x){note('相册读取失败：'+x.message,true)}}q('#album-more').onclick=()=>loadAlbum(albumCursor);q('#upload').image.onchange=e=>{const file=e.target.files&&e.target.files[0];if(!file)return;if(file.size>12582912){note('源图片超过 12 MiB。',true);return}const url=URL.createObjectURL(file),img=new Image;img.onload=()=>{const c=q('#upload-preview'),ctx=c.getContext('2d',{alpha:false}),scale=Math.max(c.width/img.width,c.height/img.height),w=img.width*scale,h=img.height*scale;ctx.fillStyle='#fff';ctx.fillRect(0,0,c.width,c.height);ctx.drawImage(img,(c.width-w)/2,(c.height-h)/2,w,h);c.toBlob(b=>{URL.revokeObjectURL(url);if(!b||b.size>1500000){uploadBlob=null;note('转换后的 PNG 超过 1.5 MB。',true);return}uploadBlob=b;note('图片预览已就绪：'+b.size+' B。')},'image/png')};img.onerror=()=>{URL.revokeObjectURL(url);note('无法读取图片。',true)};img.src=url};q('#upload').onsubmit=async e=>{e.preventDefault();if(!uploadBlob){note('请先选择并预览图片。',true);return}try{const title=e.target.title.value;await json('/api/album/upload?title='+encodeURIComponent(title),{method:'POST',headers:{'Content-Type':'image/png'},body:uploadBlob});note('上传已交给本地存储队列。');albumCursor='';await loadAlbum('')}catch(x){note('上传失败：'+x.message,true)}};async function loadChat(after=0){try{const v=await json('/api/chat?limit=24&after='+after),root=q('#chat');root.replaceChildren();for(const m of v.messages){const li=document.createElement('li'),role=document.createElement('span');role.className='chat-role';role.textContent={user:'我',assistant:'MyAI',tool:'设备'}[m.role]||'设备';li.append(role,document.createTextNode(m.text));root.append(li)}chatAfter=v.page.nextAfter;q('#chat-more').disabled=!v.page.hasMore}catch(x){note('本地聊天读取失败：'+x.message,true)}}q('#chat-more').onclick=()=>loadChat(chatAfter);q('#chat-clear').onclick=async()=>{if(!confirm('清空设备本地聊天？'))return;try{await form('/api/chat/clear',{});note('清聊天已排队。')}catch(x){note(x.message,true)}};window.__inkloopPortalRefresh=async cycle=>{const active=q('[data-tab].active')?.dataset.tab||'device';if(active!=='settings')await loadState();if(active==='album'&&cycle%2===0)await loadAlbum('');if(active==='myai')await loadChat(0)};tab('device')})();</script>
<script>(()=>{'use strict';const cadence=5000;let timer=0,busy=false,cycle=0;const portal=document.querySelector('#portal');function stop(){if(timer){clearTimeout(timer);timer=0}}async function poll(){stop();if(document.hidden||portal.hidden||typeof window.__inkloopPortalRefresh!=='function')return;if(!busy){busy=true;try{await window.__inkloopPortalRefresh(++cycle)}catch(_){}finally{busy=false}}timer=setTimeout(poll,cadence)}document.addEventListener('visibilitychange',()=>document.hidden?stop():poll());new MutationObserver(()=>{if(portal.hidden)stop();else poll()}).observe(portal,{attributes:true,attributeFilter:['hidden']})})();</script>
<script>(()=>{'use strict';let timing=null;const previous=window.fetch.bind(window),root=document.querySelector('#device-state');function render(){if(!timing||!timing.completedRefreshes)return;const signature=[timing.loadDecodeMs,timing.conversionMs,timing.panelRefreshMs,timing.totalMs].join(':');if(root.dataset.displayTimingSignature===signature&&root.querySelector('[data-display-timing]'))return;root.querySelectorAll('[data-display-timing]').forEach(x=>x.remove());root.dataset.displayTimingSignature=signature;for(const [name,value] of [['最近图片读取/解码',timing.loadDecodeMs+' ms'],['最近六色转换',timing.conversionMs+' ms'],['最近物理刷新',timing.panelRefreshMs+' ms'],['最近上屏总耗时',timing.totalMs+' ms']]){const dt=document.createElement('dt'),dd=document.createElement('dd');dt.dataset.displayTiming='1';dd.dataset.displayTiming='1';dt.textContent=name;dd.textContent=value;root.append(dt,dd)}}window.fetch=async(...args)=>{const response=await previous(...args);if(String(args[0])==='/api/state'&&response.ok){response.clone().json().then(v=>{timing=v.state&&v.state.displayTiming;render()}).catch(()=>{})}return response};new MutationObserver(render).observe(root,{childList:true})})();</script>
<script>(()=>{'use strict';const labels=['输入','语音','控制','状态灯','存储','显示','网络','Portal'],body=document.querySelector('#runtime-diagnostics'),summary=document.querySelector('#runtime-summary'),previous=window.fetch.bind(window),u32=x=>Number.isInteger(x)&&x>=0&&x<=4294967295,bytes=x=>{if(!u32(x))return'—';for(const unit of ['B','KiB','MiB','GiB']){if(x<1024||unit==='GiB')return(unit==='B'?x:x.toFixed(1))+' '+unit;x/=1024}},micros=(value,count)=>u32(value)&&u32(count)&&count?value.toLocaleString()+' μs':'—';function unavailable(text){summary.replaceChildren();const box=document.createElement('div'),dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent='采样状态';dd.textContent=text;box.append(dt,dd);summary.append(box);body.replaceChildren();const tr=document.createElement('tr'),td=document.createElement('td');td.className='empty';td.colSpan=7;td.textContent=text;tr.append(td);body.append(tr)}function render(runtime){if(!runtime||runtime.available!==true||runtime.laneCount!==labels.length||!Array.isArray(runtime.lanes)||runtime.lanes.length!==labels.length){unavailable('运行诊断暂不可用');return}for(let index=0;index<labels.length;index++){const lane=runtime.lanes[index];if(!lane||lane.index!==index||![lane.queueCapacity,lane.queueDepth,lane.queueHighWater,lane.stackLowWaterBytes,lane.handlerCount,lane.handlerMaxUs,lane.tickCount,lane.tickMaxUs,lane.tickLateCount,lane.tickMissed,lane.tickLateMaxUs].every(u32)||lane.queueDepth>lane.queueHighWater||lane.queueHighWater>lane.queueCapacity){unavailable('运行诊断数据无效');return}}summary.replaceChildren();const fields=[['内部堆最低余量',runtime.internalHeapSampled?bytes(runtime.internalHeapMinFreeBytes):'未采样'],['PSRAM 最低余量',runtime.psramAvailable?bytes(runtime.psramMinFreeBytes):'不可用'],['资源采样',u32(runtime.resourceSampleCount)?runtime.resourceSampleCount.toLocaleString()+' 次':'—'],['快照序号',u32(runtime.sequence)?runtime.sequence.toLocaleString():'—']];for(const [name,value] of fields){const box=document.createElement('div'),dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=name;dd.textContent=value;box.append(dt,dd);summary.append(box)}body.replaceChildren();runtime.lanes.forEach((lane,index)=>{const tr=document.createElement('tr'),name=document.createElement('th');name.scope='row';name.textContent=labels[index];tr.append(name);const core=lane.running?'运行 · C'+lane.observedCore+' / P'+lane.observedPriority:'停止 · C'+lane.configuredCore+' / P'+lane.configuredPriority,values=[core,[lane.queueDepth,lane.queueHighWater,lane.queueCapacity].join(' / '),lane.stackSampled?bytes(lane.stackLowWaterBytes):'未采样',micros(lane.handlerMaxUs,lane.handlerCount),micros(lane.tickMaxUs,lane.tickCount),lane.tickLateCount+' / '+lane.tickMissed+(lane.tickLateCount?' · '+lane.tickLateMaxUs.toLocaleString()+' μs':'')];for(const value of values){const td=document.createElement('td');td.textContent=value;tr.append(td)}body.append(tr)})}window.fetch=async(...args)=>{const response=await previous(...args);if(String(args[0])==='/api/state'&&response.ok)response.clone().json().then(v=>render(v.state&&v.state.runtimeTelemetry)).catch(()=>unavailable('运行诊断响应无效'));return response}})();</script>
<script>(()=>{'use strict';
const button=document.querySelector('#firmware-update'),status=document.querySelector('#firmware-update-state'),detail=document.querySelector('#firmware-update-detail'),notice=document.querySelector('#status'),previous=window.fetch.bind(window),phases={unavailable:'固件更新不可用',ready:'可以请求签名更新',accepted_offline:'更新请求已接受，Portal 即将离线'},codes={none:'',up_to_date:'当前已经是最新版本',configuration_invalid:'固件更新配置不可用',network_unavailable:'无法连接更新服务',timed_out:'更新服务响应超时',manifest_rejected:'更新说明未通过安全检查',image_rejected:'固件镜像未通过安全检查',verification_failed:'固件签名验证失败',staging_failed:'固件暂存失败',internal_error:'固件更新内部错误',update_confirmed:'新固件已确认运行',update_rolled_back:'新固件未通过启动健康检查，已回滚'},phaseNames=Object.keys(phases),codeNames=Object.keys(codes);let snapshot=null,requestAccepted=false;
function fail(){snapshot=null;button.disabled=true;status.textContent='固件状态不可用';detail.textContent='设备返回了无效的固件状态。'}
function render(update){if(!update||typeof update.configured!=='boolean'||typeof update.acceptedOffline!=='boolean'||typeof update.currentVersion!=='string'||update.currentVersion.length<1||update.currentVersion.length>64||!phaseNames.includes(update.status)||!codeNames.includes(update.code)){fail();return}const valid=update.acceptedOffline?(update.configured&&update.status==='accepted_offline'&&update.code==='none'):(update.status===(update.configured?'ready':'unavailable')&&(!update.configured||update.code!=='configuration_invalid'));if(!valid){fail();return}snapshot=update;status.textContent=phases[update.status];detail.textContent='当前版本：'+update.currentVersion+(codes[update.code]?' · '+codes[update.code]:'');button.disabled=requestAccepted||!update.configured||update.acceptedOffline}
window.fetch=async(...args)=>{const response=await previous(...args);if(String(args[0])==='/api/state'&&response.ok)response.clone().json().then(v=>render(v.state&&v.state.firmwareUpdate)).catch(fail);return response};
button.onclick=async()=>{if(button.disabled||!snapshot)return;if(!confirm('请求签名固件更新？接受后本地 Portal 会离线，设备将在验证安装后重启；请保持供电，终态成功记录时可在重连后查看。'))return;button.disabled=true;status.textContent='正在提交固件更新请求';try{const response=await previous('/api/system/update',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Inkloop-CSRF':window.__inkloopCsrf||''},body:new URLSearchParams({confirm:'install-signed-firmware'})}),text=await response.text();if(text.length>1024)throw Error('response_too_large');let value;try{value=JSON.parse(text)}catch(_){throw Error('invalid_response')}if(!response.ok||value.ok!==true||value.state!=='accepted'||value.command!=='REQUEST_FIRMWARE_UPDATE'||value.offlineAfterAcceptance!==true||value.resultAfterReboot!=='when_recorded'||!Number.isSafeInteger(value.requestId)||value.requestId<1)throw Error(value.error||('http_'+response.status));requestAccepted=true;status.textContent=phases.accepted_offline;detail.textContent='设备会主动停止本地 Portal；终态成功记录时，重启并重连后可查看。';notice.classList.remove('error');notice.textContent='更新请求已接受；本地 Portal 即将离线，设备将在验证安装后重启。'}catch(error){button.disabled=!snapshot.configured||snapshot.acceptedOffline;status.textContent=phases[snapshot.status];notice.textContent='固件更新请求失败：'+error.message;notice.classList.add('error')}}})();</script></body></html>)INKLOOP";
  return kHtml;
}

const char* portalResultName(PortalResult value) {
  switch (value) {
    case PortalResult::Ok: return "OK";
    case PortalResult::InvalidConfiguration: return "INVALID_CONFIGURATION";
    case PortalResult::InvalidRequest: return "INVALID_REQUEST";
    case PortalResult::Unauthorized: return "UNAUTHORIZED";
    case PortalResult::Forbidden: return "FORBIDDEN";
    case PortalResult::TooLarge: return "TOO_LARGE";
    case PortalResult::Busy: return "BUSY";
    case PortalResult::Unavailable: return "UNAVAILABLE";
    case PortalResult::InvalidData: return "INVALID_DATA";
  }
  return "INVALID_DATA";
}

const char* portalCommandTypeName(PortalCommandType value) {
  switch (value) {
    case PortalCommandType::UpdateSettings: return "UPDATE_SETTINGS";
    case PortalCommandType::PreviewVolume: return "PREVIEW_VOLUME";
    case PortalCommandType::StartMyAiPairing: return "START_MYAI_PAIRING";
    case PortalCommandType::RebindMyAi: return "REBIND_MYAI";
    case PortalCommandType::DisplayAlbumItem: return "DISPLAY_ALBUM_ITEM";
    case PortalCommandType::DeleteAlbumItem: return "DELETE_ALBUM_ITEM";
    case PortalCommandType::SetAlbumRenderStrategy: return "SET_ALBUM_RENDER_STRATEGY";
    case PortalCommandType::GenerateImage: return "GENERATE_IMAGE";
    case PortalCommandType::ClearLocalChat: return "CLEAR_LOCAL_CHAT";
    case PortalCommandType::RequestFirmwareUpdate:
      return "REQUEST_FIRMWARE_UPDATE";
  }
  return "UNKNOWN";
}

}  // namespace portal
}  // namespace inkloop
