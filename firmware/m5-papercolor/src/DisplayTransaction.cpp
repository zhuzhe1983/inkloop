#include "DisplayTransaction.h"

#include <ArduinoJson.h>

#include "AppConfig.h"
#include "BackendTransactionIo.h"
#include "Diagnostics.h"
#include "DisplayFinalizeCore.h"
#include "JsonRecordCodec.h"

namespace inkloop {

namespace {

bool validAssetId(const String& value, bool allowEmpty = false) {
  if (allowEmpty && !value.length()) return true;
  if (value.length() != 64) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F'))) return false;
  }
  return true;
}

void populateJournal(
  JsonDocument& journal,
  DisplayJournalStage stage,
  const char* backend,
  const String& assetId,
  const String& assetPath,
  const String& previousCurrent,
  const String& operation,
  size_t assetBytes,
  bool landscape,
  size_t page,
  const DueTask* task,
  time_t runAt,
  uint32_t runDay
) {
  journal["schema"] = 1;
  journal["stage"] = static_cast<uint8_t>(stage);
  journal["backend"] = backend;
  journal["assetId"] = assetId;
  journal["assetPath"] = assetPath;
  journal["previousCurrent"] = previousCurrent;
  journal["operation"] = operation;
  journal["bytes"] = assetBytes;
  journal["landscape"] = landscape;
  journal["page"] = page;
  journal["hasTask"] = task != nullptr;
  journal["runAt"] = static_cast<uint32_t>(runAt);
  journal["runDay"] = runDay;
  if (task) {
    journal["taskId"] = task->id;
    journal["taskRevision"] = task->revision;
    journal["taskFrameUrl"] = task->frameUrl;
    journal["taskFrameHash"] = task->frameHash;
  }
}

bool journalSchemaValid(JsonDocument& journal) {
  if (!journal.is<JsonObject>() || !journal["schema"].is<uint16_t>() ||
      static_cast<uint16_t>(journal["schema"] | 0) != 1 ||
      !journal["stage"].is<uint8_t>() || !journal["backend"].is<const char*>() ||
      !journal["assetId"].is<const char*>() || !journal["assetPath"].is<const char*>() ||
      !journal["previousCurrent"].is<const char*>() ||
      !journal["operation"].is<const char*>() || !journal["bytes"].is<uint32_t>() ||
      !journal["landscape"].is<bool>() || !journal["page"].is<uint32_t>() ||
      !journal["hasTask"].is<bool>() || !journal["runAt"].is<uint32_t>() ||
      !journal["runDay"].is<uint32_t>()) return false;
  const uint8_t encodedStage = journal["stage"] | 0;
  if (encodedStage < static_cast<uint8_t>(DisplayJournalStage::Prepared) ||
      encodedStage > static_cast<uint8_t>(DisplayJournalStage::Aborted)) return false;
  const bool hasTask = journal["hasTask"] | false;
  if (!hasTask) return String(journal["operation"] | "") == "page" &&
    static_cast<uint32_t>(journal["runAt"] | 0U) == 0;
  return String(journal["operation"] | "") == "task" &&
    static_cast<uint32_t>(journal["runAt"] | 0U) > 0 &&
    journal["taskId"].is<const char*>() && journal["taskRevision"].is<uint32_t>() &&
    journal["taskFrameUrl"].is<const char*>() && journal["taskFrameHash"].is<const char*>() &&
    String(journal["taskId"] | "").length() && String(journal["taskFrameUrl"] | "").length();
}

}  // namespace

class DisplayFinalizeOperations {
 public:
  explicit DisplayFinalizeOperations(DisplayTransaction& transaction) : transaction_(transaction) {}
  bool persist(DisplayJournalStage) { return transaction_.persist(); }
  bool commitCurrent() {
    return transaction_.album_.markCurrent(transaction_.backend_, transaction_.assetId_);
  }
  bool taskAcknowledged() {
    return transaction_.tasks_.isRunAcknowledged(transaction_.task_, transaction_.runAt_);
  }
  bool acknowledgeTask() {
    return transaction_.tasks_.markRunWithDay(
      transaction_.task_, transaction_.runAt_, transaction_.runDay_
    );
  }
  bool clear() { return transaction_.clear(); }

 private:
  DisplayTransaction& transaction_;
};

constexpr const char* DisplayTransaction::kJournalPath;
constexpr const char* DisplayTransaction::kJournalNextPath;
constexpr const char* DisplayTransaction::kJournalPreviousPath;

