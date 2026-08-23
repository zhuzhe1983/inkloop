#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {
namespace recovery {

inline constexpr size_t kMaximumRecoveryRequestBodyBytes = 192U;
inline constexpr size_t kMaximumRecoveryResponseBytes = 16384U;
inline constexpr size_t kMaximumRecoveryIdentifierBytes = 48U;
inline constexpr size_t kMaximumRecoveryAccessCodeBytes = 63U;
inline constexpr size_t kMaximumRecoveryTokenBytes = 64U;
inline constexpr size_t kMaximumRecoveryHosts = 4U;
inline constexpr size_t kMaximumRecoveryOrigins = 4U;

enum class RecoveryReason : uint8_t {
  BootAuditRefused,
  MigrationRefused,
  OtaHealthRefused,
  StorageIntegrityRefused,
};

enum class RecoveryPhase : uint8_t {
  BootAudit,
  Migration,
  OtaHealth,
  StorageAudit,
};

enum class RecoveryOutcome : uint8_t {
  Refused,
  RequiresOperator,
  Failed,
  Incomplete,
};

// This is deliberately not a generic key/value structure. Callers can only
// publish aggregate counts; namespace names, file paths and record contents do
// not fit in this contract.
struct RecoveryRecordCounts {
  uint32_t nvs_namespaces = 0;
  uint32_t files = 0;
  uint32_t settings_records = 0;
  uint32_t task_records = 0;
  uint32_t album_assets = 0;
  uint32_t ota_slots = 0;
};

// Fixed-size and credential-free. The owner prepares this snapshot outside the
// HTTP callback; the server only copies and validates it.
struct RecoveryDiagnosticSnapshot {
  RecoveryReason reason = RecoveryReason::BootAuditRefused;
  RecoveryPhase phase = RecoveryPhase::BootAudit;
  RecoveryOutcome outcome = RecoveryOutcome::Refused;
  std::array<char, kMaximumRecoveryIdentifierBytes + 1U> firmware_id{};
  std::array<char, kMaximumRecoveryIdentifierBytes + 1U> board_id{};
  RecoveryRecordCounts records{};
  bool normal_startup_refused = false;
};

enum class RecoveryReadResult : uint8_t {
  Ok,
  Busy,
  Unavailable,
  InvalidData,
};

class IRecoveryDiagnosticCache {
 public:
  virtual ~IRecoveryDiagnosticCache() = default;
  // Must only copy a precomputed fixed-size snapshot. No storage/NVS scans,
  // network access, hardware waits or mutations are permitted here.
  virtual RecoveryReadResult readRecoveryDiagnostic(
      RecoveryDiagnosticSnapshot& output) const = 0;
};

inline constexpr size_t kRecoveryActionCandidateCount = 3U;
inline constexpr size_t kMaximumRecoveryActionSnapshots = 4U;
inline constexpr size_t kRecoveryActionDigestBytes = 32U;

enum class RecoveryActionDomain : uint8_t {
  Display,
  Tasks,
  Album,
};

enum class RecoveryActionBackend : uint8_t {
  None,
  Internal,
  Removable,
};

enum class RecoveryActionChoice : uint8_t {
  Current,
  Next,
  Previous,
};

enum class RecoveryActionCandidateState : uint8_t {
  Missing,
  Valid,
  Invalid,
  IoError,
};

enum class RecoveryActionState : uint8_t {
  Empty,
  Recoverable,
  ChoiceRequired,
  Corrupt,
  IoError,
  Disabled,
};

struct RecoveryActionCandidate {
  RecoveryActionCandidateState state = RecoveryActionCandidateState::Missing;
  uint64_t byte_count = 0U;
  std::array<uint8_t, kRecoveryActionDigestBytes> digest{};
  bool digest_present = false;
  // Content-free diagnostics. For file transactions this is a task count or
  // album-entry count plus an optional coarse filesystem modification time.
  // Display candidates leave both absent.
  uint32_t item_count = 0U;
  bool item_count_present = false;
  uint32_t modified_unix_seconds = 0U;
  bool modified_time_present = false;
};

// A fixed, content-free inspection result. Display and Tasks use backend None;
// Album uses Internal or Removable. Candidate indexes are possible operator
// choices in Current/Next/Previous order, not underlying journal slots. For
// Display, Current means complete the target transaction, Previous means keep
// the previous display, and Next must remain unavailable.
struct RecoveryActionSnapshot {
  RecoveryActionDomain domain = RecoveryActionDomain::Display;
  RecoveryActionBackend backend = RecoveryActionBackend::None;
  RecoveryActionState state = RecoveryActionState::Disabled;
  std::array<RecoveryActionCandidate, kRecoveryActionCandidateCount>
      candidates{};
  std::array<uint8_t, kRecoveryActionDigestBytes> inspection_id{};
  uint8_t valid_candidates = 0U;
};

struct RecoveryActionInventory {
  std::array<RecoveryActionSnapshot, kMaximumRecoveryActionSnapshots>
      snapshots{};
  uint8_t count = 0U;
};

enum class RecoveryActionReadResult : uint8_t {
  Ok,
  Busy,
  Unavailable,
  InvalidData,
};

struct RecoveryActionRequest {
  RecoveryActionDomain domain = RecoveryActionDomain::Display;
  RecoveryActionBackend backend = RecoveryActionBackend::None;
  RecoveryActionChoice choice = RecoveryActionChoice::Current;
  std::array<uint8_t, kRecoveryActionDigestBytes> inspection_id{};
};

enum class RecoveryActionResolveResult : uint8_t {
  Ok,
  Busy,
  InvalidRequest,
  SourceChanged,
  SourceUnavailable,
  SelectedUnavailable,
  IoError,
  VerificationFailed,
};

// Implemented by the recovery-mode composition owner. The Portal never sees
// a storage type, path or key and never selects a candidate automatically.
class IRecoveryActionOwner {
 public:
  virtual ~IRecoveryActionOwner() = default;
  // Must inspect without mutation and bind every actionable inspection_id to
  // the owner's complete underlying typed snapshot.
  virtual RecoveryActionReadResult inspectRecoveryActions(
      RecoveryActionInventory& output) = 0;
  // Must reject an unknown/stale inspection_id and an unavailable choice,
  // re-inspect immediately before mutation, and perform only the named action.
  virtual RecoveryActionResolveResult resolveRecoveryAction(
      const RecoveryActionRequest& request) = 0;
};

struct RecoveryAccessConfig {
  std::string access_code;
  std::string session_id;
  std::string csrf_token;
  std::array<std::string, kMaximumRecoveryHosts> allowed_hosts{};
  std::array<std::string, kMaximumRecoveryOrigins> allowed_origins{};
  uint8_t allowed_host_count = 0;
  uint8_t allowed_origin_count = 0;
  uint32_t session_lifetime_seconds = 900U;
};

struct RecoveryRequest {
  std::string method;
  std::string path;
  std::string host;
  std::string origin;
  std::string cookie;
  std::string csrf_token;
  std::string content_type;
  std::string body;
  bool peer_is_local = false;
  uint64_t content_length = 0;
  uint64_t now_seconds = 0;
};

struct RecoveryResponse {
  int status = 500;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::string set_cookie;
};

class RecoveryPortalCore {
 public:
  RecoveryPortalCore(const RecoveryAccessConfig& access,
                     const IRecoveryDiagnosticCache& cache,
                     IRecoveryActionOwner* action_owner = nullptr);
  ~RecoveryPortalCore();

