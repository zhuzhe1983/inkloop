#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/storage/upgrade_recovery_planner.hpp"

namespace inkloop {
namespace storage {

inline constexpr std::size_t kEncodedMigrationMarkerBytes = 56U;
inline constexpr std::size_t kMigrationMarkerJournalSlotBytes = 72U;
inline constexpr std::uint8_t kMigrationJournalInitializedMarker = 0xD7U;

using EncodedMigrationMarker =
    std::array<std::uint8_t, kEncodedMigrationMarkerBytes>;
using EncodedMigrationJournalSlot =
    std::array<std::uint8_t, kMigrationMarkerJournalSlotBytes>;

enum class MigrationMarkerCodecCode : std::uint8_t {
  Ok,
  InvalidArgument,
  WrongSize,
  UnsupportedSchema,
  Corrupt,
};

MigrationMarkerCodecCode encodeMigrationMarkerV1(
    const MigrationMarker& marker, EncodedMigrationMarker& output);
MigrationMarkerCodecCode decodeMigrationMarkerV1(
    const std::uint8_t* bytes, std::size_t length,
    MigrationMarker& output);

enum class MigrationJournalStoreCode : std::uint8_t {
  Ok,
  InvalidArgument,
  IoError,
};

struct RawMigrationJournalSlot {
  bool present = false;
  std::size_t length = 0U;
  EncodedMigrationJournalSlot bytes{};
};

// One adapter read populates this complete value. A store error is returned by
// inspectRaw rather than encoded into a partially trusted snapshot.
struct RawMigrationMarkerJournal {
  bool namespace_available = false;
  bool initialized_present = false;
  std::uint8_t initialized = 0U;
  bool head_present = false;
  std::uint64_t head_sequence = 0U;
  std::array<RawMigrationJournalSlot, 2> slots{};
};

class IMigrationMarkerJournalStore {
 public:
  virtual ~IMigrationMarkerJournalStore() = default;
  virtual MigrationJournalStoreCode inspectRaw(
      RawMigrationMarkerJournal& state) const = 0;
  virtual MigrationJournalStoreCode writeSlotAndCommit(
      std::uint8_t slot,
      const EncodedMigrationJournalSlot& encoded) = 0;
  virtual MigrationJournalStoreCode writeHeadAndMarkerAndCommit(
      std::uint64_t sequence) = 0;
};

enum class MigrationMarkerJournalProbe : std::uint8_t {
  Missing,
  Torn,
  Corrupt,
  IoError,
  Valid,
};

struct MigrationMarkerJournalInspection {
  MigrationMarkerJournalProbe probe = MigrationMarkerJournalProbe::IoError;
  std::uint64_t sequence = 0U;
  MigrationMarker marker{};
};

enum class MigrationMarkerJournalCode : std::uint8_t {
  Ok,
  InvalidArgument,
  Conflict,
  Exhausted,
  Torn,
  Corrupt,
  IoError,
  ReadBackFailed,
};

// Journal sequence and marker generation are intentionally independent. The
// former advances for every durable phase record; the latter identifies one
// migration attempt and is also carried by target-slot evidence.
class MigrationMarkerJournalCore final {
 public:
  explicit MigrationMarkerJournalCore(IMigrationMarkerJournalStore& store)
      : store_(store) {}

  MigrationMarkerJournalCode inspect(
      MigrationMarkerJournalInspection& output) const;
  MigrationMarkerJournalCode commit(
      const MigrationMarker& marker, std::uint64_t expected_sequence,
      MigrationMarkerJournalInspection& committed);

 private:
  IMigrationMarkerJournalStore& store_;
};

const char* migrationMarkerCodecCodeName(MigrationMarkerCodecCode code);
const char* migrationMarkerJournalProbeName(MigrationMarkerJournalProbe probe);
const char* migrationMarkerJournalCodeName(MigrationMarkerJournalCode code);

}  // namespace storage
}  // namespace inkloop
