#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {
namespace storage {

enum class LegacyFileTransactionDomain : std::uint8_t {
  Tasks,
  Album,
};

enum class LegacyFileTransactionBackend : std::uint8_t {
  TaskRoot,
  Internal,
  Removable,
};

struct LegacyFileTransactionTarget {
  LegacyFileTransactionDomain domain = LegacyFileTransactionDomain::Tasks;
  LegacyFileTransactionBackend backend =
      LegacyFileTransactionBackend::TaskRoot;
};

enum class LegacyFileTransactionSlot : std::uint8_t {
  Current,
  Next,
  Previous,
};

enum class LegacyFileCandidateProbe : std::uint8_t {
  Missing,
  Valid,
  Invalid,
  IoError,
};

struct LegacyFileCandidateSummary {
  LegacyFileCandidateProbe probe = LegacyFileCandidateProbe::Missing;
  std::uint64_t byte_count = 0U;
  std::array<std::uint8_t, 32> sha256{};
  bool digest_present = false;
  // Parsed logical records: task count for Tasks, asset-entry count for Album.
  // This is safe recovery metadata only; no record contents or paths escape.
  std::uint32_t item_count = 0U;
  bool item_count_present = false;
  // Filesystem mtime, exposed only when it is a positive 32-bit Unix time.
  // FAT timestamps can be coarse or unavailable, so this is a hint and never
  // participates in automatic selection (there is no automatic selection).
  std::uint32_t modified_unix_seconds = 0U;
  bool modified_time_present = false;
};

enum class LegacyFileTransactionProbe : std::uint8_t {
  Empty,
  Recoverable,
  ChoiceRequired,
  Corrupt,
  IoError,
  InvalidTarget,
};

struct LegacyFileTransactionSnapshot {
  LegacyFileTransactionTarget target{};
  LegacyFileTransactionProbe probe =
      LegacyFileTransactionProbe::InvalidTarget;
  std::array<LegacyFileCandidateSummary, 3> candidates{};
  std::uint8_t valid_candidates = 0U;
};

struct LegacyFileTransactionChoice {
  LegacyFileTransactionTarget target{};
  LegacyFileTransactionSlot slot = LegacyFileTransactionSlot::Current;
};

enum class LegacyFileRecoveryCutOperation : std::uint8_t {
  FileFsync,
  DirectoryFsync,
  Unlink,
  Rename,
};

struct LegacyFileRecoveryCutPoint {
  LegacyFileRecoveryCutOperation operation =
      LegacyFileRecoveryCutOperation::FileFsync;
  LegacyFileTransactionTarget target{};
  LegacyFileTransactionSlot source = LegacyFileTransactionSlot::Current;
  LegacyFileTransactionSlot destination =
      LegacyFileTransactionSlot::Current;
};

// Test/acceptance seam invoked only after the named real POSIX operation has
// completed. Returning false simulates power loss at that exact cut point.
class ILegacyFileRecoveryCutObserver {
 public:
  virtual ~ILegacyFileRecoveryCutObserver() = default;
  virtual bool continueAfter(const LegacyFileRecoveryCutPoint& point) = 0;
};

enum class LegacyFileTransactionResolveCode : std::uint8_t {
  Ok,
  InvalidArgument,
  CrossBackend,
  SelectedUnavailable,
  SourceChanged,
  SourceUnavailable,
  IoError,
  VerificationFailed,
  PowerCutSimulated,
};

struct LegacyFileTransactionRecoveryConfig {
  std::string task_root;
  std::string internal_root;
  std::string removable_root;
};

// Explicit recovery-only adapter. Roots are fixed at construction; inspection
// and resolution accept only typed domain/backend/slot values, never paths.
// Higher-level composition must quiesce all task/album writers before use.
class PosixLegacyFileTransactionRecovery final {
 public:
  explicit PosixLegacyFileTransactionRecovery(
      LegacyFileTransactionRecoveryConfig config);

  LegacyFileTransactionProbe inspect(
      LegacyFileTransactionTarget target,
      LegacyFileTransactionSnapshot& output) const;

  LegacyFileTransactionResolveCode resolve(
      const LegacyFileTransactionSnapshot& expected,
      LegacyFileTransactionChoice choice,
      ILegacyFileRecoveryCutObserver* observer = nullptr);

 private:
  struct Paths {
    std::string directory;
    std::array<std::string, 3> slots{};
  };

  bool targetPaths(LegacyFileTransactionTarget target,
                   Paths& output) const;
  LegacyFileCandidateSummary readCandidate(
      LegacyFileTransactionTarget target, const std::string& path) const;
  LegacyFileTransactionResolveCode syncFile(
      const Paths& paths, LegacyFileTransactionTarget target,
      LegacyFileTransactionSlot slot,
      ILegacyFileRecoveryCutObserver* observer) const;
  LegacyFileTransactionResolveCode syncDirectory(
      const Paths& paths, LegacyFileTransactionTarget target,
      LegacyFileTransactionSlot related,
      ILegacyFileRecoveryCutObserver* observer) const;
  LegacyFileTransactionResolveCode unlinkSlot(
      const Paths& paths, LegacyFileTransactionTarget target,
      LegacyFileTransactionSlot slot,
      ILegacyFileRecoveryCutObserver* observer) const;
  LegacyFileTransactionResolveCode renameSlot(
      const Paths& paths, LegacyFileTransactionTarget target,
      LegacyFileTransactionSlot source,
      LegacyFileTransactionSlot destination,
      ILegacyFileRecoveryCutObserver* observer) const;
  LegacyFileTransactionResolveCode finishCurrent(
      const Paths& paths, LegacyFileTransactionTarget target,
      const LegacyFileCandidateSummary& selected,
      ILegacyFileRecoveryCutObserver* observer);

  LegacyFileTransactionRecoveryConfig config_;
  bool task_root_valid_ = false;
  bool internal_root_valid_ = false;
  bool removable_root_valid_ = false;
};

bool legacyFileTransactionTargetEqual(LegacyFileTransactionTarget left,
                                      LegacyFileTransactionTarget right);
bool legacyFileCandidateEqual(const LegacyFileCandidateSummary& left,
                              const LegacyFileCandidateSummary& right);
bool legacyFileTransactionSnapshotEqual(
    const LegacyFileTransactionSnapshot& left,
    const LegacyFileTransactionSnapshot& right);
std::size_t legacyFileTransactionSlotIndex(LegacyFileTransactionSlot slot);

const char* legacyFileTransactionProbeName(LegacyFileTransactionProbe probe);
const char* legacyFileCandidateProbeName(LegacyFileCandidateProbe probe);
const char* legacyFileTransactionResolveCodeName(
    LegacyFileTransactionResolveCode code);

}  // namespace storage
}  // namespace inkloop
