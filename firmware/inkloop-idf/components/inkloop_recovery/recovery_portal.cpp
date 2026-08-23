#include "inkloop/recovery/recovery_portal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>

namespace inkloop {
namespace recovery {
namespace {

constexpr size_t kMinimumAccessCodeBytes = 4U;
constexpr size_t kMinimumTokenBytes = 16U;
constexpr size_t kMaximumHostBytes = 128U;
constexpr size_t kMaximumOriginBytes = 160U;
constexpr uint32_t kMinimumSessionLifetimeSeconds = 60U;
constexpr uint32_t kMaximumSessionLifetimeSeconds = 3600U;
constexpr char kSessionCookieName[] = "inkloop_recovery_session";

bool boundedPrintable(const std::string& value, size_t minimum,
                      size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (const unsigned char ch : value) {
    if (ch < 0x20U || ch > 0x7eU) return false;
  }
  return true;
}

bool tokenText(const std::string& value) {
  if (value.size() < kMinimumTokenBytes ||
      value.size() > kMaximumRecoveryTokenBytes) {
    return false;
  }
  for (const unsigned char ch : value) {
    if (!std::isalnum(ch) && ch != '-' && ch != '_') return false;
  }
  return true;
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
  const size_t maximum = std::max(left.size(), right.size());
  unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
  for (size_t index = 0; index < maximum; ++index) {
    const unsigned char lhs = index < left.size()
                                  ? static_cast<unsigned char>(left[index])
                                  : 0U;
    const unsigned char rhs = index < right.size()
                                  ? static_cast<unsigned char>(right[index])
                                  : 0U;
    difference |= static_cast<unsigned int>(lhs ^ rhs);
  }
  return difference == 0U;
}

bool validEndpointList(const std::array<std::string, kMaximumRecoveryHosts>& values,
                       uint8_t count, size_t maximum) {
  if (count == 0U || static_cast<size_t>(count) > values.size()) return false;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index < count) {
      if (!boundedPrintable(values[index], 1U, maximum)) return false;
    } else if (!values[index].empty()) {
      return false;
    }
  }
  return true;
}

bool validOrigins(
    const std::array<std::string, kMaximumRecoveryOrigins>& values,
    uint8_t count) {
  if (!validEndpointList(values, count, kMaximumOriginBytes)) return false;
  for (size_t index = 0; index < count; ++index) {
    if (values[index].rfind("http://", 0U) != 0U &&
        values[index].rfind("https://", 0U) != 0U) {
      return false;
    }
  }
  return true;
}

bool exactCookieValue(const std::string& header, std::string& output) {
  output.clear();
  bool found = false;
  size_t offset = 0U;
  while (offset <= header.size()) {
    const size_t end = header.find(';', offset);
    const size_t last = end == std::string::npos ? header.size() : end;
    size_t first = offset;
    while (first < last && header[first] == ' ') ++first;
    size_t trimmed_last = last;
    while (trimmed_last > first && header[trimmed_last - 1U] == ' ')
      --trimmed_last;
    const size_t equal = header.find('=', first);
    if (equal != std::string::npos && equal < trimmed_last) {
      const std::string name = header.substr(first, equal - first);
      if (name == kSessionCookieName) {
        if (found) return false;
        output = header.substr(equal + 1U, trimmed_last - equal - 1U);
        found = true;
      }
    }
    if (end == std::string::npos) break;
    offset = end + 1U;
  }
  return found;
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool parseAccessCode(const std::string& body, std::string& output) {
  static constexpr char kPrefix[] = "code=";
  if (body.rfind(kPrefix, 0U) != 0U || body.find('&') != std::string::npos) {
    return false;
  }
  output.clear();
  output.reserve(body.size());
  for (size_t index = sizeof(kPrefix) - 1U; index < body.size(); ++index) {
    const char ch = body[index];
    if (ch == '+') {
      output.push_back(' ');
    } else if (ch == '%') {
      if (index + 2U >= body.size()) return false;
      const int high = hexValue(body[index + 1U]);
      const int low = hexValue(body[index + 2U]);
      if (high < 0 || low < 0) return false;
      output.push_back(static_cast<char>((high << 4U) | low));
      index += 2U;
    } else {
      output.push_back(ch);
    }
    if (output.size() > kMaximumRecoveryAccessCodeBytes) return false;
  }
  return boundedPrintable(output, kMinimumAccessCodeBytes,
                          kMaximumRecoveryAccessCodeBytes);
}

int lowerHexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

template <size_t Size>
std::string hexEncode(const std::array<uint8_t, Size>& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output(Size * 2U, '0');
  for (size_t index = 0; index < Size; ++index) {
    output[index * 2U] = kHex[value[index] >> 4U];
    output[index * 2U + 1U] = kHex[value[index] & 0x0fU];
  }
  return output;
}

template <size_t Size>
bool hexDecode(const std::string& value, std::array<uint8_t, Size>& output) {
  if (value.size() != Size * 2U) return false;
  output.fill(0U);
  for (size_t index = 0; index < Size; ++index) {
    const int high = lowerHexValue(value[index * 2U]);
    const int low = lowerHexValue(value[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}

template <size_t Size>
bool allZero(const std::array<uint8_t, Size>& value) {
  uint8_t combined = 0U;
  for (const uint8_t byte : value) combined |= byte;
  return combined == 0U;
}

bool parseActionDomain(const std::string& value,
                       RecoveryActionDomain& output) {
  if (value == "display") output = RecoveryActionDomain::Display;
  else if (value == "tasks") output = RecoveryActionDomain::Tasks;
  else if (value == "album") output = RecoveryActionDomain::Album;
  else return false;
  return true;
}

bool parseActionBackend(const std::string& value,
                        RecoveryActionBackend& output) {
  if (value == "none") output = RecoveryActionBackend::None;
  else if (value == "internal") output = RecoveryActionBackend::Internal;
  else if (value == "removable") output = RecoveryActionBackend::Removable;
  else return false;
  return true;
}

bool parseActionChoice(const std::string& value,
                       RecoveryActionChoice& output) {
  if (value == "current") output = RecoveryActionChoice::Current;
  else if (value == "next") output = RecoveryActionChoice::Next;
  else if (value == "previous") output = RecoveryActionChoice::Previous;
  else return false;
  return true;
}

bool takeFormField(const std::string& body, size_t& offset,
                   const char* name, std::string& output, bool final) {
  const std::string prefix = std::string(name) + "=";
  if (body.compare(offset, prefix.size(), prefix) != 0) return false;
  const size_t first = offset + prefix.size();
  const size_t end = body.find('&', first);
  if (final) {
    if (end != std::string::npos) return false;
    output = body.substr(first);
    offset = body.size();
  } else {
    if (end == std::string::npos) return false;
    output = body.substr(first, end - first);
    offset = end + 1U;
  }
  return !output.empty();
}

bool parseActionRequest(const std::string& body,
                        RecoveryActionRequest& output) {
  size_t offset = 0U;
  std::string domain;
  std::string backend;
  std::string choice;
  std::string snapshot;
  std::string backup;
  std::string confirm;
  if (!takeFormField(body, offset, "domain", domain, false) ||
      !takeFormField(body, offset, "backend", backend, false) ||
      !takeFormField(body, offset, "choice", choice, false) ||
      !takeFormField(body, offset, "snapshot", snapshot, false) ||
      !takeFormField(body, offset, "backup", backup, false) ||
      !takeFormField(body, offset, "confirm", confirm, true) ||
      offset != body.size() || confirm != "resolve" ||
      !parseActionDomain(domain, output.domain) ||
      !parseActionBackend(backend, output.backend) ||
      !parseActionChoice(choice, output.choice) ||
      !hexDecode(snapshot, output.inspection_id)) {
    return false;
  }
  const bool domain_backend_valid =
      (output.domain == RecoveryActionDomain::Display &&
       output.backend == RecoveryActionBackend::None) ||
      (output.domain == RecoveryActionDomain::Tasks &&
       output.backend == RecoveryActionBackend::None) ||
      (output.domain == RecoveryActionDomain::Album &&
       (output.backend == RecoveryActionBackend::Internal ||
        output.backend == RecoveryActionBackend::Removable));
  const bool removable_album =
      output.domain == RecoveryActionDomain::Album &&
      output.backend == RecoveryActionBackend::Removable;
  output.external_backup_confirmed = backup == "verified_external";
  return domain_backend_valid && !allZero(output.inspection_id) &&
         (removable_album ? output.external_backup_confirmed
                          : backup == "not_required" &&
                                !output.external_backup_confirmed) &&
         !(output.domain == RecoveryActionDomain::Display &&
           output.choice == RecoveryActionChoice::Next);
}

bool parseExportPrepareRequest(const std::string& body,
                               RecoveryExportExpectedIndexes& output) {
  size_t offset = 0U;
  std::string current;
  std::string next;
  std::string previous;
  std::string confirm;
  return takeFormField(body, offset, "current", current, false) &&
      takeFormField(body, offset, "next", next, false) &&
      takeFormField(body, offset, "previous", previous, false) &&
      takeFormField(body, offset, "confirm", confirm, true) &&
      offset == body.size() && confirm == "readonly_export" &&
      hexDecode(current, output.digests[0]) &&
      hexDecode(next, output.digests[1]) &&
      hexDecode(previous, output.digests[2]) &&
      !allZero(output.digests[0]) && !allZero(output.digests[1]) &&
      !allZero(output.digests[2]);
}

bool parseExportSessionBody(
    const std::string& body, const char* expected_confirm,
    std::array<uint8_t, kRecoveryExportSessionBytes>& session_id) {
  size_t offset = 0U;
  std::string session;
  std::string confirm;
  return takeFormField(body, offset, "session", session, false) &&
      takeFormField(body, offset, "confirm", confirm, true) &&
      offset == body.size() && confirm == expected_confirm &&
      hexDecode(session, session_id) && !allZero(session_id);
}

bool parseDecimal(const std::string& value, uint32_t& output) {
  if (value.empty() || value.size() > 10U) return false;
  uint64_t parsed = 0U;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10U + static_cast<uint8_t>(ch - '0');
    if (parsed > std::numeric_limits<uint32_t>::max()) return false;
  }
  output = static_cast<uint32_t>(parsed);
  return true;
}

bool parseExportPath(
    const std::string& path, const char* prefix,
    std::array<uint8_t, kRecoveryExportSessionBytes>& session_id,
    uint32_t& value) {
  const size_t prefix_length = std::strlen(prefix);
  if (path.compare(0U, prefix_length, prefix) != 0) return false;
  const size_t slash = path.find('/', prefix_length);
  if (slash == std::string::npos ||
      path.find('/', slash + 1U) != std::string::npos) {
    return false;
  }
  return hexDecode(path.substr(prefix_length, slash - prefix_length),
                   session_id) &&
      !allZero(session_id) && parseDecimal(path.substr(slash + 1U), value);
}

template <size_t Size>
bool fixedIdentifier(const std::array<char, Size>& value, std::string& output) {
  const auto terminator = std::find(value.begin(), value.end(), '\0');
  if (terminator == value.end() || terminator == value.begin()) return false;
  output.assign(value.begin(), terminator);
  return boundedPrintable(output, 1U, kMaximumRecoveryIdentifierBytes);
}

std::string jsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8U);
  constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '<': escaped += "\\u003c"; break;
      case '>': escaped += "\\u003e"; break;
      case '&': escaped += "\\u0026"; break;
      default:
        if (ch < 0x20U) {
          escaped += "\\u00";
          escaped.push_back(kHex[ch >> 4U]);
          escaped.push_back(kHex[ch & 0x0FU]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return escaped;
}

RecoveryResponse response(int status, std::string body,
                          const char* content_type =
                              "application/json; charset=utf-8") {
  RecoveryResponse output;
  output.status = status;
  output.content_type = content_type;
  output.body = std::move(body);
  if (output.body.size() > kMaximumRecoveryResponseBytes) {
    output.status = 500;
    output.content_type = "application/json; charset=utf-8";
    output.body = "{\"ok\":false,\"error\":\"response_too_large\"}";
  }
  return output;
}

RecoveryResponse errorResponse(int status, const char* error) {
  return response(status, std::string("{\"ok\":false,\"error\":\"") +
                              error + "\"}");
}

RecoveryResponse exportError(RecoveryExportResult result) {
  switch (result) {
    case RecoveryExportResult::Busy:
      return errorResponse(503, "recovery_export_busy");
    case RecoveryExportResult::InvalidRequest:
      return errorResponse(422, "recovery_export_request_invalid");
    case RecoveryExportResult::SessionStale:
      return errorResponse(409, "recovery_export_session_stale");
    case RecoveryExportResult::SourceChanged:
      return errorResponse(409, "recovery_export_source_changed");
    case RecoveryExportResult::SourceUnavailable:
      return errorResponse(409, "recovery_export_source_unavailable");
    case RecoveryExportResult::IoError:
      return errorResponse(503, "recovery_export_io_error");
    case RecoveryExportResult::VerificationFailed:
      return errorResponse(500, "recovery_export_verification_failed");
    case RecoveryExportResult::Ok:
    case RecoveryExportResult::Complete:
      break;
  }
  return errorResponse(500, "recovery_export_result_invalid");
}

bool validSnapshot(const RecoveryDiagnosticSnapshot& snapshot,
                   std::string& firmware, std::string& board) {
  return static_cast<uint8_t>(snapshot.reason) <=
             static_cast<uint8_t>(RecoveryReason::StorageIntegrityRefused) &&
         static_cast<uint8_t>(snapshot.phase) <=
             static_cast<uint8_t>(RecoveryPhase::StorageAudit) &&
         static_cast<uint8_t>(snapshot.outcome) <=
             static_cast<uint8_t>(RecoveryOutcome::Incomplete) &&
         snapshot.normal_startup_refused &&
         fixedIdentifier(snapshot.firmware_id, firmware) &&
         fixedIdentifier(snapshot.board_id, board);
}

bool validActionKey(RecoveryActionDomain domain,
                    RecoveryActionBackend backend) {
  return (domain == RecoveryActionDomain::Display &&
          backend == RecoveryActionBackend::None) ||
         (domain == RecoveryActionDomain::Tasks &&
          backend == RecoveryActionBackend::None) ||
         (domain == RecoveryActionDomain::Album &&
          (backend == RecoveryActionBackend::Internal ||
           backend == RecoveryActionBackend::Removable));
}

bool validActionCandidate(const RecoveryActionCandidate& candidate) {
  if (static_cast<uint8_t>(candidate.state) >
      static_cast<uint8_t>(RecoveryActionCandidateState::IoError)) {
    return false;
  }
  if (candidate.state == RecoveryActionCandidateState::Missing) {
    return candidate.byte_count == 0U && !candidate.digest_present &&
           allZero(candidate.digest) && candidate.item_count == 0U &&
           !candidate.item_count_present &&
           candidate.modified_unix_seconds == 0U &&
           !candidate.modified_time_present;
  }
  if (!candidate.digest_present && !allZero(candidate.digest)) return false;
  if (!candidate.item_count_present && candidate.item_count != 0U)
    return false;
  if (!candidate.modified_time_present &&
      candidate.modified_unix_seconds != 0U) {
    return false;
  }
  return candidate.state != RecoveryActionCandidateState::Valid ||
         candidate.digest_present;
}

bool sameActionKey(const RecoveryActionSnapshot& left,
                   const RecoveryActionSnapshot& right) {
  return left.domain == right.domain && left.backend == right.backend;
}

bool validActionSnapshot(const RecoveryActionSnapshot& snapshot) {
  if (static_cast<uint8_t>(snapshot.domain) >
          static_cast<uint8_t>(RecoveryActionDomain::Album) ||
      static_cast<uint8_t>(snapshot.backend) >
          static_cast<uint8_t>(RecoveryActionBackend::Removable) ||
      static_cast<uint8_t>(snapshot.state) >
          static_cast<uint8_t>(RecoveryActionState::Disabled) ||
      !validActionKey(snapshot.domain, snapshot.backend)) {
    return false;
  }
  uint8_t valid_count = 0U;
  for (size_t index = 0; index < snapshot.candidates.size(); ++index) {
    const RecoveryActionCandidate& candidate = snapshot.candidates[index];
    if (!validActionCandidate(candidate)) return false;
    if (snapshot.domain != RecoveryActionDomain::Display &&
        candidate.state == RecoveryActionCandidateState::Valid &&
        !candidate.item_count_present) {
      return false;
    }
    if (candidate.state == RecoveryActionCandidateState::Valid) ++valid_count;
    if (snapshot.domain == RecoveryActionDomain::Display &&
        index == static_cast<size_t>(RecoveryActionChoice::Next) &&
        candidate.state != RecoveryActionCandidateState::Missing) {
      return false;
    }
  }
  if (valid_count != snapshot.valid_candidates) return false;
  const bool actionable = snapshot.state == RecoveryActionState::Recoverable ||
                          snapshot.state == RecoveryActionState::ChoiceRequired;
  if (actionable && allZero(snapshot.inspection_id)) return false;
  switch (snapshot.state) {
    case RecoveryActionState::Empty:
    case RecoveryActionState::Disabled:
      return valid_count == 0U;
    case RecoveryActionState::Recoverable:
      return valid_count == 1U;
    case RecoveryActionState::ChoiceRequired:
      return valid_count > 1U;
    case RecoveryActionState::Corrupt:
    case RecoveryActionState::IoError:
      return true;
  }
  return false;
}

bool validActionInventory(const RecoveryActionInventory& inventory) {
  if (inventory.count > inventory.snapshots.size()) return false;
  for (size_t index = 0; index < inventory.snapshots.size(); ++index) {
    if (index >= inventory.count) continue;
    if (!validActionSnapshot(inventory.snapshots[index])) return false;
    for (size_t previous = 0; previous < index; ++previous) {
      if (sameActionKey(inventory.snapshots[index],
                        inventory.snapshots[previous])) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

RecoveryPortalCore::RecoveryPortalCore(const RecoveryAccessConfig& access,
                                       const IRecoveryDiagnosticCache& cache,
                                       IRecoveryActionOwner* action_owner,
                                       IRecoveryExportOwner* export_owner)
    : access_(access),
      cache_(cache),
      action_owner_(action_owner),
      export_owner_(export_owner),
      ready_(validateConfiguration()) {}

RecoveryPortalCore::~RecoveryPortalCore() { scrubCredentials(); }

void RecoveryPortalCore::scrubCredentials() {
  auto scrub = [](std::string& value) {
    volatile char* bytes = value.empty() ? nullptr : &value[0];
    for (size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
    value.clear();
  };
  scrub(access_.access_code);
  scrub(access_.session_id);
  scrub(access_.csrf_token);
  for (std::string& host : access_.allowed_hosts) scrub(host);
  for (std::string& origin : access_.allowed_origins) scrub(origin);
  access_.allowed_host_count = 0U;
  access_.allowed_origin_count = 0U;
  access_.session_lifetime_seconds = 0U;
  ready_ = false;
  session_issued_ = false;
  session_expires_at_seconds_ = 0U;
}

bool RecoveryPortalCore::validateConfiguration() const {
  return boundedPrintable(access_.access_code, kMinimumAccessCodeBytes,
                          kMaximumRecoveryAccessCodeBytes) &&
         tokenText(access_.session_id) && tokenText(access_.csrf_token) &&
         !constantTimeEqual(access_.access_code, access_.session_id) &&
         !constantTimeEqual(access_.access_code, access_.csrf_token) &&
         !constantTimeEqual(access_.session_id, access_.csrf_token) &&
         validEndpointList(access_.allowed_hosts, access_.allowed_host_count,
                           kMaximumHostBytes) &&
         validOrigins(access_.allowed_origins, access_.allowed_origin_count) &&
         access_.session_lifetime_seconds >= kMinimumSessionLifetimeSeconds &&
         access_.session_lifetime_seconds <= kMaximumSessionLifetimeSeconds;
}

bool RecoveryPortalCore::hostAllowed(const std::string& host) const {
  for (size_t index = 0; index < access_.allowed_host_count; ++index) {
    if (host == access_.allowed_hosts[index]) return true;
  }
  return false;
}

bool RecoveryPortalCore::originAllowed(const std::string& origin) const {
  for (size_t index = 0; index < access_.allowed_origin_count; ++index) {
    if (origin == access_.allowed_origins[index]) return true;
  }
  return false;
}

bool RecoveryPortalCore::sessionAuthorized(
    const RecoveryRequest& request) const {
  if (!session_issued_ || request.now_seconds >= session_expires_at_seconds_)
    return false;
  std::string cookie;
  return exactCookieValue(request.cookie, cookie) &&
         constantTimeEqual(cookie, access_.session_id);
}

RecoveryResponse RecoveryPortalCore::handleLogin(
    const RecoveryRequest& request) {
  if (request.origin.empty() || !originAllowed(request.origin))
    return errorResponse(403, "origin_forbidden");
  if (request.content_type != "application/x-www-form-urlencoded")
    return errorResponse(415, "content_type_required");
  if (request.content_length != request.body.size() ||
      request.content_length > kMaximumRecoveryRequestBodyBytes)
    return errorResponse(413, "request_body_invalid");
  std::string candidate;
  if (!parseAccessCode(request.body, candidate) ||
      !constantTimeEqual(candidate, access_.access_code)) {
    return errorResponse(401, "invalid_access_code");
  }
  session_issued_ = true;
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  session_expires_at_seconds_ =
      request.now_seconds > maximum - access_.session_lifetime_seconds
          ? maximum
          : request.now_seconds + access_.session_lifetime_seconds;
  RecoveryResponse output = response(
      200, std::string("{\"ok\":true,\"csrfToken\":\"") +
               jsonEscape(access_.csrf_token) + "\"}");
  output.set_cookie = std::string(kSessionCookieName) + "=" +
                      access_.session_id +
                      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                      std::to_string(access_.session_lifetime_seconds);
  return output;
}

RecoveryResponse RecoveryPortalCore::renderDiagnostic() const {
  RecoveryDiagnosticSnapshot snapshot;
  const RecoveryReadResult result = cache_.readRecoveryDiagnostic(snapshot);
  if (result == RecoveryReadResult::Busy)
    return errorResponse(503, "diagnostic_busy");
  if (result == RecoveryReadResult::Unavailable)
    return errorResponse(503, "diagnostic_unavailable");
  if (result != RecoveryReadResult::Ok)
    return errorResponse(422, "diagnostic_invalid");
  std::string firmware;
  std::string board;
  if (!validSnapshot(snapshot, firmware, board))
    return errorResponse(422, "diagnostic_invalid");
  std::ostringstream json;
  json << "{\"ok\":true,\"recovery\":{\"reason\":\""
       << recoveryReasonName(snapshot.reason) << "\",\"phase\":\""
       << recoveryPhaseName(snapshot.phase) << "\",\"outcome\":\""
       << recoveryOutcomeName(snapshot.outcome)
       << "\",\"firmwareId\":\"" << jsonEscape(firmware)
       << "\",\"boardId\":\"" << jsonEscape(board)
       << "\",\"normalStartupRefused\":true,\"dataErased\":false,"
          "\"normalWritersStarted\":false,\"recordCounts\":{"
          "\"nvsNamespaces\":"
       << snapshot.records.nvs_namespaces << ",\"files\":"
       << snapshot.records.files << ",\"settingsRecords\":"
       << snapshot.records.settings_records << ",\"taskRecords\":"
       << snapshot.records.task_records << ",\"albumAssets\":"
       << snapshot.records.album_assets << ",\"otaSlots\":"
       << snapshot.records.ota_slots << "}}}";
  return response(200, json.str());
}

RecoveryResponse RecoveryPortalCore::renderRecoveryActions() {
  if (!action_owner_)
    return errorResponse(503, "recovery_actions_unavailable");
  RecoveryActionInventory inventory;
  const RecoveryActionReadResult result =
      action_owner_->inspectRecoveryActions(inventory);
  if (result == RecoveryActionReadResult::Busy)
    return errorResponse(503, "recovery_actions_busy");
  if (result == RecoveryActionReadResult::Unavailable)
    return errorResponse(503, "recovery_actions_unavailable");
  if (result != RecoveryActionReadResult::Ok ||
      !validActionInventory(inventory)) {
    return errorResponse(422, "recovery_actions_invalid");
  }

  std::ostringstream json;
  json << "{\"ok\":true,\"actions\":[";
  for (size_t index = 0; index < inventory.count; ++index) {
    if (index != 0U) json << ',';
    const RecoveryActionSnapshot& snapshot = inventory.snapshots[index];
    json << "{\"domain\":\"" << recoveryActionDomainName(snapshot.domain)
         << "\",\"backend\":\""
         << recoveryActionBackendName(snapshot.backend)
         << "\",\"state\":\"" << recoveryActionStateName(snapshot.state)
         << "\",\"snapshot\":\"" << hexEncode(snapshot.inspection_id)
         << "\",\"candidates\":[";
    for (size_t candidate_index = 0U;
         candidate_index < snapshot.candidates.size(); ++candidate_index) {
      if (candidate_index != 0U) json << ',';
      const RecoveryActionCandidate& candidate =
          snapshot.candidates[candidate_index];
      json << "{\"choice\":\""
           << recoveryActionChoiceName(
                  static_cast<RecoveryActionChoice>(candidate_index))
           << "\",\"state\":\""
           << recoveryActionCandidateStateName(candidate.state)
           << "\",\"bytes\":" << candidate.byte_count << ",\"items\":";
      if (candidate.item_count_present)
        json << candidate.item_count;
      else
        json << "null";
      json << ",\"modifiedAt\":";
      if (candidate.modified_time_present)
        json << candidate.modified_unix_seconds;
      else
        json << "null";
      json << ",\"digest\":";
      if (candidate.digest_present)
        json << '\"' << hexEncode(candidate.digest) << '\"';
      else
        json << "null";
      json << '}';
    }
    json << "]}";
  }
  json << "]}";
  return response(200, json.str());
}

RecoveryResponse RecoveryPortalCore::resolveRecoveryAction(
    const RecoveryRequest& request) {
  if (!action_owner_)
    return errorResponse(503, "recovery_actions_unavailable");
  if (request.content_type != "application/x-www-form-urlencoded")
    return errorResponse(415, "content_type_required");
  if (request.content_length != request.body.size() ||
      request.content_length == 0U ||
      request.content_length > kMaximumRecoveryRequestBodyBytes) {
    return errorResponse(413, "request_body_invalid");
  }
  RecoveryActionRequest action;
  if (!parseActionRequest(request.body, action))
    return errorResponse(422, "recovery_action_request_invalid");

  switch (action_owner_->resolveRecoveryAction(action)) {
    case RecoveryActionResolveResult::Ok:
      return response(200, "{\"ok\":true,\"result\":\"resolved\"}");
    case RecoveryActionResolveResult::Busy:
      return errorResponse(503, "recovery_action_busy");
    case RecoveryActionResolveResult::InvalidRequest:
      return errorResponse(422, "recovery_action_request_invalid");
    case RecoveryActionResolveResult::SourceChanged:
      return errorResponse(409, "recovery_action_snapshot_stale");
    case RecoveryActionResolveResult::SourceUnavailable:
      return errorResponse(409, "recovery_action_source_unavailable");
    case RecoveryActionResolveResult::SelectedUnavailable:
      return errorResponse(409, "recovery_action_selected_unavailable");
    case RecoveryActionResolveResult::IoError:
      return errorResponse(503, "recovery_action_io_error");
    case RecoveryActionResolveResult::VerificationFailed:
      return errorResponse(500, "recovery_action_verification_failed");
  }
  return errorResponse(500, "recovery_action_result_invalid");
}

RecoveryResponse RecoveryPortalCore::prepareRecoveryExport(
    const RecoveryRequest& request) {
  if (!export_owner_)
    return errorResponse(503, "recovery_export_unavailable");
  if (request.content_type != "application/x-www-form-urlencoded")
    return errorResponse(415, "content_type_required");
  if (request.content_length != request.body.size() ||
      request.content_length == 0U ||
      request.content_length > kMaximumRecoveryRequestBodyBytes) {
    return errorResponse(413, "request_body_invalid");
  }
  RecoveryExportExpectedIndexes expected;
  if (!parseExportPrepareRequest(request.body, expected) ||
      expected.digests[0] == expected.digests[1] ||
      expected.digests[0] == expected.digests[2] ||
      expected.digests[1] == expected.digests[2]) {
    return errorResponse(422, "recovery_export_request_invalid");
  }
  RecoveryExportSnapshot snapshot;
  const RecoveryExportResult result =
      export_owner_->prepareRecoveryExport(expected, snapshot);
  if (result != RecoveryExportResult::Ok) return exportError(result);
  bool candidates_valid = true;
  uint64_t candidate_bytes = 0U;
  uint32_t candidate_entries = 0U;
  for (size_t at = 0U; at < snapshot.candidates.size(); ++at) {
    const RecoveryExportCandidate& candidate = snapshot.candidates[at];
    candidates_valid = candidates_valid &&
        candidate.digest == expected.digests[at] &&
        !allZero(candidate.digest) && candidate.byte_count > 0U &&
        candidate.byte_count <= kMaximumRecoveryExportIndexBytes &&
        candidate.asset_entries <= 96U &&
        candidate_bytes <= kMaximumRecoveryExportTotalBytes -
            candidate.byte_count;
    if (candidates_valid) candidate_bytes += candidate.byte_count;
    candidate_entries += candidate.asset_entries;
  }
  const uint64_t minimum_total = candidate_bytes +
      static_cast<uint64_t>(snapshot.asset_count) *
          kMinimumRecoveryExportAssetBytes;
  const uint64_t maximum_total = candidate_bytes +
      static_cast<uint64_t>(snapshot.asset_count) *
          kMaximumRecoveryExportAssetBytes;
  if (!candidates_valid || allZero(snapshot.session_id) ||
      snapshot.asset_count > kMaximumRecoveryExportAssets ||
      snapshot.asset_count > candidate_entries ||
      snapshot.inventory_pages !=
          (snapshot.asset_count + kRecoveryExportInventoryPageAssets - 1U) /
              kRecoveryExportInventoryPageAssets ||
      snapshot.total_bytes < minimum_total ||
      snapshot.total_bytes > maximum_total ||
      snapshot.total_bytes > kMaximumRecoveryExportTotalBytes) {
    export_owner_->abortRecoveryExport(snapshot.session_id);
    return errorResponse(500, "recovery_export_snapshot_invalid");
  }
  std::ostringstream json;
  json << "{\"ok\":true,\"session\":\"" << hexEncode(snapshot.session_id)
       << "\",\"assetCount\":" << snapshot.asset_count
       << ",\"inventoryPages\":" << snapshot.inventory_pages
       << ",\"totalBytes\":" << snapshot.total_bytes
       << ",\"candidates\":[";
  for (size_t at = 0U; at < snapshot.candidates.size(); ++at) {
    if (at) json << ',';
    const RecoveryExportCandidate& candidate = snapshot.candidates[at];
    json << "{\"item\":" << at << ",\"bytes\":"
         << candidate.byte_count << ",\"sha256\":\""
         << hexEncode(candidate.digest) << "\",\"assetEntries\":"
         << candidate.asset_entries << '}';
  }
  json << "]}";
  return response(200, json.str());
}

RecoveryResponse RecoveryPortalCore::renderRecoveryExportInventory(
    const RecoveryRequest& request) {
  if (!export_owner_)
    return errorResponse(503, "recovery_export_unavailable");
  std::array<uint8_t, kRecoveryExportSessionBytes> session{};
  uint32_t page = 0U;
  if (!parseExportPath(request.path, "/api/recovery/export/inventory/",
                       session, page) ||
      page >= (kMaximumRecoveryExportAssets +
               kRecoveryExportInventoryPageAssets - 1U) /
                  kRecoveryExportInventoryPageAssets) {
    return errorResponse(422, "recovery_export_request_invalid");
  }
  RecoveryExportInventoryPage inventory;
  const RecoveryExportResult result =
      export_owner_->readRecoveryExportInventory(session, page, inventory);
  if (result != RecoveryExportResult::Ok) return exportError(result);
  if (inventory.session_id != session || inventory.page != page ||
      inventory.count == 0U || inventory.count > inventory.assets.size() ||
      inventory.asset_offset !=
          page * kRecoveryExportInventoryPageAssets) {
    return errorResponse(500, "recovery_export_inventory_invalid");
  }
  for (size_t at = 0U; at < inventory.count; ++at) {
    const RecoveryExportAsset& asset = inventory.assets[at];
    if (allZero(asset.digest) ||
        asset.byte_count < kMinimumRecoveryExportAssetBytes ||
        asset.byte_count > kMaximumRecoveryExportAssetBytes ||
        asset.candidate_mask == 0U || asset.candidate_mask > 0x07U) {
      return errorResponse(500, "recovery_export_inventory_invalid");
    }
    for (size_t previous = 0U; previous < at; ++previous) {
      if (asset.digest == inventory.assets[previous].digest)
        return errorResponse(500, "recovery_export_inventory_invalid");
    }
  }
  std::ostringstream json;
  json << "{\"ok\":true,\"session\":\"" << hexEncode(session)
       << "\",\"page\":" << page << ",\"assetOffset\":"
       << inventory.asset_offset << ",\"assets\":[";
  for (size_t at = 0U; at < inventory.count; ++at) {
    if (at) json << ',';
    const RecoveryExportAsset& asset = inventory.assets[at];
    json << "{\"ordinal\":" << inventory.asset_offset + at
         << ",\"item\":"
         << kRecoveryActionCandidateCount + inventory.asset_offset + at
         << ",\"bytes\":" << asset.byte_count
         << ",\"sha256\":\"" << hexEncode(asset.digest)
         << "\",\"candidateMask\":"
         << static_cast<unsigned>(asset.candidate_mask) << '}';
  }
  json << "]}";
  return response(200, json.str());
}

RecoveryResponse RecoveryPortalCore::finishRecoveryExport(
    const RecoveryRequest& request) {
  if (!export_owner_)
    return errorResponse(503, "recovery_export_unavailable");
  if (request.content_type != "application/x-www-form-urlencoded")
    return errorResponse(415, "content_type_required");
  std::array<uint8_t, kRecoveryExportSessionBytes> session{};
  if (request.content_length != request.body.size() ||
      request.content_length == 0U ||
      request.content_length > kMaximumRecoveryRequestBodyBytes ||
      !parseExportSessionBody(request.body, "verify_export", session)) {
    return errorResponse(422, "recovery_export_request_invalid");
  }
  const RecoveryExportResult result =
      export_owner_->finishRecoveryExport(session);
  return result == RecoveryExportResult::Complete
      ? response(200, "{\"ok\":true,\"result\":\"verified\"}")
      : exportError(result);
}

RecoveryResponse RecoveryPortalCore::abortRecoveryExport(
    const RecoveryRequest& request) {
  if (!export_owner_)
    return errorResponse(503, "recovery_export_unavailable");
  if (request.content_type != "application/x-www-form-urlencoded")
    return errorResponse(415, "content_type_required");
  std::array<uint8_t, kRecoveryExportSessionBytes> session{};
  if (request.content_length != request.body.size() ||
      request.content_length == 0U ||
      request.content_length > kMaximumRecoveryRequestBodyBytes ||
      !parseExportSessionBody(request.body, "abort_export", session)) {
    return errorResponse(422, "recovery_export_request_invalid");
  }
  export_owner_->abortRecoveryExport(session);
  return response(200, "{\"ok\":true,\"result\":\"aborted\"}");
}

RecoveryResponse RecoveryPortalCore::openRecoveryExportFile(
    const RecoveryRequest& request, RecoveryExportStream& output) {
  output = RecoveryExportStream{};
  if (!ready_) return errorResponse(503, "recovery_portal_unavailable");
  if (!export_owner_)
    return errorResponse(503, "recovery_export_unavailable");
  if (request.method != "GET" || request.path.size() > 128U ||
      request.host.size() > kMaximumHostBytes ||
      request.origin.size() > kMaximumOriginBytes ||
      request.cookie.size() > 256U ||
      request.csrf_token.size() > kMaximumRecoveryTokenBytes ||
      request.content_length != 0U || !request.body.empty()) {
    return errorResponse(400, "request_invalid");
  }
  if (!request.peer_is_local) return errorResponse(403, "local_peer_required");
  if (request.host.empty() || !hostAllowed(request.host))
    return errorResponse(400, "host_invalid");
  if (!request.origin.empty() && !originAllowed(request.origin))
    return errorResponse(403, "origin_forbidden");
  if (!sessionAuthorized(request)) return errorResponse(401, "unauthorized");
  if (!constantTimeEqual(request.csrf_token, access_.csrf_token))
    return errorResponse(403, "csrf_forbidden");
  RecoveryExportOpenRequest open;
  if (!parseExportPath(request.path, "/api/recovery/export/file/",
                       open.session_id, open.item) ||
      open.item >=
          kRecoveryActionCandidateCount + kMaximumRecoveryExportAssets) {
    return errorResponse(422, "recovery_export_request_invalid");
  }
  const RecoveryExportResult result =
      export_owner_->openRecoveryExport(open, output);
  if (result == RecoveryExportResult::Ok &&
      (output.handle == 0U || output.item != open.item ||
       allZero(output.digest) || output.byte_count == 0U ||
       (open.item < kRecoveryActionCandidateCount
            ? output.byte_count > kMaximumRecoveryExportIndexBytes
            : output.byte_count < kMinimumRecoveryExportAssetBytes ||
                  output.byte_count > kMaximumRecoveryExportAssetBytes))) {
    export_owner_->closeRecoveryExport(output.handle);
    output = RecoveryExportStream{};
    return errorResponse(500, "recovery_export_stream_invalid");
  }
  return result == RecoveryExportResult::Ok
      ? response(200, std::string(), "application/octet-stream")
      : exportError(result);
}

RecoveryExportResult RecoveryPortalCore::readRecoveryExportFile(
    uint32_t handle, uint8_t* output, size_t capacity, size_t& bytes_read) {
  bytes_read = 0U;
  if (!export_owner_) return RecoveryExportResult::SourceUnavailable;
  if (handle == 0U || !output || capacity == 0U ||
      capacity > kMaximumRecoveryExportChunkBytes) {
    return RecoveryExportResult::InvalidRequest;
  }
  const RecoveryExportResult result = export_owner_->readRecoveryExport(
      handle, output, capacity, bytes_read);
  if (bytes_read > capacity ||
      (result == RecoveryExportResult::Complete && bytes_read != 0U) ||
      (result != RecoveryExportResult::Ok &&
       result != RecoveryExportResult::Complete && bytes_read != 0U)) {
    bytes_read = 0U;
    return RecoveryExportResult::VerificationFailed;
  }
  return result;
}

void RecoveryPortalCore::closeRecoveryExportFile(uint32_t handle) {
  if (export_owner_) export_owner_->closeRecoveryExport(handle);
}

RecoveryResponse RecoveryPortalCore::handle(const RecoveryRequest& request) {
  if (!ready_) return errorResponse(503, "recovery_portal_unavailable");
  if (request.method.size() > 8U || request.path.size() > 128U ||
      request.host.size() > kMaximumHostBytes ||
      request.origin.size() > kMaximumOriginBytes ||
      request.cookie.size() > 256U ||
      request.csrf_token.size() > kMaximumRecoveryTokenBytes ||
      request.content_type.size() > 64U ||
      request.body.size() > kMaximumRecoveryRequestBodyBytes) {
    return errorResponse(400, "request_too_large");
  }
  if (!request.peer_is_local) return errorResponse(403, "local_peer_required");
  if (request.host.empty() || !hostAllowed(request.host))
    return errorResponse(400, "host_invalid");
  if (!request.origin.empty() && !originAllowed(request.origin))
    return errorResponse(403, "origin_forbidden");

  const bool known_path = request.path == "/" ||
                          request.path == "/api/session" ||
                          request.path == "/api/diagnostics" ||
                          request.path == "/api/recovery/actions" ||
                          request.path == "/api/recovery/actions/resolve" ||
                          request.path == "/api/recovery/export/prepare" ||
                          request.path == "/api/recovery/export/finish" ||
                          request.path == "/api/recovery/export/abort" ||
                          request.path.rfind(
                              "/api/recovery/export/inventory/", 0U) == 0U;
  if (!known_path) return errorResponse(404, "route_not_found");
  if (request.path == "/api/session") {
    if (request.method != "POST")
      return errorResponse(405, "method_not_allowed");
    return handleLogin(request);
  }
  if (request.path == "/") {
    if (request.method != "GET")
      return errorResponse(405, "method_not_allowed");
    if (request.content_length != 0U || !request.body.empty())
      return errorResponse(400, "get_body_forbidden");
    return response(200, dashboardHtml(), "text/html; charset=utf-8");
  }
  const bool protected_post =
      request.path == "/api/recovery/actions/resolve" ||
      request.path == "/api/recovery/export/prepare" ||
      request.path == "/api/recovery/export/finish" ||
      request.path == "/api/recovery/export/abort";
  if (protected_post) {
    if (request.method != "POST")
      return errorResponse(405, "method_not_allowed");
  } else if (request.method != "GET") {
    return errorResponse(405, "method_not_allowed");
  }
  if (!sessionAuthorized(request)) return errorResponse(401, "unauthorized");
  if (!constantTimeEqual(request.csrf_token, access_.csrf_token))
    return errorResponse(403, "csrf_forbidden");
  if (protected_post) {
    if (request.origin.empty() || !originAllowed(request.origin))
      return errorResponse(403, "origin_forbidden");
    if (request.path == "/api/recovery/actions/resolve")
      return resolveRecoveryAction(request);
    if (request.path == "/api/recovery/export/prepare")
      return prepareRecoveryExport(request);
    if (request.path == "/api/recovery/export/finish")
      return finishRecoveryExport(request);
    return abortRecoveryExport(request);
  }
  if (request.content_length != 0U || !request.body.empty())
    return errorResponse(400, "get_body_forbidden");
  if (request.path == "/api/diagnostics") return renderDiagnostic();
  if (request.path.rfind("/api/recovery/export/inventory/", 0U) == 0U)
    return renderRecoveryExportInventory(request);
  return renderRecoveryActions();
}

const char* RecoveryPortalCore::dashboardHtml() {
  static constexpr char kDashboard[] = R"INKLOOP_RECOVERY(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Inkloop 安全恢复</title><style>
:root{color-scheme:light;--ink:#172033;--muted:#526078;--paper:#f8fafc;--surface:#fff;--line:#cbd5e1;--safe:#17633b;--alert:#9a3412;--focus:#1d4ed8}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif}main{width:min(800px,100%);margin:auto;padding:24px}.eyebrow{font-size:.78rem;font-weight:750;letter-spacing:.1em;text-transform:uppercase;color:var(--alert)}h1{font-size:clamp(1.8rem,7vw,3rem);line-height:1.12;margin:.35rem 0 1rem}.lede{font-size:1.08rem;max-width:64ch}.notice,.card,.action{background:var(--surface);border:1px solid var(--line);border-radius:14px;padding:18px;margin:16px 0}.notice{border-left:5px solid var(--safe)}.notice strong{color:var(--safe)}h2{font-size:1.2rem;margin:0 0 10px}.steps{padding-left:24px}.steps code,.digest{overflow-wrap:anywhere}.field{display:grid;gap:6px}.field input{min-height:48px;border:1px solid #94a3b8;border-radius:9px;padding:10px 12px;font:inherit}.field input:focus,button:focus-visible,input:focus-visible{outline:3px solid var(--focus);outline-offset:2px}button{min-height:48px;margin-top:12px;border:0;border-radius:9px;background:var(--ink);color:#fff;padding:10px 18px;font:inherit;font-weight:700;cursor:pointer}button:disabled{cursor:not-allowed;opacity:.55}.status{min-height:1.6em;color:var(--muted)}.status.error{color:#a12b1c}.facts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:0}.facts div{border-top:1px solid var(--line);padding-top:8px}.facts dt{font-size:.8rem;color:var(--muted)}.facts dd{margin:2px 0;font-weight:700;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}.counts{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}.counts th,.counts td{padding:8px;border-top:1px solid var(--line);text-align:left}.counts td{text-align:right}.action h3{margin-top:0}.choice{display:grid;grid-template-columns:auto 1fr;gap:3px 10px;border-top:1px solid var(--line);padding:9px 0}.choice small{grid-column:2;color:var(--muted)}.confirm{display:flex;align-items:flex-start;gap:9px;margin-top:14px}.confirm input{margin-top:.4em}.muted{color:var(--muted)}[hidden]{display:none!important}@media(max-width:520px){main{padding:16px}.facts{grid-template-columns:1fr}}@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important}}
</style></head><body><main><p class="eyebrow">受控恢复模式</p><h1>设备数据仍然保留</h1><p class="lede">启动安全检查拒绝了普通产品写入。登录后可查看诊断；只有页面列出的固定选择可以在明确确认后提交。</p><section class="notice" aria-labelledby="safety-title"><h2 id="safety-title">当前安全边界</h2><p><strong>数据没有被删除、格式化或覆盖。</strong>普通写入服务已停止，门户不会提供通用删除、修复、迁移或自动选择。</p></section><section class="card" aria-labelledby="entry-title"><h2 id="entry-title">如何进入与排查</h2><ol class="steps"><li>通过串口查看完整启动诊断和精确访问地址。</li><li>恢复诊断入口为 <code>http://inkloop.local:8080/</code>。</li><li>当前入口：<code id="current-origin">正在读取本地地址…</code></li><li>端口 80 的 <code>http://inkloop.local/</code> 仅用于 Wi-Fi 设置（出现设置热点时）。</li></ol></section><section class="card" aria-labelledby="login-title"><h2 id="login-title">查看诊断与固定恢复选择</h2><form id="login"><label class="field" for="code">本地管理密码<input id="code" name="code" type="password" minlength="4" maxlength="63" autocomplete="current-password" required></label><button id="submit" type="submit">验证并读取</button></form><p id="status" class="status" role="status" aria-live="polite">请输入本地管理密码；默认与已保存的家庭 Wi-Fi 密码相同。</p></section><section id="diagnostic" class="card" aria-labelledby="diag-title" hidden><h2 id="diag-title">诊断摘要</h2><dl id="facts" class="facts"></dl><h3>发现的记录数量</h3><table class="counts"><thead><tr><th scope="col">类别</th><th scope="col">数量</th></tr></thead><tbody id="counts"></tbody></table><p class="muted">这些数字仅用于判断恢复范围；页面无法读取记录内容。</p></section><section id="action-section" class="card" aria-labelledby="action-title" hidden><h2 id="action-title">明确恢复选择</h2><p>Current、Next、Previous 仅是物理槽位名；断电重命名后不代表已提交、待提交或回滚，也不保证新旧来源。只能依据每份的摘要、条目数和文件时间核对差异，时间仅供参考。选择会重排或移除其他索引。可移除相册须先完成只读外部导出；确认框不是备份。每次提交只处理一个已检查快照，必须手动选择并确认。</p><div id="actions"></div><p id="action-status" class="status" role="status" aria-live="polite"></p></section></main><script>
(()=>{'use strict';const form=document.querySelector('#login'),button=document.querySelector('#submit'),status=document.querySelector('#status'),section=document.querySelector('#diagnostic'),facts=document.querySelector('#facts'),counts=document.querySelector('#counts'),actionSection=document.querySelector('#action-section'),actions=document.querySelector('#actions'),actionStatus=document.querySelector('#action-status'),csrfKey='inkloop-recovery-csrf',labels={boot_audit_refused:'启动审计拒绝',migration_refused:'迁移被拒绝',ota_health_refused:'OTA 健康检查拒绝',storage_integrity_refused:'存储完整性拒绝',boot_audit:'启动审计',migration:'迁移检查',ota_health:'OTA 健康检查',storage_audit:'存储检查',refused:'已拒绝',requires_operator:'等待人工处理',failed:'失败',incomplete:'未完成'},countLabels={nvsNamespaces:'NVS 命名空间',files:'文件',settingsRecords:'设置记录',taskRecords:'计划任务',albumAssets:'相册素材',otaSlots:'OTA 分区'},stateLabels={empty:'没有候选',recoverable:'一个可用选择',choice_required:'必须人工选择',corrupt:'候选损坏',io_error:'读取错误',disabled:'未启用',missing:'不存在',valid:'可选择',invalid:'无效'},errorLabels={recovery_action_snapshot_stale:'存储状态已经变化。已刷新候选，请重新检查并选择。',recovery_action_busy:'恢复 owner 正忙，请稍后重试。',recovery_action_source_unavailable:'恢复来源当前不可用，请检查介质后刷新。',recovery_action_selected_unavailable:'所选候选已不可用，请刷新后重新选择。',recovery_action_io_error:'恢复操作遇到读写错误；未确认成功。',recovery_action_verification_failed:'恢复后的验证失败；未确认成功。'};let activeCsrf='';function message(text,error=false){status.textContent=text;status.classList.toggle('error',error)}function actionMessage(text,error=false){actionStatus.textContent=text;actionStatus.classList.toggle('error',error)}function render(value){const state=value&&value.recovery;if(!state||state.normalStartupRefused!==true||state.dataErased!==false||state.normalWritersStarted!==false)throw new Error('invalid_state');facts.replaceChildren();for(const [name,value] of [['原因',labels[state.reason]||'未知'],['阶段',labels[state.phase]||'未知'],['结果',labels[state.outcome]||'未知'],['固件',state.firmwareId],['硬件',state.boardId]]){const box=document.createElement('div'),dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=name;dd.textContent=String(value);box.append(dt,dd);facts.append(box)}counts.replaceChildren();const records=state.recordCounts||{};for(const [key,name] of Object.entries(countLabels)){const number=records[key];if(!Number.isInteger(number)||number<0)throw new Error('invalid_count');const row=document.createElement('tr'),label=document.createElement('th'),cell=document.createElement('td');label.scope='row';label.textContent=name;cell.textContent=number.toLocaleString();row.append(label,cell);counts.append(row)}section.hidden=false;message('已读取诊断。')}function choiceLabel(snapshot,choice){if(snapshot.domain==='display'){if(choice==='current')return '采用目标画面并完成事务';if(choice==='previous')return '保留上一画面并放弃事务';return '不可用';}return {current:'采用 Current 槽位',next:'采用 Next 槽位',previous:'采用 Previous 槽位'}[choice]||'未知';}function titleOf(snapshot){if(snapshot.domain==='display')return '显示事务';if(snapshot.domain==='tasks')return '任务清单';return snapshot.backend==='internal'?'相册索引（内部存储）':'相册索引（可移除存储）';}function modified(value){if(value===null)return '不可用';const date=new Date(value*1000);return Number.isFinite(date.getTime())?date.toLocaleString():'不可用'}function renderActions(value){if(!value||value.ok!==true||!Array.isArray(value.actions)||value.actions.length>4)throw new Error('actions_invalid');actions.replaceChildren();for(const snapshot of value.actions){if(!['display','tasks','album'].includes(snapshot.domain)||!['none','internal','removable'].includes(snapshot.backend)||!['empty','recoverable','choice_required','corrupt','io_error','disabled'].includes(snapshot.state)||typeof snapshot.snapshot!=='string'||!/^[0-9a-f]{64}$/.test(snapshot.snapshot)||!Array.isArray(snapshot.candidates)||snapshot.candidates.length!==3)throw new Error('actions_invalid');const box=document.createElement('form'),heading=document.createElement('h3'),summary=document.createElement('p');box.className='action';box.dataset.domain=snapshot.domain;box.dataset.backend=snapshot.backend;box.dataset.snapshot=snapshot.snapshot;box.dataset.state=snapshot.state;heading.textContent=titleOf(snapshot);summary.textContent='状态：'+(stateLabels[snapshot.state]||'未知');box.append(heading,summary);let available=0;for(const candidate of snapshot.candidates){if(!['current','next','previous'].includes(candidate.choice)||!['missing','valid','invalid','io_error'].includes(candidate.state)||!Number.isSafeInteger(candidate.bytes)||candidate.bytes<0||(candidate.items!==null&&(!Number.isSafeInteger(candidate.items)||candidate.items<0))||(candidate.modifiedAt!==null&&(!Number.isSafeInteger(candidate.modifiedAt)||candidate.modifiedAt<=0))||(candidate.digest!==null&&(typeof candidate.digest!=='string'||!/^[0-9a-f]{64}$/.test(candidate.digest))))throw new Error('actions_invalid');const row=document.createElement('label'),radio=document.createElement('input'),text=document.createElement('span'),detail=document.createElement('small');row.className='choice';radio.type='radio';radio.name='choice';radio.value=candidate.choice;radio.disabled=candidate.state!=='valid'||!['recoverable','choice_required'].includes(snapshot.state);text.textContent=choiceLabel(snapshot,candidate.choice)+' · '+(stateLabels[candidate.state]||'未知');detail.className='digest';detail.textContent='条目：'+(candidate.items===null?'不可用':candidate.items)+'；更新时间：'+modified(candidate.modifiedAt)+'；字节：'+candidate.bytes+'；摘要：'+(candidate.digest||'不可用');row.append(radio,text,detail);box.append(row);if(!radio.disabled)available++;}if(available>0){if(snapshot.domain==='album'&&snapshot.backend==='removable'){const bk=document.createElement('label'),bc=document.createElement('input'),bw=document.createElement('span');bk.className='confirm';bc.type='checkbox';bc.name='backup-confirmed';bw.textContent='已验证只读导出：manifest complete=true，三份摘要一致。';bk.append(bc,bw);box.append(bk);}const confirmation=document.createElement('label'),check=document.createElement('input'),words=document.createElement('span'),submit=document.createElement('button');confirmation.className='confirm';check.type='checkbox';check.name='confirmed';words.textContent='我已核对所选状态、条目数、时间和摘要，并明确执行这一项。';confirmation.append(check,words);submit.type='submit';submit.textContent='提交明确选择';box.append(confirmation,submit);}actions.append(box);}actionSection.hidden=false;actionMessage(value.actions.length?'未执行任何操作。请逐项核对。':'当前没有可展示的恢复选择。');}async function loadActions(token){const response=await fetch('/api/recovery/actions',{headers:{'X-Inkloop-CSRF':token},credentials:'same-origin',cache:'no-store'});if(!response.ok)throw new Error('actions_'+response.status);renderActions(await response.json());}async function load(token){activeCsrf=token;const response=await fetch('/api/diagnostics',{headers:{'X-Inkloop-CSRF':token},credentials:'same-origin',cache:'no-store'});if(!response.ok)throw new Error('diagnostic_'+response.status);render(await response.json());try{await loadActions(token)}catch(error){actionSection.hidden=false;actionMessage('恢复选择暂不可用；诊断仍保持只读可见。',true)}}form.addEventListener('submit',async event=>{event.preventDefault();button.disabled=true;message('正在验证…');try{const body=new URLSearchParams(new FormData(form)),response=await fetch('/api/session',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body,credentials:'same-origin',cache:'no-store'}),value=await response.json();if(!response.ok||typeof value.csrfToken!=='string')throw new Error('login_'+response.status);sessionStorage.setItem(csrfKey,value.csrfToken);form.reset();await load(value.csrfToken)}catch(error){message(error.message.startsWith('login_')?'本地管理密码不正确或恢复服务暂不可用。':'无法读取诊断，请核对本地连接并查看串口。',true)}finally{button.disabled=false}});actions.addEventListener('submit',async event=>{event.preventDefault();const actionForm=event.target,selected=actionForm.querySelector('input[name="choice"]:checked'),confirmed=actionForm.querySelector('input[name="confirmed"]'),nb=actionForm.dataset.domain==='album'&&actionForm.dataset.backend==='removable',bc=actionForm.querySelector('input[name="backup-confirmed"]');if(!['recoverable','choice_required'].includes(actionForm.dataset.state)){actionMessage('当前状态不可执行恢复选择。',true);return;}if(nb&&(!bc||!bc.checked)){actionMessage('请先验证只读外部导出。',true);return;}if(!selected||!confirmed||!confirmed.checked){actionMessage('必须手动选择一项并勾选明确确认。',true);return;}const submit=actionForm.querySelector('button[type="submit"]');submit.disabled=true;actionMessage('正在提交；在 owner 确认前不会显示成功。');const body=new URLSearchParams([['domain',actionForm.dataset.domain],['backend',actionForm.dataset.backend],['choice',selected.value],['snapshot',actionForm.dataset.snapshot],['backup',nb?'verified_external':'not_required'],['confirm','resolve']]);try{const response=await fetch('/api/recovery/actions/resolve',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Inkloop-CSRF':activeCsrf},body,credentials:'same-origin',cache:'no-store'}),value=await response.json();if(!response.ok||value.result!=='resolved'){const key=value&&value.error;try{if(key==='recovery_action_snapshot_stale')await loadActions(activeCsrf)}catch(ignore){}throw new Error(key||('resolve_'+response.status));}actionMessage('owner 已确认恢复操作成功，正在刷新候选。');try{await loadActions(activeCsrf);actionMessage('owner 已确认恢复操作成功。')}catch(refreshError){actionMessage('owner 已确认恢复操作成功，但候选刷新失败；请重新登录后核对。',true)}}catch(error){actionMessage(errorLabels[error.message]||'恢复操作失败；未确认成功。请刷新状态并查看串口。',true)}finally{submit.disabled=false;}});const saved=sessionStorage.getItem(csrfKey);if(saved)load(saved).catch(()=>{sessionStorage.removeItem(csrfKey);message('会话已失效，请重新输入本地管理密码。',true)});
document.querySelector('#current-origin').textContent=location.origin+'/';
})();
</script></body></html>)INKLOOP_RECOVERY";
  static_assert(sizeof(kDashboard) - 1U <= kMaximumRecoveryResponseBytes,
                "recovery dashboard exceeds response cap");
  return kDashboard;
}

const char* recoveryReasonName(RecoveryReason value) {
  switch (value) {
    case RecoveryReason::BootAuditRefused: return "boot_audit_refused";
    case RecoveryReason::MigrationRefused: return "migration_refused";
    case RecoveryReason::OtaHealthRefused: return "ota_health_refused";
    case RecoveryReason::StorageIntegrityRefused:
      return "storage_integrity_refused";
  }
  return "invalid";
}

const char* recoveryPhaseName(RecoveryPhase value) {
  switch (value) {
    case RecoveryPhase::BootAudit: return "boot_audit";
    case RecoveryPhase::Migration: return "migration";
    case RecoveryPhase::OtaHealth: return "ota_health";
    case RecoveryPhase::StorageAudit: return "storage_audit";
  }
  return "invalid";
}

const char* recoveryOutcomeName(RecoveryOutcome value) {
  switch (value) {
    case RecoveryOutcome::Refused: return "refused";
    case RecoveryOutcome::RequiresOperator: return "requires_operator";
    case RecoveryOutcome::Failed: return "failed";
    case RecoveryOutcome::Incomplete: return "incomplete";
  }
  return "invalid";
}

const char* recoveryActionDomainName(RecoveryActionDomain value) {
  switch (value) {
    case RecoveryActionDomain::Display: return "display";
    case RecoveryActionDomain::Tasks: return "tasks";
    case RecoveryActionDomain::Album: return "album";
  }
  return "invalid";
}

const char* recoveryActionBackendName(RecoveryActionBackend value) {
  switch (value) {
    case RecoveryActionBackend::None: return "none";
    case RecoveryActionBackend::Internal: return "internal";
    case RecoveryActionBackend::Removable: return "removable";
  }
  return "invalid";
}

const char* recoveryActionChoiceName(RecoveryActionChoice value) {
  switch (value) {
    case RecoveryActionChoice::Current: return "current";
    case RecoveryActionChoice::Next: return "next";
    case RecoveryActionChoice::Previous: return "previous";
  }
  return "invalid";
}

const char* recoveryActionCandidateStateName(
    RecoveryActionCandidateState value) {
  switch (value) {
    case RecoveryActionCandidateState::Missing: return "missing";
    case RecoveryActionCandidateState::Valid: return "valid";
    case RecoveryActionCandidateState::Invalid: return "invalid";
    case RecoveryActionCandidateState::IoError: return "io_error";
  }
  return "invalid";
}

const char* recoveryActionStateName(RecoveryActionState value) {
  switch (value) {
    case RecoveryActionState::Empty: return "empty";
    case RecoveryActionState::Recoverable: return "recoverable";
    case RecoveryActionState::ChoiceRequired: return "choice_required";
    case RecoveryActionState::Corrupt: return "corrupt";
    case RecoveryActionState::IoError: return "io_error";
    case RecoveryActionState::Disabled: return "disabled";
  }
  return "invalid";
}

}  // namespace recovery
}  // namespace inkloop
