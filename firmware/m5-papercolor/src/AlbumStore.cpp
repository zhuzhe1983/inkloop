#include "AlbumStore.h"

#include "ImageProcessing.h"

#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "AppConfig.h"
#include "AlbumUploadPrimitives.h"
#include "BackendTransactionIo.h"
#include "Diagnostics.h"
#include "JsonRecordCodec.h"

namespace inkloop {

constexpr const char* AlbumStore::kAlbumDirectory;
constexpr const char* AlbumStore::kIndexPath;
constexpr const char* AlbumStore::kIndexNextPath;
constexpr const char* AlbumStore::kIndexPreviousPath;
constexpr const char* AlbumStore::kAssetPartPath;

namespace {
constexpr uint16_t kIndexSchema = 1;
constexpr size_t kLittleFsAlbumLimit = 3000000;
constexpr size_t kLittleFsEntryLimit = 2;
constexpr size_t kSdEntryLimit = 96;
constexpr size_t kUploadTitleLimit = 64;

void initializeIndex(JsonDocument& index) {
  index.clear();
  index["schema"] = kIndexSchema;
  index["current"] = "";
  index["assets"].to<JsonArray>();
}

}  // namespace

StorageBackendRef AlbumStore::activeStorage() {
  return storage_.assetBackend();
}

const char* AlbumStore::backendName() {
  const StorageBackendRef backend = activeStorage();
  return backend.valid() ? backend.identity : "unavailable";
}

bool AlbumStore::validatePng(
    const uint8_t* bytes, size_t length, bool& landscape,
    const char** failure) {
  PaperColorPngStreamValidator validator(length);
  const bool valid = validator.append(bytes, length) &&
      validator.finish(length);
  if (valid) landscape = validator.landscape();
  if (failure) *failure = valid ? "none" : validator.failureName();
  return valid;
}

String AlbumStore::sha256Hex(const uint8_t* bytes, size_t length) {
  uint8_t digest[32]{};
  if (mbedtls_sha256_ret(bytes, length, digest, 0) != 0) return "";
  static constexpr char digits[] = "0123456789abcdef";
  char result[65]{};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    result[i * 2] = digits[digest[i] >> 4];
    result[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  return String(result);
}

String AlbumStore::sha256File(IStorageBackend& storage, const char* path) {
  File file = storage.open(path, FILE_READ);
  if (!file) return "";
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  if (mbedtls_sha256_starts_ret(&context, 0) != 0) {
    mbedtls_sha256_free(&context);
    file.close();
    return "";
  }
  uint8_t buffer[1024];
  while (file.available()) {
    const size_t received = file.read(buffer, sizeof(buffer));
    if (!received || mbedtls_sha256_update_ret(&context, buffer, received) != 0) {
      mbedtls_sha256_free(&context);
      file.close();
      return "";
    }
  }
  file.close();
  uint8_t digest[32]{};
  if (mbedtls_sha256_finish_ret(&context, digest) != 0) {
    mbedtls_sha256_free(&context);
    return "";
  }
  mbedtls_sha256_free(&context);
  static constexpr char digits[] = "0123456789abcdef";
  char result[65]{};
  for (size_t i = 0; i < sizeof(digest); ++i) {
    result[i * 2] = digits[digest[i] >> 4];
    result[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  return String(result);
}

size_t AlbumStore::albumBytes(JsonArrayConst assets) {
  size_t total = 0;
  for (JsonObjectConst asset : assets) total += static_cast<size_t>(asset["bytes"] | 0U);
  return total;
}

int AlbumStore::findAsset(JsonArrayConst assets, const String& id) {
  int index = 0;
  for (JsonObjectConst asset : assets) {
    if (id == String(asset["id"] | "")) return index;
    ++index;
  }
  return -1;
}

bool AlbumStore::loadIndexFile(IStorageBackend& storage, const char* path, JsonDocument& index) {
  File file = storage.open(path, FILE_READ);
  if (!file) return false;
  const DeserializationError error = deserializeJson(index, file);
  file.close();
  if (error || !index.is<JsonObject>() || !index["schema"].is<uint16_t>() ||
      static_cast<uint16_t>(index["schema"] | 0) != kIndexSchema ||
      !index["assets"].is<JsonArray>() || !index["current"].is<const char*>()) return false;
  for (JsonVariantConst value : index["assets"].as<JsonArrayConst>()) {
    // The iterator yields a const variant. Checking it against mutable
    // JsonObject always fails in ArduinoJson 7 and made every non-empty album
    // index impossible to commit (surfacing as album_index_commit_failed).
    if (!value.is<JsonObjectConst>()) return false;
    JsonObjectConst asset = value.as<JsonObjectConst>();
    if (!asset["id"].is<const char*>() || !asset["path"].is<const char*>() ||
        !asset["bytes"].is<uint32_t>() || !asset["landscape"].is<bool>() ||
        !asset["created"].is<uint32_t>() || !asset["taskId"].is<const char*>()) return false;
  }
  return true;
}

bool AlbumStore::commitIndex(IStorageBackend& storage, JsonDocument& index) {
  String payload;
  if (!serializeJsonRecordExactly(index, payload)) return false;
  BackendTransactionIo io(storage);
  TransactionalFileStore transaction(io);
  const auto validator = [this, &storage](const char* path) {
      JsonDocument candidate;
      return loadIndexFile(storage, path, candidate);
    };
  RecordCommitResult result = transaction.commitValidatedRecordDetailed(
      kIndexPath, kIndexNextPath, kIndexPreviousPath,
      reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
      validator);
  if (result == RecordCommitResult::Committed) return true;

  // A prior interrupted write or a transient LittleFS/SD rename failure must
  // not turn an otherwise valid upload into a permanently unusable album.
  // Recover one known-good index, then retry the exact validated payload once.
  Diagnostics::event("ALBUM_INDEX_COMMIT_RETRY", recordCommitResultName(result));
  const RecordRecovery recovered = recoverTransactionalRecord(
      io, kIndexPath, kIndexNextPath, kIndexPreviousPath, validator);
  if (recovered == RecordRecovery::Failed || !io.available()) {
    Diagnostics::event("ALBUM_INDEX_COMMIT_FAILED", recordCommitResultName(result));
    return false;
  }
  if (io.exists(kIndexPath) && io.contentEquals(
          kIndexPath, reinterpret_cast<const uint8_t*>(payload.c_str()),
          payload.length())) {
    return true;
  }
  result = transaction.commitValidatedRecordDetailed(
      kIndexPath, kIndexNextPath, kIndexPreviousPath,
      reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
      validator);
  if (result != RecordCommitResult::Committed)
    Diagnostics::event("ALBUM_INDEX_COMMIT_FAILED", recordCommitResultName(result));
  return result == RecordCommitResult::Committed;
}

bool AlbumStore::validateAssetFile(
  IStorageBackend& storage,
  const char* path,
  size_t expectedBytes,
  bool* landscape
) {
  File file = storage.open(path, FILE_READ);
  if (!file || file.size() != expectedBytes || expectedBytes < 45U ||
      expectedBytes > kMaxFrameBytes) {
    if (file) file.close();
    return false;
  }
  PaperColorPngStreamValidator validator(expectedBytes);
  uint8_t buffer[1024];
  size_t total = 0;
  bool valid = true;
  while (valid && total < expectedBytes) {
    const size_t remaining = expectedBytes - total;
    const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t received = file.read(buffer, wanted);
    if (!received || received > wanted ||
        !validator.append(buffer, received)) {
      valid = false;
      break;
    }
    total += received;
  }
  file.close();
  valid = valid && total == expectedBytes && validator.finish(expectedBytes);
  if (valid && landscape) *landscape = validator.landscape();
  return valid;
}

bool AlbumStore::sanitizeIndex(IStorageBackend& storage, JsonDocument& index) {
  const String previousCurrent = index["current"] | "";
  JsonDocument sanitized;
  initializeIndex(sanitized);
  JsonArray target = sanitized["assets"].as<JsonArray>();
  JsonDocument removals;
  JsonArray pathsToRemove = removals.to<JsonArray>();
  bool changed = false;
  for (JsonObjectConst source : index["assets"].as<JsonArrayConst>()) {
    const String path = source["path"] | "";
    const size_t bytes = source["bytes"] | 0U;
    const String id = source["id"] | "";
    File candidate = storage.open(path.c_str(), FILE_READ);
    const bool fileExists = static_cast<bool>(candidate);
    const size_t fileBytes = fileExists ? candidate.size() : 0;
    if (candidate) candidate.close();
    const bool headerValid = fileExists && validateAssetFile(storage, path.c_str(), bytes);
    if (!assetRecordUsable(
          id.length() == 64,
          path.startsWith(String(kAlbumDirectory) + "/"),
          fileExists,
          bytes,
          fileBytes,
          headerValid
        )) {
      if (path.startsWith(String(kAlbumDirectory) + "/")) pathsToRemove.add(path);
      changed = true;
      continue;
    }
    JsonObject copied = target.add<JsonObject>();
    copied.set(source);
  }
  const bool currentSurvived = previousCurrent.length() && findAsset(target, previousCurrent) >= 0;
  sanitized["current"] = currentSurvived ? previousCurrent : "";
  if (previousCurrent.length() && !currentSurvived) changed = true;
  if (!changed) {
    cleanupOrphans(storage, index["assets"].as<JsonArrayConst>());
    return true;
  }
  if (!commitIndex(storage, sanitized)) return false;
  for (const char* path : pathsToRemove) storage.remove(path);
  index.clear();
  index.set(sanitized);
  cleanupOrphans(storage, index["assets"].as<JsonArrayConst>());
  Diagnostics::event("ALBUM_REPAIRED", storage.name());
  return true;
}

bool AlbumStore::discardAsset(
  IStorageBackend& storage,
  JsonDocument& index,
  const String& assetId,
  const String& path
) {
  JsonDocument repaired;
  initializeIndex(repaired);
  JsonArray retained = repaired["assets"].as<JsonArray>();
  for (JsonObjectConst candidate : index["assets"].as<JsonArrayConst>()) {
    if (String(candidate["id"] | "") == assetId) continue;
    JsonObject copy = retained.add<JsonObject>();
    copy.set(candidate);
  }
  const String oldCurrent = index["current"] | "";
  repaired["current"] = oldCurrent == assetId ? "" : oldCurrent;
  if (!commitIndex(storage, repaired)) return false;
  storage.remove(path.c_str());
  index.clear();
  index.set(repaired);
  return true;
}

void AlbumStore::cleanupOrphans(IStorageBackend& storage, JsonArrayConst assets) {
  File directory = storage.open(kAlbumDirectory, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }
  JsonDocument removals;
  JsonArray paths = removals.to<JsonArray>();
  File entry = directory.openNextFile();
  while (entry) {
    String path = entry.path();
    entry.close();
    if (path.endsWith(".png")) {
      bool indexed = false;
      for (JsonObjectConst asset : assets) {
        if (path == String(asset["path"] | "")) {
          indexed = true;
          break;
        }
      }
      if (!indexed) paths.add(path);
    }
    entry = directory.openNextFile();
  }
  directory.close();
  for (const char* path : paths) {
    storage.remove(path);
    Diagnostics::event("ALBUM_ORPHAN_REMOVED", path);
  }
}

bool AlbumStore::prepare(IStorageBackend& storage, JsonDocument& index) {
  if (!albumPrepareMayMutate(uploadActive_)) return false;
  if (!storage.capabilities().mounted || !storage.mkdir(kAlbumDirectory)) return false;
  // FS.remove() logs an error when the recovery file is absent. Album reads
  // call prepare() frequently, so probe first instead of flooding serial with
  // a harmless "asset.part does not exist" message.
  if (storage.exists(kAssetPartPath) && !storage.remove(kAssetPartPath)) {
    return false;
  }
  BackendTransactionIo io(storage);
  const RecordRecovery recovery = recoverTransactionalRecord(
    io,
    kIndexPath,
    kIndexNextPath,
    kIndexPreviousPath,
    [this, &storage](const char* path) {
      JsonDocument candidate;
      return loadIndexFile(storage, path, candidate);
    }
  );
  if (recovery == RecordRecovery::Failed) return false;
  if (recovery == RecordRecovery::Empty) {
    initializeIndex(index);
    if (!commitIndex(storage, index)) return false;
  } else {
    if (!loadIndexFile(storage, kIndexPath, index)) return false;
    if (recovery == RecordRecovery::PromoteNext ||
        recovery == RecordRecovery::RestorePrevious) {
      Diagnostics::event("ALBUM_INDEX_RECOVERED", storage.name());
    }
  }
  return sanitizeIndex(storage, index);
}

bool AlbumStore::begin() {
  JsonDocument index;
  const StorageBackendRef selected = activeStorage();
  if (!selected.available()) return false;
  IStorageBackend& backend = *selected.backend;
  const bool ready = prepare(backend, index);
  Diagnostics::event("ALBUM_STORAGE", ready ? backend.name() : String("ERROR_") + backend.name());
  return ready;
}

bool AlbumStore::hasAdmissionHeadroom(
  const StorageBackendRef& assetBackend,
  size_t incomingBytes,
  size_t indexNextBytes,
  const MetadataBudget& metadataBudget
) {
  if (!assetBackend.available()) return false;
  const StorageBackendRef controlBackend = storage_.taskBackend();
  if (!controlBackend.available()) return false;
  const bool sharedControlBackend = assetBackend.backend == controlBackend.backend;
  size_t assetTransactionBytes = 0;
  if (!metadataTransactionBytes(
        metadataBudget,
        indexNextBytes,
        sharedControlBackend,
        assetTransactionBytes
      )) return false;
  const bool removable = assetBackend.backend->capabilities().removable;
  if (!storageCanPreserveReserve(
        assetBackend.backend->totalBytes(),
        assetBackend.backend->usedBytes(),
        incomingBytes,
        assetTransactionBytes,
        removable ? kSdFinalReserveBytes : kLittleFsFinalReserveBytes
      )) return false;
  if (sharedControlBackend) return true;
  size_t controlBytes = 0;
  return controlTransactionBytes(metadataBudget, controlBytes) &&
    storageCanPreserveReserve(
      controlBackend.backend->totalBytes(),
      controlBackend.backend->usedBytes(),
      0,
      controlBytes,
      kLittleFsFinalReserveBytes
    );
}

bool AlbumStore::cacheFrame(
  const DownloadedFrame& frame,
  const String& expectedHash,
  const String& taskId,
  const String& requestedRenderStrategy,
  const MetadataBudget& metadataBudget,
  AlbumAsset& asset
) {
  asset = AlbumAsset{};
  displaypower::RenderStrategy parsedStrategy;
  const bool hasRequestedStrategy = requestedRenderStrategy.length() &&
      displaypower::parseRenderStrategyId(
          requestedRenderStrategy.c_str(), &parsedStrategy);
  const String canonicalStrategy = hasRequestedStrategy
      ? displaypower::renderStrategyId(parsedStrategy) : String();
  bool landscape = false;
  const char* pngFailure = "unknown";
  if (!validatePng(frame.bytes, frame.length, landscape, &pngFailure)) {
    Diagnostics::event(
        "ALBUM_CACHE_REJECT", String("INVALID_PNG:") + pngFailure);
    return false;
  }
  if (landscape != frame.landscape) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "ORIENTATION_MISMATCH");
    return false;
  }
  const String hash = sha256Hex(frame.bytes, frame.length);
  if (hash.length() != 64 || (expectedHash.length() && !hash.equalsIgnoreCase(expectedHash))) {
    Diagnostics::event("ERROR", "ALBUM_HASH_MISMATCH");
    return false;
  }

  const StorageBackendRef pinnedBackend = activeStorage();
  if (!pinnedBackend.available()) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "BACKEND_UNAVAILABLE");
    return false;
  }
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "PREPARE_FAILED");
    return false;
  }
  JsonArray assets = index["assets"].as<JsonArray>();
  int existing = findAsset(assets, hash);
  if (existing >= 0) {
    JsonObject found = assets[existing].as<JsonObject>();
    const String existingPath = found["path"] | "";
    const size_t existingBytes = found["bytes"] | 0U;
    if (validateAssetFile(backend, existingPath.c_str(), existingBytes) &&
        sha256File(backend, existingPath.c_str()) == hash) {
      asset.id = hash;
      asset.backend = pinnedBackend;
      asset.path = existingPath;
      asset.bytes = existingBytes;
      asset.landscape = found["landscape"] | false;
      asset.page = static_cast<size_t>(existing);
      asset.renderStrategy = found["renderStrategy"] | "official-quality";
      if (hasRequestedStrategy && asset.renderStrategy != canonicalStrategy) {
        found["renderStrategy"] = canonicalStrategy;
        if (!commitIndex(backend, index)) return false;
        asset.renderStrategy = canonicalStrategy;
      }
      const size_t indexNextBytes = measureJson(index);
      if (!indexNextBytes || !hasAdmissionHeadroom(
            pinnedBackend,
            0,
            indexNextBytes,
            metadataBudget
          )) return false;
      Diagnostics::event("ALBUM_DEDUP", hash.substring(0, 12));
      return true;
    }
    if (!discardAsset(backend, index, hash, existingPath)) return false;
    assets = index["assets"].as<JsonArray>();
    Diagnostics::event("ALBUM_CORRUPT_CLEANED", hash.substring(0, 12));
  }

  const StorageCapabilities capabilities = backend.capabilities();
  const bool removable = capabilities.removable;
  const size_t total = backend.totalBytes();
  const size_t albumLimit = removable
    ? (total > kSdFinalReserveBytes ? total - kSdFinalReserveBytes : 0)
    : (total < kLittleFsAlbumLimit ? total : kLittleFsAlbumLimit);
  const CacheCapacity capacity(
    total,
    backend.usedBytes(),
    albumBytes(assets),
    albumLimit,
    0,
    assets.size(),
    removable ? kSdEntryLimit : kLittleFsEntryLimit
  );
  const CacheAdmission admission = cacheAdmission(capacity, frame.length, false);
  if (admission != CacheAdmission::Accept) {
    Diagnostics::event("ERROR", String("ALBUM_CAPACITY_") + String(static_cast<int>(admission)));
    return false;
  }

  const String finalPath = String(kAlbumDirectory) + "/" + hash + ".png";
  JsonDocument predictedIndex;
  predictedIndex.set(index);
  JsonArray predictedAssets = predictedIndex["assets"].as<JsonArray>();
  JsonObject added = predictedAssets.add<JsonObject>();
  added["id"] = hash;
  added["path"] = finalPath;
  added["bytes"] = frame.length;
  added["landscape"] = frame.landscape;
  added["created"] = static_cast<uint32_t>(time(nullptr));
  added["taskId"] = taskId;
  added["renderStrategy"] = hasRequestedStrategy
      ? canonicalStrategy : String("official-quality");
  const size_t indexNextBytes = measureJson(predictedIndex);
  if (!indexNextBytes || !hasAdmissionHeadroom(
        pinnedBackend,
        frame.length,
        indexNextBytes,
        metadataBudget
      )) {
    Diagnostics::event("ERROR", "ALBUM_METADATA_CAPACITY");
    return false;
  }

  BackendTransactionIo io(backend);
  TransactionalFileStore transaction(io);
  if (!transaction.promoteBlob(
          kAssetPartPath, finalPath.c_str(), frame.bytes, frame.length)) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "ASSET_PROMOTE_FAILED");
    backend.remove(finalPath.c_str());
    return false;
  }
  if (!validateAssetFile(backend, finalPath.c_str(), frame.length)) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "ASSET_VALIDATE_FAILED");
    backend.remove(finalPath.c_str());
    return false;
  }
  if (sha256File(backend, finalPath.c_str()) != hash) {
    Diagnostics::event("ALBUM_CACHE_REJECT", "ASSET_HASH_FAILED");
    backend.remove(finalPath.c_str());
    return false;
  }

  if (!commitIndex(backend, predictedIndex)) {
    backend.remove(finalPath.c_str());
    return false;
  }

  asset.id = hash;
  asset.backend = pinnedBackend;
  asset.path = finalPath;
  asset.bytes = frame.length;
  asset.landscape = frame.landscape;
  asset.page = predictedAssets.size() - 1;
  asset.renderStrategy = added["renderStrategy"] | "official-quality";
  Diagnostics::event("ALBUM_CACHED", hash.substring(0, 12));
  return true;
}

