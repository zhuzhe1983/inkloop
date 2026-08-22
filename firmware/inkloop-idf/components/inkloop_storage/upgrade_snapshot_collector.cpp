#include "inkloop/storage/upgrade_snapshot_collector.hpp"

#include <algorithm>
#include <limits>

#include "inkloop/storage/sha256.hpp"

namespace inkloop {
namespace storage {
namespace {

constexpr std::array<std::uint64_t, kProtectedFilePaths.size()>
    kMaximumFileBytes{{
        256U * 1024U, 256U * 1024U, 256U * 1024U,
        16U * 1024U, 16U * 1024U, 16U * 1024U,
        64U * 1024U, 64U * 1024U, 64U * 1024U,
        512U * 1024U, 512U * 1024U,
    }};

class FingerprintSink final : public IUpgradeByteSink {
 public:
  explicit FingerprintSink(std::uint64_t maximum) : maximum_(maximum) {}

  bool write(const std::uint8_t* bytes, std::size_t length) override {
    if (failed_ || (!bytes && length != 0U) ||
        length > std::numeric_limits<std::uint64_t>::max() - bytes_ ||
        bytes_ + static_cast<std::uint64_t>(length) > maximum_ ||
        !hash_.update(bytes, length)) {
      failed_ = true;
      return false;
    }
    bytes_ += static_cast<std::uint64_t>(length);
    return true;
  }

  bool finish(FingerprintedUpgradeRecord& output) {
    output.logical_bytes = bytes_;
    return !failed_ && hash_.finish(output.content_fingerprint);
  }

