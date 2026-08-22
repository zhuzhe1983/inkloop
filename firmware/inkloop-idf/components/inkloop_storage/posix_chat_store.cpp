#include "inkloop/storage/posix_chat_store.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace inkloop {
namespace storage {
namespace {

bool validPath(const std::string& path) {
  return !path.empty() && path.size() <= 192 && path.front() == '/' &&
         path.find("..") == std::string::npos &&
         path.find('\n') == std::string::npos &&
         path.find('\r') == std::string::npos;
}

bool writeAll(int fd, const char* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const ssize_t written = ::write(fd, data + offset, length - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool unlinkIfPresent(const std::string& path) {
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

}  // namespace

PosixChatLineStore::PosixChatLineStore(std::string current_path,
                                       std::string previous_path)
    : current_path_(std::move(current_path)),
      previous_path_(std::move(previous_path)) {
  paths_valid_ = validPath(current_path_) && validPath(previous_path_) &&
                 current_path_ != previous_path_;
}

ChatLogResult PosixChatLineStore::appendLine(const std::string& line,
                                             size_t rotate_at_bytes) {
  if (!paths_valid_) return {ChatLogCode::InvalidArgument};
  if (line.empty() || line.size() > kMaximumChatLineBytes ||
      line.find('\n') != std::string::npos ||
      line.find('\r') != std::string::npos || rotate_at_bytes < 64 ||
      rotate_at_bytes > kDefaultChatLogBytes)
    return {ChatLogCode::InvalidArgument};
  if (line.size() + 1U > rotate_at_bytes) return {ChatLogCode::TooLarge};

  struct stat info {};
  if (::stat(current_path_.c_str(), &info) == 0) {
    if (info.st_size < 0) return {ChatLogCode::IoError};
    const uint64_t current_bytes = static_cast<uint64_t>(info.st_size);
    if (current_bytes + line.size() + 1U > rotate_at_bytes) {
      if (!unlinkIfPresent(previous_path_)) return {ChatLogCode::IoError};
      if (::rename(current_path_.c_str(), previous_path_.c_str()) != 0)
        return {ChatLogCode::IoError};
    }
  } else if (errno != ENOENT) {
    return {ChatLogCode::IoError};
  }

  const int fd = ::open(current_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                        S_IRUSR | S_IWUSR);
  if (fd < 0) return {ChatLogCode::IoError};
  const bool written = writeAll(fd, line.data(), line.size()) &&
                       writeAll(fd, "\n", 1);
  const bool synced = written && ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  return {synced && closed ? ChatLogCode::Ok : ChatLogCode::IoError};
}

ChatLogResult PosixChatLineStore::scanFile(
    const std::string& path, IChatLineVisitor& visitor) const {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return {errno == ENOENT ? ChatLogCode::Ok : ChatLogCode::IoError};
  std::array<char, kMaximumChatLineBytes + 2> buffer{};
  bool keep_scanning = true;
  while (keep_scanning && std::fgets(buffer.data(), buffer.size(), file)) {
    size_t length = std::strlen(buffer.data());
    const bool complete = length > 0 && buffer[length - 1] == '\n';
    if (!complete) {
      int ch = 0;
      bool found_newline = false;
      while ((ch = std::fgetc(file)) != EOF) {
        if (ch == '\n') { found_newline = true; break; }
      }
      (void)found_newline;
      keep_scanning = visitor.onMalformedLine();
      continue;
    }
    --length;
    if (length > 0 && buffer[length - 1] == '\r') --length;
    if (length == 0) keep_scanning = visitor.onMalformedLine();
    else keep_scanning = visitor.onLine(buffer.data(), length);
  }
  const bool io_ok = std::ferror(file) == 0;
  const bool closed = std::fclose(file) == 0;
  return {io_ok && closed ? ChatLogCode::Ok : ChatLogCode::IoError};
}

ChatLogResult PosixChatLineStore::scan(IChatLineVisitor& visitor) const {
  if (!paths_valid_) return {ChatLogCode::InvalidArgument};
  ChatLogResult result = scanFile(previous_path_, visitor);
  if (!result.ok()) return result;
  return scanFile(current_path_, visitor);
}

ChatLogResult PosixChatLineStore::clear() {
  if (!paths_valid_) return {ChatLogCode::InvalidArgument};
  const bool current = unlinkIfPresent(current_path_);
  const bool previous = unlinkIfPresent(previous_path_);
  return {current && previous ? ChatLogCode::Ok : ChatLogCode::IoError};
}

bool PosixChatLineStore::rotatedHistoryPresent() const {
  if (!paths_valid_) return false;
  struct stat info {};
  return ::stat(previous_path_.c_str(), &info) == 0 && info.st_size > 0;
}

}  // namespace storage
}  // namespace inkloop
