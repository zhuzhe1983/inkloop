#include "CredentialPersistence.h"

#include <limits>

namespace inkloop {
namespace myai {
namespace {

Status storageFailure(const char* detail) {
  return Status(ErrorCode::Storage, 0, detail);
}

void secureClear(std::string& value) {
  value.assign(value.size(), '\0');
  value.clear();
}

bool safeOpaque(const std::string& value, size_t maximum, bool empty_allowed) {
  if ((!empty_allowed && value.empty()) || value.size() > maximum) return false;
  for (unsigned char ch : value) {
    if (ch < 0x20U || ch == 0x7FU) return false;
  }
  return true;
}

}  // namespace

void CredentialJournalState::redact() {
  for (std::string& value : slot) secureClear(value);
}

bool credentialSnapshotCoherent(const CredentialSnapshot& snapshot) {
  if (snapshot.generation == 0 ||
      !safeOpaque(snapshot.installationFingerprint, 256, false) ||
      !safeOpaque(snapshot.deviceToken, 2048, true) ||
      !safeOpaque(snapshot.pending.pairingToken, 1024, true) ||
      !safeOpaque(snapshot.pending.bindingUrl, 1024, true) ||
      !safeOpaque(snapshot.pending.expiresAt, 128, true)) return false;
  const bool bound = !snapshot.deviceToken.empty();
  const bool pending = !snapshot.pending.empty();
  if (bound && pending) return false;
  if (bound) return isSixDigitCode(snapshot.deviceId);
  if (pending) {
    return !snapshot.active && snapshot.pending.valid() &&
           snapshot.pending.deviceId == snapshot.deviceId &&
           snapshot.pending.bindingUrl.rfind("https://", 0) == 0;
  }
  return snapshot.deviceId.empty() && !snapshot.active;
}

CredentialPersistenceCore::CredentialPersistenceCore(
    ICredentialJournalStore& journal, const ICredentialRecordCodec& codec)
    : journal_(journal), codec_(codec) {}

Status CredentialPersistenceCore::load(CredentialSnapshot& snapshot) {
  snapshot.redact();
  snapshot = CredentialSnapshot();
  CredentialJournalState state;
  Status status = journal_.inspect(state);
  if (!status.ok()) {
    state.redact();
    return status;
  }
  if (!state.namespaceAvailable) {
    state.redact();
    return storageFailure("credential namespace unavailable");
  }
  const bool any_key = state.markerPresent || state.headPresent ||
                       state.slotPresent[0] || state.slotPresent[1];
  if (!any_key) {
    state.redact();
    return Status::success();
  }
  if ((state.markerPresent && !state.markerValid) || !state.headPresent ||
      state.head == 0) {
    state.redact();
    return storageFailure("credential journal metadata invalid");
  }
  const uint32_t committed_generation = state.head;
  const uint8_t selected = static_cast<uint8_t>(committed_generation & 1U);
  if (!state.slotPresent[selected]) {
    state.redact();
    return storageFailure("credential committed slot missing");
  }
  CredentialSnapshot decoded;
  status = codec_.decode(state.slot[selected], decoded);
  state.redact();
  if (!status.ok() || decoded.generation != committed_generation ||
      !credentialSnapshotCoherent(decoded)) {
    decoded.redact();
    return storageFailure("credential committed slot invalid");
  }
  snapshot = decoded;
  return Status::success();
}

Status CredentialPersistenceCore::storeNext(CredentialSnapshot& snapshot) {
  CredentialSnapshot current;
  Status status = load(current);
  if (!status.ok()) return status;
  if (current.generation == std::numeric_limits<uint32_t>::max()) {
    current.redact();
    return storageFailure("credential generation exhausted");
  }
  const uint32_t expected = current.generation + 1U;
  current.redact();
  if (snapshot.generation != expected || !credentialSnapshotCoherent(snapshot))
    return storageFailure("credential generation conflict");

  std::string encoded;
  status = codec_.encode(snapshot, encoded);
  if (!status.ok() || encoded.empty() ||
      encoded.size() > kMaximumCredentialRecordBytes) {
    secureClear(encoded);
    return storageFailure("credential serialization failed");
  }
  const uint8_t slot = static_cast<uint8_t>(snapshot.generation & 1U);
  status = journal_.writeSlotAndCommit(slot, encoded);
  if (!status.ok()) {
    secureClear(encoded);
    return storageFailure("credential slot commit failed");
  }

  CredentialJournalState verify;
  status = journal_.inspect(verify);
  CredentialSnapshot decoded;
  const bool slot_valid = status.ok() && verify.namespaceAvailable &&
                          verify.slotPresent[slot] &&
                          codec_.decode(verify.slot[slot], decoded).ok() &&
                          decoded.generation == snapshot.generation &&
                          credentialSnapshotCoherent(decoded);
  verify.redact();
  decoded.redact();
  secureClear(encoded);
  if (!slot_valid) return storageFailure("credential slot verification failed");

  status = journal_.writeHeadAndMarkerAndCommit(snapshot.generation);
  if (!status.ok()) return storageFailure("credential head commit failed");
  CredentialJournalState committed;
  status = journal_.inspect(committed);
  const bool committed_valid = status.ok() && committed.namespaceAvailable &&
                               committed.markerPresent && committed.markerValid &&
                               committed.headPresent &&
                               committed.head == snapshot.generation &&
                               committed.slotPresent[slot];
  committed.redact();
  return committed_valid ? Status::success()
                         : storageFailure("credential commit verification failed");
}

Status CredentialPersistenceCore::initializeFingerprintAtomically(
    const std::string& installationFingerprint) {
  if (!safeOpaque(installationFingerprint, 256, false))
    return Status(ErrorCode::InvalidArgument, 0,
                  "invalid installation fingerprint");
  CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (!snapshot.installationFingerprint.empty()) {
    const bool same = snapshot.installationFingerprint == installationFingerprint;
    snapshot.redact();
    return same ? Status::success()
                : Status(ErrorCode::Conflict, 0, "installation fingerprint conflict");
  }
  snapshot.installationFingerprint = installationFingerprint;
  snapshot.generation = 1;
  return storeNext(snapshot);
}

Status CredentialPersistenceCore::savePendingAtomically(
    const PendingPairing& pending) {
  if (!pending.valid() || !safeOpaque(pending.pairingToken, 1024, false) ||
      !safeOpaque(pending.bindingUrl, 1024, false) ||
      !safeOpaque(pending.expiresAt, 128, false) ||
      pending.bindingUrl.rfind("https://", 0) != 0)
    return Status(ErrorCode::InvalidArgument, 0, "invalid pending pairing");
  CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (snapshot.installationFingerprint.empty() || snapshot.hasDeviceToken()) {
    snapshot.redact();
    return Status(ErrorCode::Conflict, 0, "credential state rejects pairing");
  }
  snapshot.deviceId = pending.deviceId;
  snapshot.pending = pending;
  snapshot.active = false;
  ++snapshot.generation;
  return storeNext(snapshot);
}

Status CredentialPersistenceCore::promoteBoundAtomically(
    const std::string& expectedPairingToken, const std::string& deviceId,
    const std::string& deviceToken, bool active) {
  if (!isSixDigitCode(deviceId) ||
      !safeOpaque(expectedPairingToken, 1024, false) ||
      !safeOpaque(deviceToken, 2048, false))
    return Status(ErrorCode::InvalidArgument, 0, "invalid bound credential");
  CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (!snapshot.pending.valid() ||
      snapshot.pending.pairingToken != expectedPairingToken ||
      snapshot.pending.deviceId != deviceId) {
    snapshot.redact();
    return Status(ErrorCode::Conflict, 0, "pending pairing changed");
  }
  snapshot.deviceId = deviceId;
  snapshot.deviceToken = deviceToken;
  snapshot.active = active;
  snapshot.pending.clearSensitive();
  snapshot.pending = PendingPairing();
  ++snapshot.generation;
  return storeNext(snapshot);
}

Status CredentialPersistenceCore::clearPendingAtomically() {
  CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (snapshot.installationFingerprint.empty()) {
    snapshot.redact();
    return Status(ErrorCode::InvalidState, 0, "credential not initialized");
  }
  snapshot.pending.clearSensitive();
  snapshot.pending = PendingPairing();
  if (snapshot.deviceToken.empty()) snapshot.deviceId.clear();
  ++snapshot.generation;
  return storeNext(snapshot);
}

Status CredentialPersistenceCore::clearRuntimeCredentialAtomically() {
  CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (snapshot.installationFingerprint.empty()) {
    snapshot.redact();
    return Status(ErrorCode::InvalidState, 0, "credential not initialized");
  }
  snapshot.pending.clearSensitive();
  snapshot.pending = PendingPairing();
  snapshot.deviceToken.assign(snapshot.deviceToken.size(), '\0');
  snapshot.deviceToken.clear();
  snapshot.deviceId.clear();
  snapshot.active = false;
  ++snapshot.generation;
  return storeNext(snapshot);
}

}  // namespace myai
}  // namespace inkloop
