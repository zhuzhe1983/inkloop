#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ota_update_owner.hpp"

namespace inkloop {

inline constexpr std::size_t kEncodedOtaOutcomeSlotBytes = 40U;
inline constexpr std::uint8_t kMaximumOtaOutcomeBootAge = 2U;

using EncodedOtaOutcomeSlot =
    std::array<std::uint8_t, kEncodedOtaOutcomeSlotBytes>;

enum class OtaOutcomeKind : std::uint8_t {
  None = 0U,
  ImageSelected = 1U,
  AcquisitionFailed = 2U,
  Confirmed = 3U,
  RollbackObserved = 4U,
};

struct OtaOutcomeSnapshot {
  OtaOutcomeKind kind = OtaOutcomeKind::None;
  OtaUpdateCode code = OtaUpdateCode::Ready;
  std::uint64_t request_id = 0U;
  bool available = false;
};

struct RawOtaOutcomeSlot {
  bool present = false;
  std::size_t length = 0U;
  EncodedOtaOutcomeSlot bytes{};
};

enum class OtaOutcomeStoreCode : std::uint8_t {
  Ok,
  InvalidArgument,
  IoError,
};

class IOtaOutcomeJournalStore {
 public:
  virtual ~IOtaOutcomeJournalStore() = default;
  virtual OtaOutcomeStoreCode inspectRaw(
      std::array<RawOtaOutcomeSlot, 2U>& slots) const = 0;
  virtual OtaOutcomeStoreCode writeSlotAndCommit(
      std::uint8_t slot, const EncodedOtaOutcomeSlot& encoded) = 0;
  virtual OtaOutcomeStoreCode clearAndCommit() = 0;
};

enum class OtaOutcomeJournalCode : std::uint8_t {
  Ok,
  Missing,
  InvalidArgument,
  Corrupt,
  Stale,
  Exhausted,
  IoError,
  ReadBackFailed,
};

// Dedicated two-slot journal for one credential-free OTA outcome. NVS commits
// make each slot write atomic; the sequence and CRC retain the older valid
// slot when the replacement is torn. Records expire after a bounded number of
// subsequent boots and are replaced by the next deliberate OTA attempt.
class OtaOutcomeJournal final {
 public:
  explicit OtaOutcomeJournal(IOtaOutcomeJournalStore& store)
      : store_(store) {}

  OtaOutcomeJournal(const OtaOutcomeJournal&) = delete;
  OtaOutcomeJournal& operator=(const OtaOutcomeJournal&) = delete;

  OtaOutcomeSnapshot snapshot() const;
  OtaOutcomeJournalCode beginBoot(OtaTextView current_firmware_version,
                                  bool pending_verification);
  OtaOutcomeJournalCode recordTerminal(
      const OtaUpdateRequest& request, OtaUpdateCode terminal_code,
      OtaTextView source_firmware_version);
  OtaOutcomeJournalCode recordConfirmed();

  // Exposed only so the fixed-format codec can remain a small freestanding
  // implementation detail in ota_outcome_journal.cpp. No secret or remote
  // content is represented by this record.
  struct PersistentRecord {
    std::uint64_t sequence = 0U;
    std::uint64_t request_id = 0U;
    std::uint64_t source_version_fingerprint = 0U;
    OtaOutcomeKind kind = OtaOutcomeKind::None;
    OtaUpdateCode code = OtaUpdateCode::Ready;
    std::uint8_t boot_age = 0U;
  };

 private:
  struct alignas(16) AtomicSnapshot {
    std::uint64_t request_id = 0U;
    std::uint32_t kind_and_code = 0U;
    std::uint32_t reserved = 0U;
  };

  static_assert(std::is_trivially_copyable<AtomicSnapshot>::value,
                "OTA outcome snapshot must be atomically copyable");
  static_assert(sizeof(AtomicSnapshot) == 16U,
                "OTA outcome atomic snapshot layout changed");

  OtaOutcomeJournalCode inspect(PersistentRecord& record) const;
  OtaOutcomeJournalCode commit(PersistentRecord record);
  void publish(const PersistentRecord& record);
  void clearSnapshot();

  IOtaOutcomeJournalStore& store_;
  std::atomic<AtomicSnapshot> snapshot_{};
};

#ifdef ESP_PLATFORM
class EspNvsOtaOutcomeJournalStore final
    : public IOtaOutcomeJournalStore {
 public:
  OtaOutcomeStoreCode inspectRaw(
      std::array<RawOtaOutcomeSlot, 2U>& slots) const override;
  OtaOutcomeStoreCode writeSlotAndCommit(
      std::uint8_t slot, const EncodedOtaOutcomeSlot& encoded) override;
  OtaOutcomeStoreCode clearAndCommit() override;
};

OtaOutcomeJournal& systemOtaOutcomeJournal();
#endif

const char* otaOutcomeKindName(OtaOutcomeKind kind);
const char* otaOutcomeJournalCodeName(OtaOutcomeJournalCode code);

}  // namespace inkloop
