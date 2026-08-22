#include "inkloop/storage/posix_upgrade_inventory.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/local_chat_log.hpp"
#include "inkloop/storage/posix_chat_store.hpp"
#include "inkloop/storage/posix_task_store.hpp"

namespace inkloop {
namespace storage {
namespace {

constexpr size_t kMaximumTaskBytes = 256U * 1024U;
constexpr size_t kMaximumDisplayJournalBytes = 16U * 1024U;

enum class ReadCode : uint8_t { Missing, Ok, Invalid, IoError };

ReadCode readBoundedFile(const std::string& path, size_t maximum,
                         std::string& output) {
  output.clear();
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0)
    return errno == ENOENT ? ReadCode::Missing : ReadCode::IoError;
  if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > maximum)
    return ReadCode::Invalid;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return ReadCode::IoError;
  output.resize(static_cast<size_t>(info.st_size));
  const size_t read = std::fread(output.data(), 1, output.size(), file);
  const bool io_ok = read == output.size() && std::ferror(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!io_ok || !closed) {
    output.clear();
    return ReadCode::IoError;
  }
  return ReadCode::Ok;
}

class JsonShapeCursor {
 public:
  explicit JsonShapeCursor(const std::string& input) : input_(input) {}

  bool topArrayOfObjects() {
    skip();
    if (!take('[')) return false;
    skip();
    if (take(']')) return ended();
    size_t count = 0;
    do {
      skip();
      if (at_ >= input_.size() || input_[at_] != '{' ||
          ++count > kMaximumTaskRecords || !value(0)) return false;
      skip();
      if (take(']')) return ended();
    } while (take(','));
    return false;
  }

  bool topObject() {
    skip();
    return at_ < input_.size() && input_[at_] == '{' && value(0) && ended();
  }

 private:
  static constexpr size_t kMaximumDepth = 16;
  static constexpr size_t kMaximumTaskRecords = 256;

  void skip() {
    while (at_ < input_.size() &&
           (input_[at_] == ' ' || input_[at_] == '\t' ||
            input_[at_] == '\r' || input_[at_] == '\n')) ++at_;
  }

  bool take(char expected) {
    skip();
    if (at_ >= input_.size() || input_[at_] != expected) return false;
    ++at_;
    return true;
  }

  bool ended() {
    skip();
    return at_ == input_.size();
  }

  bool literal(const char* text) {
    const size_t length = std::strlen(text);
    if (length > input_.size() - at_ ||
        input_.compare(at_, length, text) != 0) return false;
    at_ += length;
    return true;
  }

  bool string() {
    if (!take('"')) return false;
    while (at_ < input_.size()) {
      const uint8_t ch = static_cast<uint8_t>(input_[at_++]);
      if (ch == '"') return true;
      if (ch < 0x20U) return false;
      if (ch != '\\') continue;
      if (at_ >= input_.size()) return false;
      const char escaped = input_[at_++];
      if (escaped == '"' || escaped == '\\' || escaped == '/' ||
          escaped == 'b' || escaped == 'f' || escaped == 'n' ||
          escaped == 'r' || escaped == 't') continue;
      if (escaped != 'u' || at_ + 4U > input_.size()) return false;
      for (size_t index = 0; index < 4U; ++index) {
        const char digit = input_[at_++];
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f') ||
              (digit >= 'A' && digit <= 'F'))) return false;
      }
    }
    return false;
  }

  bool number() {
    skip();
    if (at_ < input_.size() && input_[at_] == '-') ++at_;
    if (at_ >= input_.size()) return false;
    if (input_[at_] == '0') ++at_;
    else {
      if (input_[at_] < '1' || input_[at_] > '9') return false;
      while (at_ < input_.size() && input_[at_] >= '0' &&
             input_[at_] <= '9') ++at_;
    }
    if (at_ < input_.size() && input_[at_] == '.') {
      ++at_;
      if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9')
        return false;
      while (at_ < input_.size() && input_[at_] >= '0' &&
             input_[at_] <= '9') ++at_;
    }
    if (at_ < input_.size() &&
        (input_[at_] == 'e' || input_[at_] == 'E')) {
      ++at_;
      if (at_ < input_.size() &&
          (input_[at_] == '+' || input_[at_] == '-')) ++at_;
      if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9')
        return false;
      while (at_ < input_.size() && input_[at_] >= '0' &&
             input_[at_] <= '9') ++at_;
    }
    return true;
  }

  bool object(size_t depth) {
    if (!take('{')) return false;
    if (take('}')) return true;
    size_t fields = 0;
    do {
      if (++fields > 128U || !string() || !take(':') || !value(depth + 1U))
        return false;
      if (take('}')) return true;
    } while (take(','));
    return false;
  }

  bool array(size_t depth) {
    if (!take('[')) return false;
    if (take(']')) return true;
    size_t items = 0;
    do {
      if (++items > 512U || !value(depth + 1U)) return false;
      if (take(']')) return true;
    } while (take(','));
    return false;
  }

  bool value(size_t depth) {
    if (depth > kMaximumDepth) return false;
    skip();
    if (at_ >= input_.size()) return false;
    if (input_[at_] == '{') return object(depth);
    if (input_[at_] == '[') return array(depth);
    if (input_[at_] == '"') return string();
    if (input_[at_] == '-' ||
        (input_[at_] >= '0' && input_[at_] <= '9')) return number();
    return literal("true") || literal("false") || literal("null");
  }

  const std::string& input_;
  size_t at_ = 0;
};

