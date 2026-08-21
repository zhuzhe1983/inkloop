#pragma once

namespace inkloop {

enum class RecoveryAssetMode {
  Internal,
  SdAssetsOnly,
  Unavailable,
};

struct StorageRecoveryState {
  bool internalMounted;
  bool internalRecoveryRequired;
  bool taskStoreReady;
  bool dataPreserved;
  RecoveryAssetMode assetMode;
};

constexpr StorageRecoveryState storageRecoveryState(
  bool internalMounted,
  bool sdMounted
) {
  return StorageRecoveryState{
    internalMounted,
    !internalMounted,
    internalMounted,
    true,
    internalMounted
      ? RecoveryAssetMode::Internal
      : (sdMounted ? RecoveryAssetMode::SdAssetsOnly : RecoveryAssetMode::Unavailable),
  };
}

constexpr bool taskControlAllowed(const StorageRecoveryState& state) {
  return state.taskStoreReady && state.internalMounted && !state.internalRecoveryRequired;
}

constexpr const char* recoveryAssetModeName(RecoveryAssetMode mode) {
  return mode == RecoveryAssetMode::Internal
    ? "internal"
    : (mode == RecoveryAssetMode::SdAssetsOnly ? "sd_assets_only" : "unavailable");
}

}  // namespace inkloop
