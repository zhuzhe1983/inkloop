#include "inkloop/native_device_state_owner.hpp"

#include "inkloop/board_prompt_policy.hpp"

namespace inkloop {
namespace {

local_tools::AdapterResult local(local_tools::AdapterCode code) {
  return {code};
}

bool capacity(storage::PosixAtomicAlbumStore* album,
              local_tools::StorageInfo& output) {
  output = local_tools::StorageInfo{};
  return album &&
      album->queryCapacity(output.total_bytes, output.remaining_bytes);
}

}  // namespace

settings::DeviceSettings NativeDeviceStateOwner::defaultsFor(
    const BoardDescriptor& board) {
  settings::DeviceSettings output = settings::makeGenericDeviceDefaults();
  output.assistant_prompt = defaultAssistantPrompt(board);
  output.aigc_prompt_template = defaultImagePromptTemplate(board);
  output.negative_prompt = defaultNegativePrompt(board);
  // A malformed third-party descriptor cannot inject oversized defaults into
  // the settings journal. Fall back to the conservative SKU-neutral profile.
  if (!settings::validDeviceSettings(output))
    output = settings::makeGenericDeviceDefaults();
  return output;
}

NativeDeviceStateOwner::NativeDeviceStateOwner(
    const BoardDescriptor& board, storage::EspStorageMountOwner& storage)
    : storage_(storage), defaults_(defaultsFor(board)),
      store_(journal_, defaults_) {}

esp_err_t NativeDeviceStateOwner::attachStorageMaintenanceCoordinator(
    IStorageMaintenanceCoordinator& coordinator) {
  if (storage_maintenance_) return ESP_ERR_INVALID_STATE;
  storage_maintenance_ = &coordinator;
  return ESP_OK;
}

bool NativeDeviceStateOwner::take() const {
  return mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100U)) == pdTRUE;
}

void NativeDeviceStateOwner::give() const {
  if (mutex_) xSemaphoreGive(mutex_);
}

esp_err_t NativeDeviceStateOwner::initialize() {
  if (initialized_) return ESP_ERR_INVALID_STATE;
  mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
  if (!mutex_) return ESP_ERR_NO_MEM;
  settings::SettingsStatus loaded = store_.load(snapshot_);
  if (!loaded.ok()) return ESP_FAIL;

  // A verified Arduino journal is the user's own operational state. The
  // product composition explicitly accepts it on a fresh native journal so
  // the migration is painless, while the legacy source itself remains strict
  // NVS_READONLY and rollback-safe.
  if (snapshot_.generation == 0U) {
    settings::LegacySettingsImport candidate;
    loaded = settings::inspectLegacyPortalSettings(
        legacy_, legacy_sha_, defaults_, candidate);
    if (!loaded.ok()) return ESP_FAIL;
    if (candidate.state == settings::LegacyImportState::Candidate) {
      settings::SettingsSnapshot committed;
      loaded = store_.save(candidate.values, 0U, committed);
      if (!loaded.ok()) return ESP_FAIL;
      snapshot_ = std::move(committed);
    }
  }
  const storage::AssetStoragePreference requested =
      storagePreference(snapshot_.values.asset_storage_preference);
  boot_effective_preference_ = storage_.selectedAssetRoot(requested)
      ? requested : storage::AssetStoragePreference::Automatic;
  initialized_ = true;
  return ESP_OK;
}

settings::SettingsSnapshot NativeDeviceStateOwner::snapshot() const {
  if (!initialized_ || !take()) return settings::SettingsSnapshot{};
  const settings::SettingsSnapshot value = snapshot_;
  give();
  return value;
}

storage::AssetStoragePreference NativeDeviceStateOwner::storagePreference(
    settings::AssetStoragePreference value) {
  switch (value) {
    case settings::AssetStoragePreference::Internal:
      return storage::AssetStoragePreference::Internal;
    case settings::AssetStoragePreference::Removable:
      return storage::AssetStoragePreference::SdCard;
    case settings::AssetStoragePreference::Automatic:
      return storage::AssetStoragePreference::Automatic;
  }
  return storage::AssetStoragePreference::Automatic;
}

