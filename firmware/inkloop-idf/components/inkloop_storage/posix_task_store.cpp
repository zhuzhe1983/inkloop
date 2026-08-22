#include "inkloop/storage/posix_task_store.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "cJSON.h"
#include "inkloop/storage/album_index.hpp"

namespace inkloop {
namespace storage {
namespace {

constexpr size_t kMaximumTaskCount = 128U;
constexpr size_t kMaximumTaskBytes = 256U * 1024U;

bool validRoot(const std::string& value) {
  return !value.empty() && value.size() <= 96U && value.front() == '/' &&
      value.back() != '/' && value.find("..") == std::string::npos &&
      value.find('\0') == std::string::npos;
}

bool regularFile(const std::string& path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool safeUnlink(const std::string& path) {
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool boundedString(const cJSON* object, const char* key, size_t minimum,
                   size_t maximum, std::string& output) {
  output.clear();
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(value) || !value->valuestring) return false;
  output = value->valuestring;
  return output.size() >= minimum && output.size() <= maximum &&
      output.find('\0') == std::string::npos;
}

bool optionalUint32(const cJSON* object, const char* key, uint32_t fallback,
                    uint32_t& output) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!value) {
    output = fallback;
    return true;
  }
  if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
      value->valuedouble > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint32_t converted = static_cast<uint32_t>(value->valuedouble);
  if (static_cast<double>(converted) != value->valuedouble) return false;
  output = converted;
  return true;
}

bool parseTask(const cJSON* object, InkloopTaskRecord& task) {
  task = InkloopTaskRecord{};
  if (!cJSON_IsObject(object) ||
      !boundedString(object, "id", 1U, 100U, task.id) ||
      !boundedString(object, "title", 1U, 192U, task.title) ||
      !boundedString(object, "scheduleMode", 4U, 8U, task.schedule_mode) ||
      !boundedString(object, "dailyTime", 5U, 5U, task.daily_time) ||
      !boundedString(object, "frameUrl", 8U, 1024U, task.frame_url) ||
      !boundedString(object, "frameHash", 64U, 64U, task.frame_hash) ||
      !boundedString(object, "renderStrategy", 5U, 32U,
                     task.render_strategy) ||
      !optionalUint32(object, "customMinutes", 30U, task.custom_minutes) ||
      !optionalUint32(object, "revision", 0U, task.revision) ||
      !optionalUint32(object, "lastRun", 0U, task.last_run) ||
      !optionalUint32(object, "lastDay", 0U, task.last_day)) {
    return false;
  }
  return PosixTaskStore::validTask(task);
}

bool readBounded(const std::string& path, std::string& output) {
  output.clear();
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return false;
  struct stat info {};
  bool valid = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_size > 0 && static_cast<uint64_t>(info.st_size) <=
                              kMaximumTaskBytes;
  if (valid) output.resize(static_cast<size_t>(info.st_size));
  size_t at = 0;
  while (valid && at < output.size()) {
    const ssize_t count =
        ::read(descriptor, output.data() + at, output.size() - at);
    if (count <= 0 || static_cast<size_t>(count) > output.size() - at)
      valid = false;
    else
      at += static_cast<size_t>(count);
  }
  if (::close(descriptor) != 0) valid = false;
  if (!valid) output.clear();
  return valid && at == output.size();
}

bool addString(cJSON* object, const char* key, const std::string& value) {
  return cJSON_AddStringToObject(object, key, value.c_str()) != nullptr;
}

}  // namespace

PosixTaskStore::PosixTaskStore(std::string root) : root_(std::move(root)) {
  paths_valid_ = validRoot(root_);
  if (!paths_valid_) return;
  current_path_ = root_ + "/tasks.json";
  next_path_ = root_ + "/tasks.next";
  previous_path_ = root_ + "/tasks.prev";
}

bool PosixTaskStore::validTask(const InkloopTaskRecord& task) {
  if (task.id.empty() || task.id.size() > 100U || task.title.empty() ||
      task.title.size() > 192U || task.frame_url.size() < 8U ||
      task.frame_url.size() > 1024U || !validAlbumAssetId(task.frame_hash) ||
      !validRenderStrategy(task.render_strategy) || task.custom_minutes == 0U ||
      task.custom_minutes > 10080U) {
    return false;
  }
  if (task.schedule_mode != "once" && task.schedule_mode != "hourly" &&
      task.schedule_mode != "custom" && task.schedule_mode != "daily") {
    return false;
  }
  return task.daily_time.size() == 5U && task.daily_time[2] == ':' &&
      task.daily_time[0] >= '0' && task.daily_time[0] <= '2' &&
      task.daily_time[1] >= '0' && task.daily_time[1] <= '9' &&
      task.daily_time[3] >= '0' && task.daily_time[3] <= '5' &&
      task.daily_time[4] >= '0' && task.daily_time[4] <= '9' &&
      (task.daily_time[0] != '2' || task.daily_time[1] <= '3');
}

bool PosixTaskStore::loadFile(
    const std::string& path, std::vector<InkloopTaskRecord>& tasks) const {
  tasks.clear();
  std::string json;
  if (!readBounded(path, json)) return false;
  return decodeManifest(json, tasks);
}

bool PosixTaskStore::decodeManifest(
    const std::string& json, std::vector<InkloopTaskRecord>& tasks) {
  tasks.clear();
  if (json.empty() || json.size() > kMaximumTaskBytes) return false;
  cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
  if (!root || !cJSON_IsArray(root) ||
      cJSON_GetArraySize(root) > static_cast<int>(kMaximumTaskCount)) {
    cJSON_Delete(root);
    return false;
  }
  bool valid = true;
  cJSON* value = nullptr;
  cJSON_ArrayForEach(value, root) {
    InkloopTaskRecord task;
    if (!parseTask(value, task) ||
        std::any_of(tasks.begin(), tasks.end(), [&](const auto& existing) {
          return existing.id == task.id;
        })) {
      valid = false;
      break;
    }
    tasks.push_back(std::move(task));
  }
  cJSON_Delete(root);
  if (!valid) tasks.clear();
  return valid;
}

bool PosixTaskStore::recover(std::vector<InkloopTaskRecord>& tasks) {
  const bool current = regularFile(current_path_);
  const bool next = regularFile(next_path_);
  const bool previous = regularFile(previous_path_);
  if (current && loadFile(current_path_, tasks))
    return !next || safeUnlink(next_path_);
  std::vector<InkloopTaskRecord> recovered;
  if (next && loadFile(next_path_, recovered)) {
    if ((current && !safeUnlink(current_path_)) ||
        ::rename(next_path_.c_str(), current_path_.c_str()) != 0) return false;
    tasks = std::move(recovered);
    return true;
  }
  if (previous && loadFile(previous_path_, recovered)) {
    if ((current && !safeUnlink(current_path_)) ||
        (next && !safeUnlink(next_path_)) ||
        ::rename(previous_path_.c_str(), current_path_.c_str()) != 0) {
      return false;
    }
    tasks = std::move(recovered);
    return true;
  }
  if (current || next || previous) return false;
  tasks.clear();
  return true;
}

TaskStoreCode PosixTaskStore::initialize() {
  if (!paths_valid_ || ready_) return TaskStoreCode::InvalidArgument;
  std::vector<InkloopTaskRecord> ignored;
  if (!recover(ignored)) return TaskStoreCode::RecoveryRequired;
  ready_ = true;
  return TaskStoreCode::Ok;
}

TaskStoreCode PosixTaskStore::load(std::vector<InkloopTaskRecord>& tasks) {
  tasks.clear();
  if (!ready_) return TaskStoreCode::NotReady;
  return recover(tasks) ? TaskStoreCode::Ok : TaskStoreCode::RecoveryRequired;
}

bool PosixTaskStore::encode(const std::vector<InkloopTaskRecord>& tasks,
                            std::string& json) const {
  json.clear();
  if (tasks.size() > kMaximumTaskCount) return false;
  cJSON* root = cJSON_CreateArray();
  if (!root) return false;
  bool valid = true;
  for (const InkloopTaskRecord& task : tasks) {
    cJSON* object = validTask(task) ? cJSON_CreateObject() : nullptr;
    if (!object || !addString(object, "id", task.id) ||
        !addString(object, "title", task.title) ||
        !addString(object, "scheduleMode", task.schedule_mode) ||
        !cJSON_AddNumberToObject(object, "customMinutes", task.custom_minutes) ||
        !addString(object, "dailyTime", task.daily_time) ||
        !cJSON_AddNumberToObject(object, "revision", task.revision) ||
        !addString(object, "frameUrl", task.frame_url) ||
        !addString(object, "frameHash", task.frame_hash) ||
        !addString(object, "renderStrategy", task.render_strategy) ||
        !cJSON_AddNumberToObject(object, "lastRun", task.last_run) ||
        !cJSON_AddNumberToObject(object, "lastDay", task.last_day) ||
        !cJSON_AddItemToArray(root, object)) {
      cJSON_Delete(object);
      valid = false;
      break;
    }
  }
  char* encoded = valid ? cJSON_PrintUnformatted(root) : nullptr;
  cJSON_Delete(root);
  if (!encoded) return false;
  json = encoded;
  cJSON_free(encoded);
  return !json.empty() && json.size() <= kMaximumTaskBytes;
}

bool PosixTaskStore::writeAll(const std::string& path,
                              const std::string& bytes) const {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) return false;
  size_t at = 0;
  bool valid = true;
  while (valid && at < bytes.size()) {
    const ssize_t count =
        ::write(descriptor, bytes.data() + at, bytes.size() - at);
    if (count <= 0 || static_cast<size_t>(count) > bytes.size() - at)
      valid = false;
    else
      at += static_cast<size_t>(count);
  }
  if (valid && ::fsync(descriptor) != 0) valid = false;
  if (::close(descriptor) != 0) valid = false;
  return valid && at == bytes.size();
}

