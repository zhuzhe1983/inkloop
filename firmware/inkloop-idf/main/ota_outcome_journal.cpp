#include "ota_outcome_journal.hpp"

#include <algorithm>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace inkloop {
namespace {

constexpr std::array<std::uint8_t, 4U> kMagic{{'I', 'O', 'T', 'A'}};
constexpr std::uint8_t kSchemaVersion = 1U;
constexpr std::size_t kSchemaAt = 4U;
constexpr std::size_t kKindAt = 5U;
constexpr std::size_t kCodeAt = 6U;
constexpr std::size_t kBootAgeAt = 7U;
constexpr std::size_t kSequenceAt = 8U;
constexpr std::size_t kRequestIdAt = 16U;
constexpr std::size_t kVersionFingerprintAt = 24U;
constexpr std::size_t kChecksumAt = 36U;
constexpr std::uint32_t kSnapshotAvailable = 1U << 16U;

void put32(std::uint32_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

void put64(std::uint64_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 64U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
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

std::uint64_t fingerprint(OtaTextView value) {
  if (!value.data || value.length == 0U ||
      value.length > kMaximumOtaFirmwareVersionBytes)
    return 0U;
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t at = 0U; at < value.length; ++at) {
    hash ^= static_cast<std::uint8_t>(value.data[at]);
    hash *= 1099511628211ULL;
  }
  return hash == 0U ? 1U : hash;
}

bool terminalFailure(OtaUpdateCode code) {
  switch (code) {
    case OtaUpdateCode::QuiesceFailed:
    case OtaUpdateCode::PlatformUnavailable:
    case OtaUpdateCode::VerifierUnavailable:
    case OtaUpdateCode::AcquisitionInvalidState:
    case OtaUpdateCode::AcquisitionConfigurationRejected:
    case OtaUpdateCode::DeadlineExceeded:
    case OtaUpdateCode::ManifestFetchFailed:
    case OtaUpdateCode::ManifestRejected:
    case OtaUpdateCode::ImageOriginMismatch:
    case OtaUpdateCode::StagingBeginFailed:
    case OtaUpdateCode::ImageFetchFailed:
    case OtaUpdateCode::StagingFinishFailed:
      return true;
    case OtaUpdateCode::Ok:
    case OtaUpdateCode::Ready:
    case OtaUpdateCode::ConfigurationMissing:
    case OtaUpdateCode::ManifestUrlRejected:
    case OtaUpdateCode::PlaceholderEndpointRejected:
    case OtaUpdateCode::PublicKeyRejected:
    case OtaUpdateCode::DeadlineRejected:
    case OtaUpdateCode::Disabled:
    case OtaUpdateCode::InvalidRequestId:
    case OtaUpdateCode::DuplicateRequest:
    case OtaUpdateCode::Busy:
    case OtaUpdateCode::NoRequest:
    case OtaUpdateCode::RequestMismatch:
    case OtaUpdateCode::InvalidTerminalCode:
    case OtaUpdateCode::ImageSelected:
      return false;
  }
  return false;
}

bool recordValid(const OtaOutcomeJournal::PersistentRecord& record) {
  if (record.sequence == 0U || record.request_id == 0U ||
      record.source_version_fingerprint == 0U ||
      record.boot_age > kMaximumOtaOutcomeBootAge)
    return false;
  switch (record.kind) {
    case OtaOutcomeKind::ImageSelected:
    case OtaOutcomeKind::Confirmed:
    case OtaOutcomeKind::RollbackObserved:
      return record.code == OtaUpdateCode::ImageSelected;
    case OtaOutcomeKind::AcquisitionFailed:
      return terminalFailure(record.code);
    case OtaOutcomeKind::None:
      return false;
  }
  return false;
}

bool recordsEqual(const OtaOutcomeJournal::PersistentRecord& left,
                  const OtaOutcomeJournal::PersistentRecord& right) {
  return left.sequence == right.sequence &&
      left.request_id == right.request_id &&
      left.source_version_fingerprint == right.source_version_fingerprint &&
      left.kind == right.kind && left.code == right.code &&
      left.boot_age == right.boot_age;
}

bool encode(const OtaOutcomeJournal::PersistentRecord& record,
            EncodedOtaOutcomeSlot& output) {
  output.fill(0U);
  if (!recordValid(record)) return false;
  std::copy(kMagic.begin(), kMagic.end(), output.begin());
  output[kSchemaAt] = kSchemaVersion;
  output[kKindAt] = static_cast<std::uint8_t>(record.kind);
  output[kCodeAt] = static_cast<std::uint8_t>(record.code);
  output[kBootAgeAt] = record.boot_age;
  put64(record.sequence, output.data() + kSequenceAt);
  put64(record.request_id, output.data() + kRequestIdAt);
  put64(record.source_version_fingerprint,
        output.data() + kVersionFingerprintAt);
  put32(crc32(output.data(), kChecksumAt), output.data() + kChecksumAt);
  return true;
}

bool decode(const RawOtaOutcomeSlot& raw,
            OtaOutcomeJournal::PersistentRecord& record) {
  record = OtaOutcomeJournal::PersistentRecord{};
  if (!raw.present || raw.length != raw.bytes.size() ||
      !std::equal(kMagic.begin(), kMagic.end(), raw.bytes.begin()) ||
      raw.bytes[kSchemaAt] != kSchemaVersion ||
      crc32(raw.bytes.data(), kChecksumAt) !=
          get32(raw.bytes.data() + kChecksumAt)) {
    return false;
  }
  record.sequence = get64(raw.bytes.data() + kSequenceAt);
  record.request_id = get64(raw.bytes.data() + kRequestIdAt);
  record.source_version_fingerprint =
      get64(raw.bytes.data() + kVersionFingerprintAt);
  record.kind = static_cast<OtaOutcomeKind>(raw.bytes[kKindAt]);
  record.code = static_cast<OtaUpdateCode>(raw.bytes[kCodeAt]);
  record.boot_age = raw.bytes[kBootAgeAt];
  return recordValid(record);
}

}  // namespace

OtaOutcomeSnapshot OtaOutcomeJournal::snapshot() const {
  const AtomicSnapshot value = snapshot_.load(std::memory_order_acquire);
  OtaOutcomeSnapshot output;
  output.available = (value.kind_and_code & kSnapshotAvailable) != 0U;
  output.kind = static_cast<OtaOutcomeKind>(value.kind_and_code & 0xFFU);
  output.code = static_cast<OtaUpdateCode>(
      (value.kind_and_code >> 8U) & 0xFFU);
  output.request_id = value.request_id;
  if (!output.available) output = OtaOutcomeSnapshot{};
  return output;
}

void OtaOutcomeJournal::publish(const PersistentRecord& record) {
  AtomicSnapshot value;
  value.request_id = record.request_id;
  value.kind_and_code = kSnapshotAvailable |
      static_cast<std::uint32_t>(record.kind) |
      (static_cast<std::uint32_t>(record.code) << 8U);
  snapshot_.store(value, std::memory_order_release);
}

void OtaOutcomeJournal::clearSnapshot() {
  snapshot_.store(AtomicSnapshot{}, std::memory_order_release);
}

OtaOutcomeJournalCode OtaOutcomeJournal::inspect(
    PersistentRecord& record) const {
  record = PersistentRecord{};
  std::array<RawOtaOutcomeSlot, 2U> raw{};
  if (store_.inspectRaw(raw) != OtaOutcomeStoreCode::Ok)
    return OtaOutcomeJournalCode::IoError;
  const bool any = raw[0].present || raw[1].present;
  PersistentRecord decoded[2]{};
  const bool valid[2]{decode(raw[0], decoded[0]),
                      decode(raw[1], decoded[1])};
  if (!valid[0] && !valid[1])
    return any ? OtaOutcomeJournalCode::Corrupt
               : OtaOutcomeJournalCode::Missing;
  if (valid[0] && valid[1] &&
      decoded[0].sequence == decoded[1].sequence &&
      !recordsEqual(decoded[0], decoded[1])) {
    return OtaOutcomeJournalCode::Corrupt;
  }
  const std::size_t selected = valid[1] &&
      (!valid[0] || decoded[1].sequence > decoded[0].sequence) ? 1U : 0U;
  record = decoded[selected];
  return OtaOutcomeJournalCode::Ok;
}

OtaOutcomeJournalCode OtaOutcomeJournal::commit(PersistentRecord record) {
  if (record.sequence == UINT64_MAX)
    return OtaOutcomeJournalCode::Exhausted;
  ++record.sequence;
  EncodedOtaOutcomeSlot encoded{};
  if (!encode(record, encoded))
    return OtaOutcomeJournalCode::InvalidArgument;
  const std::uint8_t slot = static_cast<std::uint8_t>(record.sequence & 1U);
  if (store_.writeSlotAndCommit(slot, encoded) != OtaOutcomeStoreCode::Ok)
    return OtaOutcomeJournalCode::IoError;
  PersistentRecord read_back;
  if (inspect(read_back) != OtaOutcomeJournalCode::Ok ||
      !recordsEqual(record, read_back)) {
    return OtaOutcomeJournalCode::ReadBackFailed;
  }
  publish(read_back);
  return OtaOutcomeJournalCode::Ok;
}

OtaOutcomeJournalCode OtaOutcomeJournal::beginBoot(
    OtaTextView current_firmware_version, bool pending_verification) {
  clearSnapshot();
  const std::uint64_t current_fingerprint =
      fingerprint(current_firmware_version);
  if (current_fingerprint == 0U)
    return OtaOutcomeJournalCode::InvalidArgument;
  PersistentRecord record;
  const OtaOutcomeJournalCode inspected = inspect(record);
  if (inspected == OtaOutcomeJournalCode::Corrupt) {
    return store_.clearAndCommit() == OtaOutcomeStoreCode::Ok
        ? OtaOutcomeJournalCode::Corrupt
        : OtaOutcomeJournalCode::IoError;
  }
  if (inspected != OtaOutcomeJournalCode::Ok) return inspected;
  if (record.boot_age >= kMaximumOtaOutcomeBootAge) {
    if (store_.clearAndCommit() != OtaOutcomeStoreCode::Ok)
      return OtaOutcomeJournalCode::IoError;
    return OtaOutcomeJournalCode::Stale;
  }
  if (record.kind == OtaOutcomeKind::ImageSelected &&
      !pending_verification) {
    record.kind = current_fingerprint == record.source_version_fingerprint
        ? OtaOutcomeKind::RollbackObserved
        : OtaOutcomeKind::Confirmed;
  }
  ++record.boot_age;
  return commit(record);
}

OtaOutcomeJournalCode OtaOutcomeJournal::recordTerminal(
    const OtaUpdateRequest& request, OtaUpdateCode terminal_code,
    OtaTextView source_firmware_version) {
  const std::uint64_t source_fingerprint =
      fingerprint(source_firmware_version);
  if (request.request_id == 0U || source_fingerprint == 0U ||
      (terminal_code != OtaUpdateCode::ImageSelected &&
       !terminalFailure(terminal_code))) {
    return OtaOutcomeJournalCode::InvalidArgument;
  }
  PersistentRecord prior;
  const OtaOutcomeJournalCode inspected = inspect(prior);
  if (inspected == OtaOutcomeJournalCode::IoError)
    return inspected;
  if (inspected == OtaOutcomeJournalCode::Corrupt &&
      store_.clearAndCommit() != OtaOutcomeStoreCode::Ok) {
    return OtaOutcomeJournalCode::IoError;
  }
  PersistentRecord record;
  if (inspected == OtaOutcomeJournalCode::Ok) record.sequence = prior.sequence;
  record.request_id = request.request_id;
  record.source_version_fingerprint = source_fingerprint;
  record.kind = terminal_code == OtaUpdateCode::ImageSelected
      ? OtaOutcomeKind::ImageSelected
      : OtaOutcomeKind::AcquisitionFailed;
  record.code = terminal_code;
  return commit(record);
}

OtaOutcomeJournalCode OtaOutcomeJournal::recordConfirmed() {
  PersistentRecord record;
  const OtaOutcomeJournalCode inspected = inspect(record);
  if (inspected != OtaOutcomeJournalCode::Ok) return inspected;
  if (record.kind == OtaOutcomeKind::Confirmed) {
    publish(record);
    return OtaOutcomeJournalCode::Ok;
  }
  if (record.kind != OtaOutcomeKind::ImageSelected)
    return OtaOutcomeJournalCode::InvalidArgument;
  record.kind = OtaOutcomeKind::Confirmed;
  return commit(record);
}

#ifdef ESP_PLATFORM
namespace {

constexpr char kNvsNamespace[] = "ink-ota-out-v1";
constexpr char kSlotKeys[2][6]{"slot0", "slot1"};

}  // namespace

OtaOutcomeStoreCode EspNvsOtaOutcomeJournalStore::inspectRaw(
    std::array<RawOtaOutcomeSlot, 2U>& slots) const {
  slots = {};
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return OtaOutcomeStoreCode::Ok;
  if (opened != ESP_OK) return OtaOutcomeStoreCode::IoError;
  OtaOutcomeStoreCode code = OtaOutcomeStoreCode::Ok;
  for (std::size_t at = 0U; at < slots.size(); ++at) {
    std::size_t length = 0U;
    esp_err_t result = nvs_get_blob(handle, kSlotKeys[at], nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) continue;
    if (result != ESP_OK) {
      code = OtaOutcomeStoreCode::IoError;
      break;
    }
    slots[at].present = true;
    slots[at].length = length;
    if (length != slots[at].bytes.size()) continue;
    result = nvs_get_blob(handle, kSlotKeys[at], slots[at].bytes.data(),
                          &length);
    if (result != ESP_OK || length != slots[at].bytes.size()) {
      code = OtaOutcomeStoreCode::IoError;
      break;
    }
  }
  nvs_close(handle);
  if (code != OtaOutcomeStoreCode::Ok) slots = {};
  return code;
}

OtaOutcomeStoreCode EspNvsOtaOutcomeJournalStore::writeSlotAndCommit(
    std::uint8_t slot, const EncodedOtaOutcomeSlot& encoded) {
  if (slot > 1U) return OtaOutcomeStoreCode::InvalidArgument;
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return OtaOutcomeStoreCode::IoError;
  esp_err_t result =
      nvs_set_blob(handle, kSlotKeys[slot], encoded.data(), encoded.size());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? OtaOutcomeStoreCode::Ok
                          : OtaOutcomeStoreCode::IoError;
}

OtaOutcomeStoreCode EspNvsOtaOutcomeJournalStore::clearAndCommit() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return OtaOutcomeStoreCode::IoError;
  esp_err_t result = ESP_OK;
  for (const auto& key : kSlotKeys) {
    const esp_err_t erased = nvs_erase_key(handle, key);
    if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) {
      result = erased;
      break;
    }
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? OtaOutcomeStoreCode::Ok
                          : OtaOutcomeStoreCode::IoError;
}

