#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inkloop {
namespace storage {

enum class ChatRecordKind : uint8_t {
  AsrFinal,
  AssistantFinal,
  ToolState,
  AigcState,
};

enum class ChatLogCode : uint8_t {
  Ok,
  IgnoredPartial,
  IgnoredEmpty,
  IgnoredBlankAudio,
  InvalidArgument,
  NotReady,
  TooLarge,
  IoError,
  SequenceExhausted,
};

struct ChatLogResult {
  ChatLogCode code = ChatLogCode::Ok;

  bool ok() const { return code == ChatLogCode::Ok; }
};

struct ChatRecord {
  uint64_t sequence = 0;
  ChatRecordKind kind = ChatRecordKind::AsrFinal;
  std::string utc;
  std::string text;
};

struct ChatPage {
  std::vector<ChatRecord> records;
  uint64_t next_cursor = 0;
  bool has_more = false;
  bool corruption_observed = false;
};

struct ChatRecovery {
  uint64_t next_sequence = 1;
  size_t valid_records = 0;
  bool corruption_observed = false;
  bool rotated_history_present = false;
};

inline constexpr size_t kMaximumChatTextBytes = 3072;
inline constexpr size_t kMaximumChatLineBytes = 4096;
inline constexpr size_t kMaximumChatPageItems = 32;
inline constexpr size_t kMaximumChatPageTextBytes = 24U * 1024U;
inline constexpr size_t kDefaultChatLogBytes = 512U * 1024U;

class IChatLineVisitor {
 public:
  virtual ~IChatLineVisitor() = default;
  // Returning false stops a scan successfully after the current line.
  virtual bool onLine(const char* line, size_t length) = 0;
  virtual bool onMalformedLine() = 0;
};

class IChatLineStore {
 public:
  virtual ~IChatLineStore() = default;
  virtual ChatLogResult appendLine(const std::string& line,
                                   size_t rotate_at_bytes) = 0;
  virtual ChatLogResult scan(IChatLineVisitor& visitor) const = 0;
  virtual ChatLogResult clear() = 0;
  virtual bool rotatedHistoryPresent() const = 0;
};

// This class is deliberately synchronous and non-thread-safe. Only the slow
// storage owner may call it. Voice/network/Portal tasks exchange typed commands
// with that owner and never touch files or this object directly.
class LocalChatLog {
 public:
  explicit LocalChatLog(IChatLineStore& store,
                        size_t rotate_at_bytes = kDefaultChatLogBytes);

  ChatLogResult recover(ChatRecovery& recovery);
  ChatLogResult appendAsr(const std::string& text, bool final,
                          const std::string& utc);
  ChatLogResult appendAssistant(const std::string& text, bool final,
                                const std::string& utc);
  ChatLogResult appendToolState(const std::string& text,
                                const std::string& utc);
  ChatLogResult appendAigcState(const std::string& text,
                                const std::string& utc);
  ChatLogResult readPage(uint64_t after_sequence, size_t limit,
                         ChatPage& page) const;
  ChatLogResult clear();

  uint64_t nextSequence() const { return next_sequence_; }
  bool ready() const { return ready_; }
  bool corruptionObserved() const { return corruption_observed_; }

  static const char* roleName(ChatRecordKind kind);
  static const char* kindName(ChatRecordKind kind);
  static bool isBlankAudioArtifact(const std::string& text);

 private:
  ChatLogResult append(ChatRecordKind kind, const std::string& text,
                       const std::string& utc);

  IChatLineStore& store_;
  size_t rotate_at_bytes_;
  uint64_t next_sequence_ = 1;
  bool ready_ = false;
  bool corruption_observed_ = false;
};

const char* chatLogCodeName(ChatLogCode code);

}  // namespace storage
}  // namespace inkloop
