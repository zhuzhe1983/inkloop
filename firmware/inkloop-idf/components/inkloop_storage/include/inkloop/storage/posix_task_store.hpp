#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace inkloop {
namespace storage {

struct InkloopTaskRecord {
  std::string id;
  std::string title;
  std::string schedule_mode = "once";
  uint32_t custom_minutes = 30;
  std::string daily_time = "08:00";
  uint32_t revision = 0;
  std::string frame_url;
  std::string frame_hash;
  std::string render_strategy = "official-quality";
  uint32_t last_run = 0;
  uint32_t last_day = 0;
};

enum class TaskStoreCode : uint8_t {
  Ok,
  InvalidArgument,
  InvalidRecord,
  TooLarge,
  RecoveryRequired,
  IoError,
  NotReady,
};

// Atomic POSIX journal compatible with /tasks.{json,next,prev}. A server-side
// replacement preserves run acknowledgement only for an exact id+revision.
class PosixTaskStore final {
 public:
  explicit PosixTaskStore(std::string root);

  // Strict, side-effect-free parser shared with the first-boot compatibility
  // audit.  Accepting only a generic JSON array during the audit would let a
  // manifest pass boot and then fail after product writers start.
  static bool decodeManifest(const std::string& json,
                             std::vector<InkloopTaskRecord>& tasks);

  TaskStoreCode initialize();
  TaskStoreCode load(std::vector<InkloopTaskRecord>& tasks);
  TaskStoreCode replace(const std::vector<InkloopTaskRecord>& tasks);
  TaskStoreCode firstDue(std::time_t now, const std::tm& local,
                         InkloopTaskRecord& task);
  TaskStoreCode markRun(const std::string& id, uint32_t revision,
                        std::time_t run_at, uint32_t local_day);
  TaskStoreCode nextDueEpoch(std::time_t now, uint64_t& next_epoch);

  bool ready() const { return ready_; }
  bool pathsValid() const { return paths_valid_; }
  static bool validTask(const InkloopTaskRecord& task);
  static uint32_t localDayStamp(const std::tm& local);
  static const char* codeName(TaskStoreCode code);

 private:
  bool loadFile(const std::string& path,
                std::vector<InkloopTaskRecord>& tasks) const;
  bool recover(std::vector<InkloopTaskRecord>& tasks);
  bool commit(const std::vector<InkloopTaskRecord>& tasks);
  bool encode(const std::vector<InkloopTaskRecord>& tasks,
              std::string& json) const;
  bool writeAll(const std::string& path, const std::string& bytes) const;
  static bool due(const InkloopTaskRecord& task, std::time_t now,
                  const std::tm& local);

  std::string root_;
  std::string current_path_;
  std::string next_path_;
  std::string previous_path_;
  bool paths_valid_ = false;
  bool ready_ = false;
};

}  // namespace storage
}  // namespace inkloop