bool AlbumStore::beginUserUpload(
  const String& boundedTitle,
  size_t declaredUpperBound,
  String& error
) {
  error = "";
  if (uploadActive_) {
    error = "upload_already_active";
    return false;
  }
  if (!validUploadTitle(
          std::string(boundedTitle.c_str(), boundedTitle.length()),
          kUploadTitleLimit) ||
      declaredUpperBound < 45 || declaredUpperBound > kMaxFrameBytes) {
    error = "invalid_upload_metadata";
    return false;
  }
  const StorageBackendRef pinned = activeStorage();
  if (!pinned.available() || !pinned.backend->capabilities().writable) {
    error = "upload_backend_unavailable";
    return false;
  }
  JsonDocument index;
  if (!prepare(*pinned.backend, index)) {
    error = "album_unavailable";
    return false;
  }
  const bool removable = pinned.backend->capabilities().removable;
  const size_t entryLimit = removable ? kSdEntryLimit : kLittleFsEntryLimit;
  if (index["assets"].as<JsonArrayConst>().size() >= entryLimit) {
    error = "album_entry_limit";
    return false;
  }
  const size_t estimatedIndexBytes = measureJson(index) + boundedTitle.length() + 256U;
  if (!hasAdmissionHeadroom(
        pinned, declaredUpperBound, estimatedIndexBytes, MetadataBudget(0, 0))) {
    error = "insufficient_storage";
    return false;
  }
  pinned.backend->remove(kAssetPartPath);
  File part = pinned.backend->open(kAssetPartPath, FILE_WRITE);
  if (!part) {
    error = "upload_part_open_failed";
    return false;
  }
  uploadBackend_ = pinned;
  uploadFile_ = part;
  uploadTitle_ = boundedTitle;
  uploadBytes_ = 0;
  uploadMaximumBytes_ = declaredUpperBound;
  uploadValidator_.reset(declaredUpperBound);
  uploadActive_ = true;
  return true;
}

