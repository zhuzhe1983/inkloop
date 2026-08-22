#pragma once

#include <string>

#include "inkloop/storage/local_chat_log.hpp"

namespace inkloop {
namespace storage {

// ESP-IDF VFS exposes LittleFS/FAT/SD mounts through POSIX. Paths are supplied
// only by the storage owner after mount selection; user input never becomes a
// path. This adapter never creates, formats, mounts or erases a filesystem.
class PosixChatLineStore final : public IChatLineStore {
 public:
  PosixChatLineStore(std::string current_path, std::string previous_path);

  ChatLogResult appendLine(const std::string& line,
                           size_t rotate_at_bytes) override;
  ChatLogResult scan(IChatLineVisitor& visitor) const override;
  ChatLogResult clear() override;
  bool rotatedHistoryPresent() const override;

  bool pathsValid() const { return paths_valid_; }

 private:
  ChatLogResult scanFile(const std::string& path,
                         IChatLineVisitor& visitor) const;

  std::string current_path_;
  std::string previous_path_;
  bool paths_valid_ = false;
};

}  // namespace storage
}  // namespace inkloop