void DisplayTransaction::reset() {
  backend_ = StorageBackendRef{};
  assetId_ = "";
  assetPath_ = "";
  previousCurrent_ = "";
  operation_ = "";
  assetBytes_ = 0;
  landscape_ = false;
  page_ = 0;
  hasTask_ = false;
  task_ = DueTask{};
  runAt_ = 0;
  runDay_ = 0;
  stage_ = DisplayJournalStage::None;
  persistedStage_ = DisplayJournalStage::None;
  active_ = false;
  corrupt_ = false;
  journalCanonical_ = true;
  recoveryCandidatePath_ = "";
}

void DisplayTransaction::blockOnCorruptJournal(const StorageBackendRef& control) {
  reset();
  active_ = true;
  corrupt_ = true;
  backend_ = control;
  operation_ = "corrupt-journal";
  stage_ = DisplayJournalStage::Prepared;
  persistedStage_ = DisplayJournalStage::None;
  Diagnostics::event("FATAL", "DISPLAY_TXN_CORRUPT");
}

bool DisplayTransaction::serializeJournal(DisplayJournalStage stage, String& payload) const {
  JsonDocument journal;
  populateJournal(
    journal, stage, backend_.identity, assetId_, assetPath_, previousCurrent_, operation_,
    assetBytes_, landscape_, page_, hasTask_ ? &task_ : nullptr, runAt_, runDay_
  );
  return serializeJsonRecordExactly(journal, payload);
}

bool DisplayTransaction::persist() {
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available() || !active_) return false;
  String payload;
  if (!serializeJournal(stage_, payload)) return false;
  BackendTransactionIo io(*control.backend);
  TransactionalFileStore transaction(io);
  if (!transaction.commitValidatedRecord(
        kJournalPath,
        kJournalNextPath,
        kJournalPreviousPath,
        reinterpret_cast<const uint8_t*>(payload.c_str()),
        payload.length(),
        [this](const char* path) { return validateJournalFile(path); }
      )) return false;
  persistedStage_ = stage_;
  journalCanonical_ = true;
  recoveryCandidatePath_ = "";
  Diagnostics::event("DISPLAY_TXN_STAGE", String(static_cast<int>(stage_)));
  return true;
}

bool DisplayTransaction::estimateJournalRecordBytes(
  const DueTask* task,
  time_t runAt,
  uint32_t runDay,
  size_t& bytes
) {
  const String id = "0000000000000000000000000000000000000000000000000000000000000000";
  const String path = String("/inkloop-album/") + id + ".png";
  JsonDocument journal;
  populateJournal(
    journal,
    DisplayJournalStage::TaskAcknowledged,
    "littlefs",
    id,
    path,
    id,
    task ? "task" : "page",
    kMaxFrameBytes,
    true,
    96,
    task,
    runAt,
    runDay
  );
  bytes = measureJson(journal);
  return bytes > 0;
}

bool DisplayTransaction::hasFinalizeHeadroom() {
  size_t journalBytes = 0;
  for (uint8_t encoded = static_cast<uint8_t>(DisplayJournalStage::Prepared);
       encoded <= static_cast<uint8_t>(DisplayJournalStage::TaskAcknowledged); ++encoded) {
    String payload;
    if (!serializeJournal(static_cast<DisplayJournalStage>(encoded), payload)) return false;
    if (payload.length() > journalBytes) journalBytes = payload.length();
  }
  size_t taskNextBytes = 0;
  if (hasTask_ && !tasks_.acknowledgementPayloadSize(task_, runAt_, runDay_, taskNextBytes)) {
    return false;
  }
  size_t indexNextBytes = 0;
  if (!album_.indexSerializedSize(backend_, indexNextBytes)) return false;
  const MetadataBudget budget(taskNextBytes, journalBytes);
  const StorageBackendRef control = storage_.taskBackend();
  if (!control.available()) return false;
  const bool shared = control.backend == backend_.backend;
  size_t assetTransactionBytes = 0;
  if (!metadataTransactionBytes(budget, indexNextBytes, shared, assetTransactionBytes) ||
      !storageCanPreserveReserve(
        backend_.backend->totalBytes(), backend_.backend->usedBytes(), 0,
        assetTransactionBytes,
        backend_.backend->capabilities().removable
          ? kSdFinalReserveBytes
          : kLittleFsFinalReserveBytes
      )) return false;
  if (shared) return true;
  size_t controlBytes = 0;
  return controlTransactionBytes(budget, controlBytes) &&
    storageCanPreserveReserve(
      control.backend->totalBytes(), control.backend->usedBytes(), 0,
      controlBytes, kLittleFsFinalReserveBytes
    );
}

bool DisplayTransaction::clear() {
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available()) return false;
  BackendTransactionIo io(*control.backend);
  if (!io.remove(kJournalNextPath) || !io.remove(kJournalPreviousPath) || !io.remove(kJournalPath)) {
    return false;
  }
  Diagnostics::event("DISPLAY_TXN", "COMMITTED");
  reset();
  return true;
}