bool AlbumStore::appendUserUpload(
  const uint8_t* bytes, size_t length, String& error) {
  error = "";
  if (!uploadActive_ || !uploadFile_ || !bytes || !length) {
    error = "upload_not_active";
    return false;
  }
  if (!boundedUploadAppend(
          uploadBytes_, length, uploadMaximumBytes_, kMaxFrameBytes)) {
    error = "upload_too_large";
    resetUserUpload(true);
    return false;
  }
  if (!uploadValidator_.append(bytes, length)) {
    error = "invalid_png_stream";
    resetUserUpload(true);
    return false;
  }
  if (uploadFile_.write(bytes, length) != length) {
    error = "upload_write_failed";
    resetUserUpload(true);
    return false;
  }
  uploadBytes_ += length;
  return true;
}

bool AlbumStore::finishUserUpload(AlbumAsset& asset, String& error) {
  asset = AlbumAsset{};
  error = "";
  if (!uploadActive_ || !uploadFile_ || !uploadBackend_.available()) {
    error = "upload_not_active";
    return false;
  }
  uploadFile_.flush();
  uploadFile_.close();
  bool landscape = false;
  if (uploadBytes_ != uploadMaximumBytes_ || uploadBytes_ < 45 ||
      !uploadValidator_.finish(uploadMaximumBytes_) ||
      uploadBytes_ > kMaxFrameBytes ||
      !validateAssetFile(
          *uploadBackend_.backend, kAssetPartPath, uploadBytes_, &landscape)) {
    error = uploadBytes_ != uploadMaximumBytes_
        ? "upload_length_mismatch" : "invalid_png_structure";
    resetUserUpload(true);
    return false;
  }
  const String hash = sha256File(*uploadBackend_.backend, kAssetPartPath);
  if (hash.length() != 64) {
    error = "upload_hash_failed";
    resetUserUpload(true);
    return false;
  }
  JsonDocument index;
  if (!loadIndexFile(*uploadBackend_.backend, kIndexPath, index)) {
    error = "album_index_unavailable";
    resetUserUpload(true);
    return false;
  }
  JsonArray assets = index["assets"].as<JsonArray>();
  const int duplicate = findAsset(assets, hash);
  if (duplicate >= 0) {
    JsonObjectConst existing = assets[duplicate];
    const String existingPath = existing["path"] | "";
    const size_t existingBytes = existing["bytes"] | 0U;
    const bool pathScoped = existingPath.startsWith(
        String(kAlbumDirectory) + "/");
    if (pathScoped &&
        validateAssetFile(
            *uploadBackend_.backend, existingPath.c_str(), existingBytes) &&
        sha256File(*uploadBackend_.backend, existingPath.c_str()) == hash) {
      uploadBackend_.backend->remove(kAssetPartPath);
      asset.backend = uploadBackend_;
      asset.id = hash;
      asset.path = existingPath;
      asset.bytes = existingBytes;
      asset.landscape = existing["landscape"] | false;
      asset.page = static_cast<size_t>(duplicate);
      asset.renderStrategy = existing["renderStrategy"] | "official-quality";
      resetUserUpload(false);
      return true;
    }
    if (!pathScoped || !discardAsset(
            *uploadBackend_.backend, index, hash, existingPath)) {
      error = "corrupt_duplicate_asset";
      resetUserUpload(true);
      return false;
    }
    assets = index["assets"].as<JsonArray>();
  }
  JsonDocument predicted;
  predicted.set(index);
  JsonArray predictedAssets = predicted["assets"].as<JsonArray>();
  const String finalPath = String(kAlbumDirectory) + "/" + hash + ".png";
  JsonObject added = predictedAssets.add<JsonObject>();
  added["id"] = hash;
  added["path"] = finalPath;
  added["bytes"] = uploadBytes_;
  added["landscape"] = landscape;
  added["created"] = static_cast<uint32_t>(time(nullptr));
  added["taskId"] = String("upload:") + uploadTitle_;
  added["renderStrategy"] = "official-quality";
  const size_t indexNextBytes = measureJson(predicted);
  if (!indexNextBytes || !hasAdmissionHeadroom(
        uploadBackend_, uploadBytes_, indexNextBytes, MetadataBudget(0, 0))) {
    error = "insufficient_storage";
    resetUserUpload(true);
    return false;
  }
  uploadBackend_.backend->remove(finalPath.c_str());
  if (!uploadBackend_.backend->rename(kAssetPartPath, finalPath.c_str()) ||
      !validateAssetFile(
          *uploadBackend_.backend, finalPath.c_str(), uploadBytes_, &landscape) ||
      sha256File(*uploadBackend_.backend, finalPath.c_str()) != hash) {
    uploadBackend_.backend->remove(finalPath.c_str());
    error = "upload_atomic_promote_failed";
    resetUserUpload(true);
    return false;
  }
  if (!commitIndex(*uploadBackend_.backend, predicted)) {
    uploadBackend_.backend->remove(finalPath.c_str());
    error = "album_index_commit_failed";
    resetUserUpload(true);
    return false;
  }
  asset.backend = uploadBackend_;
  asset.id = hash;
  asset.path = finalPath;
  asset.bytes = uploadBytes_;
  asset.landscape = landscape;
  asset.page = predictedAssets.size() - 1;
  asset.renderStrategy = added["renderStrategy"] | "official-quality";
  resetUserUpload(false);
  return true;
}

