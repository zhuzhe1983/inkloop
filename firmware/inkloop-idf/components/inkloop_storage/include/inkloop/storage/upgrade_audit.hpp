#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {
namespace storage {

enum class RecordProbe : uint8_t {
  Missing,
  Valid,
  Recoverable,
  Ambiguous,
  Invalid,
  Unvalidated,
  IoError,
};

struct TransactionProbe {
  RecordProbe current = RecordProbe::Missing;
  RecordProbe next = RecordProbe::Missing;
  RecordProbe previous = RecordProbe::Missing;
};

enum class TransactionAudit : uint8_t {
  Empty,
  Clean,
  RecoveryRequired,
  Ambiguous,
  SourceUnavailable,
};

enum class UpgradeAuditResult : uint8_t {
  Fresh,
  Compatible,
  RecoveryRequired,
  DisplayResolutionRequired,
  Ambiguous,
  SourceUnavailable,
};

// Fixed legacy surface. Adding or deleting protected records is a source/API
// change and must update the migration audit before a firmware can be flashed.
inline constexpr std::array<const char*, 9> kProtectedNvsNamespaces{{
    "inkloop-v2", "inkloop", "ink-myai-v1", "ink-portal",
    "ink-album-meta", "ink-pair-ui", "nvs.net80211", "phy", "cal_data",
}};

inline constexpr std::array<const char*, 11> kProtectedFilePaths{{
    "/tasks.json",
    "/tasks.next",
    "/tasks.prev",
    "/display-txn.json",
    "/display-txn.next",
    "/display-txn.prev",
    "/inkloop-album/index.json",
    "/inkloop-album/index.next",
    "/inkloop-album/index.prev",
    "/inkloop/myai-chat.txt",
    "/inkloop/myai-chat.prev.txt",
}};

struct UpgradeAuditInput {
  bool internal_mounted = false;
  TransactionProbe tasks;
  TransactionProbe album;
  // Display transactions are intentionally pre-classified by their semantic
  // validator. Prepared is Ambiguous and never auto-resolved during upgrade.
  RecordProbe display_transaction = RecordProbe::Missing;
  std::array<RecordProbe, kProtectedNvsNamespaces.size()> application_nvs{};
  RecordProbe chat_current = RecordProbe::Missing;
  RecordProbe chat_previous = RecordProbe::Missing;
};

struct UpgradeAuditReport {
  UpgradeAuditResult result = UpgradeAuditResult::SourceUnavailable;
  TransactionAudit tasks = TransactionAudit::SourceUnavailable;
  TransactionAudit album = TransactionAudit::SourceUnavailable;
  size_t protected_records_present = 0;

  bool allowsInitialization() const {
    return result == UpgradeAuditResult::Fresh ||
           result == UpgradeAuditResult::Compatible;
  }
};

TransactionAudit classifyTransaction(const TransactionProbe& probe);
UpgradeAuditReport auditUpgrade(const UpgradeAuditInput& input);
const char* upgradeAuditResultName(UpgradeAuditResult result);
const char* transactionAuditName(TransactionAudit result);

}  // namespace storage
}  // namespace inkloop
