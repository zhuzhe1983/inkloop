#include "inkloop/inkloop_cloud_client.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "cJSON.h"

namespace inkloop {
namespace cloud {
namespace {

constexpr size_t kMaximumTasks = 128U;

InkloopCloudStatus failure(InkloopCloudCode code, int http,
                           const char* detail, uint32_t retry = 0) {
  InkloopCloudStatus result;
  result.code = code;
  result.http_status = http;
  result.retry_after_ms = retry;
  result.detail = detail ? detail : "";
  return result;
}

bool sixDigits(const std::string& value) {
  if (value.size() != 6U) return false;
  for (char ch : value)
    if (ch < '0' || ch > '9') return false;
  return true;
}

bool lowercaseHex(const std::string& value, size_t size) {
  if (value.size() != size) return false;
  for (char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}

bool hardwareId(const std::string& value) {
  if (value.size() < 6U || value.size() > 80U) return false;
  for (char ch : value) {
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
          ch == ':' || ch == '_' || ch == '-')) return false;
  }
  return true;
}

bool deviceId(const std::string& value) {
  if (value.size() < 20U || value.size() > 80U) return false;
  for (char ch : value)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-')) return false;
  return true;
}

bool safeHttpsUrl(const std::string& value, size_t maximum) {
  if (value.size() < 12U || value.size() > maximum ||
      value.compare(0, 8, "https://") != 0) return false;
  for (char ch : value)
    if (static_cast<unsigned char>(ch) <= 0x20U || ch == '\\') return false;
  return true;
}

const cJSON* uniqueItem(const cJSON* object, const char* key) {
  if (!cJSON_IsObject(object) || !key) return nullptr;
  const cJSON* match = nullptr;
  for (const cJSON* child = object->child; child; child = child->next) {
    if (!child->string || std::strcmp(child->string, key) != 0) continue;
    if (match) return nullptr;
    match = child;
  }
  return match;
}

bool stringItem(const cJSON* object, const char* key, size_t minimum,
                size_t maximum, std::string& output) {
  output.clear();
  const cJSON* value = uniqueItem(object, key);
  if (!cJSON_IsString(value) || !value->valuestring) return false;
  output = value->valuestring;
  return output.size() >= minimum && output.size() <= maximum;
}

bool boolItem(const cJSON* object, const char* key, bool& output) {
  const cJSON* value = uniqueItem(object, key);
  if (!cJSON_IsBool(value)) return false;
  output = cJSON_IsTrue(value);
  return true;
}

bool uintItem(const cJSON* object, const char* key, uint32_t minimum,
              uint32_t maximum, uint32_t& output) {
  const cJSON* value = uniqueItem(object, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < minimum || value->valuedouble > maximum) {
    return false;
  }
  output = static_cast<uint32_t>(value->valuedouble);
  return static_cast<double>(output) == value->valuedouble;
}

bool optionalUintItem(const cJSON* object, const char* key, uint32_t fallback,
                      uint32_t minimum, uint32_t maximum, uint32_t& output) {
  size_t occurrences = 0;
  if (!cJSON_IsObject(object) || !key) return false;
  for (const cJSON* child = object->child; child; child = child->next) {
    if (child->string && std::strcmp(child->string, key) == 0) ++occurrences;
  }
  if (occurrences > 1U) return false;
  const cJSON* value = uniqueItem(object, key);
  if (!value) {
    output = fallback;
    return true;
  }
  return uintItem(object, key, minimum, maximum, output);
}

cJSON* parseObject(const std::string& body, size_t maximum) {
  if (body.empty() || body.size() > maximum ||
      body.find("\\u0000") != std::string::npos) return nullptr;
  const char* end = nullptr;
  cJSON* root = cJSON_ParseWithLengthOpts(
      body.c_str(), body.size() + 1U, &end, 1);
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return nullptr;
  }
  return root;
}

bool addString(cJSON* object, const char* key, const std::string& value) {
  return cJSON_AddStringToObject(object, key, value.c_str()) != nullptr;
}

std::string encode(cJSON* root) {
  if (!root) return {};
  char* value = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!value) return {};
  std::string output(value);
  cJSON_free(value);
  return output;
}

bool parseTask(const cJSON* object, storage::InkloopTaskRecord& task) {
  task = storage::InkloopTaskRecord{};
  if (!cJSON_IsObject(object) ||
      !stringItem(object, "id", 1U, 100U, task.id) ||
      !stringItem(object, "title", 1U, 192U, task.title) ||
      !stringItem(object, "scheduleMode", 4U, 8U, task.schedule_mode) ||
      !uintItem(object, "customMinutes", 1U, 10080U,
                task.custom_minutes) ||
      !stringItem(object, "dailyTime", 5U, 5U, task.daily_time) ||
      !uintItem(object, "revision", 0U,
                std::numeric_limits<uint32_t>::max(), task.revision) ||
      !stringItem(object, "frameUrl", 12U, 1024U, task.frame_url) ||
      !stringItem(object, "frameHash", 64U, 64U, task.frame_hash) ||
      !stringItem(object, "renderStrategy", 5U, 32U,
                  task.render_strategy) ||
      !safeHttpsUrl(task.frame_url, 1024U)) {
    return false;
  }
  return storage::PosixTaskStore::validTask(task);
}