void AlbumStore::resetUserUpload(bool removePart) {
  if (uploadFile_) uploadFile_.close();
  if (removePart && uploadBackend_.valid())
    uploadBackend_.backend->remove(kAssetPartPath);
  uploadBackend_ = StorageBackendRef();
  uploadTitle_ = "";
  uploadBytes_ = 0;
  uploadMaximumBytes_ = 0;
  uploadValidator_.reset(0);
  uploadActive_ = false;
}

void AlbumStore::abortUserUpload() {
  resetUserUpload(true);
}

bool AlbumStore::allocateFrame(size_t length, DownloadedFrame& frame) {
  frame.release();
  frame.bytes = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame.bytes) frame.bytes = static_cast<uint8_t*>(malloc(length));
  return frame.bytes != nullptr;
}

bool AlbumStore::loadPage(
  const StorageBackendRef& pinnedBackend,
  size_t page,
  DownloadedFrame& frame,
  AlbumAsset& asset
) {
  frame.release();
  asset = AlbumAsset{};
  if (!pinnedBackend.available()) return false;
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  JsonArrayConst assets = index["assets"].as<JsonArrayConst>();
  if (page >= assets.size()) return false;
  JsonObjectConst source = assets[page];
  const String path = source["path"] | "";
  const size_t bytes = source["bytes"] | 0U;
  if (!allocateFrame(bytes, frame)) return false;
  File file = backend.open(path.c_str(), FILE_READ);
  const size_t received = file ? file.read(frame.bytes, bytes) : 0;
  if (file) file.close();
  bool landscape = false;
  const String expectedId = source["id"] | "";
  if (received != bytes || !validatePng(frame.bytes, received, landscape) ||
      sha256Hex(frame.bytes, received) != expectedId) {
    frame.release();
    discardAsset(backend, index, expectedId, path);
    Diagnostics::event("ERROR", "ALBUM_CORRUPT_ASSET");
    return false;
  }
  frame.length = received;
  frame.landscape = landscape;
  asset.id = expectedId;
  asset.backend = pinnedBackend;
  asset.path = path;
  asset.bytes = bytes;
  asset.landscape = landscape;
  asset.page = page;
  asset.renderStrategy = source["renderStrategy"] | "official-quality";
  return true;
}