  RecoveryPortalCore(const RecoveryPortalCore&) = delete;
  RecoveryPortalCore& operator=(const RecoveryPortalCore&) = delete;

  bool ready() const { return ready_; }
  RecoveryResponse handle(const RecoveryRequest& request);
  static const char* dashboardHtml();

 private:
  void scrubCredentials();
  bool validateConfiguration() const;
  bool hostAllowed(const std::string& host) const;
  bool originAllowed(const std::string& origin) const;
  bool sessionAuthorized(const RecoveryRequest& request) const;
  RecoveryResponse handleLogin(const RecoveryRequest& request);
  RecoveryResponse renderDiagnostic() const;
  RecoveryResponse renderRecoveryActions();
  RecoveryResponse resolveRecoveryAction(const RecoveryRequest& request);

  RecoveryAccessConfig access_;
  const IRecoveryDiagnosticCache& cache_;
  IRecoveryActionOwner* action_owner_ = nullptr;
  bool ready_ = false;
  bool session_issued_ = false;
  uint64_t session_expires_at_seconds_ = 0;
};

const char* recoveryReasonName(RecoveryReason value);
const char* recoveryPhaseName(RecoveryPhase value);
const char* recoveryOutcomeName(RecoveryOutcome value);
const char* recoveryActionDomainName(RecoveryActionDomain value);
const char* recoveryActionBackendName(RecoveryActionBackend value);
const char* recoveryActionChoiceName(RecoveryActionChoice value);
const char* recoveryActionCandidateStateName(
    RecoveryActionCandidateState value);
const char* recoveryActionStateName(RecoveryActionState value);

}  // namespace recovery
}  // namespace inkloop