RecordProbe mapRead(ReadCode code, bool valid) {
  if (code == ReadCode::Missing) return RecordProbe::Missing;
  if (code == ReadCode::IoError) return RecordProbe::IoError;
  return code == ReadCode::Ok && valid ? RecordProbe::Valid
                                      : RecordProbe::Invalid;
}

bool validRoot(const std::string& root) {
  return !root.empty() && root.size() <= 96U && root.front() == '/' &&
         root.back() != '/' && root.find("..") == std::string::npos &&
         root.find('\n') == std::string::npos &&
         root.find('\r') == std::string::npos;
}

bool presentPath(const std::string& path, bool& present) {
  struct stat info {};
  if (::stat(path.c_str(), &info) == 0) {
    present = true;
    return S_ISREG(info.st_mode);
  }
  present = false;
  return errno == ENOENT;
}

}  // namespace

PosixUpgradeInventory::PosixUpgradeInventory(std::string internal_root)
    : internal_root_(std::move(internal_root)),
      paths_valid_(validRoot(internal_root_)) {}

std::string PosixUpgradeInventory::path(const char* relative_path) const {
  if (!paths_valid_ || !relative_path || relative_path[0] != '/' ||
      std::strstr(relative_path, "..")) return {};
  return internal_root_ + relative_path;
}

RecordProbe PosixUpgradeInventory::probeTasks(const char* relative_path) const {
  std::string record;
  const ReadCode code = readBoundedFile(
      path(relative_path), kMaximumTaskBytes, record);
  std::vector<InkloopTaskRecord> tasks;
  return mapRead(code, code == ReadCode::Ok &&
                           PosixTaskStore::decodeManifest(record, tasks));
}

RecordProbe PosixUpgradeInventory::probeAlbum(const char* relative_path) const {
  std::string record;
  const ReadCode code = readBoundedFile(
      path(relative_path), kMaximumAlbumIndexBytes, record);
  AlbumIndex ignored;
  return mapRead(code, code == ReadCode::Ok &&
                           parseAlbumIndex(record, ignored) == AlbumIndexCode::Ok);
}

RecordProbe PosixUpgradeInventory::probeDisplay(
    const char* relative_path) const {
  std::string record;
  const ReadCode code = readBoundedFile(
      path(relative_path), kMaximumDisplayJournalBytes, record);
  if (code == ReadCode::Missing) return RecordProbe::Missing;
  if (code == ReadCode::IoError) return RecordProbe::IoError;
  if (code != ReadCode::Ok || !JsonShapeCursor(record).topObject())
    return RecordProbe::Invalid;
  // A syntactically sound display journal still encodes physical panel state
  // that cannot be inferred during an upgrade. It always requires an explicit
  // target/previous resolution and is deliberately never reported Valid.
  return RecordProbe::Unvalidated;
}