bool AlbumStore::markCurrent(const StorageBackendRef& pinnedBackend, const String& assetId) {
  if (!pinnedBackend.available()) return false;
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  if (findAsset(index["assets"].as<JsonArrayConst>(), assetId) < 0) return false;
  if (assetId == String(index["current"] | "")) return true;
  index["current"] = assetId;
  return commitIndex(backend, index);
}

bool AlbumStore::setRenderStrategy(
    const StorageBackendRef& pinnedBackend,
    const String& assetId,
    const String& requestedRenderStrategy) {
  if (!pinnedBackend.available() || assetId.length() != 64) return false;
  displaypower::RenderStrategy strategy;
  if (!displaypower::parseRenderStrategyId(
          requestedRenderStrategy.c_str(), &strategy)) return false;
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  const int found = findAsset(index["assets"].as<JsonArrayConst>(), assetId);
  if (found < 0) return false;
  JsonObject entry = index["assets"][found].as<JsonObject>();
  const String canonical = displaypower::renderStrategyId(strategy);
  if (String(entry["renderStrategy"] | "official-quality") == canonical) return true;
  entry["renderStrategy"] = canonical;
  return commitIndex(backend, index);
}

bool AlbumStore::pageState(AlbumPageState& state) {
  return pageState(activeStorage(), state);
}