uint32_t retryAfter(const myai::HttpResponse& response) {
  auto value = response.headers.find("retry-after");
  if (value == response.headers.end()) value = response.headers.find("Retry-After");
  if (value == response.headers.end() || value->second.empty() ||
      value->second.size() > 8U) return 0;
  uint64_t seconds = 0;
  for (char ch : value->second) {
    if (ch < '0' || ch > '9') return 0;
    seconds = seconds * 10U + static_cast<unsigned>(ch - '0');
    if (seconds > std::numeric_limits<uint32_t>::max() / 1000U) return 0;
  }
  return static_cast<uint32_t>(seconds * 1000U);
}

}  // namespace

InkloopCloudClient::InkloopCloudClient(
    InkloopCloudConfig config, myai::IHttpTransport& http,
    IInkloopIdentityStore& identity, storage::PosixTaskStore& tasks)
    : config_(std::move(config)), http_(http), identity_store_(identity),
      tasks_(tasks) {}

bool InkloopCloudClient::validConfig(const InkloopCloudConfig& config) {
  return safeHttpsUrl(config.api_url, 256U) && !config.sku_id.empty() &&
      config.sku_id.size() <= 80U && !config.firmware_version.empty() &&
      config.firmware_version.size() <= 40U &&
      config.request_timeout_ms >= 1000U &&
      config.request_timeout_ms <= 60000U &&
      config.maximum_response_bytes >= 1024U &&
      config.maximum_response_bytes <= 512U * 1024U;
}

bool InkloopCloudClient::validIdentity(
    const InkloopIdentitySnapshot& identity) {
  return hardwareId(identity.hardware_id) &&
      (identity.device_id.empty() || deviceId(identity.device_id)) &&
      lowercaseHex(identity.secret, 64U);
}

InkloopCloudStatus InkloopCloudClient::initialize() {
  if (initialized_ || !validConfig(config_))
    return failure(InkloopCloudCode::InvalidArgument, 0, "invalid_config");
  InkloopCloudStatus status = identity_store_.loadOrCreate(identity_snapshot_);
  if (!status.ok()) return status;
  if (!validIdentity(identity_snapshot_))
    return failure(InkloopCloudCode::Storage, 0, "invalid_identity");
  if (!tasks_.ready()) {
    const storage::TaskStoreCode tasks = tasks_.initialize();
    if (tasks != storage::TaskStoreCode::Ok)
      return failure(InkloopCloudCode::Storage, 0,
                     storage::PosixTaskStore::codeName(tasks));
  }
  initialized_ = true;
  return InkloopCloudStatus::success();
}

std::string InkloopCloudClient::registrationBody(
    const std::string& pairing_code) const {
  cJSON* root = cJSON_CreateObject();
  if (!root || !addString(root, "action", "register") ||
      !addString(root, "hardwareId", identity_snapshot_.hardware_id) ||
      !addString(root, "secret", identity_snapshot_.secret) ||
      !addString(root, "skuId", config_.sku_id) ||
      !addString(root, "firmwareVersion", config_.firmware_version) ||
      (!pairing_code.empty() &&
       !addString(root, "pairingCode", pairing_code))) {
    cJSON_Delete(root);
    return {};
  }
  return encode(root);
}

std::string InkloopCloudClient::syncBody() const {
  cJSON* root = cJSON_CreateObject();
  if (!root || !addString(root, "action", "sync") ||
      !cJSON_AddNumberToObject(root, "appliedRevision",
                              identity_snapshot_.applied_revision) ||
      !addString(root, "firmwareVersion", config_.firmware_version)) {
    cJSON_Delete(root);
    return {};
  }
  return encode(root);
}

InkloopCloudStatus InkloopCloudClient::perform(
    const std::string& body, bool authenticated,
    myai::HttpResponse& response) {
  if (body.empty())
    return failure(InkloopCloudCode::Protocol, 0, "json_encode_failed");
  myai::HttpRequest request;
  request.method = "POST";
  request.url = config_.api_url;
  request.headers["Content-Type"] = "application/json";
  if (authenticated) {
    if (!deviceId(identity_snapshot_.device_id))
      return failure(InkloopCloudCode::NotInitialized, 0,
                     "missing_device_id");
    request.headers["Authorization"] = "InkloopDevice " +
        identity_snapshot_.device_id + ":" + identity_snapshot_.secret;
  }
  request.body = body;
  request.timeoutMs = config_.request_timeout_ms;
  request.maxResponseBytes = config_.maximum_response_bytes;
  request.tlsPeerVerificationRequired = true;
  request.rejectPrivateResolvedAddresses = true;
  request.redirectsAllowed = false;
  const myai::Status transported = http_.perform(request, response);
  if (!transported.ok()) {
    return failure(transported.code == myai::ErrorCode::Security
                       ? InkloopCloudCode::Security
                       : InkloopCloudCode::Transport,
                   transported.httpStatus, "transport_failed",
                   transported.retryAfterMs);
  }
  return classifyHttp(response);
}

