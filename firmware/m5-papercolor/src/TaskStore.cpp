#include "TaskStore.h"

#include "ImageProcessing.h"

#include "AppConfig.h"
#include "BackendTransactionIo.h"
#include "Diagnostics.h"
#include "JsonRecordCodec.h"
#include "TaskPersistenceCore.h"

namespace inkloop {

bool TaskStore::loadFile(const char* path, JsonDocument& tasks) {
  if (!ready()) return false;
  tasks.clear();
  File file = storage_.open(path, FILE_READ);
  if (!file) return false;
  const size_t serializedBytes = file.size();
  const int firstByte = file.peek();
  const DeserializationError error = deserializeJson(tasks, file);
  file.close();
  bool valid = !error && tasks.is<JsonArray>();
  if (valid) {
    for (JsonVariantConst task : tasks.as<JsonArrayConst>()) {
      // JsonVariantConst cannot be checked against the mutable JsonObject
      // type in ArduinoJson 7; doing so always returns false even for a valid
      // object. This previously rejected every downloaded task array.
      if (!task.is<JsonObjectConst>()) {
        valid = false;
        break;
      }
    }
  }
  if (!valid) {
    String detail(path);
    detail += ":";
    detail += error ? error.c_str()
        : (tasks.is<JsonArray>() ? "array_item_not_object" : "top_level_not_array");
    detail += ":";
    detail += String(serializedBytes);
    detail += ":first=";
    detail += String(firstByte);
    Diagnostics::event("TASK_INDEX_VALIDATE_FAILED", detail);
    tasks.clear();
    tasks.to<JsonArray>();
    return false;
  }
  return true;
}

bool TaskStore::load(JsonDocument& tasks) {
  if (!ready()) return false;
  if (recoveryChecked_) {
    if (knownEmpty_) {
      tasks.clear();
      tasks.to<JsonArray>();
      return true;
    }
    return loadFile(kTasksPath, tasks);
  }
  BackendTransactionIo io(storage_);
  TaskPersistenceCore persistence(io);
  const RecordRecovery recovery = persistence.recover(
    [this](const char* path) {
      JsonDocument candidate;
      return loadFile(path, candidate);
    }
  );
  if (recovery == RecordRecovery::Empty) {
    recoveryChecked_ = true;
    knownEmpty_ = true;
    tasks.clear();
    tasks.to<JsonArray>();
    return true;
  }
  if (recovery == RecordRecovery::Failed) return false;
  recoveryChecked_ = true;
  knownEmpty_ = false;
  return loadFile(kTasksPath, tasks);
}

bool TaskStore::save(JsonArrayConst tasks) {
  if (!ready()) return false;
  String payload;
  if (!serializeJsonRecordExactly(tasks, payload)) return false;
  if (!payload.length() || payload[0] != '[') {
    Diagnostics::event(
        "TASK_INDEX_SERIALIZE_ROOT_FAILED",
        payload.length() ? String(static_cast<unsigned int>(payload[0])) : "empty");
    return false;
  }
  BackendTransactionIo io(storage_);
  TaskPersistenceCore persistence(io);
  const RecordCommitResult committed = persistence.commitValidatedDetailed(
    reinterpret_cast<const uint8_t*>(payload.c_str()),
    payload.length(),
    [this](const char* path) {
      JsonDocument candidate;
      return loadFile(path, candidate);
    }
  );
  if (committed != RecordCommitResult::Committed) {
    Diagnostics::event("TASK_INDEX_COMMIT_FAILED", recordCommitResultName(committed));
    return false;
  }
  recoveryChecked_ = true;
  knownEmpty_ = false;
  return true;
}

bool TaskStore::replace(JsonArrayConst incoming) {
  if (!ready()) return false;
  JsonDocument previousDoc;
  load(previousDoc);
  JsonArrayConst previous = previousDoc.as<JsonArrayConst>();
  JsonDocument nextDoc;
  JsonArray next = nextDoc.to<JsonArray>();
  for (JsonObjectConst source : incoming) {
    JsonObject target = next.add<JsonObject>();
    target.set(source);
    displaypower::RenderStrategy renderStrategy;
    const char* requestedRenderStrategy = source["renderStrategy"] | "official-quality";
    if (!displaypower::parseRenderStrategyId(
            requestedRenderStrategy, &renderStrategy)) {
      renderStrategy = displaypower::RenderStrategy::OfficialQuality;
    }
    target["renderStrategy"] = displaypower::renderStrategyId(renderStrategy);
    const String id = source["id"] | "";
    const uint32_t revision = source["revision"] | 0;
    uint32_t lastRun = 0;
    uint32_t lastDay = 0;
    for (JsonObjectConst oldTask : previous) {
      if (id == String(oldTask["id"] | "") && revision == static_cast<uint32_t>(oldTask["revision"] | 0)) {
        lastRun = oldTask["lastRun"] | 0;
        lastDay = oldTask["lastDay"] | 0;
        break;
      }
    }
    target["lastRun"] = lastRun;
    target["lastDay"] = lastDay;
  }
  return save(nextDoc.as<JsonArrayConst>());
}

bool TaskStore::findTitle(const String& id, String& title) {
  title = "";
  if (!id.length() || id.length() > 100) return false;
  JsonDocument tasksDoc;
  if (!load(tasksDoc)) return false;
  for (JsonObjectConst task : tasksDoc.as<JsonArrayConst>()) {
    if (id != String(task["id"] | "")) continue;
    const String candidate = task["title"] | "";
    if (!candidate.length() || candidate.length() > 192) return false;
    title = candidate;
    return true;
  }
  return false;
}

uint32_t TaskStore::localDayStamp(const tm& local) {
  return static_cast<uint32_t>((local.tm_year + 1900) * 1000 + local.tm_yday);
}

bool TaskStore::taskDue(JsonObjectConst task, time_t now, const tm& local) {
  const String mode = task["scheduleMode"] | "once";
  const uint32_t lastRun = task["lastRun"] | 0;
  if (!lastRun) return true;
  if (mode == "once") return false;
  if (mode == "hourly") return now - lastRun >= 3600;
  if (mode == "custom") {
    const uint32_t minutes = constrain(static_cast<uint32_t>(task["customMinutes"] | 30), 1U, 10080U);
    return now - lastRun >= minutes * 60;
  }
  if (mode == "daily") {
    const String daily = task["dailyTime"] | "08:00";
    const int hour = daily.substring(0, 2).toInt();
    const int minute = daily.substring(3, 5).toInt();
    return local.tm_hour == hour && local.tm_min >= minute &&
      localDayStamp(local) != static_cast<uint32_t>(task["lastDay"] | 0);
  }
  return false;
}

bool TaskStore::firstDue(time_t now, const tm& local, DueTask& due) {
  if (!ready()) return false;
  JsonDocument tasksDoc;
  if (!load(tasksDoc)) return false;
  for (JsonObjectConst task : tasksDoc.as<JsonArrayConst>()) {
    if (!taskDue(task, now, local)) continue;
    const String frameUrl = task["frameUrl"] | "";
    if (!frameUrl.length()) continue;
    due.id = task["id"] | "";
    due.frameUrl = frameUrl;
    due.frameHash = task["frameHash"] | "";
    due.renderStrategy = task["renderStrategy"] | "official-quality";
    due.revision = task["revision"] | 0;
    return true;
  }
  return false;
}

bool TaskStore::nextDueEpoch(time_t now, uint64_t& nextEpoch) {
  nextEpoch = 0;
  if (!ready()) return false;
  if (now < 1700000000) return false;
  JsonDocument tasksDoc;
  if (!load(tasksDoc)) return false;
  tm local{};
  localtime_r(&now, &local);
  const uint32_t today = localDayStamp(local);
  for (JsonObjectConst task : tasksDoc.as<JsonArrayConst>()) {
    if (!String(task["frameUrl"] | "").length()) continue;
    const String mode = task["scheduleMode"] | "once";
    const uint32_t lastRun = task["lastRun"] | 0;
    uint64_t candidate = 0;
    if (!lastRun) {
      candidate = static_cast<uint64_t>(now);
    } else if (mode == "hourly") {
      candidate = static_cast<uint64_t>(lastRun) + 3600U;
    } else if (mode == "custom") {
      const uint32_t minutes = constrain(
        static_cast<uint32_t>(task["customMinutes"] | 30), 1U, 10080U);
      candidate = static_cast<uint64_t>(lastRun) + minutes * 60ULL;
    } else if (mode == "daily") {
      const String daily = task["dailyTime"] | "08:00";
      const int hour = daily.substring(0, 2).toInt();
      const int minute = daily.substring(3, 5).toInt();
      if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
      tm scheduled = local;
      scheduled.tm_hour = hour;
      scheduled.tm_min = minute;
      scheduled.tm_sec = 0;
      time_t scheduledEpoch = mktime(&scheduled);
      const bool ranToday = today == static_cast<uint32_t>(task["lastDay"] | 0);
      if (!ranToday && scheduledEpoch <= now && local.tm_hour == hour) {
        candidate = static_cast<uint64_t>(now);
      } else {
        if (ranToday || scheduledEpoch <= now) {
          scheduled.tm_mday += 1;
          scheduled.tm_isdst = -1;
          scheduledEpoch = mktime(&scheduled);
        }
        if (scheduledEpoch <= now) return false;
        candidate = static_cast<uint64_t>(scheduledEpoch);
      }
    } else if (mode == "once") {
      continue;
    } else {
      return false;
    }
    if (!nextEpoch || candidate < nextEpoch) nextEpoch = candidate;
  }
  return true;
}

bool TaskStore::markRun(const DueTask& due, time_t now, const tm& local) {
  if (!ready()) return false;
  return markRunWithDay(due, now, localDayStamp(local));
}

bool TaskStore::buildAcknowledgedTasks(
  const DueTask& due,
  time_t now,
  uint32_t dayStamp,
  JsonDocument& tasksDoc
) {
  if (!ready()) return false;
  if (!load(tasksDoc)) return false;
  for (JsonObject task : tasksDoc.as<JsonArray>()) {
    if (due.id == String(task["id"] | "") && due.revision == static_cast<uint32_t>(task["revision"] | 0)) {
      if (static_cast<uint32_t>(task["lastRun"] | 0) >= static_cast<uint32_t>(now)) return true;
      task["lastRun"] = static_cast<uint32_t>(now);
      task["lastDay"] = dayStamp;
      return true;
    }
  }
  return false;
}

bool TaskStore::markRunWithDay(const DueTask& due, time_t now, uint32_t dayStamp) {
  if (!ready()) return false;
  JsonDocument tasksDoc;
  if (!buildAcknowledgedTasks(due, now, dayStamp, tasksDoc)) return false;
  return isRunAcknowledged(due, now) || save(tasksDoc.as<JsonArrayConst>());
}

bool TaskStore::acknowledgementPayloadSize(
  const DueTask& due,
  time_t runAt,
  uint32_t dayStamp,
  size_t& bytes
) {
  bytes = 0;
  if (!ready()) return false;
  JsonDocument tasksDoc;
  if (!buildAcknowledgedTasks(due, runAt, dayStamp, tasksDoc)) return false;
  bytes = measureJson(tasksDoc.as<JsonArrayConst>());
  return bytes > 0;
}

bool TaskStore::markRunAt(const DueTask& due, time_t runAt) {
  if (!ready()) return false;
  tm local{};
  localtime_r(&runAt, &local);
  return markRun(due, runAt, local);
}

bool TaskStore::isRunAcknowledged(const DueTask& due, time_t runAt) {
  if (!ready()) return false;
  JsonDocument tasksDoc;
  if (!load(tasksDoc)) return false;
  for (JsonObjectConst task : tasksDoc.as<JsonArrayConst>()) {
    if (due.id == String(task["id"] | "") &&
        due.revision == static_cast<uint32_t>(task["revision"] | 0)) {
      return static_cast<uint32_t>(task["lastRun"] | 0) >= static_cast<uint32_t>(runAt);
    }
  }
  return false;
}

}  // namespace inkloop