bool AlbumStore::pageState(const StorageBackendRef& pinnedBackend, AlbumPageState& state) {
  state = AlbumPageState{};
  state.backend = pinnedBackend;
  if (!state.backend.available()) return false;
  IStorageBackend& backend = *state.backend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  JsonArrayConst assets = index["assets"].as<JsonArrayConst>();
  state.count = assets.size();
  if (!state.count) return true;
  const int found = findAsset(assets, String(index["current"] | ""));
  state.current = found >= 0 ? static_cast<size_t>(found) : 0;
  return true;
}

bool AlbumStore::currentId(const StorageBackendRef& pinnedBackend, String& current) {
  current = "";
  if (!pinnedBackend.available()) return false;
  JsonDocument index;
  if (!prepare(*pinnedBackend.backend, index)) return false;
  current = index["current"] | "";
  return true;
}

bool AlbumStore::indexSerializedSize(const StorageBackendRef& pinnedBackend, size_t& bytes) {
  bytes = 0;
  if (!pinnedBackend.available()) return false;
  JsonDocument index;
  if (!prepare(*pinnedBackend.backend, index)) return false;
  bytes = measureJson(index);
  return bytes > 0;
}

bool AlbumStore::readCatalogPage(
  const StorageBackendRef& pinnedBackend,
  size_t offset,
  size_t maximumItems,
  size_t maximumFieldBytes,
  std::vector<AlbumCatalogEntry>& entries,
  size_t& totalItems,
  size_t& nextOffset
) {
  entries.clear();
  totalItems = 0;
  nextOffset = 0;
  if (!pinnedBackend.available() || maximumItems == 0 || maximumItems > 16) return false;
  JsonDocument index;
  if (!prepare(*pinnedBackend.backend, index)) return false;
  JsonArrayConst assets = index["assets"].as<JsonArrayConst>();
  totalItems = assets.size();
  if (offset > totalItems) return false;
  const String current = index["current"] | "";
  size_t fieldBytes = 0;
  size_t ordinal = 0;
  for (JsonObjectConst source : assets) {
    if (ordinal++ < offset) continue;
    if (entries.size() >= maximumItems) break;
    AlbumCatalogEntry entry;
    entry.id = source["id"] | "";
    entry.path = source["path"] | "";
    entry.taskId = source["taskId"] | "";
    entry.bytes = source["bytes"] | 0U;
    entry.ordinal = ordinal;
    entry.current = entry.id == current;
    entry.factoryAsset = entry.taskId.startsWith("factory") ||
      entry.taskId.startsWith("tutorial");
    entry.renderStrategy = source["renderStrategy"] | "official-quality";
    displaypower::RenderStrategy parsed;
    if (entry.id.length() != 64 || entry.path.length() > 192 ||
        entry.taskId.length() > 192 || entry.renderStrategy.length() > 32 ||
        !displaypower::parseRenderStrategyId(
            entry.renderStrategy.c_str(), &parsed)) return false;
    entry.renderStrategy = displaypower::renderStrategyId(parsed);
    const size_t incoming = entry.id.length() + entry.path.length() +
        entry.taskId.length() + entry.renderStrategy.length();
    if (incoming > maximumFieldBytes - fieldBytes) return false;
    fieldBytes += incoming;
    entries.push_back(entry);
  }
  nextOffset = offset + entries.size();
  if (nextOffset >= totalItems) nextOffset = 0;
  return true;
}

