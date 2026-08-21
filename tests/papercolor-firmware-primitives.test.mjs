import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const firmwareSource = new URL("../firmware/m5-papercolor/src/", import.meta.url);
const arduinoJsonSource = new URL(
  "../firmware/m5-papercolor/.pio/libdeps/m5stack-papercolor/ArduinoJson/src/",
  import.meta.url,
);

test("PaperColor firmware primitives execute deterministically under C++11", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-papercolor-primitives-"));
  const harnessPath = join(temporaryDirectory, "primitives_test.cpp");
  const executablePath = join(temporaryDirectory, "primitives_test");
  const harness = String.raw`
#include <cassert>
#include <cstring>
	#include <map>
	#include <limits>
	#include <string>
	#include <vector>
	#include <ArduinoJson.h>
#include "FirmwarePrimitives.h"
#include "AlbumPrimitives.h"
#include "DisplayTransactionPrimitives.h"
#include "DisplayFinalizeCore.h"
#include "MetadataPrimitives.h"
#include "CompatibilityPrimitives.h"
#include "BusyButtonPrimitives.h"
	#include "TaskPersistenceCore.h"
	#include "TransactionalIo.h"
	#include "JsonRecordCodec.h"

using namespace inkloop;

bool same(RgbColor left, RgbColor right) {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

class FaultIo final : public ITransactionalIo {
 public:
  bool online = true;
  bool failWrite = false;
  bool failRemove = false;
  std::string failRemovePath;
  int failRenameAt = 0;
  std::string failRenameFrom;
  int removeMediaAfterRenameAt = 0;
  int renameCalls = 0;
  size_t capacity = 1024 * 1024;
  std::map<std::string, std::vector<uint8_t> > files;

  bool available() const override { return online; }
  bool exists(const char* path) override {
    return online && files.find(path) != files.end();
  }
  bool remove(const char* path) override {
    if (!online || failRemove || failRemovePath == path) return false;
    files.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    if (!online) return false;
    ++renameCalls;
    if (failRenameAt == renameCalls || failRenameFrom == from) return false;
    const std::map<std::string, std::vector<uint8_t> >::iterator source = files.find(from);
    if (source == files.end() || files.find(to) != files.end()) return false;
    files[to] = source->second;
    files.erase(source);
    if (removeMediaAfterRenameAt == renameCalls) online = false;
    return true;
  }
  bool writeAll(const char* path, const uint8_t* bytes, size_t length) override {
    if (!online || failWrite || !bytes) return false;
    size_t used = 0;
    for (std::map<std::string, std::vector<uint8_t> >::const_iterator item = files.begin();
         item != files.end(); ++item) {
      if (item->first != path) used += item->second.size();
    }
    if (length > capacity || used > capacity - length) return false;
    files[path] = std::vector<uint8_t>(bytes, bytes + length);
    return true;
  }
  bool contentEquals(const char* path, const uint8_t* bytes, size_t length) override {
    if (!online || !bytes) return false;
    const std::map<std::string, std::vector<uint8_t> >::const_iterator item = files.find(path);
    return item != files.end() && item->second.size() == length &&
      std::memcmp(item->second.data(), bytes, length) == 0;
  }

	  void put(const char* path, const char* value) {
	    put(path, std::string(value));
	  }
	  void put(const char* path, const std::string& value) {
	    const uint8_t* first = reinterpret_cast<const uint8_t*>(value.data());
	    files[path] = std::vector<uint8_t>(first, first + value.size());
	  }
  bool is(const char* path, const char* value) {
    return contentEquals(path, reinterpret_cast<const uint8_t*>(value), std::strlen(value));
  }
	};

	class FaultJsonString {
	 public:
	  bool failReserve = false;
	  size_t appendLimit = std::numeric_limits<size_t>::max();
	  size_t reserved = 0;

	  FaultJsonString& operator=(const char* value) {
	    value_ = value ? value : "";
	    return *this;
	  }
	  bool reserve(size_t capacity) {
	    reserved = capacity;
	    if (failReserve) return false;
	    value_.reserve(capacity);
	    return true;
	  }
	  bool concat(const char* bytes, size_t length) {
	    if (!bytes || value_.size() >= appendLimit) return false;
	    const size_t available = appendLimit - value_.size();
	    const size_t accepted = length < available ? length : available;
	    value_.append(bytes, accepted);
	    return accepted == length;
	  }
	  bool concat(char value) { return concat(&value, 1); }
	  size_t length() const { return value_.size(); }
	  const char* c_str() const { return value_.c_str(); }
	  const std::string& str() const { return value_; }

	 private:
	  std::string value_;
	};

	bool loadJson(const FaultIo& storage, const char* path, JsonDocument& document) {
	  const std::map<std::string, std::vector<uint8_t> >::const_iterator item =
	    storage.files.find(path);
	  if (item == storage.files.end()) return false;
	  return !deserializeJson(document, item->second.data(), item->second.size());
	}

	bool validTaskDocument(const FaultIo& storage, const char* path) {
	  JsonDocument document;
	  return loadJson(storage, path, document) && document.is<JsonArray>();
	}

	bool validJournalDocument(const FaultIo& storage, const char* path) {
	  JsonDocument document;
	  if (!loadJson(storage, path, document) || !document.is<JsonObject>() ||
	      static_cast<uint16_t>(document["schema"] | 0) != 1) return false;
	  const uint8_t stage = document["stage"] | 0;
	  return stage >= static_cast<uint8_t>(DisplayJournalStage::Prepared) &&
	    stage <= static_cast<uint8_t>(DisplayJournalStage::Aborted);
	}

	std::string taskPayload(uint32_t lastRun) {
	  JsonDocument document;
	  JsonObject task = document.to<JsonArray>().add<JsonObject>();
	  task["id"] = "task-1";
	  task["revision"] = 7;
	  task["frameUrl"] = "https://inkloop.invalid/frame.png";
	  task["lastRun"] = lastRun;
	  task["lastDay"] = lastRun ? 20260821 : 0;
	  FaultJsonString payload;
	  assert(serializeJsonRecordExactly(document.as<JsonArrayConst>(), payload));
	  assert(payload.reserved == measureJson(document.as<JsonArrayConst>()));
	  return payload.str();
	}

	std::string journalPayload(DisplayJournalStage stage) {
	  JsonDocument document;
	  document["schema"] = 1;
	  document["stage"] = static_cast<uint8_t>(stage);
	  document["backend"] = "littlefs";
	  document["assetId"] = "0000000000000000000000000000000000000000000000000000000000000000";
	  document["assetPath"] = "/inkloop-album/0000000000000000000000000000000000000000000000000000000000000000.png";
	  document["previousCurrent"] = "";
	  document["operation"] = "task";
	  document["bytes"] = 1024;
	  document["landscape"] = false;
	  document["page"] = 0;
	  document["hasTask"] = true;
	  document["runAt"] = 100;
	  document["runDay"] = 20260821;
	  document["taskId"] = "task-1";
	  document["taskRevision"] = 7;
	  document["taskFrameUrl"] = "https://inkloop.invalid/frame.png";
	  document["taskFrameHash"] = "";
	  FaultJsonString payload;
	  assert(serializeJsonRecordExactly(document, payload));
	  assert(payload.reserved == measureJson(document));
	  return payload.str();
	}

	bool taskIsAcknowledged(const FaultIo& storage) {
	  JsonDocument document;
	  if (!loadJson(storage, "/tasks.json", document) || !document.is<JsonArray>()) return false;
	  JsonObjectConst task = document.as<JsonArrayConst>()[0];
	  return static_cast<uint32_t>(task["lastRun"] | 0U) >= 100;
	}

	bool journalIsStage(const FaultIo& storage, DisplayJournalStage stage) {
	  JsonDocument document;
	  return loadJson(storage, "/display-txn.json", document) &&
	    static_cast<uint8_t>(document["stage"] | 0) == static_cast<uint8_t>(stage);
	}

	class FinalizeOps {
 public:
  explicit FinalizeOps(FaultIo& storage)
    : storage_(storage), tasks_(storage), journals_(storage) {}

	  bool persist(DisplayJournalStage stage) {
	    ++journalPersists;
	    if (failJournalPersist) return false;
	    JsonDocument document;
	    document["schema"] = 1;
	    document["stage"] = static_cast<uint8_t>(stage);
	    document["backend"] = "littlefs";
	    document["assetId"] = "0000000000000000000000000000000000000000000000000000000000000000";
	    document["operation"] = "task";
	    FaultJsonString payload;
	    payload.failReserve = failJournalReserve;
	    payload.appendLimit = journalAppendLimit;
	    if (!serializeJsonRecordExactly(document, payload)) return false;
	    return journals_.commitValidatedRecord(
	      "/display-txn.json", "/display-txn.next", "/display-txn.prev",
	      reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
	      [this](const char* path) { return validJournalDocument(storage_, path); }
	    );
	  }
  bool commitCurrent() {
    ++currentCommits;
    return !failCurrent;
  }
	  bool taskAcknowledged() { return taskIsAcknowledged(storage_); }
	  bool acknowledgeTask() {
	    ++taskAckAttempts;
	    JsonDocument document;
	    if (!loadJson(storage_, "/tasks.json", document) || !document.is<JsonArray>()) return false;
	    JsonObject task = document.as<JsonArray>()[0];
	    task["lastRun"] = 100;
	    task["lastDay"] = 20260821;
	    FaultJsonString payload;
	    payload.failReserve = failTaskReserve;
	    payload.appendLimit = taskAppendLimit;
	    if (!serializeJsonRecordExactly(document.as<JsonArrayConst>(), payload)) return false;
	    return tasks_.commitValidated(
	      reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
	      [this](const char* path) { return validTaskDocument(storage_, path); }
	    );
	  }
  bool clear() {
    ++clears;
    return !failClear && storage_.remove("/display-txn.next") &&
      storage_.remove("/display-txn.prev") && storage_.remove("/display-txn.json");
  }

  int journalPersists = 0;
  int currentCommits = 0;
  int taskAckAttempts = 0;
  int clears = 0;
	  bool failJournalPersist = false;
	  bool failJournalReserve = false;
	  bool failTaskReserve = false;
	  size_t journalAppendLimit = std::numeric_limits<size_t>::max();
	  size_t taskAppendLimit = std::numeric_limits<size_t>::max();
	  bool failCurrent = false;
  bool failClear = false;

 private:
  FaultIo& storage_;
  TaskPersistenceCore tasks_;
  TransactionalFileStore journals_;
};

int main() {
  static_assert(buttonEventForPhysical(PhysicalButton::C) == ButtonEvent::Voice, "C must be voice");
  static_assert(buttonEventForPhysical(PhysicalButton::A) == ButtonEvent::PreviousPage, "A must be previous");
  static_assert(buttonEventForPhysical(PhysicalButton::B) == ButtonEvent::NextPage, "B must be next");

  const LedFrame mirrored = resolveLedFrame(
    false, 0, 2, LedState::Speaking, LedState::Error
  );
  assert(same(mirrored.pixels[0], ledStateColor(LedState::Error)));
  assert(same(mirrored.pixels[0], mirrored.pixels[1]));

  const LedFrame voiceLeft = resolveLedFrame(
    true, 0, 2, LedState::Listening, LedState::Downloading
  );
  assert(same(voiceLeft.pixels[0], ledStateColor(LedState::Listening)));
  assert(same(voiceLeft.pixels[1], ledStateColor(LedState::Downloading)));

  const LedFrame voiceRight = resolveLedFrame(
    true, 1, 2, LedState::Listening, LedState::Downloading
  );
  assert(same(voiceRight.pixels[0], ledStateColor(LedState::Downloading)));
  assert(same(voiceRight.pixels[1], ledStateColor(LedState::Listening)));

  const LedFrame onePixelFallback = resolveLedFrame(
    true, 1, 1, LedState::Thinking, LedState::Writing
  );
  assert(same(onePixelFallback.pixels[0], ledStateColor(LedState::Writing)));
  assert(same(onePixelFallback.pixels[0], onePixelFallback.pixels[1]));

  assert(optionalStorageEligible(StorageSelectionInput(true, true, true, true)));
  assert(!optionalStorageEligible(StorageSelectionInput(false, true, true, true)));
  assert(!optionalStorageEligible(StorageSelectionInput(true, false, true, true)));
  assert(!optionalStorageEligible(StorageSelectionInput(true, true, false, true)));
  assert(!optionalStorageEligible(StorageSelectionInput(true, true, true, false)));

  assert(!PersistenceReadiness(false, false).safeToStartNetwork());
  assert(!PersistenceReadiness(true, false).safeToStartNetwork());
  assert(!PersistenceReadiness(false, true).safeToStartNetwork());
  assert(PersistenceReadiness(true, true).safeToStartNetwork());

  const CacheCapacity roomy(4000000, 400000, 1000000, 3000000, 300000, 1, 2);
  assert(cacheAdmission(roomy, 1000000, false) == CacheAdmission::Accept);
  assert(cacheAdmission(roomy, 1000000, true) == CacheAdmission::Deduplicated);
  assert(cacheAdmission(CacheCapacity(4000000, 400000, 1000000, 3000000, 300000, 2, 2),
    1000000, false) == CacheAdmission::EntryLimit);
  assert(cacheAdmission(CacheCapacity(4000000, 400000, 2500000, 3000000, 300000, 1, 2),
    1000000, false) == CacheAdmission::AlbumLimit);
  assert(cacheAdmission(CacheCapacity(4000000, 3000000, 1000000, 3000000, 300000, 1, 2),
    800000, false) == CacheAdmission::FreeSpace);

  assert(selectIndexRecovery(true, true) == IndexRecoveryAction::UseCurrent);
  assert(selectIndexRecovery(false, true) == IndexRecoveryAction::RestoreBackup);
  assert(selectIndexRecovery(false, false) == IndexRecoveryAction::InitializeEmpty);
  const IndexCommitOutcome committed = resolveIndexCommit(true, true, true);
  assert(committed.success && !committed.restoreBackup && committed.priorIndexStillValid);
  const IndexCommitOutcome restore = resolveIndexCommit(true, true, false);
  assert(!restore.success && restore.restoreBackup && restore.priorIndexStillValid);
  const IndexCommitOutcome backupFailure = resolveIndexCommit(true, false, false);
  assert(!backupFailure.success && !backupFailure.restoreBackup && backupFailure.priorIndexStillValid);
  const IndexCommitOutcome firstFailure = resolveIndexCommit(false, true, false);
  assert(!firstFailure.success && !firstFailure.restoreBackup && firstFailure.priorIndexStillValid);
  assert(!transactionMayChangeCurrent(IndexTransactionStage::AssetPromoted));
  assert(!transactionMayChangeCurrent(IndexTransactionStage::IndexTempWritten));
  assert(transactionMayChangeCurrent(IndexTransactionStage::IndexCommitted));
  assert(transactionLeavesRecoverableIndex(IndexTransactionStage::Failed));

  assert(assetRecordUsable(true, true, true, 123, 123, true));
  assert(!assetRecordUsable(true, true, true, 123, 122, true));
  assert(!assetRecordUsable(true, true, true, 123, 123, false));
  assert(!assetRecordUsable(false, true, true, 123, 123, true));

  const PageSelection previous = selectAdjacentPage(1, 3, -1, false);
  assert(previous.accepted && previous.page == 0);
  const PageSelection next = selectAdjacentPage(1, 3, 1, false);
  assert(next.accepted && next.page == 2);
  assert(!selectAdjacentPage(0, 3, -1, false).accepted);
  assert(!selectAdjacentPage(2, 3, 1, false).accepted);
  assert(!selectAdjacentPage(1, 3, 1, true).accepted);

  const LedFrame imageDownload = resolveLedFrame(true, 0, 2, LedState::Off, LedState::Downloading);
  assert(same(imageDownload.pixels[1], ledStateColor(LedState::Downloading)));
  const LedFrame imageCache = resolveLedFrame(true, 0, 2, LedState::Off, LedState::Caching);
  assert(same(imageCache.pixels[1], ledStateColor(LedState::Caching)));
  const LedFrame imageWrite = resolveLedFrame(true, 0, 2, LedState::Off, LedState::Writing);
  assert(same(imageWrite.pixels[1], ledStateColor(LedState::Writing)));
  const LedFrame imageReady = resolveLedFrame(true, 0, 2, LedState::Off, LedState::Complete);
  assert(same(imageReady.pixels[1], ledStateColor(LedState::Complete)));
  const LedFrame imageError = resolveLedFrame(true, 0, 2, LedState::Off, LedState::Error);
  assert(same(imageError.pixels[1], ledStateColor(LedState::Error)));

  assert(displayStageIsAmbiguous(DisplayJournalStage::Prepared));
  assert(!displayStageMustNotRedraw(DisplayJournalStage::Prepared));
  assert(displayStageMustNotRedraw(DisplayJournalStage::Displayed));
  assert(displayStageMustNotRedraw(DisplayJournalStage::CurrentCommitted));
  assert(displayStageNeedsCurrentCommit(DisplayJournalStage::Displayed));
  assert(displayStageNeedsTaskAck(DisplayJournalStage::CurrentCommitted, true));
  assert(!displayStageNeedsTaskAck(DisplayJournalStage::CurrentCommitted, false));

  const uint8_t frame[] = {1, 2, 3, 4};
  FaultIo blobIo;
  TransactionalFileStore blobStore(blobIo);
  assert(blobStore.promoteBlob("/asset.part", "/asset.png", frame, sizeof(frame)));
  assert(blobIo.contentEquals("/asset.png", frame, sizeof(frame)));
  assert(!blobIo.exists("/asset.part"));
  assert(blobStore.promoteBlob("/asset.part", "/asset.png", frame, sizeof(frame)));
  const uint8_t differentFrame[] = {4, 3, 2, 1};
  assert(!blobStore.promoteBlob("/asset.part", "/asset.png", differentFrame, sizeof(differentFrame)));
  assert(blobIo.contentEquals("/asset.png", frame, sizeof(frame)));

  FaultIo recordIo;
  recordIo.put("/index.json", "old");
  TransactionalFileStore recordStore(recordIo);
  const uint8_t recordBytes[] = {'n', 'e', 'w'};
  assert(recordStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  assert(recordIo.is("/index.json", "new"));
  assert(recordIo.is("/index.prev", "old"));
  assert(!recordIo.exists("/index.next"));

  FaultIo fullIo;
  fullIo.capacity = 3;
  fullIo.put("/index.json", "old");
  TransactionalFileStore fullStore(fullIo);
  assert(!fullStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  assert(fullIo.is("/index.json", "old"));

  FaultIo backupRenameIo;
  backupRenameIo.put("/index.json", "old");
  backupRenameIo.failRenameAt = 1;
  TransactionalFileStore backupRenameStore(backupRenameIo);
  assert(!backupRenameStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  assert(backupRenameIo.is("/index.json", "old"));
  assert(!backupRenameIo.exists("/index.next"));

  FaultIo promotionIo;
  promotionIo.put("/index.json", "old");
  promotionIo.failRenameAt = 2;
  TransactionalFileStore promotionStore(promotionIo);
  assert(!promotionStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  assert(promotionIo.is("/index.json", "old"));
  assert(!promotionIo.exists("/index.next"));

  FaultIo removalIo;
  removalIo.put("/index.json", "old");
  removalIo.put("/index.prev", "older");
  removalIo.failRemovePath = "/index.prev";
  TransactionalFileStore removalStore(removalIo);
  assert(!removalStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  assert(removalIo.is("/index.json", "old"));
  assert(removalIo.is("/index.prev", "older"));
  assert(!removalIo.exists("/index.next"));

  FaultIo hotRemovalIo;
  hotRemovalIo.put("/index.json", "old");
  hotRemovalIo.removeMediaAfterRenameAt = 1;
  TransactionalFileStore hotRemovalStore(hotRemovalIo);
  assert(!hotRemovalStore.commitRecord("/index.json", "/index.next", "/index.prev", recordBytes, sizeof(recordBytes)));
  hotRemovalIo.online = true;
  assert(hotRemovalIo.is("/index.prev", "old"));
  assert(hotRemovalIo.is("/index.next", "new"));
  assert(hotRemovalIo.rename("/index.next", "/index.json"));
  assert(hotRemovalIo.is("/index.json", "new"));

  FaultIo displayedIo;
  displayedIo.put("/display-txn.json", "prepared");
  TransactionalFileStore displayedStore(displayedIo);
  const uint8_t displayed[] = {'d', 'i', 's', 'p', 'l', 'a', 'y', 'e', 'd'};
  displayedIo.failWrite = true;
  assert(!displayedStore.commitRecord(
    "/display-txn.json", "/display-txn.next", "/display-txn.prev", displayed, sizeof(displayed)));
  displayedIo.failWrite = false;
  assert(displayedIo.is("/display-txn.json", "prepared"));
  assert(displayedStore.commitRecord(
    "/display-txn.json", "/display-txn.next", "/display-txn.prev", displayed, sizeof(displayed)));
  assert(displayedIo.is("/display-txn.json", "displayed"));

	  FaultIo taskIo;
	  taskIo.put("/tasks.json", taskPayload(0));
	  taskIo.put("/display-txn.json", journalPayload(DisplayJournalStage::Displayed));
	  taskIo.failRenameFrom = "/tasks.next";
  FinalizeOps finalizeOps(taskIo);
  DisplayJournalStage finalizeStage = DisplayJournalStage::Displayed;
  DisplayJournalStage finalizedOnDisk = DisplayJournalStage::Displayed;
  assert(finalizeDisplayMetadata(finalizeStage, finalizedOnDisk, true, finalizeOps) ==
    FinalizeResult::Retry);
  assert(finalizeStage == DisplayJournalStage::CurrentCommitted);
  assert(finalizedOnDisk == DisplayJournalStage::CurrentCommitted);
  assert(finalizeOps.currentCommits == 1);
  assert(finalizeOps.taskAckAttempts == 1);
	  assert(!taskIsAcknowledged(taskIo));
	  assert(journalIsStage(taskIo, DisplayJournalStage::CurrentCommitted));
  taskIo.failRenameFrom = "";
  assert(finalizeDisplayMetadata(finalizeStage, finalizedOnDisk, true, finalizeOps) ==
    FinalizeResult::Complete);
  assert(finalizeStage == DisplayJournalStage::TaskAcknowledged);
  assert(finalizeOps.currentCommits == 1);
  assert(finalizeOps.taskAckAttempts == 2);
	  assert(taskIsAcknowledged(taskIo));
	  assert(!taskIo.exists("/display-txn.json"));

	  FaultIo powerLossIo;
	  powerLossIo.put("/tasks.prev", taskPayload(0));
	  powerLossIo.put("/tasks.next", taskPayload(100));
	  TaskPersistenceCore powerLossTasks(powerLossIo);
	  const RecordRecovery recoveredNext = powerLossTasks.recover(
	    [&](const char* path) {
	      return validTaskDocument(powerLossIo, path);
	    }
	  );
	  assert(recoveredNext == RecordRecovery::PromoteNext);
	  assert(taskIsAcknowledged(powerLossIo));
  FinalizeOps recoveredFinalize(powerLossIo);
  DisplayJournalStage recoveredStage = DisplayJournalStage::CurrentCommitted;
  DisplayJournalStage recoveredPersisted = DisplayJournalStage::CurrentCommitted;
  assert(finalizeDisplayMetadata(
    recoveredStage, recoveredPersisted, true, recoveredFinalize
  ) == FinalizeResult::Complete);
  assert(recoveredFinalize.taskAckAttempts == 0);

	  FaultIo backupRecoveryIo;
	  backupRecoveryIo.put("/tasks.next", "corrupt");
	  backupRecoveryIo.put("/tasks.prev", taskPayload(0));
  TaskPersistenceCore backupRecoveryTasks(backupRecoveryIo);
  const RecordRecovery recoveredPrevious = backupRecoveryTasks.recover(
    [&](const char* path) {
	      return validTaskDocument(backupRecoveryIo, path);
    }
  );
  assert(recoveredPrevious == RecordRecovery::RestorePrevious);
	  assert(validTaskDocument(backupRecoveryIo, "/tasks.json"));
	  assert(!taskIsAcknowledged(backupRecoveryIo));
  assert(!backupRecoveryIo.exists("/tasks.next"));

	  FaultIo canonicalTaskIo;
	  canonicalTaskIo.put("/tasks.json", taskPayload(0));
	  canonicalTaskIo.put("/tasks.next", taskPayload(100));
  TaskPersistenceCore canonicalTasks(canonicalTaskIo);
  assert(canonicalTasks.recover([&](const char* path) {
	    return validTaskDocument(canonicalTaskIo, path);
	  }) == RecordRecovery::UseCurrent);
	  assert(!taskIsAcknowledged(canonicalTaskIo));
	  assert(!canonicalTaskIo.exists("/tasks.next"));

	  // Actual production JSON serialization must fail closed when reserve or
	  // append fails. The active journal stays durable and the finalizer retries
	  // without another current-image commit.
	  FaultIo serializationIo;
	  serializationIo.put("/tasks.json", taskPayload(0));
	  serializationIo.put(
	    "/display-txn.json", journalPayload(DisplayJournalStage::CurrentCommitted)
	  );
	  FinalizeOps serializationOps(serializationIo);
	  DisplayJournalStage serializationStage = DisplayJournalStage::CurrentCommitted;
	  DisplayJournalStage serializationPersisted = DisplayJournalStage::CurrentCommitted;
	  serializationOps.failTaskReserve = true;
	  assert(finalizeDisplayMetadata(
	    serializationStage, serializationPersisted, true, serializationOps
	  ) == FinalizeResult::Retry);
	  assert(!taskIsAcknowledged(serializationIo));
	  assert(journalIsStage(serializationIo, DisplayJournalStage::CurrentCommitted));
	  assert(serializationOps.clears == 0);
	  serializationOps.failTaskReserve = false;
	  serializationOps.taskAppendLimit = 9;
	  assert(finalizeDisplayMetadata(
	    serializationStage, serializationPersisted, true, serializationOps
	  ) == FinalizeResult::Retry);
	  assert(!taskIsAcknowledged(serializationIo));
	  assert(journalIsStage(serializationIo, DisplayJournalStage::CurrentCommitted));
	  assert(serializationOps.clears == 0);
	  serializationOps.taskAppendLimit = std::numeric_limits<size_t>::max();
	  assert(finalizeDisplayMetadata(
	    serializationStage, serializationPersisted, true, serializationOps
	  ) == FinalizeResult::Complete);
	  assert(taskIsAcknowledged(serializationIo));
	  assert(!serializationIo.exists("/display-txn.json"));

	  FaultIo journalSerializationIo;
	  journalSerializationIo.put("/tasks.json", taskPayload(0));
	  journalSerializationIo.put(
	    "/display-txn.json", journalPayload(DisplayJournalStage::Displayed)
	  );
	  FinalizeOps journalSerializationOps(journalSerializationIo);
	  DisplayJournalStage journalSerializationStage = DisplayJournalStage::Displayed;
	  DisplayJournalStage journalSerializationPersisted = DisplayJournalStage::Displayed;
	  journalSerializationOps.failJournalReserve = true;
	  assert(finalizeDisplayMetadata(
	    journalSerializationStage, journalSerializationPersisted, true, journalSerializationOps
	  ) == FinalizeResult::Retry);
	  assert(journalSerializationStage == DisplayJournalStage::CurrentCommitted);
	  assert(journalSerializationPersisted == DisplayJournalStage::Displayed);
	  assert(journalIsStage(journalSerializationIo, DisplayJournalStage::Displayed));
	  assert(journalSerializationOps.currentCommits == 1);
	  assert(journalSerializationOps.clears == 0);
	  journalSerializationOps.failJournalReserve = false;
	  assert(finalizeDisplayMetadata(
	    journalSerializationStage, journalSerializationPersisted, true, journalSerializationOps
	  ) == FinalizeResult::Complete);
	  assert(journalSerializationOps.currentCommits == 1);
	  assert(taskIsAcknowledged(journalSerializationIo));
	  assert(!journalSerializationIo.exists("/display-txn.json"));

  FaultIo emptyTaskIo;
  TaskPersistenceCore emptyTasks(emptyTaskIo);
  assert(emptyTasks.recover([&](const char*) { return false; }) == RecordRecovery::Empty);

  int directDisplays = 0;
  int directAcknowledgements = 0;
  const DirectDisplayResult direct = runAlbumDisabledDirectPath(
    [&]() { ++directDisplays; return true; },
    [&]() { ++directAcknowledgements; return true; }
  );
  assert(direct.displayed && direct.acknowledged);
  assert(directDisplays == 1 && directAcknowledgements == 1);
  const DirectDisplayResult failedDirect = runAlbumDisabledDirectPath(
    [&]() { ++directDisplays; return false; },
    [&]() { ++directAcknowledgements; return true; }
  );
  assert(!failedDirect.displayed && !failedDirect.acknowledged);
  assert(directDisplays == 2 && directAcknowledgements == 1);

  size_t transactionBytes = 0;
  assert(metadataTransactionBytes(MetadataBudget(1000, 200), 300, true, transactionBytes));
  assert(transactionBytes == 1900);
  assert(metadataTransactionBytes(MetadataBudget(1000, 200), 300, false, transactionBytes));
  assert(transactionBytes == 300);
  assert(controlTransactionBytes(MetadataBudget(1000, 200), transactionBytes));
  assert(transactionBytes == 1600);
  assert(storageCanPreserveReserve(10000, 1000, 2000, 3000, 4000));
  assert(!storageCanPreserveReserve(9999, 1000, 2000, 3000, 4000));
  assert(!metadataTransactionBytes(
    MetadataBudget(static_cast<size_t>(-1), 1), 1, true, transactionBytes
  ));

  uint8_t stableButton = 1;
  uint8_t candidateButton = 1;
  uint8_t candidateSamples = 0;
  assert(!updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 0, true
  ));
  assert(updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 0, true
  ));
  assert(!updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 1, true
  ));
  assert(!updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 1, true
  ));
  assert(!updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 0, false
  ));
  assert(!updateDebouncedActiveLowButton(
    stableButton, candidateButton, candidateSamples, 0, false
  ));
  return 0;
}
`;

  try {
    await writeFile(harnessPath, harness);
    const compile = spawnSync(process.env.CXX || "c++", [
      "-std=c++11",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-I",
      decodeURIComponent(firmwareSource.pathname),
	  "-I",
	  decodeURIComponent(arduinoJsonSource.pathname),
      harnessPath,
      "-o",
      executablePath,
    ], { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr || compile.stdout);

    const run = spawnSync(executablePath, [], { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr || run.stdout);
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
});
