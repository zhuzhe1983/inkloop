#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {
namespace storage {

enum class LegacyDisplayRecordSlot : std::uint8_t {
  Current,
  Next,
  Previous,
};

enum class LegacyDisplayJournalStage : std::uint8_t {
  Prepared = 1U,
  Displayed = 2U,
  CurrentCommitted = 3U,
  TaskAcknowledged = 4U,
  Aborted = 5U,
};

struct RawLegacyDisplayRecord {
  bool present = false;
  bool io_error = false;
  std::string bytes;

  void clear();
};

struct LegacyDisplayJournal {
  LegacyDisplayJournalStage stage = LegacyDisplayJournalStage::Prepared;
  std::string backend;
  std::string asset_id;
  std::string asset_path;
  std::string previous_current;
  std::string operation;
  std::uint32_t asset_bytes = 0U;
  bool landscape = false;
  std::uint32_t page = 0U;
  bool has_task = false;
  std::uint32_t run_at = 0U;
  std::uint32_t run_day = 0U;
  std::string task_id;
  std::uint32_t task_revision = 0U;
  std::string task_frame_url;
  std::string task_frame_hash;

  void clear();
};

enum class LegacyDisplayRecoveryProbe : std::uint8_t {
  Empty,
  Recoverable,
  Corrupt,
  IoError,
};

struct LegacyDisplayRecoverySnapshot {
  LegacyDisplayRecoveryProbe probe = LegacyDisplayRecoveryProbe::IoError;
  LegacyDisplayRecordSlot selected_slot = LegacyDisplayRecordSlot::Current;
  LegacyDisplayJournal journal{};
  std::array<bool, 3> present{{false, false, false}};
  std::array<bool, 3> valid{{false, false, false}};

  void clear();
};

// Parses all three released Arduino journal candidates without modifying
// them. Selection order exactly matches the released recovery code:
// current -> next -> previous. A valid current remains authoritative even if
// an interrupted candidate is also present.
LegacyDisplayRecoveryProbe inspectLegacyDisplayRecovery(
    const std::array<RawLegacyDisplayRecord, 3>& records,
    LegacyDisplayRecoverySnapshot& output);

enum class LegacyDisplayResolutionChoice : std::uint8_t {
  Target,
  Previous,
};

struct LegacyDisplayResolutionPlan {
  bool apply_target_current = false;
  bool acknowledge_task = false;
  bool clear_journal_set = false;
  LegacyDisplayJournal journal{};

  void clear();
};

// Creates an explicit mutation plan only. Execution belongs to the sole
// storage owner after local authentication and a second fresh inspection.
bool planLegacyDisplayResolution(
    const LegacyDisplayRecoverySnapshot& snapshot,
    LegacyDisplayResolutionChoice choice,
    LegacyDisplayResolutionPlan& output);

class ILegacyDisplayRecoverySource {
 public:
  virtual ~ILegacyDisplayRecoverySource() = default;
  virtual LegacyDisplayRecoveryProbe inspect(
      LegacyDisplayRecoverySnapshot& output) const = 0;
};

enum class LegacyDisplayResolutionAdapterCode : std::uint8_t {
  Ok,
  Unavailable,
  Conflict,
  IoError,
};

class ILegacyDisplayResolutionAdapter {
 public:
  virtual ~ILegacyDisplayResolutionAdapter() = default;
  virtual LegacyDisplayResolutionAdapterCode applyTargetCurrent(
      const LegacyDisplayJournal& journal) = 0;
  virtual LegacyDisplayResolutionAdapterCode acknowledgeTask(
      const LegacyDisplayJournal& journal) = 0;
  virtual LegacyDisplayResolutionAdapterCode clearJournalSet() = 0;
};

enum class LegacyDisplayResolutionCode : std::uint8_t {
  Ok,
  InvalidArgument,
  SourceChanged,
  SourceUnavailable,
  TargetUnavailable,
  TargetCommitFailed,
  TaskCommitFailed,
  JournalClearFailed,
};

// Re-inspects immediately before mutation and performs the idempotent order
// album current -> task acknowledgement -> journal clear. A failure before
// the final clear leaves the source journal available for an exact retry.
LegacyDisplayResolutionCode executeLegacyDisplayResolution(
    const LegacyDisplayRecoverySnapshot& expected,
    LegacyDisplayResolutionChoice choice,
    const ILegacyDisplayRecoverySource& source,
    ILegacyDisplayResolutionAdapter& adapter);

const char* legacyDisplayRecoveryProbeName(LegacyDisplayRecoveryProbe probe);
const char* legacyDisplayJournalStageName(LegacyDisplayJournalStage stage);
const char* legacyDisplayResolutionCodeName(LegacyDisplayResolutionCode code);

}  // namespace storage
}  // namespace inkloop
