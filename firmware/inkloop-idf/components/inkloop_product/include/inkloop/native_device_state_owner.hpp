#pragma once

#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inkloop/board.hpp"
#include "inkloop/local_tools/local_tools.hpp"
#include "inkloop/native_portal_owner.hpp"
#include "inkloop/settings/device_settings.hpp"
#include "inkloop/settings/esp_nvs_settings_store.hpp"
#include "inkloop/settings/settings_journal.hpp"
#include "inkloop/storage/esp_storage_mount.hpp"
#include "inkloop/storage_maintenance.hpp"

namespace inkloop {

// Sole native operational-settings and local-mutation owner. It bridges the
// WebUI and voice local-tool contracts to one atomic NVS journal and one
// selected storage/album owner; neither caller invents a second persistence
// path. All mutating methods run on the low-priority Portal lane.
class NativeDeviceStateOwner final : public local_tools::ILocalToolsAdapter,
                                     public IPortalSettingsOwner,
                                     public IPortalAlbumMutationOwner {
 public:
  NativeDeviceStateOwner(const BoardDescriptor& board,
                         storage::EspStorageMountOwner& storage);

  esp_err_t initialize();
  esp_err_t attachStorageMaintenanceCoordinator(
      IStorageMaintenanceCoordinator& coordinator);
  bool ready() const { return initialized_; }
  storage::AssetStoragePreference effectiveAssetPreference() const;
  settings::SettingsSnapshot snapshot() const;

  portal::PortalResult readPortalSettings(
      portal::PortalSettingsSnapshot& output) const override;
  portal::PortalResult applyPortalSettings(
      const portal::PortalSettingsPatch& patch,
      portal::PortalSettingsSnapshot& output) override;
  portal::PortalResult deletePortalAlbumItem(
      const std::string& exact_asset_id) override;

  local_tools::AdapterResult queryStorage(
      local_tools::StorageInfo& output) override;
  local_tools::AdapterResult deleteImageByOrdinal(
      uint32_t one_based_ordinal) override;
  local_tools::AdapterResult deleteImageById(
      const std::string& exact_id) override;
  local_tools::AdapterResult clearAlbum() override;
  local_tools::AdapterResult queryVolume(uint8_t& percent) override;
  local_tools::AdapterResult setVolume(uint8_t percent) override;
  local_tools::AdapterResult formatTfCard() override;
  local_tools::AdapterResult queryAssistantPrompt(
      std::string& output) override;
  local_tools::AdapterResult setAssistantPrompt(
      const std::string& prompt) override;
  local_tools::AdapterResult queryAigcPrompt(std::string& output) override;
  local_tools::AdapterResult queryAigcNegativePrompt(
      std::string& output) override;
  local_tools::AdapterResult queryDefaultRenderStrategy(
      std::string& output) override;
  local_tools::AdapterResult setAigcPrompt(
      const std::string& prompt) override;
  local_tools::AdapterResult setLedMaximumBrightness(
      uint8_t percent) override;

 private:
  static settings::DeviceSettings defaultsFor(const BoardDescriptor& board);
  static storage::AssetStoragePreference storagePreference(
      settings::AssetStoragePreference value);
  static portal::PortalSettingsSnapshot portalSnapshot(
      const settings::DeviceSettings& value);
  static portal::PortalResult portalSettingsResult(
      const settings::SettingsStatus& status);
  static local_tools::AdapterResult localSettingsResult(
      const settings::SettingsStatus& status);
  static local_tools::AdapterResult albumResult(
      storage::AlbumMutationCode code);

  bool take() const;
  void give() const;
  settings::SettingsStatus commitLocked(
      const settings::DeviceSettings& next);
  storage::PosixAtomicAlbumStore* selectedAlbum() const;
  const char* selectedRoot() const;

  storage::EspStorageMountOwner& storage_;
  IStorageMaintenanceCoordinator* storage_maintenance_ = nullptr;
  settings::DeviceSettings defaults_;
  settings::EspNvsSettingsJournalStore journal_{};
  settings::SettingsStoreCore store_;
  settings::EspNvsReadOnlyLegacyPortalSource legacy_{};
  settings::EspPsaLegacySha256Verifier legacy_sha_{};
  mutable StaticSemaphore_t mutex_storage_{};
  mutable SemaphoreHandle_t mutex_ = nullptr;
  settings::SettingsSnapshot snapshot_{};
  // Captured once after journal/legacy load. Online settings updates are
  // persisted for the next boot but cannot make owners operate on different
  // albums during the current boot.
  storage::AssetStoragePreference boot_effective_preference_ =
      storage::AssetStoragePreference::Automatic;
  bool initialized_ = false;
};

}  // namespace inkloop