storage::AssetStoragePreference
NativeDeviceStateOwner::effectiveAssetPreference() const {
  return initialized_ ? boot_effective_preference_
                      : storage::AssetStoragePreference::Automatic;
}

const char* NativeDeviceStateOwner::selectedRoot() const {
  return storage_.selectedAssetRoot(effectiveAssetPreference());
}

storage::PosixAtomicAlbumStore* NativeDeviceStateOwner::selectedAlbum() const {
  return storage_.selectedAlbumStore(effectiveAssetPreference());
}

portal::PortalSettingsSnapshot NativeDeviceStateOwner::portalSnapshot(
    const settings::DeviceSettings& value) {
  portal::PortalSettingsSnapshot output;
  output.volume = value.volume_percent;
  output.led_maximum_brightness_percent =
      value.led_maximum_brightness_percent;
  output.voice_assistance_enabled = value.voice_assistance_enabled;
  output.assistant_prompt = value.assistant_prompt;
  output.image_prompt_template = value.aigc_prompt_template;
  output.negative_prompt = value.negative_prompt;
  output.asset_storage_preference =
      settings::assetStoragePreferenceName(value.asset_storage_preference);
  output.default_render_strategy = value.default_render_strategy;
  output.local_management_password_overridden =
      !value.local_management_password_override.empty();
  return output;
}

portal::PortalResult NativeDeviceStateOwner::portalSettingsResult(
    const settings::SettingsStatus& status) {
  switch (status.code) {
    case settings::SettingsError::None:
      return portal::PortalResult::Ok;
    case settings::SettingsError::InvalidArgument:
    case settings::SettingsError::TooLarge:
      return portal::PortalResult::InvalidData;
    case settings::SettingsError::Conflict:
    case settings::SettingsError::InvalidState:
      return portal::PortalResult::Busy;
    case settings::SettingsError::Storage:
    case settings::SettingsError::Corrupt:
    case settings::SettingsError::Exhausted:
      return portal::PortalResult::Unavailable;
  }
  return portal::PortalResult::Unavailable;
}

local_tools::AdapterResult NativeDeviceStateOwner::localSettingsResult(
    const settings::SettingsStatus& status) {
  switch (status.code) {
    case settings::SettingsError::None:
      return local(local_tools::AdapterCode::Ok);
    case settings::SettingsError::InvalidArgument:
    case settings::SettingsError::TooLarge:
      return local(local_tools::AdapterCode::Unsupported);
    case settings::SettingsError::Conflict:
    case settings::SettingsError::InvalidState:
      return local(local_tools::AdapterCode::Conflict);
    case settings::SettingsError::Storage:
    case settings::SettingsError::Corrupt:
    case settings::SettingsError::Exhausted:
      return local(local_tools::AdapterCode::IoError);
  }
  return local(local_tools::AdapterCode::IoError);
}

local_tools::AdapterResult NativeDeviceStateOwner::albumResult(
    storage::AlbumMutationCode code) {
  switch (code) {
    case storage::AlbumMutationCode::Ok:
      return local(local_tools::AdapterCode::Ok);
    case storage::AlbumMutationCode::Busy:
      return local(local_tools::AdapterCode::Conflict);
    case storage::AlbumMutationCode::NotFound:
      return local(local_tools::AdapterCode::NotFound);
    case storage::AlbumMutationCode::RecoveryRequired:
      return local(local_tools::AdapterCode::NotReady);
    case storage::AlbumMutationCode::PersistenceFailed:
    case storage::AlbumMutationCode::UnlinkFailed:
      return local(local_tools::AdapterCode::IoError);
  }
  return local(local_tools::AdapterCode::IoError);
}

settings::SettingsStatus NativeDeviceStateOwner::commitLocked(
    const settings::DeviceSettings& next) {
  settings::SettingsSnapshot committed;
  const settings::SettingsStatus status =
      store_.save(next, snapshot_.generation, committed);
  if (status.ok()) snapshot_ = std::move(committed);
  return status;
}

