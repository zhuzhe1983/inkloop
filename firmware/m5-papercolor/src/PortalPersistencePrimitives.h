#pragma once

#include <stdint.h>

namespace inkloop {

enum class PortalSnapshotLoadResult : uint8_t {
  Loaded,
  LoadedLegacy,
  Absent,
  Corrupt,
  Unavailable,
};

enum class PortalIdentityState : uint8_t {
  Unconfigured,
  Pairing,
  BoundActive,
  BoundInactive,
};

struct PortalStorageProbe {
  bool namespaceAvailable;
  bool markerPresent;
  bool markerValid;
  bool headPresent;
  bool headValid;
  bool slotAPresent;
  bool slotBPresent;
  bool headSlotValid;
  bool fallbackSlotValid;

  PortalStorageProbe()
      : namespaceAvailable(false), markerPresent(false), markerValid(false),
        headPresent(false), headValid(false), slotAPresent(false),
        slotBPresent(false), headSlotValid(false), fallbackSlotValid(false) {}
};

inline PortalSnapshotLoadResult classifyPortalStorage(
    const PortalStorageProbe& probe) {
  if (!probe.namespaceAvailable)
    return PortalSnapshotLoadResult::Unavailable;
  const bool noRecordMaterial = !probe.markerPresent && !probe.headPresent &&
      !probe.slotAPresent && !probe.slotBPresent;
  if (noRecordMaterial) return PortalSnapshotLoadResult::Absent;
  if (probe.markerPresent && !probe.markerValid)
    return PortalSnapshotLoadResult::Corrupt;
  if (!probe.headPresent || !probe.headValid ||
      (!probe.headSlotValid && !probe.fallbackSlotValid)) {
    return PortalSnapshotLoadResult::Corrupt;
  }
  return probe.markerPresent ? PortalSnapshotLoadResult::Loaded
                             : PortalSnapshotLoadResult::LoadedLegacy;
}

inline bool portalStorageMayInitializeFresh(
    PortalSnapshotLoadResult result, PortalIdentityState identity) {
  return result == PortalSnapshotLoadResult::Absent &&
      identity == PortalIdentityState::Unconfigured;
}

inline bool portalIdentityMatchesSnapshot(
    PortalIdentityState identity, bool inkloopBound, bool myAiActive,
    bool hasTransientCode, bool portalPairingPending) {
  if (identity == PortalIdentityState::BoundActive ||
      identity == PortalIdentityState::BoundInactive) {
    return inkloopBound;
  }
  if (identity == PortalIdentityState::Unconfigured)
    return !inkloopBound && !myAiActive && !hasTransientCode &&
        !portalPairingPending;
  return !myAiActive;
}

}  // namespace inkloop