bool PosixTaskStore::commit(const std::vector<InkloopTaskRecord>& tasks) {
  std::string json;
  if (!encode(tasks, json) || !safeUnlink(next_path_) ||
      !writeAll(next_path_, json)) {
    safeUnlink(next_path_);
    return false;
  }
  std::vector<InkloopTaskRecord> verified;
  if (!loadFile(next_path_, verified) || !safeUnlink(previous_path_)) {
    safeUnlink(next_path_);
    return false;
  }
  const bool current = regularFile(current_path_);
  if (current &&
      ::rename(current_path_.c_str(), previous_path_.c_str()) != 0) {
    safeUnlink(next_path_);
    return false;
  }
  if (::rename(next_path_.c_str(), current_path_.c_str()) == 0) return true;
  if (current) ::rename(previous_path_.c_str(), current_path_.c_str());
  safeUnlink(next_path_);
  return false;
}

TaskStoreCode PosixTaskStore::replace(
    const std::vector<InkloopTaskRecord>& incoming) {
  if (!ready_) return TaskStoreCode::NotReady;
  if (incoming.size() > kMaximumTaskCount) return TaskStoreCode::TooLarge;
  for (size_t at = 0; at < incoming.size(); ++at) {
    if (!validTask(incoming[at])) return TaskStoreCode::InvalidRecord;
    for (size_t other = at + 1U; other < incoming.size(); ++other) {
      if (incoming[at].id == incoming[other].id)
        return TaskStoreCode::InvalidRecord;
    }
  }
  std::vector<InkloopTaskRecord> previous;
  if (!recover(previous)) return TaskStoreCode::RecoveryRequired;
  std::vector<InkloopTaskRecord> next = incoming;
  for (InkloopTaskRecord& task : next) {
    for (const InkloopTaskRecord& old : previous) {
      if (task.id == old.id && task.revision == old.revision) {
        task.last_run = old.last_run;
        task.last_day = old.last_day;
        break;
      }
    }
  }
  return commit(next) ? TaskStoreCode::Ok : TaskStoreCode::IoError;
}