portal::PortalResult NativeDeviceStateOwner::readPortalSettings(
    portal::PortalSettingsSnapshot& output) const {
  output = portal::PortalSettingsSnapshot{};
  if (!initialized_ || !take()) return portal::PortalResult::Unavailable;
  output = portalSnapshot(snapshot_.values);
  give();
  return portal::PortalResult::Ok;
}

portal::PortalResult NativeDeviceStateOwner::applyPortalSettings(
    const portal::PortalSettingsPatch& patch,
    portal::PortalSettingsSnapshot& output) {
  output = portal::PortalSettingsSnapshot{};
  if (!initialized_ || !take()) return portal::PortalResult::Unavailable;
  settings::DeviceSettings next = snapshot_.values;
  if (patch.has_volume) next.volume_percent = patch.volume;
  if (patch.has_led_maximum_brightness) {
    next.led_maximum_brightness_percent =
        patch.led_maximum_brightness_percent;
  }
  if (patch.has_voice_assistance_enabled)
    next.voice_assistance_enabled = patch.voice_assistance_enabled;
  if (patch.has_assistant_prompt) next.assistant_prompt = patch.assistant_prompt;
  if (patch.has_image_prompt_template)
    next.aigc_prompt_template = patch.image_prompt_template;
  if (patch.has_negative_prompt) next.negative_prompt = patch.negative_prompt;
  if (patch.has_asset_storage_preference) {
    if (patch.asset_storage_preference == "automatic") {
      next.asset_storage_preference =
          settings::AssetStoragePreference::Automatic;
    } else if (patch.asset_storage_preference == "internal") {
      next.asset_storage_preference =
          settings::AssetStoragePreference::Internal;
    } else if (patch.asset_storage_preference == "removable") {
      next.asset_storage_preference =
          settings::AssetStoragePreference::Removable;
    } else {
      give();
      return portal::PortalResult::InvalidData;
    }
  }
  if (patch.has_default_render_strategy)
    next.default_render_strategy = patch.default_render_strategy;
  if (patch.has_local_management_password_override) {
    next.local_management_password_override =
        patch.local_management_password_override;
  }
  const settings::SettingsStatus saved = commitLocked(next);
  if (saved.ok()) output = portalSnapshot(snapshot_.values);
  give();
  return portalSettingsResult(saved);
}

portal::PortalResult NativeDeviceStateOwner::deletePortalAlbumItem(
    const std::string& exact_asset_id) {
  if (!initialized_) return portal::PortalResult::Unavailable;
  storage::PosixAtomicAlbumStore* album = selectedAlbum();
  if (!album) return portal::PortalResult::Unavailable;
  switch (album->removeAssetById(exact_asset_id)) {
    case storage::AlbumMutationCode::Ok:
      return portal::PortalResult::Ok;
    case storage::AlbumMutationCode::Busy:
      return portal::PortalResult::Busy;
    case storage::AlbumMutationCode::NotFound:
      return portal::PortalResult::InvalidData;
    case storage::AlbumMutationCode::RecoveryRequired:
    case storage::AlbumMutationCode::PersistenceFailed:
    case storage::AlbumMutationCode::UnlinkFailed:
      return portal::PortalResult::Unavailable;
  }
  return portal::PortalResult::Unavailable;
}

local_tools::AdapterResult NativeDeviceStateOwner::queryStorage(
    local_tools::StorageInfo& output) {
  output = local_tools::StorageInfo{};
  if (!initialized_) return local(local_tools::AdapterCode::NotReady);
  return capacity(selectedAlbum(), output)
      ? local(local_tools::AdapterCode::Ok)
      : local(local_tools::AdapterCode::NotReady);
}

local_tools::AdapterResult NativeDeviceStateOwner::deleteImageByOrdinal(
    uint32_t one_based_ordinal) {
  if (!initialized_) return local(local_tools::AdapterCode::NotReady);
  storage::PosixAtomicAlbumStore* album = selectedAlbum();
  if (!album || one_based_ordinal == 0U)
    return local(local_tools::AdapterCode::NotReady);
  return albumResult(album->removeAssetByOrdinal(one_based_ordinal - 1U));
}

