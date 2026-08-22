#include "inkloop/storage/legacy_file_transaction_recovery.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>
#include <vector>

#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/posix_task_store.hpp"
#include "inkloop/storage/sha256.hpp"

namespace inkloop {
namespace storage {
namespace {

constexpr std::uint64_t kMaximumTaskTransactionBytes = 256U * 1024U;

bool validRoot(const std::string& root, std::size_t maximum) {
  return !root.empty() && root.size() <= maximum && root.front() == '/' &&
      root.back() != '/' && root.find("..") == std::string::npos &&
      root.find('\0') == std::string::npos;
}

bool isValid(const LegacyFileCandidateSummary& candidate) {
  return candidate.probe == LegacyFileCandidateProbe::Valid &&
      candidate.digest_present;
}

bool isPresent(const LegacyFileCandidateSummary& candidate) {
  return candidate.probe != LegacyFileCandidateProbe::Missing;
}

LegacyFileTransactionResolveCode notify(
    ILegacyFileRecoveryCutObserver* observer,
    LegacyFileRecoveryCutOperation operation,
    LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot source,
    LegacyFileTransactionSlot destination) {
  if (!observer) return LegacyFileTransactionResolveCode::Ok;
  const LegacyFileRecoveryCutPoint point{
      operation, target, source, destination};
  return observer->continueAfter(point)
      ? LegacyFileTransactionResolveCode::Ok
      : LegacyFileTransactionResolveCode::PowerCutSimulated;
}

}  // namespace

std::size_t legacyFileTransactionSlotIndex(LegacyFileTransactionSlot slot) {
  switch (slot) {
    case LegacyFileTransactionSlot::Current: return 0U;
    case LegacyFileTransactionSlot::Next: return 1U;
    case LegacyFileTransactionSlot::Previous: return 2U;
  }
  return 3U;
}

bool legacyFileTransactionTargetEqual(LegacyFileTransactionTarget left,
                                      LegacyFileTransactionTarget right) {
  return left.domain == right.domain && left.backend == right.backend;
}

bool legacyFileCandidateEqual(const LegacyFileCandidateSummary& left,
                              const LegacyFileCandidateSummary& right) {
  return left.probe == right.probe && left.byte_count == right.byte_count &&
      left.digest_present == right.digest_present &&
      left.sha256 == right.sha256;
}

bool legacyFileTransactionSnapshotEqual(
    const LegacyFileTransactionSnapshot& left,
    const LegacyFileTransactionSnapshot& right) {
  if (!legacyFileTransactionTargetEqual(left.target, right.target) ||
      left.probe != right.probe ||
      left.valid_candidates != right.valid_candidates) {
    return false;
  }
  for (std::size_t at = 0U; at < left.candidates.size(); ++at) {
    if (!legacyFileCandidateEqual(left.candidates[at],
                                  right.candidates[at])) {
      return false;
    }
  }
  return true;
}

PosixLegacyFileTransactionRecovery::PosixLegacyFileTransactionRecovery(
    LegacyFileTransactionRecoveryConfig config)
    : config_(std::move(config)) {
  task_root_valid_ = validRoot(config_.task_root, 96U);
  internal_root_valid_ = validRoot(config_.internal_root, 160U);
  removable_root_valid_ = validRoot(config_.removable_root, 160U) &&
      config_.removable_root != config_.internal_root &&
      config_.removable_root != config_.task_root;
}

bool PosixLegacyFileTransactionRecovery::targetPaths(
    LegacyFileTransactionTarget target, Paths& output) const {
  output = Paths{};
  if (target.domain == LegacyFileTransactionDomain::Tasks) {
    if (target.backend != LegacyFileTransactionBackend::TaskRoot ||
        !task_root_valid_)
      return false;
    output.directory = config_.task_root;
    output.slots = {{config_.task_root + "/tasks.json",
                     config_.task_root + "/tasks.next",
                     config_.task_root + "/tasks.prev"}};
    return true;
  }
  if (target.domain != LegacyFileTransactionDomain::Album ||
      target.backend == LegacyFileTransactionBackend::TaskRoot) {
    return false;
  }
  if ((target.backend == LegacyFileTransactionBackend::Internal &&
       !internal_root_valid_) ||
      (target.backend == LegacyFileTransactionBackend::Removable &&
       !removable_root_valid_)) {
    return false;
  }
  const std::string& root =
      target.backend == LegacyFileTransactionBackend::Internal
          ? config_.internal_root : config_.removable_root;
  output.directory = root + "/inkloop-album";
  output.slots = {{output.directory + "/index.json",
                   output.directory + "/index.next",
                   output.directory + "/index.prev"}};
  return true;
}

LegacyFileCandidateSummary
PosixLegacyFileTransactionRecovery::readCandidate(
    LegacyFileTransactionTarget target, const std::string& path) const {
  LegacyFileCandidateSummary output;
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    output.probe = errno == ENOENT ? LegacyFileCandidateProbe::Missing
                                   : LegacyFileCandidateProbe::IoError;
    return output;
  }
  const std::uint64_t maximum =
      target.domain == LegacyFileTransactionDomain::Tasks
          ? kMaximumTaskTransactionBytes : kMaximumAlbumIndexBytes;
  if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum) {
    output.probe = LegacyFileCandidateProbe::Invalid;
    if (status.st_size > 0)
      output.byte_count = static_cast<std::uint64_t>(status.st_size);
    return output;
  }
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    output.probe = LegacyFileCandidateProbe::IoError;
    return output;
  }
  struct stat opened_status {};
  if (::fstat(descriptor, &opened_status) != 0) {
    (void)::close(descriptor);
    output.probe = LegacyFileCandidateProbe::IoError;
    return output;
  }
  if (!S_ISREG(opened_status.st_mode) || opened_status.st_size <= 0 ||
      static_cast<std::uint64_t>(opened_status.st_size) > maximum) {
    const bool closed = ::close(descriptor) == 0;
    output.probe = closed ? LegacyFileCandidateProbe::Invalid
                          : LegacyFileCandidateProbe::IoError;
    if (opened_status.st_size > 0)
      output.byte_count =
          static_cast<std::uint64_t>(opened_status.st_size);
    return output;
  }
  std::string bytes(static_cast<std::size_t>(opened_status.st_size), '\0');
  Sha256 hash;
  std::size_t at = 0U;
  bool readable = true;
  while (at < bytes.size()) {
    const ssize_t count =
        ::read(descriptor, bytes.data() + at, bytes.size() - at);
    if (count <= 0 ||
        static_cast<std::size_t>(count) > bytes.size() - at) {
      readable = false;
      break;
    }
    if (!hash.update(
            reinterpret_cast<const std::uint8_t*>(bytes.data() + at),
            static_cast<std::size_t>(count))) {
      readable = false;
      break;
    }
    at += static_cast<std::size_t>(count);
  }
  if (::close(descriptor) != 0) readable = false;
  if (!readable || at != bytes.size() || !hash.finish(output.sha256)) {
    output = LegacyFileCandidateSummary{};
    output.probe = LegacyFileCandidateProbe::IoError;
    return output;
  }
  output.byte_count = bytes.size();
  output.digest_present = true;
  bool parsed = false;
  if (target.domain == LegacyFileTransactionDomain::Tasks) {
    std::vector<InkloopTaskRecord> tasks;
    parsed = PosixTaskStore::decodeManifest(bytes, tasks);
  } else {
    AlbumIndex index;
    parsed = parseAlbumIndex(bytes, index) == AlbumIndexCode::Ok;
  }
  std::fill(bytes.begin(), bytes.end(), '\0');
  output.probe = parsed ? LegacyFileCandidateProbe::Valid
                        : LegacyFileCandidateProbe::Invalid;
  return output;
}

