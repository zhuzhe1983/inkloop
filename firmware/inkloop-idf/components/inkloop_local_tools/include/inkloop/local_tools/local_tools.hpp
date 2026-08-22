#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace inkloop {
namespace local_tools {

inline constexpr size_t kMaximumTranscriptBytes = 512U;
inline constexpr size_t kMaximumImageIdBytes = 64U;
// Voice-authored values are bounded by the final-ASR envelope. Persisted
// Portal settings have their own larger boundary and must not be truncated to
// the voice parser limit.
inline constexpr size_t kMaximumPromptBytes = 512U;
inline constexpr size_t kMaximumStoredPromptBytes = 1024U;
inline constexpr size_t kMaximumConfirmationTokenBytes = 48U;
inline constexpr uint32_t kMaximumImageOrdinal = 96U;
inline constexpr uint64_t kConfirmationLifetimeMs = 30000U;

enum class CommandKind : uint8_t {
  None,
  QueryStorage,
  DeleteImageOrdinal,
  DeleteImageId,
  ClearAlbum,
  QueryVolume,
  SetVolume,
  FormatTfCard,
  QueryAssistantPrompt,
  SetAssistantPrompt,
  QueryAigcPrompt,
  SetAigcPrompt,
  SetLedMaximumBrightness,
};

enum class StorageMetric : uint8_t { Remaining, Total, Both };

enum class ParseCode : uint8_t {
  Matched,
  IgnoredEmpty,
  IgnoredBlankAudio,
  NoMatch,
  Ambiguous,
  InvalidEncoding,
  TooLong,
  InvalidValue,
};

struct Command {
  CommandKind kind = CommandKind::None;
  StorageMetric storage_metric = StorageMetric::Both;
  uint32_t number = 0;
  std::string text;
};

struct ParseResult {
  ParseCode code = ParseCode::NoMatch;
  Command command{};

  bool matched() const { return code == ParseCode::Matched; }
  bool ignored() const {
    return code == ParseCode::IgnoredEmpty ||
           code == ParseCode::IgnoredBlankAudio;
  }
};

class LocalCommandParser {
 public:
  ParseResult parseFinalAsr(std::string_view transcript) const;

  static bool validImageId(std::string_view value);
  static bool validPrompt(std::string_view value);
  static bool validStoredPrompt(std::string_view value);
  static bool destructive(CommandKind kind);
};

enum class AdapterCode : uint8_t {
  Ok,
  Unsupported,
  NotReady,
  NotFound,
  Conflict,
  IoError,
};

struct AdapterResult {
  AdapterCode code = AdapterCode::Ok;

  bool ok() const { return code == AdapterCode::Ok; }
};

struct StorageInfo {
  uint64_t remaining_bytes = 0;
  uint64_t total_bytes = 0;
};

// A SKU implements this typed boundary. User text is never used as a path or
// storage target, and formatting has no target parameter: it can only mean TF.
class ILocalToolsAdapter {
 public:
  virtual ~ILocalToolsAdapter() = default;

  virtual AdapterResult queryStorage(StorageInfo& output) = 0;
  virtual AdapterResult deleteImageByOrdinal(uint32_t one_based_ordinal) = 0;
  virtual AdapterResult deleteImageById(const std::string& exact_id) = 0;
  virtual AdapterResult clearAlbum() = 0;
  virtual AdapterResult queryVolume(uint8_t& percent) = 0;
  virtual AdapterResult setVolume(uint8_t percent) = 0;
  virtual AdapterResult formatTfCard() = 0;
  virtual AdapterResult queryAssistantPrompt(std::string& output) = 0;
  virtual AdapterResult setAssistantPrompt(const std::string& prompt) = 0;
  virtual AdapterResult queryAigcPrompt(std::string& output) = 0;
  // Settings-only read used when composing the real MyAI image request; it is
  // intentionally not exposed as a spoken local tool command.
  virtual AdapterResult queryAigcNegativePrompt(std::string& output) = 0;
  virtual AdapterResult queryDefaultRenderStrategy(std::string& output) = 0;
  virtual AdapterResult setAigcPrompt(const std::string& prompt) = 0;
  virtual AdapterResult setLedMaximumBrightness(uint8_t percent) = 0;
};

// Production SKUs should issue an unpredictable token. The portable core
// validates and binds it but deliberately owns no RNG or hardware facility.
class IConfirmationTokenSource {
 public:
  virtual ~IConfirmationTokenSource() = default;
  virtual bool issue(std::string& token) = 0;
};

enum class ExecutionCode : uint8_t {
  Ignored,
  Rejected,
  ConfirmationRequired,
  Executed,
  AdapterFailure,
  TokenUnavailable,
  ConfirmationMissing,
  ConfirmationMismatch,
  ConfirmationExpired,
  AdapterContractViolation,
};

struct ToolOutcome {
  ExecutionCode code = ExecutionCode::Rejected;
  ParseCode parse_code = ParseCode::NoMatch;
  CommandKind command = CommandKind::None;
  AdapterCode adapter_code = AdapterCode::Ok;
  StorageMetric storage_metric = StorageMetric::Both;
  StorageInfo storage{};
  uint8_t percent = 0;
  std::string text;
  std::string confirmation_token;
};

class LocalToolsSession {
 public:
  ToolOutcome handleFinalAsr(std::string_view transcript, uint64_t now_ms,
                             ILocalToolsAdapter& adapter,
                             IConfirmationTokenSource& tokens);
  ToolOutcome confirm(std::string_view token, uint64_t now_ms,
                      ILocalToolsAdapter& adapter);
  void cancelConfirmation();
  bool confirmationPending() const { return pending_; }

 private:
  ToolOutcome execute(const Command& command, ILocalToolsAdapter& adapter);
  static bool validToken(std::string_view token);
  static bool tokensEqual(std::string_view left, std::string_view right);

  LocalCommandParser parser_{};
  Command pending_command_{};
  std::string pending_token_;
  uint64_t pending_issued_ms_ = 0;
  bool pending_ = false;
};

const char* commandName(CommandKind kind);
const char* parseCodeName(ParseCode code);
const char* executionCodeName(ExecutionCode code);

}  // namespace local_tools
}  // namespace inkloop
