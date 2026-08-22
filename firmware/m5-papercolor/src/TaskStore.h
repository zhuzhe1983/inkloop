#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

#include "Storage.h"

namespace inkloop {

struct DueTask {
  String id;
  String frameUrl;
  String frameHash;
  String renderStrategy;
  uint32_t revision = 0;
};

class TaskStore {
 public:
  explicit TaskStore(IStorageBackend& storage) : storage_(storage) {}

  bool ready() const { return storage_.capabilities().mounted; }
  bool load(JsonDocument& tasks);
  bool replace(JsonArrayConst incoming);
  bool findTitle(const String& id, String& title);
  bool firstDue(time_t now, const tm& local, DueTask& due);
  bool nextDueEpoch(time_t now, uint64_t& nextEpoch);
  bool markRun(const DueTask& due, time_t now, const tm& local);
  bool markRunAt(const DueTask& due, time_t runAt);
  bool markRunWithDay(const DueTask& due, time_t runAt, uint32_t dayStamp);
  bool isRunAcknowledged(const DueTask& due, time_t runAt);
  bool acknowledgementPayloadSize(
    const DueTask& due,
    time_t runAt,
    uint32_t dayStamp,
    size_t& bytes
  );
  static uint32_t localDayStamp(const tm& local);

 private:
  bool loadFile(const char* path, JsonDocument& tasks);
  bool buildAcknowledgedTasks(
    const DueTask& due,
    time_t runAt,
    uint32_t dayStamp,
    JsonDocument& tasks
  );
  bool save(JsonArrayConst tasks);
  static bool taskDue(JsonObjectConst task, time_t now, const tm& local);

  IStorageBackend& storage_;
  bool recoveryChecked_ = false;
  bool knownEmpty_ = false;
};

}  // namespace inkloop