InkloopCloudStatus InkloopCloudClient::classifyHttp(
    const myai::HttpResponse& response) const {
  if (response.status >= 200 && response.status < 300)
    return InkloopCloudStatus::success();
  if (response.status == 401 || response.status == 403)
    return failure(InkloopCloudCode::Unauthorized, response.status,
                   "unauthorized");
  if (response.status == 409)
    return failure(InkloopCloudCode::Conflict, response.status, "conflict");
  if (response.status == 413)
    return failure(InkloopCloudCode::TooLarge, response.status, "too_large");
  if (response.status == 429 || response.status >= 500)
    return failure(InkloopCloudCode::ServerUnavailable, response.status,
                   "server_unavailable", retryAfter(response));
  return failure(InkloopCloudCode::Protocol, response.status,
                 "request_rejected");
}

InkloopCloudStatus InkloopCloudClient::parseRegistration(
    const std::string& body, const std::string& requested_code,
    std::string& next_device_id, InkloopRegistrationResult& result) const {
  result = {};
  next_device_id.clear();
  cJSON* root = parseObject(body, config_.maximum_response_bytes);
  bool paired = false;
  if (!root || !stringItem(root, "deviceId", 20U, 80U, next_device_id) ||
      !deviceId(next_device_id) || !boolItem(root, "paired", paired) ||
      !optionalUintItem(root, "pollSeconds", 15U, 1U, 3600U,
                        result.poll_seconds)) {
    cJSON_Delete(root);
    return failure(InkloopCloudCode::Protocol, 0,
                   "invalid_registration_response");
  }
  const cJSON* code = uniqueItem(root, "pairingCode");
  const cJSON* expiry = uniqueItem(root, "pairingExpiresAt");
  if (!paired) {
    if (!cJSON_IsString(code) || !code->valuestring ||
        !cJSON_IsString(expiry) || !expiry->valuestring) {
      cJSON_Delete(root);
      return failure(InkloopCloudCode::Protocol, 0,
                     "missing_registration_code");
    }
    result.pairing_code = code->valuestring;
    result.pairing_expires_at = expiry->valuestring;
    if (!sixDigits(result.pairing_code) ||
        result.pairing_expires_at.empty() ||
        result.pairing_expires_at.size() > 128U) {
      cJSON_Delete(root);
      return failure(InkloopCloudCode::Protocol, 0,
                     "invalid_registration_code");
    }
  } else if ((!cJSON_IsNull(code) && code) || (!cJSON_IsNull(expiry) && expiry)) {
    cJSON_Delete(root);
    return failure(InkloopCloudCode::Protocol, 0,
                   "paired_response_exposed_code");
  }
  cJSON_Delete(root);
  if (!requested_code.empty() && !paired &&
      result.pairing_code != requested_code) {
    result = {};
    return failure(InkloopCloudCode::Conflict, 0,
                   "authoritative_pairing_code_mismatch");
  }
  result.paired = paired;
  result.requested_pairing_code_accepted =
      !requested_code.empty() && (paired || result.pairing_code == requested_code);
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopCloudClient::registerDevice(
    const std::string& authoritative_pairing_code,
    InkloopRegistrationResult& result) {
  result = {};
  if (!initialized_)
    return failure(InkloopCloudCode::NotInitialized, 0, "not_initialized");
  if (!authoritative_pairing_code.empty() &&
      !sixDigits(authoritative_pairing_code)) {
    return failure(InkloopCloudCode::InvalidArgument, 0,
                   "invalid_pairing_code");
  }
  myai::HttpResponse response;
  InkloopCloudStatus status = perform(
      registrationBody(authoritative_pairing_code), false, response);
  if (!status.ok()) return status;
  std::string next_device_id;
  status = parseRegistration(response.body, authoritative_pairing_code,
                             next_device_id, result);
  if (!status.ok()) return status;
  status = identity_store_.saveDeviceId(next_device_id);
  if (!status.ok()) return status;
  identity_snapshot_.device_id = next_device_id;
  paired_ = result.paired;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopCloudClient::parseSync(
    const std::string& body, InkloopSyncResult& result,
    std::vector<storage::InkloopTaskRecord>& tasks) const {
  result = {};
  tasks.clear();
  cJSON* root = parseObject(body, config_.maximum_response_bytes);
  if (!root || !boolItem(root, "paired", result.paired) ||
      !uintItem(root, "revision", 0U,
                std::numeric_limits<uint32_t>::max(), result.revision)) {
    cJSON_Delete(root);
    return failure(InkloopCloudCode::Protocol, 0, "invalid_sync_response");
  }
  if (!result.paired) {
    const cJSON* values = uniqueItem(root, "tasks");
    const bool valid = cJSON_IsArray(values) && cJSON_GetArraySize(values) == 0;
    cJSON_Delete(root);
    result.requires_registration = true;
    return valid ? InkloopCloudStatus::success()
                 : failure(InkloopCloudCode::Protocol, 0,
                           "invalid_unpaired_response");
  }
  if (!boolItem(root, "changed", result.changed) ||
      !optionalUintItem(root, "pollSeconds", 15U, 1U, 3600U,
                        result.poll_seconds)) {
    cJSON_Delete(root);
    return failure(InkloopCloudCode::Protocol, 0, "invalid_sync_response");
  }
  if (!result.changed) {
    const bool valid = result.revision == identity_snapshot_.applied_revision;
    cJSON_Delete(root);
    return valid ? InkloopCloudStatus::success()
                 : failure(InkloopCloudCode::Protocol, 0,
                           "unchanged_revision_mismatch");
  }
  bool replace = false;
  const cJSON* values = uniqueItem(root, "tasks");
  if (!boolItem(root, "replace", replace) || !replace ||
      !cJSON_IsArray(values) ||
      cJSON_GetArraySize(values) > static_cast<int>(kMaximumTasks)) {
    cJSON_Delete(root);
    return failure(InkloopCloudCode::Protocol, 0,
                   "invalid_task_replacement");
  }
  bool valid = true;
  cJSON* value = nullptr;
  cJSON_ArrayForEach(value, values) {
    storage::InkloopTaskRecord task;
    if (!parseTask(value, task)) {
      valid = false;
      break;
    }
    for (const auto& existing : tasks) {
      if (existing.id == task.id) {
        valid = false;
        break;
      }
    }
    if (!valid) break;
    tasks.push_back(std::move(task));
  }
  cJSON_Delete(root);
  if (!valid) {
    tasks.clear();
    return failure(InkloopCloudCode::Protocol, 0, "invalid_task_record");
  }
  result.task_count = tasks.size();
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopCloudClient::syncTasks(InkloopSyncResult& result) {
  result = {};
  if (!initialized_)
    return failure(InkloopCloudCode::NotInitialized, 0, "not_initialized");
  if (identity_snapshot_.device_id.empty()) {
    result.requires_registration = true;
    return failure(InkloopCloudCode::PairingRequired, 0,
                   "registration_required");
  }
  myai::HttpResponse response;
  InkloopCloudStatus status = perform(syncBody(), true, response);
  if (!status.ok()) return status;
  std::vector<storage::InkloopTaskRecord> replacement;
  status = parseSync(response.body, result, replacement);
  if (!status.ok()) return status;
  result.became_paired = result.paired && !paired_;
  paired_ = result.paired;
  if (!result.paired) return InkloopCloudStatus::success();
  if (!result.changed) return InkloopCloudStatus::success();
  const storage::TaskStoreCode replaced = tasks_.replace(replacement);
  if (replaced != storage::TaskStoreCode::Ok)
    return failure(InkloopCloudCode::Storage, 0,
                   storage::PosixTaskStore::codeName(replaced));
  status = identity_store_.saveAppliedRevision(result.revision);
  if (!status.ok()) return status;
  identity_snapshot_.applied_revision = result.revision;
  return InkloopCloudStatus::success();
}

const char* InkloopCloudClient::codeName(InkloopCloudCode code) {
  switch (code) {
    case InkloopCloudCode::Ok: return "OK";
    case InkloopCloudCode::InvalidArgument: return "INVALID_ARGUMENT";
    case InkloopCloudCode::NotInitialized: return "NOT_INITIALIZED";
    case InkloopCloudCode::Storage: return "STORAGE";
    case InkloopCloudCode::Security: return "SECURITY";
    case InkloopCloudCode::Transport: return "TRANSPORT";
    case InkloopCloudCode::Unauthorized: return "UNAUTHORIZED";
    case InkloopCloudCode::Conflict: return "CONFLICT";
    case InkloopCloudCode::ServerUnavailable: return "SERVER_UNAVAILABLE";
    case InkloopCloudCode::TooLarge: return "TOO_LARGE";
    case InkloopCloudCode::Protocol: return "PROTOCOL";
    case InkloopCloudCode::PairingRequired: return "PAIRING_REQUIRED";
  }
  return "UNKNOWN";
}

}  // namespace cloud
}  // namespace inkloop