bool DisplayTransaction::begin(
  const AlbumAsset& asset,
  const char* operation,
  const DueTask* task,
  time_t runAt,
  uint32_t runDay
) {
  const String requestedOperation = operation ? operation : "unknown";
  const bool taskShapeValid = task
    ? requestedOperation == "task" && task->id.length() && task->frameUrl.length() && runAt > 0
    : requestedOperation == "page" && runAt == 0;
  if (active_ || !asset.backend.available() || !validAssetId(asset.id) ||
      asset.path != String("/inkloop-album/") + asset.id + ".png" ||
      !asset.bytes || asset.bytes > kMaxFrameBytes || !taskShapeValid) return false;
  reset();
  active_ = true;
  backend_ = asset.backend;
  assetId_ = asset.id;
  assetPath_ = asset.path;
  assetBytes_ = asset.bytes;
  landscape_ = asset.landscape;
  page_ = asset.page;
  operation_ = requestedOperation;
  hasTask_ = task != nullptr;
  if (task) task_ = *task;
  runAt_ = runAt;
  runDay_ = runDay;
  if (!album_.currentId(backend_, previousCurrent_)) {
    reset();
    return false;
  }
  if (!hasFinalizeHeadroom()) {
    reset();
    Diagnostics::event("ERROR", "DISPLAY_METADATA_CAPACITY");
    return false;
  }
  stage_ = DisplayJournalStage::Prepared;
  if (!persist()) {
    reset();
    return false;
  }
  Diagnostics::event("DISPLAY_TXN", "PREPARED");
  return true;
}

bool DisplayTransaction::confirmDisplayed() {
  if (!active_ || stage_ != DisplayJournalStage::Prepared) return false;
  stage_ = DisplayJournalStage::Displayed;
  return persist();
}

bool DisplayTransaction::abortBeforeDisplay() {
  if (!active_ || stage_ != DisplayJournalStage::Prepared) return false;
  stage_ = DisplayJournalStage::Aborted;
  if (!persist()) return false;
  return clear();
}

bool DisplayTransaction::retryFinalize() {
  if (!active_) return true;
  if (!ensureCanonicalJournal()) return false;
  DisplayFinalizeOperations operations(*this);
  return finalizeDisplayMetadata(stage_, persistedStage_, hasTask_, operations) ==
    FinalizeResult::Complete;
}

bool DisplayTransaction::ensureCanonicalJournal() {
  if (journalCanonical_) return true;
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available() || !recoveryCandidatePath_.length() ||
      !control.backend->exists(recoveryCandidatePath_.c_str())) return false;
  if (control.backend->exists(kJournalPath) && !control.backend->remove(kJournalPath)) return false;
  if (!control.backend->rename(recoveryCandidatePath_.c_str(), kJournalPath)) return false;
  journalCanonical_ = true;
  recoveryCandidatePath_ = "";
  Diagnostics::event("DISPLAY_TXN", "RECOVERY_JOURNAL_PROMOTED");
  return true;
}

bool DisplayTransaction::loadJournal(const char* path) {
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available()) return false;
  File file = control.backend->open(path, FILE_READ);
  if (!file) return false;
  JsonDocument journal;
  const DeserializationError error = deserializeJson(journal, file);
  file.close();
  if (error || !journalSchemaValid(journal)) return false;
  const uint8_t encodedStage = journal["stage"] | 0;
  const String backendIdentity = journal["backend"] | "";
  StorageBackendRef backend = storage_.backendByIdentity(backendIdentity);
  if (!backend.valid() || encodedStage < static_cast<uint8_t>(DisplayJournalStage::Prepared) ||
      encodedStage > static_cast<uint8_t>(DisplayJournalStage::Aborted)) return false;
  reset();
  active_ = true;
  backend_ = backend;
  stage_ = static_cast<DisplayJournalStage>(encodedStage);
  persistedStage_ = stage_;
  assetId_ = journal["assetId"] | "";
  assetPath_ = journal["assetPath"] | "";
  previousCurrent_ = journal["previousCurrent"] | "";
  operation_ = journal["operation"] | "";
  assetBytes_ = journal["bytes"] | 0U;
  landscape_ = journal["landscape"] | false;
  page_ = journal["page"] | 0U;
  hasTask_ = journal["hasTask"] | false;
  runAt_ = static_cast<time_t>(static_cast<uint32_t>(journal["runAt"] | 0U));
  runDay_ = journal["runDay"] | 0U;
  if (hasTask_) {
    task_.id = journal["taskId"] | "";
    task_.revision = journal["taskRevision"] | 0U;
    task_.frameUrl = journal["taskFrameUrl"] | "";
    task_.frameHash = journal["taskFrameHash"] | "";
  }
  const bool taskShapeValid = hasTask_
    ? operation_ == "task" && task_.id.length() && task_.frameUrl.length() && runAt_ > 0
    : operation_ == "page" && runAt_ == 0;
  return validAssetId(assetId_) && validAssetId(previousCurrent_, true) &&
    assetPath_ == String("/inkloop-album/") + assetId_ + ".png" &&
    assetBytes_ > 0 && assetBytes_ <= kMaxFrameBytes && taskShapeValid;
}