  bool failed() const { return failed_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  Sha256 hash_;
  std::uint64_t maximum_ = 0U;
  std::uint64_t bytes_ = 0U;
  bool failed_ = false;
};

RecordProbe recordProbe(UpgradeRecordStreamCode code) {
  switch (code) {
    case UpgradeRecordStreamCode::Missing: return RecordProbe::Missing;
    case UpgradeRecordStreamCode::Valid: return RecordProbe::Valid;
    case UpgradeRecordStreamCode::Recoverable:
      return RecordProbe::Recoverable;
    case UpgradeRecordStreamCode::Ambiguous: return RecordProbe::Ambiguous;
    case UpgradeRecordStreamCode::Invalid: return RecordProbe::Invalid;
    case UpgradeRecordStreamCode::Unvalidated:
      return RecordProbe::Unvalidated;
    case UpgradeRecordStreamCode::IoError: return RecordProbe::IoError;
    case UpgradeRecordStreamCode::TooLarge: return RecordProbe::Invalid;
  }
  return RecordProbe::Invalid;
}

UpgradeSnapshotCollectCode collectRecord(
    const IUpgradeSnapshotSource& source, UpgradeRecordId id,
    FingerprintedUpgradeRecord& output) {
  output = FingerprintedUpgradeRecord{};
  const std::uint64_t maximum = upgradeRecordMaximumBytes(id);
  if (maximum == 0U) return UpgradeSnapshotCollectCode::InvalidEvidence;
  FingerprintSink sink(maximum);
  const UpgradeRecordStreamCode code =
      source.streamRecord(id, maximum, sink);
  output.probe = recordProbe(code);
  if (code == UpgradeRecordStreamCode::TooLarge || sink.failed()) {
    output = FingerprintedUpgradeRecord{};
    output.probe = RecordProbe::Invalid;
    return UpgradeSnapshotCollectCode::TooLarge;
  }
  if (code == UpgradeRecordStreamCode::IoError) {
    output = FingerprintedUpgradeRecord{};
    output.probe = RecordProbe::IoError;
    return UpgradeSnapshotCollectCode::IoError;
  }
  if (code == UpgradeRecordStreamCode::Missing) {
    if (sink.bytes() != 0U) return UpgradeSnapshotCollectCode::InvalidEvidence;
    output.logical_bytes = 0U;
    output.content_fingerprint.fill(0U);
    return UpgradeSnapshotCollectCode::Ok;
  }
  if (code != UpgradeRecordStreamCode::Valid &&
      code != UpgradeRecordStreamCode::Recoverable) {
    output.logical_bytes = 0U;
    output.content_fingerprint.fill(0U);
    return UpgradeSnapshotCollectCode::Ok;
  }
  return sink.finish(output) ? UpgradeSnapshotCollectCode::Ok
                             : UpgradeSnapshotCollectCode::InvalidEvidence;
}

void mergeFailure(UpgradeSnapshotCollectCode candidate,
                  UpgradeSnapshotCollectCode& aggregate) {
  if (candidate == UpgradeSnapshotCollectCode::IoError) {
    aggregate = candidate;
    return;
  }
  if (aggregate == UpgradeSnapshotCollectCode::IoError) return;
  if (candidate == UpgradeSnapshotCollectCode::TooLarge) {
    aggregate = candidate;
    return;
  }
  if (aggregate == UpgradeSnapshotCollectCode::TooLarge) return;
  if (candidate != UpgradeSnapshotCollectCode::Ok)
    aggregate = candidate;
}

UpgradeSnapshotCollectCode collectPass(
    const IUpgradeSnapshotSource& source,
    UpgradeEvidenceSnapshot& snapshot) {
  snapshot = UpgradeEvidenceSnapshot{};
  UpgradeSnapshotCollectCode result = UpgradeSnapshotCollectCode::Ok;
  UpgradeSnapshotMetadata metadata;
  if (!source.inspectMetadata(metadata)) {
    result = UpgradeSnapshotCollectCode::IoError;
  } else {
    snapshot.internal_mounted = metadata.internal_mounted;
    snapshot.source_layout = metadata.source_layout;
    snapshot.source_layout_schema_version =
        metadata.source_layout_schema_version;
    snapshot.legacy_source_durable = metadata.legacy_source_durable;
    snapshot.native_slots = metadata.native_slots;
    snapshot.marker_journal = metadata.marker_journal;
  }

  for (std::size_t at = 0U; at < snapshot.nvs.size(); ++at) {
    const UpgradeSnapshotCollectCode code = collectRecord(
        source, {UpgradeRecordDomain::NvsNamespace, at}, snapshot.nvs[at]);
    mergeFailure(code, result);
  }
  for (std::size_t at = 0U; at < snapshot.files.size(); ++at) {
    const UpgradeSnapshotCollectCode code = collectRecord(
        source, {UpgradeRecordDomain::File, at}, snapshot.files[at]);
    mergeFailure(code, result);
  }
  return result;
}

UpgradeSnapshotCollectCode mapCompose(UpgradeEvidenceComposeCode code) {
  switch (code) {
    case UpgradeEvidenceComposeCode::Ok:
      return UpgradeSnapshotCollectCode::Ok;
    case UpgradeEvidenceComposeCode::Changed:
      return UpgradeSnapshotCollectCode::Changed;
    case UpgradeEvidenceComposeCode::Ambiguous:
      return UpgradeSnapshotCollectCode::Ambiguous;
    case UpgradeEvidenceComposeCode::IoError:
      return UpgradeSnapshotCollectCode::IoError;
    case UpgradeEvidenceComposeCode::UnsupportedSchema:
      return UpgradeSnapshotCollectCode::UnsupportedSchema;
    case UpgradeEvidenceComposeCode::InvalidEvidence:
      return UpgradeSnapshotCollectCode::InvalidEvidence;
  }
  return UpgradeSnapshotCollectCode::InvalidEvidence;
}

}  // namespace

bool upgradeRecordIdValid(UpgradeRecordId record) {
  if (record.domain == UpgradeRecordDomain::NvsNamespace)
    return record.index < kProtectedNvsNamespaces.size();
  if (record.domain == UpgradeRecordDomain::File)
    return record.index < kProtectedFilePaths.size();
  return false;
}

const char* upgradeRecordName(UpgradeRecordId record) {
  if (!upgradeRecordIdValid(record)) return nullptr;
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? kProtectedNvsNamespaces[record.index]
      : kProtectedFilePaths[record.index];
}

std::uint64_t upgradeRecordMaximumBytes(UpgradeRecordId record) {
  if (!upgradeRecordIdValid(record)) return 0U;
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? kMaximumUpgradeNvsNamespaceBytes
      : kMaximumFileBytes[record.index];
}

UpgradeSnapshotCollectCode UpgradeSnapshotCollector::collect(
    CollectedUpgradeRecovery& output) const {
  output = CollectedUpgradeRecovery{};
  UpgradeEvidenceSnapshot first;
  UpgradeEvidenceSnapshot second;
  const UpgradeSnapshotCollectCode first_code = collectPass(source_, first);
  const UpgradeSnapshotCollectCode second_code = collectPass(source_, second);
  UpgradeSnapshotCollectCode pass_code = UpgradeSnapshotCollectCode::Ok;
  mergeFailure(first_code, pass_code);
  mergeFailure(second_code, pass_code);
  if (pass_code != UpgradeSnapshotCollectCode::Ok) return pass_code;

  UpgradeRecoveryEvidence evidence;
  const UpgradeSnapshotCollectCode composed = mapCompose(
      composeUpgradeRecoveryEvidence(first, second, evidence));
  if (composed != UpgradeSnapshotCollectCode::Ok) return composed;
  output.snapshot = first;
  output.evidence = evidence;
  output.plan = planUpgradeRecovery(evidence);
  return UpgradeSnapshotCollectCode::Ok;
}

const char* upgradeRecordStreamCodeName(UpgradeRecordStreamCode code) {
  switch (code) {
    case UpgradeRecordStreamCode::Missing: return "MISSING";
    case UpgradeRecordStreamCode::Valid: return "VALID";
    case UpgradeRecordStreamCode::Recoverable: return "RECOVERABLE";
    case UpgradeRecordStreamCode::Ambiguous: return "AMBIGUOUS";
    case UpgradeRecordStreamCode::Invalid: return "INVALID";
    case UpgradeRecordStreamCode::Unvalidated: return "UNVALIDATED";
    case UpgradeRecordStreamCode::TooLarge: return "TOO_LARGE";
    case UpgradeRecordStreamCode::IoError: return "IO_ERROR";
  }
  return "UNKNOWN";
}

const char* upgradeSnapshotCollectCodeName(UpgradeSnapshotCollectCode code) {
  switch (code) {
    case UpgradeSnapshotCollectCode::Ok: return "OK";
    case UpgradeSnapshotCollectCode::Changed: return "CHANGED";
    case UpgradeSnapshotCollectCode::Ambiguous: return "AMBIGUOUS";
    case UpgradeSnapshotCollectCode::IoError: return "IO_ERROR";
    case UpgradeSnapshotCollectCode::UnsupportedSchema:
      return "UNSUPPORTED_SCHEMA";
    case UpgradeSnapshotCollectCode::TooLarge: return "TOO_LARGE";
    case UpgradeSnapshotCollectCode::InvalidEvidence:
      return "INVALID_EVIDENCE";
  }
  return "UNKNOWN";
}

}  // namespace storage
}  // namespace inkloop