uint32_t PosixTaskStore::localDayStamp(const std::tm& local) {
  return static_cast<uint32_t>((local.tm_year + 1900) * 1000 + local.tm_yday);
}

bool PosixTaskStore::due(const InkloopTaskRecord& task, std::time_t now,
                         const std::tm& local) {
  if (task.last_run == 0U) return true;
  if (task.schedule_mode == "once") return false;
  if (task.schedule_mode == "hourly")
    return now >= task.last_run && now - task.last_run >= 3600;
  if (task.schedule_mode == "custom")
    return now >= task.last_run &&
        static_cast<uint64_t>(now - task.last_run) >=
            static_cast<uint64_t>(task.custom_minutes) * 60U;
  const int hour = (task.daily_time[0] - '0') * 10 + task.daily_time[1] - '0';
  const int minute = (task.daily_time[3] - '0') * 10 + task.daily_time[4] - '0';
  const bool at_or_after =
      local.tm_hour > hour || (local.tm_hour == hour && local.tm_min >= minute);
  return task.schedule_mode == "daily" && at_or_after &&
      task.last_day != localDayStamp(local);
}

TaskStoreCode PosixTaskStore::firstDue(std::time_t now, const std::tm& local,
                                       InkloopTaskRecord& task) {
  task = InkloopTaskRecord{};
  std::vector<InkloopTaskRecord> tasks;
  const TaskStoreCode loaded = load(tasks);
  if (loaded != TaskStoreCode::Ok) return loaded;
  for (const InkloopTaskRecord& candidate : tasks) {
    if (due(candidate, now, local)) {
      task = candidate;
      break;
    }
  }
  return TaskStoreCode::Ok;
}