bool DisplayTransaction::validateJournalFile(const char* path) {
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available()) return false;
  File file = control.backend->open(path, FILE_READ);
  if (!file) return false;
  JsonDocument journal;
  const DeserializationError error = deserializeJson(journal, file);
  file.close();
  if (error || !journalSchemaValid(journal)) return false;
  const uint8_t encodedStage = journal["stage"] | 0;
  if (encodedStage < static_cast<uint8_t>(DisplayJournalStage::Prepared) ||
      encodedStage > static_cast<uint8_t>(DisplayJournalStage::Aborted)) return false;
  const bool candidateHasTask = journal["hasTask"] | false;
  if (candidateHasTask != hasTask_) return false;
  const bool commonFieldsMatch =
    String(journal["backend"] | "") == String(backend_.identity) &&
    String(journal["assetId"] | "") == assetId_ &&
    String(journal["assetPath"] | "") == assetPath_ &&
    String(journal["previousCurrent"] | "") == previousCurrent_ &&
    String(journal["operation"] | "") == operation_ &&
    static_cast<size_t>(journal["bytes"] | 0U) == assetBytes_ &&
    static_cast<bool>(journal["landscape"] | false) == landscape_ &&
    static_cast<size_t>(journal["page"] | 0U) == page_ &&
    static_cast<uint32_t>(journal["runAt"] | 0U) == static_cast<uint32_t>(runAt_) &&
    static_cast<uint32_t>(journal["runDay"] | 0U) == runDay_;
  if (!commonFieldsMatch) return false;
  return !hasTask_ || (
    String(journal["taskId"] | "") == task_.id &&
    static_cast<uint32_t>(journal["taskRevision"] | 0U) == task_.revision &&
    String(journal["taskFrameUrl"] | "") == task_.frameUrl &&
    String(journal["taskFrameHash"] | "") == task_.frameHash
  );
}

bool DisplayTransaction::recoverAtBoot() {
  reset();
  StorageBackendRef control = storage_.taskBackend();
  if (!control.available()) return false;
  const bool hadCurrent = control.backend->exists(kJournalPath);
  const bool hadNext = control.backend->exists(kJournalNextPath);
  const bool hadPrevious = control.backend->exists(kJournalPreviousPath);
  if (!loadJournal(kJournalPath)) {
    reset();
    if (loadJournal(kJournalNextPath)) {
      journalCanonical_ = false;
      recoveryCandidatePath_ = kJournalNextPath;
      if (!ensureCanonicalJournal()) return false;
    } else {
      reset();
      if (loadJournal(kJournalPreviousPath)) {
        if (hadNext && !control.backend->remove(kJournalNextPath)) return false;
        journalCanonical_ = false;
        recoveryCandidatePath_ = kJournalPreviousPath;
        if (!ensureCanonicalJournal()) return false;
      } else {
        if (hadCurrent || hadNext || hadPrevious) {
          blockOnCorruptJournal(control);
          return false;
        }
        reset();
        return true;
      }
    }
  } else if (hadNext && !control.backend->remove(kJournalNextPath)) {
    // The committed journal remains authoritative. Keep the transaction
    // blocked until the stale candidate can be removed and metadata retried.
    Diagnostics::event("ERROR", "DISPLAY_TXN_STALE_NEXT");
    return false;
  }
  if (stage_ == DisplayJournalStage::Aborted) return clear();
  if (ambiguous()) {
    Diagnostics::event("FATAL", "DISPLAY_TXN_AMBIGUOUS");
    return false;
  }
  Diagnostics::event("DISPLAY_TXN", "RECOVERING_METADATA");
  return retryFinalize();
}

bool DisplayTransaction::resolveAmbiguousAsTarget() {
  if (!ambiguous() || corrupt_) return false;
  stage_ = DisplayJournalStage::Displayed;
  return persist() && retryFinalize();
}

bool DisplayTransaction::resolveAmbiguousAsPrevious() {
  if (!ambiguous()) return false;
  if (corrupt_) return clear();
  stage_ = DisplayJournalStage::Aborted;
  return persist() && clear();
}

}  // namespace inkloop
