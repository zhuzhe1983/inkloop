#include "inkloop/storage/legacy_display_recovery.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "cJSON.h"

namespace inkloop {
namespace storage {
namespace {

constexpr std::size_t kMaximumJournalBytes = 16U * 1024U;
constexpr std::uint32_t kMaximumAssetBytes = 6U * 1024U * 1024U;

bool boundedString(const cJSON* object, const char* name,
                   std::size_t minimum, std::size_t maximum,
                   std::string& output) {
  output.clear();
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(value) || !value->valuestring) return false;
  output.assign(value->valuestring);
  return output.size() >= minimum && output.size() <= maximum &&
      std::strlen(value->valuestring) == output.size();
}

bool unsigned32(const cJSON* object, const char* name,
                std::uint32_t& output) {
  output = 0U;
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
      value->valuedouble > 4294967295.0) {
    return false;
  }
  const auto converted = static_cast<std::uint32_t>(value->valuedouble);
  if (static_cast<double>(converted) != value->valuedouble) return false;
  output = converted;
  return true;
}

bool boolean(const cJSON* object, const char* name, bool& output) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsBool(value)) return false;
  output = cJSON_IsTrue(value);
  return true;
}

bool hexId(const std::string& value, bool allow_empty) {
  if (allow_empty && value.empty()) return true;
  if (value.size() != 64U) return false;
  return std::all_of(value.begin(), value.end(), [](char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');
  });
}

bool parseJournal(const std::string& input, LegacyDisplayJournal& output) {
  output.clear();
  if (input.empty() || input.size() > kMaximumJournalBytes) return false;
  cJSON* root = cJSON_ParseWithLength(input.data(), input.size());
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return false;
  }
  std::uint32_t schema = 0U;
  std::uint32_t stage = 0U;
  bool valid = unsigned32(root, "schema", schema) && schema == 1U &&
      unsigned32(root, "stage", stage) && stage >= 1U && stage <= 5U &&
      boundedString(root, "backend", 2U, 16U, output.backend) &&
      (output.backend == "littlefs" || output.backend == "sd") &&
      boundedString(root, "assetId", 64U, 64U, output.asset_id) &&
      boundedString(root, "assetPath", 1U, 160U, output.asset_path) &&
      boundedString(root, "previousCurrent", 0U, 64U,
                    output.previous_current) &&
      boundedString(root, "operation", 4U, 4U, output.operation) &&
      unsigned32(root, "bytes", output.asset_bytes) &&
      output.asset_bytes > 0U && output.asset_bytes <= kMaximumAssetBytes &&
      boolean(root, "landscape", output.landscape) &&
      unsigned32(root, "page", output.page) &&
      boolean(root, "hasTask", output.has_task) &&
      unsigned32(root, "runAt", output.run_at) &&
      unsigned32(root, "runDay", output.run_day);
  output.stage = static_cast<LegacyDisplayJournalStage>(stage);
  if (valid && output.has_task) {
    valid = output.operation == "task" && output.run_at > 0U &&
        boundedString(root, "taskId", 1U, 100U, output.task_id) &&
        unsigned32(root, "taskRevision", output.task_revision) &&
        boundedString(root, "taskFrameUrl", 8U, 1024U,
                      output.task_frame_url) &&
        boundedString(root, "taskFrameHash", 64U, 64U,
                      output.task_frame_hash) &&
        hexId(output.task_frame_hash, false);
  } else if (valid) {
    valid = output.operation == "page" && output.run_at == 0U;
  }
  valid = valid && hexId(output.asset_id, false) &&
      hexId(output.previous_current, true) &&
      output.asset_path == "/inkloop-album/" + output.asset_id + ".png";
  cJSON_Delete(root);
  if (!valid) output.clear();
  return valid;
}

}  // namespace

void RawLegacyDisplayRecord::clear() {
  std::fill(bytes.begin(), bytes.end(), '\0');
  *this = RawLegacyDisplayRecord{};
}

void LegacyDisplayJournal::clear() {
  std::string* fields[] = {
      &backend, &asset_id, &asset_path, &previous_current, &operation,
      &task_id, &task_frame_url, &task_frame_hash};
  for (std::string* field : fields)
    std::fill(field->begin(), field->end(), '\0');
  *this = LegacyDisplayJournal{};
}

