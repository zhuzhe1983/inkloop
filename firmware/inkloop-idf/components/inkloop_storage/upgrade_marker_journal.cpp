#include "inkloop/storage/upgrade_marker_journal.hpp"

#include <algorithm>
#include <limits>

namespace inkloop {
namespace storage {
namespace {

constexpr std::array<std::uint8_t, 4> kMarkerMagic{{'I', 'N', 'K', 'M'}};
constexpr std::array<std::uint8_t, 4> kJournalSlotMagic{{'I', 'M', 'J', '1'}};
constexpr std::size_t kMarkerFingerprintAt = 16U;
constexpr std::size_t kMarkerPhaseAt = 48U;
constexpr std::size_t kMarkerChecksumAt = 52U;
constexpr std::size_t kJournalSequenceAt = 4U;
constexpr std::size_t kJournalMarkerAt = 12U;
constexpr std::size_t kJournalChecksumAt = 68U;

void put16(std::uint16_t value, std::uint8_t* output) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put32(std::uint32_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

void put64(std::uint64_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

std::uint16_t get16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>(input[0]) |
      (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t get32(const std::uint8_t* input) {
  std::uint32_t output = 0U;
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output |= static_cast<std::uint32_t>(input[shift / 8U]) << shift;
  return output;
}

std::uint64_t get64(const std::uint8_t* input) {
  std::uint64_t output = 0U;
  for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
    output |= static_cast<std::uint64_t>(input[shift / 8U]) << shift;
  return output;
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t value = 0xFFFFFFFFU;
  for (std::size_t at = 0U; at < length; ++at) {
    value ^= bytes[at];
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(value & 1U);
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return value ^ 0xFFFFFFFFU;
}

bool markerEqual(const MigrationMarker& left,
                 const MigrationMarker& right) {
  return left.schema_version == right.schema_version &&
      left.source_layout_schema_version ==
          right.source_layout_schema_version &&
      left.generation == right.generation &&
      left.source_fingerprint == right.source_fingerprint &&
      left.phase == right.phase && left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source &&
      left.checksum == right.checksum;
}

MigrationMarkerCodecCode encodeJournalSlot(
    std::uint64_t sequence, const MigrationMarker& marker,
    EncodedMigrationJournalSlot& output) {
  output.fill(0U);
  if (sequence == 0U) return MigrationMarkerCodecCode::InvalidArgument;
  EncodedMigrationMarker encoded_marker{};
  const MigrationMarkerCodecCode code =
      encodeMigrationMarkerV1(marker, encoded_marker);
  if (code != MigrationMarkerCodecCode::Ok) return code;
  std::copy(kJournalSlotMagic.begin(), kJournalSlotMagic.end(), output.begin());
  put64(sequence, output.data() + kJournalSequenceAt);
  std::copy(encoded_marker.begin(), encoded_marker.end(),
            output.begin() + kJournalMarkerAt);
  put32(crc32(output.data(), kJournalChecksumAt),
        output.data() + kJournalChecksumAt);
  return MigrationMarkerCodecCode::Ok;
}

MigrationMarkerCodecCode decodeJournalSlotInternal(
    const RawMigrationJournalSlot& raw, std::uint64_t& sequence,
    MigrationMarker& marker) {
  sequence = 0U;
  marker = MigrationMarker{};
  if (!raw.present || raw.length != kMigrationMarkerJournalSlotBytes)
    return MigrationMarkerCodecCode::WrongSize;
  if (!std::equal(kJournalSlotMagic.begin(), kJournalSlotMagic.end(),
                  raw.bytes.begin()) ||
      crc32(raw.bytes.data(), kJournalChecksumAt) !=
          get32(raw.bytes.data() + kJournalChecksumAt)) {
    return MigrationMarkerCodecCode::Corrupt;
  }
  sequence = get64(raw.bytes.data() + kJournalSequenceAt);
  if (sequence == 0U) return MigrationMarkerCodecCode::Corrupt;
  return decodeMigrationMarkerV1(raw.bytes.data() + kJournalMarkerAt,
                                 kEncodedMigrationMarkerBytes, marker);
}

MigrationMarkerJournalCode classifyRaw(
    const RawMigrationMarkerJournal& raw,
    MigrationMarkerJournalInspection& output) {
  output = MigrationMarkerJournalInspection{};
  if (!raw.namespace_available) {
    output.probe = MigrationMarkerJournalProbe::IoError;
    return MigrationMarkerJournalCode::IoError;
  }
  const bool any = raw.initialized_present || raw.head_present ||
      raw.slots[0].present || raw.slots[1].present;
  if (!any) {
    output.probe = MigrationMarkerJournalProbe::Missing;
    return MigrationMarkerJournalCode::Ok;
  }
  if (!raw.initialized_present || !raw.head_present) {
    output.probe = MigrationMarkerJournalProbe::Torn;
    return MigrationMarkerJournalCode::Torn;
  }
  if (raw.initialized != kMigrationJournalInitializedMarker ||
      raw.head_sequence == 0U) {
    output.probe = MigrationMarkerJournalProbe::Corrupt;
    return MigrationMarkerJournalCode::Corrupt;
  }
  const std::size_t selected =
      static_cast<std::size_t>(raw.head_sequence & 1U);
  if (!raw.slots[selected].present ||
      raw.slots[selected].length != kMigrationMarkerJournalSlotBytes) {
    output.probe = MigrationMarkerJournalProbe::Torn;
    return MigrationMarkerJournalCode::Torn;
  }
  std::uint64_t decoded_sequence = 0U;
  MigrationMarker decoded;
  const MigrationMarkerCodecCode decoded_code = decodeJournalSlotInternal(
      raw.slots[selected], decoded_sequence, decoded);
  if (decoded_code != MigrationMarkerCodecCode::Ok ||
      decoded_sequence != raw.head_sequence) {
    output.probe = MigrationMarkerJournalProbe::Corrupt;
    return MigrationMarkerJournalCode::Corrupt;
  }
  output.probe = MigrationMarkerJournalProbe::Valid;
  output.sequence = decoded_sequence;
  output.marker = decoded;
  return MigrationMarkerJournalCode::Ok;
}

bool sameMigrationIdentity(const MigrationMarker& left,
                           const MigrationMarker& right) {
  return left.schema_version == right.schema_version &&
      left.source_layout_schema_version ==
          right.source_layout_schema_version &&
      left.generation == right.generation &&
      left.source_fingerprint == right.source_fingerprint &&
      left.target_slot == right.target_slot &&
      left.rollback_source == right.rollback_source;
}

bool phaseTransitionAllowed(MigrationPhase from, MigrationPhase to) {
  if (from == to) return true;
  if (to == MigrationPhase::RollbackRequired &&
      from != MigrationPhase::Complete)
    return true;
  switch (from) {
    case MigrationPhase::Prepared:
      return to == MigrationPhase::TargetWritten;
    case MigrationPhase::TargetWritten:
      return to == MigrationPhase::TargetVerified;
    case MigrationPhase::TargetVerified:
      return to == MigrationPhase::CommitRecorded;
    case MigrationPhase::CommitRecorded:
      return to == MigrationPhase::Complete;
    case MigrationPhase::RollbackRequired:
    case MigrationPhase::Complete:
    case MigrationPhase::None:
      return false;
  }
  return false;
}

bool commitTransitionAllowed(const MigrationMarkerJournalInspection& current,
                             const MigrationMarker& next) {
  if (current.probe == MigrationMarkerJournalProbe::Missing)
    // The journal may be introduced after an older firmware already created
    // one or more native target generations.  The product gate owns the
    // read-only proof that an arbitrary first generation is the exact next
    // settings generation; the journal only requires a nonzero Prepared
    // identity here.
    return next.generation != 0U && next.phase == MigrationPhase::Prepared;
  if (current.probe != MigrationMarkerJournalProbe::Valid) return false;
  if (markerEqual(current.marker, next)) return true;
  if (sameMigrationIdentity(current.marker, next))
    return phaseTransitionAllowed(current.marker.phase, next.phase);
  return current.marker.phase == MigrationPhase::Complete &&
      next.phase == MigrationPhase::Prepared &&
      next.generation == current.marker.generation + 1U;
}

MigrationMarkerJournalCode mapProbeFailure(
    MigrationMarkerJournalProbe probe) {
  if (probe == MigrationMarkerJournalProbe::Torn)
    return MigrationMarkerJournalCode::Torn;
  if (probe == MigrationMarkerJournalProbe::Corrupt)
    return MigrationMarkerJournalCode::Corrupt;
  return MigrationMarkerJournalCode::IoError;
}

}  // namespace

MigrationMarkerCodecCode encodeMigrationMarkerV1(
    const MigrationMarker& marker, EncodedMigrationMarker& output) {
  output.fill(0U);
  if (marker.schema_version != kMigrationMarkerSchemaVersion)
    return MigrationMarkerCodecCode::UnsupportedSchema;
  if (!migrationMarkerValid(marker))
    return MigrationMarkerCodecCode::InvalidArgument;
  std::copy(kMarkerMagic.begin(), kMarkerMagic.end(), output.begin());
  put16(marker.schema_version, output.data() + 4U);
  put16(marker.source_layout_schema_version, output.data() + 6U);
  put64(marker.generation, output.data() + 8U);
  std::copy(marker.source_fingerprint.begin(),
            marker.source_fingerprint.end(),
            output.begin() + kMarkerFingerprintAt);
  output[kMarkerPhaseAt] = static_cast<std::uint8_t>(marker.phase);
  output[kMarkerPhaseAt + 1U] =
      static_cast<std::uint8_t>(marker.target_slot);
  output[kMarkerPhaseAt + 2U] =
      static_cast<std::uint8_t>(marker.rollback_source);
  output[kMarkerPhaseAt + 3U] = 0U;
  put32(marker.checksum, output.data() + kMarkerChecksumAt);
  return MigrationMarkerCodecCode::Ok;
}

MigrationMarkerCodecCode decodeMigrationMarkerV1(
    const std::uint8_t* bytes, std::size_t length,
    MigrationMarker& output) {
  output = MigrationMarker{};
  if ((!bytes && length != 0U) || length != kEncodedMigrationMarkerBytes)
    return MigrationMarkerCodecCode::WrongSize;
  if (!std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), bytes) ||
      bytes[kMarkerPhaseAt + 3U] != 0U)
    return MigrationMarkerCodecCode::Corrupt;
  MigrationMarker decoded;
  decoded.schema_version = get16(bytes + 4U);
  decoded.source_layout_schema_version = get16(bytes + 6U);
  decoded.generation = get64(bytes + 8U);
  std::copy(bytes + kMarkerFingerprintAt,
            bytes + kMarkerFingerprintAt + decoded.source_fingerprint.size(),
            decoded.source_fingerprint.begin());
  decoded.phase = static_cast<MigrationPhase>(bytes[kMarkerPhaseAt]);
  decoded.target_slot =
      static_cast<MigrationSlot>(bytes[kMarkerPhaseAt + 1U]);
  decoded.rollback_source = static_cast<MigrationRollbackSource>(
      bytes[kMarkerPhaseAt + 2U]);
  decoded.checksum = get32(bytes + kMarkerChecksumAt);
  if (decoded.schema_version != kMigrationMarkerSchemaVersion)
    return MigrationMarkerCodecCode::UnsupportedSchema;
  if (!migrationMarkerValid(decoded))
    return MigrationMarkerCodecCode::Corrupt;
  output = decoded;
  return MigrationMarkerCodecCode::Ok;
}

MigrationMarkerCodecCode decodeMigrationJournalSlotV1(
    const RawMigrationJournalSlot& raw, std::uint64_t& sequence,
    MigrationMarker& marker) {
  return decodeJournalSlotInternal(raw, sequence, marker);
}

MigrationMarkerJournalCode MigrationMarkerJournalCore::inspect(
    MigrationMarkerJournalInspection& output) const {
  output = MigrationMarkerJournalInspection{};
  RawMigrationMarkerJournal raw;
  if (store_.inspectRaw(raw) != MigrationJournalStoreCode::Ok) {
    output.probe = MigrationMarkerJournalProbe::IoError;
    return MigrationMarkerJournalCode::IoError;
  }
  return classifyRaw(raw, output);
}

MigrationMarkerJournalCode MigrationMarkerJournalCore::commit(
    const MigrationMarker& marker, std::uint64_t expected_sequence,
    MigrationMarkerJournalInspection& committed) {
  committed = MigrationMarkerJournalInspection{};
  if (!migrationMarkerValid(marker))
    return MigrationMarkerJournalCode::InvalidArgument;

  MigrationMarkerJournalInspection current;
  const MigrationMarkerJournalCode inspected = inspect(current);
  if (inspected != MigrationMarkerJournalCode::Ok)
    return mapProbeFailure(current.probe);
  const std::uint64_t current_sequence =
      current.probe == MigrationMarkerJournalProbe::Missing
          ? 0U
          : current.sequence;
  if (current_sequence != expected_sequence)
    return MigrationMarkerJournalCode::Conflict;
  if (!commitTransitionAllowed(current, marker))
    return MigrationMarkerJournalCode::InvalidArgument;
  if (current.probe == MigrationMarkerJournalProbe::Valid &&
      markerEqual(current.marker, marker)) {
    committed = current;
    return MigrationMarkerJournalCode::Ok;
  }
  if (current_sequence == std::numeric_limits<std::uint64_t>::max())
    return MigrationMarkerJournalCode::Exhausted;

  const std::uint64_t next_sequence = current_sequence + 1U;
  EncodedMigrationJournalSlot encoded{};
  if (encodeJournalSlot(next_sequence, marker, encoded) !=
      MigrationMarkerCodecCode::Ok)
    return MigrationMarkerJournalCode::InvalidArgument;
  const std::uint8_t next_slot =
      static_cast<std::uint8_t>(next_sequence & 1U);
  if (store_.writeSlotAndCommit(next_slot, encoded) !=
      MigrationJournalStoreCode::Ok)
    return MigrationMarkerJournalCode::IoError;

  RawMigrationMarkerJournal read_back;
  if (store_.inspectRaw(read_back) != MigrationJournalStoreCode::Ok)
    return MigrationMarkerJournalCode::ReadBackFailed;
  std::uint64_t read_sequence = 0U;
  MigrationMarker read_marker;
  if (decodeJournalSlotInternal(read_back.slots[next_slot], read_sequence,
                                read_marker) !=
          MigrationMarkerCodecCode::Ok ||
      read_sequence != next_sequence || !markerEqual(read_marker, marker))
    return MigrationMarkerJournalCode::ReadBackFailed;

  if (store_.writeHeadAndMarkerAndCommit(next_sequence) !=
      MigrationJournalStoreCode::Ok)
    return MigrationMarkerJournalCode::IoError;
  const MigrationMarkerJournalCode final_code = inspect(committed);
  if (final_code != MigrationMarkerJournalCode::Ok ||
      committed.probe != MigrationMarkerJournalProbe::Valid ||
      committed.sequence != next_sequence ||
      !markerEqual(committed.marker, marker)) {
    committed = MigrationMarkerJournalInspection{};
    return MigrationMarkerJournalCode::ReadBackFailed;
  }
  return MigrationMarkerJournalCode::Ok;
}

const char* migrationMarkerCodecCodeName(MigrationMarkerCodecCode code) {
  switch (code) {
    case MigrationMarkerCodecCode::Ok: return "OK";
    case MigrationMarkerCodecCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case MigrationMarkerCodecCode::WrongSize: return "WRONG_SIZE";
    case MigrationMarkerCodecCode::UnsupportedSchema:
      return "UNSUPPORTED_SCHEMA";
    case MigrationMarkerCodecCode::Corrupt: return "CORRUPT";
  }
  return "UNKNOWN";
}

const char* migrationMarkerJournalProbeName(
    MigrationMarkerJournalProbe probe) {
  switch (probe) {
    case MigrationMarkerJournalProbe::Missing: return "MISSING";
    case MigrationMarkerJournalProbe::Torn: return "TORN";
    case MigrationMarkerJournalProbe::Corrupt: return "CORRUPT";
    case MigrationMarkerJournalProbe::IoError: return "IO_ERROR";
    case MigrationMarkerJournalProbe::Valid: return "VALID";
  }
  return "UNKNOWN";
}

const char* migrationMarkerJournalCodeName(MigrationMarkerJournalCode code) {
  switch (code) {
    case MigrationMarkerJournalCode::Ok: return "OK";
    case MigrationMarkerJournalCode::InvalidArgument:
      return "INVALID_ARGUMENT";
    case MigrationMarkerJournalCode::Conflict: return "CONFLICT";
    case MigrationMarkerJournalCode::Exhausted: return "EXHAUSTED";
    case MigrationMarkerJournalCode::Torn: return "TORN";
    case MigrationMarkerJournalCode::Corrupt: return "CORRUPT";
    case MigrationMarkerJournalCode::IoError: return "IO_ERROR";
    case MigrationMarkerJournalCode::ReadBackFailed:
      return "READ_BACK_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
