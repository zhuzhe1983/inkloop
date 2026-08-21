#include "Storage.h"

#include <ff.h>
#include <sd_diskio.h>

namespace inkloop {

StorageBackendRef::StorageBackendRef(IStorageBackend* value)
  : backend(value), identity(value ? value->name() : "") {}

bool StorageBackendRef::valid() const {
  return backend && identity && identity[0] && strcmp(identity, backend->name()) == 0;
}

bool StorageBackendRef::available() const {
  return valid() && backend->capabilities().mounted;
}

bool LittleFsStorage::begin(bool formatOnFailure) {
  mounted_ = LittleFS.begin(formatOnFailure);
  return mounted_;
}

File LittleFsStorage::open(const char* path, const char* mode) {
  return LittleFS.open(path, mode);
}

bool LittleFsStorage::remove(const char* path) {
  return LittleFS.remove(path);
}

bool LittleFsStorage::rename(const char* from, const char* to) {
  return LittleFS.rename(from, to);
}

bool LittleFsStorage::exists(const char* path) {
  return LittleFS.exists(path);
}

bool LittleFsStorage::mkdir(const char* path) {
  return LittleFS.mkdir(path) || LittleFS.exists(path);
}

size_t LittleFsStorage::totalBytes() const {
  return mounted_ ? LittleFS.totalBytes() : 0;
}

size_t LittleFsStorage::usedBytes() const {
  return mounted_ ? LittleFS.usedBytes() : 0;
}

StorageCapabilities LittleFsStorage::capabilities() const {
  StorageCapabilities result;
  result.removable = false;
  result.writable = true;
  result.mounted = mounted_;
  return result;
}

bool SdStorage::begin(bool) {
  // M5Stack's official PaperColor SD wiring uses SPI pins 15/14/13 and CS 47.
  // The final Arduino-ESP32 argument is explicitly false: media is never
  // formatted by firmware.
  SPI.begin(15, 14, 13, 47);
  mounted_ = SD.begin(47, SPI, 25000000, "/sd", 8, false) && SD.cardType() != CARD_NONE;
  return mounted_;
}

File SdStorage::open(const char* path, const char* mode) {
  return mounted_ ? SD.open(path, mode) : File{};
}

bool SdStorage::remove(const char* path) {
  return mounted_ && SD.remove(path);
}

bool SdStorage::rename(const char* from, const char* to) {
  return mounted_ && SD.rename(from, to);
}

bool SdStorage::exists(const char* path) {
  return mounted_ && SD.exists(path);
}

bool SdStorage::mkdir(const char* path) {
  return mounted_ && (SD.mkdir(path) || SD.exists(path));
}

size_t SdStorage::totalBytes() const {
  return mounted_ ? static_cast<size_t>(SD.totalBytes()) : 0;
}

size_t SdStorage::usedBytes() const {
  return mounted_ ? static_cast<size_t>(SD.usedBytes()) : 0;
}

StorageCapabilities SdStorage::capabilities() const {
  StorageCapabilities result;
  result.removable = true;
  result.writable = true;
  result.mounted = mounted_ && SD.cardType() != CARD_NONE;
  return result;
}

bool SdStorage::formatFat() {
  SD.end();
  mounted_ = false;
  SPI.begin(15, 14, 13, 47);
  const uint8_t drive = sdcard_init(47, &SPI, 25000000);
  if (drive == 0xff) return false;
  char volume[3] = {static_cast<char>('0' + drive), ':', 0};
  uint8_t* work = static_cast<uint8_t*>(malloc(FF_MAX_SS));
  const FRESULT result = work ? f_mkfs(volume, FM_ANY, 0, work, FF_MAX_SS) : FR_NOT_ENOUGH_CORE;
  free(work);
  sdcard_unmount(drive);
  sdcard_uninit(drive);
  if (result != FR_OK) return false;
  return begin(false);
}

bool StorageManager::beginDataSafeMode() {
  // Album-capable firmware must never silently erase schedules or cached
  // images after a damaged mount. Recovery is explicit and device-visible.
  return baseline_.begin(false);
}

StorageBackendRef StorageManager::assetBackend() {
  if (preference_ == AssetPreference::Internal) return StorageBackendRef(&baseline_);
  if (preference_ == AssetPreference::SdCard) {
    return hasMountedSd() ? StorageBackendRef(optionalSd_) : StorageBackendRef();
  }
  return StorageBackendRef(hasMountedSd() ? optionalSd_ : &baseline_);
}

StorageBackendRef StorageManager::backendByIdentity(const String& identity) {
  if (identity == baseline_.name()) return StorageBackendRef(&baseline_);
  if (optionalSd_ && identity == optionalSd_->name()) return StorageBackendRef(optionalSd_);
  return StorageBackendRef();
}

bool StorageManager::hasMountedSd() const {
  const StorageCapabilities capabilities = optionalSd_
    ? optionalSd_->capabilities()
    : StorageCapabilities{};
  return optionalStorageEligible(StorageSelectionInput(
    optionalSd_ != nullptr,
    capabilities.removable,
    capabilities.writable,
    capabilities.mounted
  ));
}

}  // namespace inkloop