bool AlbumStore::findCatalogEntry(
  const StorageBackendRef& pinnedBackend,
  const String& exactId,
  AlbumCatalogEntry& entry
) {
  entry = AlbumCatalogEntry{};
  if (!pinnedBackend.available() || exactId.length() != 64) return false;
  JsonDocument index;
  if (!prepare(*pinnedBackend.backend, index)) return false;
  const String current = index["current"] | "";
  size_t ordinal = 0;
  for (JsonObjectConst source : index["assets"].as<JsonArrayConst>()) {
    ++ordinal;
    if (String(source["id"] | "") != exactId) continue;
    entry.id = source["id"] | "";
    entry.path = source["path"] | "";
    entry.taskId = source["taskId"] | "";
    entry.bytes = source["bytes"] | 0U;
    entry.ordinal = ordinal;
    entry.current = entry.id == current;
    entry.factoryAsset = entry.taskId.startsWith("factory") ||
      entry.taskId.startsWith("tutorial");
    entry.renderStrategy = source["renderStrategy"] | "official-quality";
    displaypower::RenderStrategy parsed;
    if (!displaypower::parseRenderStrategyId(
            entry.renderStrategy.c_str(), &parsed)) return false;
    entry.renderStrategy = displaypower::renderStrategyId(parsed);
    return true;
  }
  return false;
}