local_tools::AdapterResult NativeDeviceStateOwner::deleteImageById(
    const std::string& exact_id) {
  if (!initialized_) return local(local_tools::AdapterCode::NotReady);
  storage::PosixAtomicAlbumStore* album = selectedAlbum();
  return album ? albumResult(album->removeAssetById(exact_id))
               : local(local_tools::AdapterCode::NotReady);
}

local_tools::AdapterResult NativeDeviceStateOwner::clearAlbum() {
  if (!initialized_) return local(local_tools::AdapterCode::NotReady);
  storage::PosixAtomicAlbumStore* album = selectedAlbum();
  if (!album) return local(local_tools::AdapterCode::NotReady);
  size_t removed = 0;
  return albumResult(album->clearAssets(removed));
}

local_tools::AdapterResult NativeDeviceStateOwner::queryVolume(
    uint8_t& percent) {
  percent = 0U;
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  percent = snapshot_.values.volume_percent;
  give();
  return local(local_tools::AdapterCode::Ok);
}

local_tools::AdapterResult NativeDeviceStateOwner::setVolume(uint8_t percent) {
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  settings::DeviceSettings next = snapshot_.values;
  next.volume_percent = percent;
  const settings::SettingsStatus saved = commitLocked(next);
  give();
  return localSettingsResult(saved);
}

local_tools::AdapterResult NativeDeviceStateOwner::formatTfCard() {
  if (!initialized_) return local(local_tools::AdapterCode::NotReady);
  if (!storage_maintenance_) return local(local_tools::AdapterCode::NotReady);
  const StorageMaintenanceResult result =
      storage_maintenance_->formatTfCardConfirmed();
  switch (result.code) {
    case StorageMaintenanceCode::Ok:
      return local(local_tools::AdapterCode::Ok);
    case StorageMaintenanceCode::Busy:
      return local(local_tools::AdapterCode::Conflict);
    case StorageMaintenanceCode::NotReady:
      return local(local_tools::AdapterCode::NotReady);
    case StorageMaintenanceCode::IoError:
      return local(local_tools::AdapterCode::IoError);
  }
  return local(local_tools::AdapterCode::IoError);
}

local_tools::AdapterResult NativeDeviceStateOwner::queryAssistantPrompt(
    std::string& output) {
  output.clear();
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  output = snapshot_.values.assistant_prompt;
  give();
  return local(local_tools::AdapterCode::Ok);
}

local_tools::AdapterResult NativeDeviceStateOwner::setAssistantPrompt(
    const std::string& prompt) {
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  settings::DeviceSettings next = snapshot_.values;
  next.assistant_prompt = prompt;
  const settings::SettingsStatus saved = commitLocked(next);
  give();
  return localSettingsResult(saved);
}

local_tools::AdapterResult NativeDeviceStateOwner::queryAigcPrompt(
    std::string& output) {
  output.clear();
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  output = snapshot_.values.aigc_prompt_template;
  give();
  return local(local_tools::AdapterCode::Ok);
}

local_tools::AdapterResult NativeDeviceStateOwner::queryAigcNegativePrompt(
    std::string& output) {
  output.clear();
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  output = snapshot_.values.negative_prompt;
  give();
  return local(local_tools::AdapterCode::Ok);
}

local_tools::AdapterResult NativeDeviceStateOwner::queryDefaultRenderStrategy(
    std::string& output) {
  output.clear();
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  output = snapshot_.values.default_render_strategy;
  give();
  return local(local_tools::AdapterCode::Ok);
}

local_tools::AdapterResult NativeDeviceStateOwner::setAigcPrompt(
    const std::string& prompt) {
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  settings::DeviceSettings next = snapshot_.values;
  next.aigc_prompt_template = prompt;
  const settings::SettingsStatus saved = commitLocked(next);
  give();
  return localSettingsResult(saved);
}

local_tools::AdapterResult NativeDeviceStateOwner::setLedMaximumBrightness(
    uint8_t percent) {
  if (!initialized_ || !take()) return local(local_tools::AdapterCode::NotReady);
  settings::DeviceSettings next = snapshot_.values;
  next.led_maximum_brightness_percent = percent;
  const settings::SettingsStatus saved = commitLocked(next);
  give();
  return localSettingsResult(saved);
}

}  // namespace inkloop
