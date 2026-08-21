#pragma once

#include <Arduino.h>

#include "AlbumStore.h"
#include "DisplayTransactionPrimitives.h"
#include "MetadataPrimitives.h"
#include "TaskStore.h"

namespace inkloop {

class DisplayTransaction {
 public:
  DisplayTransaction(
    StorageManager& storage,
    AlbumStore& album,
    TaskStore& tasks
  ) : storage_(storage), album_(album), tasks_(tasks) {}

  bool begin(
    const AlbumAsset& asset,
    const char* operation,
    const DueTask* task,
    time_t runAt,
    uint32_t runDay = 0
  );
  bool confirmDisplayed();
  bool abortBeforeDisplay();
  bool retryFinalize();
  bool recoverAtBoot();
  bool resolveAmbiguousAsTarget();
  bool resolveAmbiguousAsPrevious();
  static bool estimateJournalRecordBytes(
    const DueTask* task,
    time_t runAt,
    uint32_t runDay,
    size_t& bytes
  );

  bool active() const { return active_; }
  bool ambiguous() const { return active_ && (corrupt_ || displayStageIsAmbiguous(stage_)); }
  bool corrupt() const { return corrupt_; }
  DisplayJournalStage stage() const { return stage_; }
  const char* backendIdentity() const { return backend_.identity; }
  const String& assetId() const { return assetId_; }

 private:
  static constexpr const char* kJournalPath = "/display-txn.json";
  static constexpr const char* kJournalNextPath = "/display-txn.next";
  static constexpr const char* kJournalPreviousPath = "/display-txn.prev";

  bool persist();
  bool serializeJournal(DisplayJournalStage stage, String& payload) const;
  bool hasFinalizeHeadroom();
  bool clear();
  bool loadJournal(const char* path);
  bool validateJournalFile(const char* path);
  bool ensureCanonicalJournal();
  void blockOnCorruptJournal(const StorageBackendRef& control);
  void reset();

  StorageManager& storage_;
  AlbumStore& album_;
  TaskStore& tasks_;
  StorageBackendRef backend_;
  String assetId_;
  String assetPath_;
  String previousCurrent_;
  String operation_;
  size_t assetBytes_ = 0;
  bool landscape_ = false;
  size_t page_ = 0;
  bool hasTask_ = false;
  DueTask task_;
  time_t runAt_ = 0;
  uint32_t runDay_ = 0;
  DisplayJournalStage stage_ = DisplayJournalStage::None;
  DisplayJournalStage persistedStage_ = DisplayJournalStage::None;
  bool active_ = false;
  bool corrupt_ = false;
  bool journalCanonical_ = true;
  String recoveryCandidatePath_;

  friend class DisplayFinalizeOperations;
};

}  // namespace inkloop