bool AlbumStore::deleteUserAsset(
  const StorageBackendRef& pinnedBackend,
  const String& exactId
) {
  if (!pinnedBackend.available() || exactId.length() != 64) return false;
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  JsonArrayConst assets = index["assets"].as<JsonArrayConst>();
  const int found = findAsset(assets, exactId);
  if (found < 0) return false;
  JsonObjectConst target = assets[found];
  const String taskId = target["taskId"] | "";
  if (taskId.startsWith("factory") || taskId.startsWith("tutorial")) return false;
  const String path = target["path"] | "";
  JsonDocument next;
  initializeIndex(next);
  JsonArray retained = next["assets"].as<JsonArray>();
  for (JsonObjectConst source : assets) {
    if (String(source["id"] | "") == exactId) continue;
    retained.add<JsonObject>().set(source);
  }
  const String priorCurrent = index["current"] | "";
  next["current"] = priorCurrent == exactId
    ? (retained.size() ? String(retained[0]["id"] | "") : String(""))
    : priorCurrent;
  if (!commitIndex(backend, next)) return false;
  if (!backend.remove(path.c_str()) && backend.exists(path.c_str())) return false;
  return true;
}

bool AlbumStore::clearUserAssets(const StorageBackendRef& pinnedBackend) {
  if (!pinnedBackend.available()) return false;
  IStorageBackend& backend = *pinnedBackend.backend;
  JsonDocument index;
  if (!prepare(backend, index)) return false;
  JsonArrayConst assets = index["assets"].as<JsonArrayConst>();
  JsonDocument next;
  initializeIndex(next);
  JsonArray retained = next["assets"].as<JsonArray>();
  JsonDocument removals;
  JsonArray removePaths = removals.to<JsonArray>();
  const String priorCurrent = index["current"] | "";
  bool currentRetained = false;
  for (JsonObjectConst source : assets) {
    const String taskId = source["taskId"] | "";
    if (taskId.startsWith("factory") || taskId.startsWith("tutorial")) {
      retained.add<JsonObject>().set(source);
      if (String(source["id"] | "") == priorCurrent) currentRetained = true;
    } else {
      removePaths.add(source["path"] | "");
    }
  }
  next["current"] = currentRetained
    ? priorCurrent
    : (retained.size() ? String(retained[0]["id"] | "") : String(""));
  if (!commitIndex(backend, next)) return false;
  bool removed = true;
  for (const char* path : removePaths) {
    if (path && *path && !backend.remove(path) && backend.exists(path)) removed = false;
  }
  return removed;
}

}  // namespace inkloop
