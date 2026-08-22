#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "MyAiAdapters.h"

namespace inkloop {
namespace myai {

inline constexpr size_t kMaximumCredentialRecordBytes = 8192;
inline constexpr uint8_t kCredentialInitializedMarker = 0xA7;

struct CredentialJournalState {
  bool namespaceAvailable = false;
  bool markerPresent = false;
  bool markerValid = false;
  bool headPresent = false;
  uint32_t head = 0;
  std::array<bool, 2> slotPresent{{false, false}};
  std::array<std::string, 2> slot;

  void redact();
};

class ICredentialJournalStore {
 public:
  virtual ~ICredentialJournalStore() = default;
  virtual Status inspect(CredentialJournalState& state) = 0;
  virtual Status writeSlotAndCommit(uint8_t slot,
                                    const std::string& encoded) = 0;
  virtual Status writeHeadAndMarkerAndCommit(uint32_t generation) = 0;
};

class ICredentialRecordCodec {
 public:
  virtual ~ICredentialRecordCodec() = default;
  virtual Status encode(const CredentialSnapshot& snapshot,
                        std::string& output) const = 0;
  virtual Status decode(const std::string& input,
                        CredentialSnapshot& snapshot) const = 0;
};

// Portable dual-slot state machine. It contains no NVS/Arduino dependency and
// runs only on the network owner. Tokens never enter status details or logs.
class CredentialPersistenceCore final : public ICredentialStore {
 public:
  CredentialPersistenceCore(ICredentialJournalStore& journal,
                            const ICredentialRecordCodec& codec);

  Status load(CredentialSnapshot& snapshot) override;
  Status initializeFingerprintAtomically(
      const std::string& installationFingerprint) override;
  Status savePendingAtomically(const PendingPairing& pending) override;
  Status promoteBoundAtomically(const std::string& expectedPairingToken,
                                const std::string& deviceId,
                                const std::string& deviceToken,
                                bool active) override;
  Status clearPendingAtomically() override;
  Status clearRuntimeCredentialAtomically() override;

 private:
  Status storeNext(CredentialSnapshot& snapshot);

  ICredentialJournalStore& journal_;
  const ICredentialRecordCodec& codec_;
};

bool credentialSnapshotCoherent(const CredentialSnapshot& snapshot);

}  // namespace myai
}  // namespace inkloop
