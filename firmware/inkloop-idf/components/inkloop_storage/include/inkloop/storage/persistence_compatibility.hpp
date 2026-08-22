#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/storage/upgrade_snapshot_collector.hpp"

namespace inkloop {
namespace storage {

// The first native image deliberately keeps the released Arduino data
// authorities in place.  This contract says how each fixed protected record
// is consumed and, crucially, whether booting the previous OTA image after a
// failed native boot can still understand the bytes written by the native
// image.
enum class PersistenceCompatibilityMode : std::uint8_t {
  // Both images use the same path/schema and native writes remain readable by
  // the Arduino image.
  SharedRollbackCompatible,
  // Native code reads the legacy record and writes only a separate native
  // namespace.  The legacy authority is never modified.
  ReadOnlyImportRetained,
  // The native image does not consume or mutate the record, but it remains
  // protected against erase/format for rollback and diagnostics.
  LegacyRetained,
  // Owned by ESP-IDF system components shared by both images.  Application
  // migration never copies, erases, or rewrites it.
  EspSystemShared,
  // Physical panel state cannot be inferred from bytes.  A human must select
  // target or previous before normal writers are allowed to start.
  ExplicitPhysicalResolution,
};

struct PersistenceCompatibilityEntry {
  UpgradeRecordId record{};
  const char* name = nullptr;
  PersistenceCompatibilityMode mode =
      PersistenceCompatibilityMode::LegacyRetained;
  const char* native_consumer = nullptr;
};

inline constexpr std::size_t kPersistenceCompatibilityEntryCount =
    kProtectedNvsNamespaces.size() + kProtectedFilePaths.size();

const std::array<PersistenceCompatibilityEntry,
                 kPersistenceCompatibilityEntryCount>&
persistenceCompatibilityContract();

// Validates exact order/name coverage and policy invariants.  This is called
// before any product writer starts so changing the protected surface without
// updating the compatibility contract fails closed.
bool persistenceCompatibilityContractValid();

const PersistenceCompatibilityEntry* persistenceCompatibilityEntry(
    UpgradeRecordId record);
const char* persistenceCompatibilityModeName(
    PersistenceCompatibilityMode mode);

}  // namespace storage
}  // namespace inkloop