LegacyFileTransactionProbe PosixLegacyFileTransactionRecovery::inspect(
    LegacyFileTransactionTarget target,
    LegacyFileTransactionSnapshot& output) const {
  output = LegacyFileTransactionSnapshot{};
  output.target = target;
  Paths paths;
  if (!targetPaths(target, paths)) {
    output.probe = LegacyFileTransactionProbe::InvalidTarget;
    return output.probe;
  }
  bool any = false;
  bool io_error = false;
  std::uint8_t valid = 0U;
  for (std::size_t at = 0U; at < output.candidates.size(); ++at) {
    output.candidates[at] = readCandidate(target, paths.slots[at]);
    any = any || isPresent(output.candidates[at]);
    io_error = io_error ||
        output.candidates[at].probe == LegacyFileCandidateProbe::IoError;
    valid += static_cast<std::uint8_t>(isValid(output.candidates[at]));
  }
  output.valid_candidates = valid;
  if (io_error) output.probe = LegacyFileTransactionProbe::IoError;
  else if (!any) output.probe = LegacyFileTransactionProbe::Empty;
  else if (valid > 1U)
    output.probe = LegacyFileTransactionProbe::ChoiceRequired;
  else if (valid == 1U)
    output.probe = LegacyFileTransactionProbe::Recoverable;
  else
    output.probe = LegacyFileTransactionProbe::Corrupt;
  return output.probe;
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::syncFile(
    const Paths& paths, LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot slot,
    ILegacyFileRecoveryCutObserver* observer) const {
  const std::size_t at = legacyFileTransactionSlotIndex(slot);
  if (at >= paths.slots.size())
    return LegacyFileTransactionResolveCode::InvalidArgument;
  const int descriptor = ::open(paths.slots[at].c_str(), O_RDONLY);
  if (descriptor < 0) return LegacyFileTransactionResolveCode::IoError;
  const bool synced = ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  if (!synced || !closed) return LegacyFileTransactionResolveCode::IoError;
  return notify(observer, LegacyFileRecoveryCutOperation::FileFsync,
                target, slot, slot);
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::syncDirectory(
    const Paths& paths, LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot related,
    ILegacyFileRecoveryCutObserver* observer) const {
  const int descriptor = ::open(paths.directory.c_str(), O_RDONLY);
  if (descriptor < 0) return LegacyFileTransactionResolveCode::IoError;
  const bool synced = ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  if (!synced || !closed) return LegacyFileTransactionResolveCode::IoError;
  return notify(observer, LegacyFileRecoveryCutOperation::DirectoryFsync,
                target, related, related);
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::unlinkSlot(
    const Paths& paths, LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot slot,
    ILegacyFileRecoveryCutObserver* observer) const {
  const std::size_t at = legacyFileTransactionSlotIndex(slot);
  if (at >= paths.slots.size())
    return LegacyFileTransactionResolveCode::InvalidArgument;
  if (::unlink(paths.slots[at].c_str()) != 0) {
    if (errno == ENOENT) return LegacyFileTransactionResolveCode::Ok;
    return LegacyFileTransactionResolveCode::IoError;
  }
  LegacyFileTransactionResolveCode result = notify(
      observer, LegacyFileRecoveryCutOperation::Unlink,
      target, slot, slot);
  if (result != LegacyFileTransactionResolveCode::Ok) return result;
  return syncDirectory(paths, target, slot, observer);
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::renameSlot(
    const Paths& paths, LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot source,
    LegacyFileTransactionSlot destination,
    ILegacyFileRecoveryCutObserver* observer) const {
  const std::size_t from = legacyFileTransactionSlotIndex(source);
  const std::size_t to = legacyFileTransactionSlotIndex(destination);
  if (from >= paths.slots.size() || to >= paths.slots.size() || from == to)
    return LegacyFileTransactionResolveCode::InvalidArgument;
  if (::rename(paths.slots[from].c_str(), paths.slots[to].c_str()) != 0)
    return LegacyFileTransactionResolveCode::IoError;
  LegacyFileTransactionResolveCode result = notify(
      observer, LegacyFileRecoveryCutOperation::Rename,
      target, source, destination);
  if (result != LegacyFileTransactionResolveCode::Ok) return result;
  return syncDirectory(paths, target, destination, observer);
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::finishCurrent(
    const Paths& paths, LegacyFileTransactionTarget target,
    const LegacyFileCandidateSummary& selected,
    ILegacyFileRecoveryCutObserver* observer) {
  LegacyFileTransactionResolveCode result = syncFile(
      paths, target, LegacyFileTransactionSlot::Current, observer);
  if (result != LegacyFileTransactionResolveCode::Ok) return result;
  const LegacyFileCandidateSummary verified = readCandidate(
      target, paths.slots[0]);
  if (!isValid(verified) || !legacyFileCandidateEqual(verified, selected))
    return LegacyFileTransactionResolveCode::VerificationFailed;

  LegacyFileTransactionSnapshot fresh;
  if (inspect(target, fresh) == LegacyFileTransactionProbe::IoError)
    return LegacyFileTransactionResolveCode::SourceUnavailable;
  const LegacyFileCandidateSummary& next = fresh.candidates[1];
  const LegacyFileCandidateSummary& previous = fresh.candidates[2];
  if (isPresent(next)) {
    if (isValid(next) && !legacyFileCandidateEqual(next, selected) &&
        !isValid(previous)) {
      if (isPresent(previous)) {
        result = unlinkSlot(paths, target,
                            LegacyFileTransactionSlot::Previous, observer);
        if (result != LegacyFileTransactionResolveCode::Ok) return result;
      }
      result = renameSlot(paths, target, LegacyFileTransactionSlot::Next,
                          LegacyFileTransactionSlot::Previous, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
      result = syncFile(paths, target,
                        LegacyFileTransactionSlot::Previous, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    } else {
      result = unlinkSlot(paths, target, LegacyFileTransactionSlot::Next,
                          observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    }
  }

  LegacyFileTransactionSnapshot after_next;
  if (inspect(target, after_next) == LegacyFileTransactionProbe::IoError)
    return LegacyFileTransactionResolveCode::SourceUnavailable;
  if (isPresent(after_next.candidates[2]) &&
      !isValid(after_next.candidates[2])) {
    result = unlinkSlot(paths, target,
                        LegacyFileTransactionSlot::Previous, observer);
    if (result != LegacyFileTransactionResolveCode::Ok) return result;
  }

  LegacyFileTransactionSnapshot final_snapshot;
  const LegacyFileTransactionProbe final_probe =
      inspect(target, final_snapshot);
  if (final_probe == LegacyFileTransactionProbe::IoError)
    return LegacyFileTransactionResolveCode::SourceUnavailable;
  if (!legacyFileCandidateEqual(final_snapshot.candidates[0], selected) ||
      final_snapshot.candidates[1].probe !=
          LegacyFileCandidateProbe::Missing ||
      (final_snapshot.candidates[2].probe !=
           LegacyFileCandidateProbe::Missing &&
       final_snapshot.candidates[2].probe !=
           LegacyFileCandidateProbe::Valid)) {
    return LegacyFileTransactionResolveCode::VerificationFailed;
  }
  return LegacyFileTransactionResolveCode::Ok;
}

LegacyFileTransactionResolveCode
PosixLegacyFileTransactionRecovery::resolve(
    const LegacyFileTransactionSnapshot& expected,
    LegacyFileTransactionChoice choice,
    ILegacyFileRecoveryCutObserver* observer) {
  if (!legacyFileTransactionTargetEqual(expected.target, choice.target))
    return LegacyFileTransactionResolveCode::CrossBackend;
  Paths paths;
  if (!targetPaths(choice.target, paths))
    return LegacyFileTransactionResolveCode::InvalidArgument;
  const std::size_t selected_at =
      legacyFileTransactionSlotIndex(choice.slot);
  if (selected_at >= expected.candidates.size())
    return LegacyFileTransactionResolveCode::InvalidArgument;
  if (!isValid(expected.candidates[selected_at]))
    return LegacyFileTransactionResolveCode::SelectedUnavailable;

  LegacyFileTransactionSnapshot fresh;
  const LegacyFileTransactionProbe fresh_probe = inspect(choice.target, fresh);
  if (fresh_probe == LegacyFileTransactionProbe::IoError)
    return LegacyFileTransactionResolveCode::SourceUnavailable;
  if (!legacyFileTransactionSnapshotEqual(expected, fresh))
    return LegacyFileTransactionResolveCode::SourceChanged;
  const LegacyFileCandidateSummary selected = fresh.candidates[selected_at];

  if (legacyFileCandidateEqual(fresh.candidates[0], selected))
    return finishCurrent(paths, choice.target, selected, observer);

  LegacyFileTransactionResolveCode result = syncFile(
      paths, choice.target, choice.slot, observer);
  if (result != LegacyFileTransactionResolveCode::Ok) return result;
  const LegacyFileCandidateSummary selected_verified = readCandidate(
      choice.target, paths.slots[selected_at]);
  if (!isValid(selected_verified) ||
      !legacyFileCandidateEqual(selected_verified, selected)) {
    return LegacyFileTransactionResolveCode::VerificationFailed;
  }

  const LegacyFileCandidateSummary& current = fresh.candidates[0];
  if (choice.slot == LegacyFileTransactionSlot::Next) {
    if (isValid(current)) {
      if (isPresent(fresh.candidates[2])) {
        result = unlinkSlot(paths, choice.target,
                            LegacyFileTransactionSlot::Previous, observer);
        if (result != LegacyFileTransactionResolveCode::Ok) return result;
      }
      result = renameSlot(paths, choice.target,
                          LegacyFileTransactionSlot::Current,
                          LegacyFileTransactionSlot::Previous, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
      result = syncFile(paths, choice.target,
                        LegacyFileTransactionSlot::Previous, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    } else if (isPresent(current)) {
      result = unlinkSlot(paths, choice.target,
                          LegacyFileTransactionSlot::Current, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    }
    result = renameSlot(paths, choice.target,
                        LegacyFileTransactionSlot::Next,
                        LegacyFileTransactionSlot::Current, observer);
    if (result != LegacyFileTransactionResolveCode::Ok) return result;
  } else if (choice.slot == LegacyFileTransactionSlot::Previous) {
    if (isValid(current)) {
      if (isPresent(fresh.candidates[1])) {
        result = unlinkSlot(paths, choice.target,
                            LegacyFileTransactionSlot::Next, observer);
        if (result != LegacyFileTransactionResolveCode::Ok) return result;
      }
      result = renameSlot(paths, choice.target,
                          LegacyFileTransactionSlot::Current,
                          LegacyFileTransactionSlot::Next, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
      result = syncFile(paths, choice.target,
                        LegacyFileTransactionSlot::Next, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    } else if (isPresent(current)) {
      result = unlinkSlot(paths, choice.target,
                          LegacyFileTransactionSlot::Current, observer);
      if (result != LegacyFileTransactionResolveCode::Ok) return result;
    }
    result = renameSlot(paths, choice.target,
                        LegacyFileTransactionSlot::Previous,
                        LegacyFileTransactionSlot::Current, observer);
    if (result != LegacyFileTransactionResolveCode::Ok) return result;
  } else {
    return LegacyFileTransactionResolveCode::InvalidArgument;
  }
  return finishCurrent(paths, choice.target, selected, observer);
}

const char* legacyFileTransactionProbeName(LegacyFileTransactionProbe probe) {
  switch (probe) {
    case LegacyFileTransactionProbe::Empty: return "EMPTY";
    case LegacyFileTransactionProbe::Recoverable: return "RECOVERABLE";
    case LegacyFileTransactionProbe::ChoiceRequired:
      return "CHOICE_REQUIRED";
    case LegacyFileTransactionProbe::Corrupt: return "CORRUPT";
    case LegacyFileTransactionProbe::IoError: return "IO_ERROR";
    case LegacyFileTransactionProbe::InvalidTarget: return "INVALID_TARGET";
  }
  return "UNKNOWN";
}

const char* legacyFileCandidateProbeName(LegacyFileCandidateProbe probe) {
  switch (probe) {
    case LegacyFileCandidateProbe::Missing: return "MISSING";
    case LegacyFileCandidateProbe::Valid: return "VALID";
    case LegacyFileCandidateProbe::Invalid: return "INVALID";
    case LegacyFileCandidateProbe::IoError: return "IO_ERROR";
  }
  return "UNKNOWN";
}

const char* legacyFileTransactionResolveCodeName(
    LegacyFileTransactionResolveCode code) {
  switch (code) {
    case LegacyFileTransactionResolveCode::Ok: return "OK";
    case LegacyFileTransactionResolveCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case LegacyFileTransactionResolveCode::CrossBackend:
      return "CROSS_BACKEND";
    case LegacyFileTransactionResolveCode::SelectedUnavailable:
      return "SELECTED_UNAVAILABLE";
    case LegacyFileTransactionResolveCode::SourceChanged:
      return "SOURCE_CHANGED";
    case LegacyFileTransactionResolveCode::SourceUnavailable:
      return "SOURCE_UNAVAILABLE";
    case LegacyFileTransactionResolveCode::IoError: return "IO_ERROR";
    case LegacyFileTransactionResolveCode::VerificationFailed:
      return "VERIFICATION_FAILED";
    case LegacyFileTransactionResolveCode::PowerCutSimulated:
      return "POWER_CUT_SIMULATED";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
