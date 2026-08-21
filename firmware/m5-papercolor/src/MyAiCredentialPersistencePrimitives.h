#pragma once

#include <stdint.h>

namespace inkloop {

// Preferences::begin(readOnly=true) reports NOT_FOUND for a namespace that
// has never existed. The credential adapter therefore opens read/write without
// writing a key, then classifies the namespace contents explicitly.
struct MyAiCredentialStorageProbe {
  bool namespaceAvailable = false;
  bool markerPresent = false;
  bool markerValid = false;
  bool headPresent = false;
  bool headValid = false;
  bool slot0Present = false;
  bool slot1Present = false;
  bool committedSlotValid = false;
};

enum class MyAiCredentialLoadResult : uint8_t {
  Unavailable,
  Absent,
  Loaded,
  LoadedLegacy,
  Corrupt,
};

inline MyAiCredentialLoadResult classifyMyAiCredentialStorage(
    const MyAiCredentialStorageProbe& probe) {
  if (!probe.namespaceAvailable)
    return MyAiCredentialLoadResult::Unavailable;
  const bool anyKey = probe.markerPresent || probe.headPresent ||
      probe.slot0Present || probe.slot1Present;
  if (!anyKey) return MyAiCredentialLoadResult::Absent;
  if ((probe.markerPresent && !probe.markerValid) || !probe.headPresent ||
      !probe.headValid || !probe.committedSlotValid) {
    return MyAiCredentialLoadResult::Corrupt;
  }
  return probe.markerPresent ? MyAiCredentialLoadResult::Loaded
                             : MyAiCredentialLoadResult::LoadedLegacy;
}

}  // namespace inkloop