TaskStoreCode PosixTaskStore::markRun(const std::string& id,
                                      uint32_t revision, std::time_t run_at,
                                      uint32_t local_day) {
  if (!ready_ || id.empty() || run_at < 0 ||
      static_cast<uint64_t>(run_at) > std::numeric_limits<uint32_t>::max()) {
    return TaskStoreCode::InvalidArgument;
  }
  std::vector<InkloopTaskRecord> tasks;
  if (!recover(tasks)) return TaskStoreCode::RecoveryRequired;
  bool found = false;
  for (InkloopTaskRecord& task : tasks) {
    if (task.id == id && task.revision == revision) {
      task.last_run = std::max<uint32_t>(task.last_run,
                                         static_cast<uint32_t>(run_at));
      task.last_day = local_day;
      found = true;
      break;
    }
  }
  if (!found) return TaskStoreCode::InvalidRecord;
  return commit(tasks) ? TaskStoreCode::Ok : TaskStoreCode::IoError;
}

TaskStoreCode PosixTaskStore::nextDueEpoch(std::time_t now,
                                           uint64_t& next_epoch) {
  next_epoch = 0;
  if (now < 1700000000) return TaskStoreCode::InvalidArgument;
  std::vector<InkloopTaskRecord> tasks;
  const TaskStoreCode loaded = load(tasks);
  if (loaded != TaskStoreCode::Ok) return loaded;
  std::tm local {};
  if (!localtime_r(&now, &local)) return TaskStoreCode::InvalidArgument;
  const uint32_t today = localDayStamp(local);
  for (const InkloopTaskRecord& task : tasks) {
    uint64_t candidate = 0;
    if (due(task, now, local)) {
      candidate = static_cast<uint64_t>(now);
    } else if (task.schedule_mode == "hourly") {
      candidate = static_cast<uint64_t>(task.last_run) + 3600U;
    } else if (task.schedule_mode == "custom") {
      candidate = static_cast<uint64_t>(task.last_run) +
                  static_cast<uint64_t>(task.custom_minutes) * 60U;
    } else if (task.schedule_mode == "daily") {
      std::tm scheduled = local;
      scheduled.tm_hour = (task.daily_time[0] - '0') * 10 +
                          task.daily_time[1] - '0';
      scheduled.tm_min = (task.daily_time[3] - '0') * 10 +
                         task.daily_time[4] - '0';
      scheduled.tm_sec = 0;
      scheduled.tm_isdst = -1;
      std::time_t epoch = std::mktime(&scheduled);
      if (epoch < 0) return TaskStoreCode::InvalidRecord;
      if (task.last_day == today || epoch <= now) {
        scheduled.tm_mday += 1;
        scheduled.tm_isdst = -1;
        epoch = std::mktime(&scheduled);
      }
      if (epoch <= now) return TaskStoreCode::InvalidRecord;
      candidate = static_cast<uint64_t>(epoch);
    }
    if (candidate != 0 && (next_epoch == 0 || candidate < next_epoch))
      next_epoch = candidate;
  }
  return TaskStoreCode::Ok;
}

const char* PosixTaskStore::codeName(TaskStoreCode code) {
  switch (code) {
    case TaskStoreCode::Ok: return "OK";
    case TaskStoreCode::InvalidArgument: return "INVALID_ARGUMENT";
    case TaskStoreCode::InvalidRecord: return "INVALID_RECORD";
    case TaskStoreCode::TooLarge: return "TOO_LARGE";
    case TaskStoreCode::RecoveryRequired: return "RECOVERY_REQUIRED";
    case TaskStoreCode::IoError: return "IO_ERROR";
    case TaskStoreCode::NotReady: return "NOT_READY";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
