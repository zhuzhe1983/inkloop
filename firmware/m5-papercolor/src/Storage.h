#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include "FirmwarePrimitives.h"

namespace inkloop {

struct StorageCapabilities {
  bool removable = false;
  bool writable = true;
  bool mounted = false;
};

class IStorageBackend;

struct StorageBackendRef {
  IStorageBackend* backend = nullptr;
  const char* identity = "";

  StorageBackendRef() = default;
  explicit StorageBackendRef(IStorageBackend* value);

  bool valid() const;
  bool available() const;
};

class IStorageBackend {
 public:
  virtual ~IStorageBackend() = default;
  virtual bool begin(bool formatOnFailure) = 0;
  virtual File open(const char* path, const char* mode) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool exists(const char* path) = 0;
  virtual bool mkdir(const char* path) = 0;
  virtual size_t totalBytes() const = 0;
  virtual size_t usedBytes() const = 0;
  virtual const char* name() const = 0;
  virtual StorageCapabilities capabilities() const = 0;
};

class LittleFsStorage final : public IStorageBackend {
 public:
  bool begin(bool formatOnFailure) override;
  File open(const char* path, const char* mode) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  bool exists(const char* path) override;
  bool mkdir(const char* path) override;
  size_t totalBytes() const override;
  size_t usedBytes() const override;
  const char* name() const override { return "littlefs"; }
  StorageCapabilities capabilities() const override;

 private:
  bool mounted_ = false;
};

class SdStorage final : public IStorageBackend {
 public:
  bool begin(bool formatOnFailure) override;
  File open(const char* path, const char* mode) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  bool exists(const char* path) override;
  bool mkdir(const char* path) override;
  size_t totalBytes() const override;
  size_t usedBytes() const override;
  const char* name() const override { return "sd"; }
  StorageCapabilities capabilities() const override;
  // Destructive and intentionally not part of IStorageBackend. Callers must
  // enforce spoken + physical confirmation before reaching this method.
  bool formatFat();

 private:
  bool mounted_ = false;
};

class StorageManager {
 public:
  enum class AssetPreference : uint8_t { Automatic, Internal, SdCard };
  explicit StorageManager(IStorageBackend& baseline) : baseline_(baseline) {}

  bool beginDataSafeMode();
  IStorageBackend& taskStorage() { return baseline_; }
  StorageBackendRef taskBackend() { return StorageBackendRef(&baseline_); }
  StorageBackendRef assetBackend();
  StorageBackendRef backendByIdentity(const String& identity);
  void attachOptionalSdBackend(IStorageBackend* backend) { optionalSd_ = backend; }
  bool hasMountedSd() const;
  void setAssetPreference(AssetPreference preference) { preference_ = preference; }
  AssetPreference assetPreference() const { return preference_; }

 private:
  IStorageBackend& baseline_;
  IStorageBackend* optionalSd_ = nullptr;
  AssetPreference preference_ = AssetPreference::Automatic;
};

}  // namespace inkloop