void LegacyDisplayRecoverySnapshot::clear() {
  journal.clear();
  *this = LegacyDisplayRecoverySnapshot{};
}

LegacyDisplayRecoveryProbe inspectLegacyDisplayRecovery(
    const std::array<RawLegacyDisplayRecord, 3>& records,
    LegacyDisplayRecoverySnapshot& output) {
  output.clear();
  std::array<LegacyDisplayJournal, 3> decoded{};
  bool any = false;
  for (std::size_t at = 0U; at < records.size(); ++at) {
    output.present[at] = records[at].present;
    if (records[at].io_error) {
      for (LegacyDisplayJournal& journal : decoded) journal.clear();
      output.probe = LegacyDisplayRecoveryProbe::IoError;
      return output.probe;
    }
    any = any || records[at].present;
    output.valid[at] = records[at].present &&
        parseJournal(records[at].bytes, decoded[at]);
  }
  if (!any) {
    output.probe = LegacyDisplayRecoveryProbe::Empty;
    return output.probe;
  }
  for (std::size_t at = 0U; at < decoded.size(); ++at) {
    if (!output.valid[at]) continue;
    output.probe = LegacyDisplayRecoveryProbe::Recoverable;
    output.selected_slot = static_cast<LegacyDisplayRecordSlot>(at);
    output.journal = decoded[at];
    for (LegacyDisplayJournal& journal : decoded) journal.clear();
    return output.probe;
  }
  for (LegacyDisplayJournal& journal : decoded) journal.clear();
  output.probe = LegacyDisplayRecoveryProbe::Corrupt;
  return output.probe;
}

void LegacyDisplayResolutionPlan::clear() {
  journal.clear();
  *this = LegacyDisplayResolutionPlan{};
}

bool planLegacyDisplayResolution(
    const LegacyDisplayRecoverySnapshot& snapshot,
    LegacyDisplayResolutionChoice choice,
    LegacyDisplayResolutionPlan& output) {
  output.clear();
  if (snapshot.probe == LegacyDisplayRecoveryProbe::Empty ||
      snapshot.probe == LegacyDisplayRecoveryProbe::IoError) {
    return false;
  }
  if (choice == LegacyDisplayResolutionChoice::Previous) {
    output.clear_journal_set = true;
    return true;
  }
  if (snapshot.probe != LegacyDisplayRecoveryProbe::Recoverable ||
      snapshot.journal.stage == LegacyDisplayJournalStage::Aborted) {
    return false;
  }
  output.apply_target_current = true;
  output.acknowledge_task = snapshot.journal.has_task;
  output.clear_journal_set = true;
  output.journal = snapshot.journal;
  return true;
}

namespace {

bool journalEqual(const LegacyDisplayJournal& left,
                  const LegacyDisplayJournal& right) {
  return left.stage == right.stage && left.backend == right.backend &&
      left.asset_id == right.asset_id && left.asset_path == right.asset_path &&
      left.previous_current == right.previous_current &&
      left.operation == right.operation &&
      left.asset_bytes == right.asset_bytes &&
      left.landscape == right.landscape && left.page == right.page &&
      left.has_task == right.has_task && left.run_at == right.run_at &&
      left.run_day == right.run_day && left.task_id == right.task_id &&
      left.task_revision == right.task_revision &&
      left.task_frame_url == right.task_frame_url &&
      left.task_frame_hash == right.task_frame_hash;
}

bool snapshotEqual(const LegacyDisplayRecoverySnapshot& left,
                   const LegacyDisplayRecoverySnapshot& right) {
  return left.probe == right.probe &&
      left.selected_slot == right.selected_slot &&
      left.present == right.present && left.valid == right.valid &&
      journalEqual(left.journal, right.journal);
}

LegacyDisplayResolutionCode mapTarget(
    LegacyDisplayResolutionAdapterCode code,
    LegacyDisplayResolutionCode failure) {
  if (code == LegacyDisplayResolutionAdapterCode::Ok)
    return LegacyDisplayResolutionCode::Ok;
  if (code == LegacyDisplayResolutionAdapterCode::Unavailable)
    return LegacyDisplayResolutionCode::TargetUnavailable;
  return failure;
}

}  // namespace