void PosixUpgradeInventory::probeChatPair(RecordProbe& current,
                                          RecordProbe& previous) const {
  const std::string current_path = path("/inkloop/myai-chat.txt");
  const std::string previous_path = path("/inkloop/myai-chat.prev.txt");
  bool current_present = false;
  bool previous_present = false;
  if (!presentPath(current_path, current_present) ||
      !presentPath(previous_path, previous_present)) {
    current = previous = RecordProbe::IoError;
    return;
  }
  if (!current_present && !previous_present) {
    current = previous = RecordProbe::Missing;
    return;
  }
  struct stat info {};
  if ((current_present &&
       (::stat(current_path.c_str(), &info) != 0 || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > kDefaultChatLogBytes)) ||
      (previous_present &&
       (::stat(previous_path.c_str(), &info) != 0 || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > kDefaultChatLogBytes))) {
    current = current_present ? RecordProbe::Invalid : RecordProbe::Missing;
    previous = previous_present ? RecordProbe::Invalid : RecordProbe::Missing;
    return;
  }
  PosixChatLineStore store(current_path, previous_path);
  LocalChatLog log(store);
  ChatRecovery recovery;
  const ChatLogResult result = log.recover(recovery);
  const RecordProbe probe = !result.ok()
      ? RecordProbe::IoError
      : (recovery.corruption_observed ? RecordProbe::Recoverable
                                     : RecordProbe::Valid);
  current = current_present ? probe : RecordProbe::Missing;
  previous = previous_present ? probe : RecordProbe::Missing;
}

UpgradeAuditInput PosixUpgradeInventory::inspect(
    const std::array<RecordProbe, kProtectedNvsNamespaces.size()>&
        application_nvs) const {
  UpgradeAuditInput input;
  input.application_nvs = application_nvs;
  if (!paths_valid_) return input;
  struct stat root {};
  input.internal_mounted = ::stat(internal_root_.c_str(), &root) == 0 &&
                           S_ISDIR(root.st_mode);
  if (!input.internal_mounted) return input;
  const std::array<RecordProbe, kProtectedFilePaths.size()> files =
      inspectFiles();
  input.tasks = {files[0], files[1], files[2]};
  input.album = {files[6], files[7], files[8]};
  input.display_transaction = RecordProbe::Missing;
  for (size_t at = 3U; at <= 5U; ++at) {
    const RecordProbe probe = files[at];
    if (probe == RecordProbe::IoError) {
      input.display_transaction = RecordProbe::IoError;
      break;
    }
    if (probe == RecordProbe::Invalid) input.display_transaction = RecordProbe::Invalid;
    else if (probe != RecordProbe::Missing &&
             input.display_transaction == RecordProbe::Missing)
      input.display_transaction = RecordProbe::Unvalidated;
  }
  input.chat_current = files[9];
  input.chat_previous = files[10];
  return input;
}

std::array<RecordProbe, kProtectedFilePaths.size()>
PosixUpgradeInventory::inspectFiles() const {
  std::array<RecordProbe, kProtectedFilePaths.size()> files{};
  files.fill(RecordProbe::IoError);
  if (!paths_valid_) return files;
  struct stat root {};
  if (::stat(internal_root_.c_str(), &root) != 0 || !S_ISDIR(root.st_mode))
    return files;
  files[0] = probeTasks("/tasks.json");
  files[1] = probeTasks("/tasks.next");
  files[2] = probeTasks("/tasks.prev");
  files[3] = probeDisplay("/display-txn.json");
  files[4] = probeDisplay("/display-txn.next");
  files[5] = probeDisplay("/display-txn.prev");
  files[6] = probeAlbum("/inkloop-album/index.json");
  files[7] = probeAlbum("/inkloop-album/index.next");
  files[8] = probeAlbum("/inkloop-album/index.prev");
  probeChatPair(files[9], files[10]);
  return files;
}

}  // namespace storage
}  // namespace inkloop