OtaOutcomeJournal& systemOtaOutcomeJournal() {
  static EspNvsOtaOutcomeJournalStore store;
  static OtaOutcomeJournal journal(store);
  return journal;
}
#endif

const char* otaOutcomeKindName(OtaOutcomeKind kind) {
  switch (kind) {
    case OtaOutcomeKind::None: return "NONE";
    case OtaOutcomeKind::ImageSelected: return "IMAGE_SELECTED";
    case OtaOutcomeKind::AcquisitionFailed: return "ACQUISITION_FAILED";
    case OtaOutcomeKind::Confirmed: return "CONFIRMED";
    case OtaOutcomeKind::RollbackObserved: return "ROLLBACK_OBSERVED";
  }
  return "UNKNOWN";
}

const char* otaOutcomeJournalCodeName(OtaOutcomeJournalCode code) {
  switch (code) {
    case OtaOutcomeJournalCode::Ok: return "OK";
    case OtaOutcomeJournalCode::Missing: return "MISSING";
    case OtaOutcomeJournalCode::InvalidArgument: return "INVALID_ARGUMENT";
    case OtaOutcomeJournalCode::Corrupt: return "CORRUPT";
    case OtaOutcomeJournalCode::Stale: return "STALE";
    case OtaOutcomeJournalCode::Exhausted: return "EXHAUSTED";
    case OtaOutcomeJournalCode::IoError: return "IO_ERROR";
    case OtaOutcomeJournalCode::ReadBackFailed: return "READ_BACK_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