LegacyDisplayResolutionCode executeLegacyDisplayResolution(
    const LegacyDisplayRecoverySnapshot& expected,
    LegacyDisplayResolutionChoice choice,
    const ILegacyDisplayRecoverySource& source,
    ILegacyDisplayResolutionAdapter& adapter) {
  LegacyDisplayResolutionPlan expected_plan;
  if (!planLegacyDisplayResolution(expected, choice, expected_plan))
    return LegacyDisplayResolutionCode::InvalidArgument;
  LegacyDisplayRecoverySnapshot fresh;
  const LegacyDisplayRecoveryProbe probe = source.inspect(fresh);
  if (probe == LegacyDisplayRecoveryProbe::IoError)
    return LegacyDisplayResolutionCode::SourceUnavailable;
  if (!snapshotEqual(expected, fresh))
    return LegacyDisplayResolutionCode::SourceChanged;
  LegacyDisplayResolutionPlan plan;
  if (!planLegacyDisplayResolution(fresh, choice, plan))
    return LegacyDisplayResolutionCode::SourceChanged;
  if (plan.apply_target_current) {
    LegacyDisplayResolutionCode result = mapTarget(
        adapter.applyTargetCurrent(plan.journal),
        LegacyDisplayResolutionCode::TargetCommitFailed);
    if (result != LegacyDisplayResolutionCode::Ok) return result;
  }
  if (plan.acknowledge_task) {
    LegacyDisplayResolutionCode result = mapTarget(
        adapter.acknowledgeTask(plan.journal),
        LegacyDisplayResolutionCode::TaskCommitFailed);
    if (result != LegacyDisplayResolutionCode::Ok) return result;
  }
  return adapter.clearJournalSet() == LegacyDisplayResolutionAdapterCode::Ok
      ? LegacyDisplayResolutionCode::Ok
      : LegacyDisplayResolutionCode::JournalClearFailed;
}

const char* legacyDisplayRecoveryProbeName(LegacyDisplayRecoveryProbe probe) {
  switch (probe) {
    case LegacyDisplayRecoveryProbe::Empty: return "EMPTY";
    case LegacyDisplayRecoveryProbe::Recoverable: return "RECOVERABLE";
    case LegacyDisplayRecoveryProbe::Corrupt: return "CORRUPT";
    case LegacyDisplayRecoveryProbe::IoError: return "IO_ERROR";
  }
  return "UNKNOWN";
}

const char* legacyDisplayJournalStageName(LegacyDisplayJournalStage stage) {
  switch (stage) {
    case LegacyDisplayJournalStage::Prepared: return "PREPARED";
    case LegacyDisplayJournalStage::Displayed: return "DISPLAYED";
    case LegacyDisplayJournalStage::CurrentCommitted:
      return "CURRENT_COMMITTED";
    case LegacyDisplayJournalStage::TaskAcknowledged:
      return "TASK_ACKNOWLEDGED";
    case LegacyDisplayJournalStage::Aborted: return "ABORTED";
  }
  return "UNKNOWN";
}

const char* legacyDisplayResolutionCodeName(
    LegacyDisplayResolutionCode code) {
  switch (code) {
    case LegacyDisplayResolutionCode::Ok: return "OK";
    case LegacyDisplayResolutionCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case LegacyDisplayResolutionCode::SourceChanged: return "SOURCE_CHANGED";
    case LegacyDisplayResolutionCode::SourceUnavailable:
      return "SOURCE_UNAVAILABLE";
    case LegacyDisplayResolutionCode::TargetUnavailable:
      return "TARGET_UNAVAILABLE";
    case LegacyDisplayResolutionCode::TargetCommitFailed:
      return "TARGET_COMMIT_FAILED";
    case LegacyDisplayResolutionCode::TaskCommitFailed:
      return "TASK_COMMIT_FAILED";
    case LegacyDisplayResolutionCode::JournalClearFailed:
      return "JOURNAL_CLEAR_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
