#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {
namespace {

bool isPresent(RecordProbe value) { return value != RecordProbe::Missing; }

bool isIoError(RecordProbe value) { return value == RecordProbe::IoError; }

bool isAmbiguous(RecordProbe value) {
  return value == RecordProbe::Ambiguous || value == RecordProbe::Invalid;
}

bool needsRecovery(RecordProbe value) {
  return value == RecordProbe::Recoverable ||
         value == RecordProbe::Unvalidated;
}

size_t countPresent(const TransactionProbe& probe) {
  return static_cast<size_t>(isPresent(probe.current)) +
         static_cast<size_t>(isPresent(probe.next)) +
         static_cast<size_t>(isPresent(probe.previous));
}

}  // namespace

TransactionAudit classifyTransaction(const TransactionProbe& probe) {
  if (isIoError(probe.current) || isIoError(probe.next) ||
      isIoError(probe.previous)) {
    return TransactionAudit::SourceUnavailable;
  }
  if (!isPresent(probe.current) && !isPresent(probe.next) &&
      !isPresent(probe.previous)) {
    return TransactionAudit::Empty;
  }
  if (probe.current == RecordProbe::Valid &&
      probe.next == RecordProbe::Missing &&
      (probe.previous == RecordProbe::Missing ||
       probe.previous == RecordProbe::Valid)) {
    return TransactionAudit::Clean;
  }

  const size_t valid_recovery_candidates =
      static_cast<size_t>(probe.next == RecordProbe::Valid) +
      static_cast<size_t>(probe.previous == RecordProbe::Valid);
  if (probe.current != RecordProbe::Valid && valid_recovery_candidates > 1U)
    return TransactionAudit::Ambiguous;
  if (isAmbiguous(probe.current) && valid_recovery_candidates == 0U)
    return TransactionAudit::Ambiguous;
  if (isAmbiguous(probe.next) || isAmbiguous(probe.previous))
    return TransactionAudit::RecoveryRequired;
  if (needsRecovery(probe.current) || needsRecovery(probe.next) ||
      needsRecovery(probe.previous) || probe.current == RecordProbe::Valid ||
      valid_recovery_candidates == 1U) {
    return TransactionAudit::RecoveryRequired;
  }
  return TransactionAudit::Ambiguous;
}

UpgradeAuditReport auditUpgrade(const UpgradeAuditInput& input) {
  UpgradeAuditReport report;
  report.tasks = classifyTransaction(input.tasks);
  report.album = classifyTransaction(input.album);
  report.protected_records_present = countPresent(input.tasks) +
                                     countPresent(input.album) +
                                     static_cast<size_t>(
                                         isPresent(input.display_transaction)) +
                                     static_cast<size_t>(isPresent(input.chat_current)) +
                                     static_cast<size_t>(isPresent(input.chat_previous));
  for (RecordProbe record : input.application_nvs) {
    report.protected_records_present += static_cast<size_t>(isPresent(record));
  }

  if (!input.internal_mounted ||
      report.tasks == TransactionAudit::SourceUnavailable ||
      report.album == TransactionAudit::SourceUnavailable ||
      isIoError(input.display_transaction) || isIoError(input.chat_current) ||
      isIoError(input.chat_previous)) {
    report.result = UpgradeAuditResult::SourceUnavailable;
    return report;
  }
  for (RecordProbe record : input.application_nvs) {
    if (isIoError(record)) {
      report.result = UpgradeAuditResult::SourceUnavailable;
      return report;
    }
  }

  if (input.display_transaction == RecordProbe::Ambiguous ||
      input.display_transaction == RecordProbe::Invalid ||
      input.display_transaction == RecordProbe::Recoverable ||
      input.display_transaction == RecordProbe::Unvalidated ||
      input.display_transaction == RecordProbe::Valid) {
    report.result = UpgradeAuditResult::DisplayResolutionRequired;
    return report;
  }
  if (report.tasks == TransactionAudit::Ambiguous ||
      report.album == TransactionAudit::Ambiguous) {
    report.result = UpgradeAuditResult::Ambiguous;
    return report;
  }
  for (RecordProbe record : input.application_nvs) {
    if (isAmbiguous(record)) {
      report.result = UpgradeAuditResult::Ambiguous;
      return report;
    }
  }

  if (report.tasks == TransactionAudit::RecoveryRequired ||
      report.album == TransactionAudit::RecoveryRequired ||
      needsRecovery(input.chat_current) || needsRecovery(input.chat_previous)) {
    report.result = UpgradeAuditResult::RecoveryRequired;
    return report;
  }
  for (RecordProbe record : input.application_nvs) {
    if (needsRecovery(record)) {
      report.result = UpgradeAuditResult::RecoveryRequired;
      return report;
    }
  }
  report.result = report.protected_records_present == 0U
                      ? UpgradeAuditResult::Fresh
                      : UpgradeAuditResult::Compatible;
  return report;
}

const char* upgradeAuditResultName(UpgradeAuditResult result) {
  switch (result) {
    case UpgradeAuditResult::Fresh:
      return "FRESH";
    case UpgradeAuditResult::Compatible:
      return "COMPATIBLE";
    case UpgradeAuditResult::RecoveryRequired:
      return "RECOVERY_REQUIRED";
    case UpgradeAuditResult::DisplayResolutionRequired:
      return "DISPLAY_RESOLUTION_REQUIRED";
    case UpgradeAuditResult::Ambiguous:
      return "AMBIGUOUS";
    case UpgradeAuditResult::SourceUnavailable:
      return "SOURCE_UNAVAILABLE";
  }
  return "UNKNOWN";
}

const char* transactionAuditName(TransactionAudit result) {
  switch (result) {
    case TransactionAudit::Empty:
      return "EMPTY";
    case TransactionAudit::Clean:
      return "CLEAN";
    case TransactionAudit::RecoveryRequired:
      return "RECOVERY_REQUIRED";
    case TransactionAudit::Ambiguous:
      return "AMBIGUOUS";
    case TransactionAudit::SourceUnavailable:
      return "SOURCE_UNAVAILABLE";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
