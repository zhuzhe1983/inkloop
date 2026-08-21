#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "AlbumPrimitives.h"
#include "AlbumUploadPrimitives.h"
#include "InkloopClient.h"
#include "MetadataPrimitives.h"
#include "Storage.h"

#include <vector>

namespace inkloop {

struct AlbumAsset {
  StorageBackendRef backend;
  String id;
  String path;
  size_t bytes = 0;
  bool landscape = false;
  size_t page = 0;
};

struct AlbumPageState {
  StorageBackendRef backend;
  size_t count = 0;
  size_t current = 0;
};

struct AlbumCatalogEntry {
  String id;
  String path;
  String taskId;
  size_t bytes = 0;
  size_t ordinal = 0;
  bool current = false;
  bool factoryAsset = false;
};

class AlbumStore {
 public:
  explicit AlbumStore(StorageManager& storage) : storage_(storage) {}

  bool begin();
  bool cacheFrame(
    const DownloadedFrame& frame,
    const String& expectedHash,
    const String& taskId,
    const MetadataBudget& metadataBudget,
    AlbumAsset& asset
  );
  bool loadPage(const StorageBackendRef& backend, size_t page, DownloadedFrame& frame, AlbumAsset& asset);
  bool markCurrent(const StorageBackendRef& backend, const String& assetId);
  bool pageState(AlbumPageState& state);
  bool pageState(const StorageBackendRef& backend, AlbumPageState& state);
  bool currentId(const StorageBackendRef& backend, String& current);
  bool indexSerializedSize(const StorageBackendRef& backend, size_t& bytes);
  bool readCatalogPage(
    const StorageBackendRef& backend,
    size_t offset,
    size_t maximumItems,
    size_t maximumFieldBytes,
    std::vector<AlbumCatalogEntry>& entries,
    size_t& totalItems,
    size_t& nextOffset
  );
  bool findCatalogEntry(
    const StorageBackendRef& backend,
    const String& exactId,
    AlbumCatalogEntry& entry
  );
  bool deleteUserAsset(const StorageBackendRef& backend, const String& exactId);
  bool clearUserAssets(const StorageBackendRef& backend);
  bool beginUserUpload(
    const String& boundedTitle,
    size_t declaredUpperBound,
    String& error
  );
  bool appendUserUpload(const uint8_t* bytes, size_t length, String& error);
  bool finishUserUpload(AlbumAsset& asset, String& error);
  void abortUserUpload();
  bool userUploadActive() const { return uploadActive_; }
  const char* backendName();

 private:
  static constexpr const char* kAlbumDirectory = "/inkloop-album";
  static constexpr const char* kIndexPath = "/inkloop-album/index.json";
  static constexpr const char* kIndexNextPath = "/inkloop-album/index.next";
  static constexpr const char* kIndexPreviousPath = "/inkloop-album/index.prev";
  static constexpr const char* kAssetPartPath = "/inkloop-album/asset.part";

  StorageBackendRef activeStorage();
  bool prepare(IStorageBackend& storage, JsonDocument& index);
  bool loadIndexFile(IStorageBackend& storage, const char* path, JsonDocument& index);
  bool commitIndex(IStorageBackend& storage, JsonDocument& index);
  bool sanitizeIndex(IStorageBackend& storage, JsonDocument& index);
  bool discardAsset(IStorageBackend& storage, JsonDocument& index, const String& assetId, const String& path);
  bool hasAdmissionHeadroom(
    const StorageBackendRef& assetBackend,
    size_t incomingBytes,
    size_t indexNextBytes,
    const MetadataBudget& metadataBudget
  );
  void cleanupOrphans(IStorageBackend& storage, JsonArrayConst assets);
  bool validateAssetFile(IStorageBackend& storage, const char* path, size_t expectedBytes, bool* landscape = nullptr);
  static bool validatePng(const uint8_t* bytes, size_t length, bool& landscape);
  static String sha256Hex(const uint8_t* bytes, size_t length);
  static String sha256File(IStorageBackend& storage, const char* path);
  static size_t albumBytes(JsonArrayConst assets);
  static int findAsset(JsonArrayConst assets, const String& id);
  static bool allocateFrame(size_t length, DownloadedFrame& frame);
  void resetUserUpload(bool removePart);

  StorageManager& storage_;
  StorageBackendRef uploadBackend_;
  File uploadFile_;
  String uploadTitle_;
  size_t uploadBytes_ = 0;
  size_t uploadMaximumBytes_ = 0;
  bool uploadActive_ = false;
  PaperColorPngStreamValidator uploadValidator_;
};

}  // namespace inkloop
