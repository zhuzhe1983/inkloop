#include "inkloop/storage/posix_atomic_album_store.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <limits>

#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/papercolor_png.hpp"
#include "inkloop/storage/sha256.hpp"

#if defined(ESP_PLATFORM)
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#endif

namespace inkloop {
namespace storage {
namespace {

constexpr size_t kReadBufferBytes = 1024U;
constexpr size_t kLittleFsEntryLimit = 2U;
constexpr size_t kSdEntryLimit = 96U;
constexpr uint64_t kLittleFsReserveBytes = 320U * 1024U;
constexpr uint64_t kSdReserveBytes = 1024U * 1024U;

myai::Status storageFailure(const char* detail) {
  return myai::Status(myai::ErrorCode::Storage, 0, detail);
}

bool validRoot(const std::string& root) {
  if (root.empty() || root.size() > 160U || root.front() != '/' ||
      root.find("..") != std::string::npos || root.find('\0') != std::string::npos)
    return false;
  return root.size() == 1U || root.back() != '/';
}

bool inspectPath(const std::string& path, struct stat& status) {
#if defined(ESP_PLATFORM)
  // ESP-IDF's VFS backends used here (LittleFS/FatFS) do not expose symbolic
  // links, and Newlib declares lstat without providing a linked implementation.
  return ::stat(path.c_str(), &status) == 0;
#else
  // Host tests retain the stronger no-follow check so a future POSIX backend
  // cannot silently turn album paths into links outside the selected root.
  return ::lstat(path.c_str(), &status) == 0;
#endif
}

bool exists(const std::string& path) {
  struct stat status {};
  return inspectPath(path, status) && S_ISREG(status.st_mode);
}

bool readBounded(const std::string& path, size_t maximum, std::string& output) {
  output.clear();
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return false;
  struct stat status {};
  bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
               status.st_size >= 0 &&
               static_cast<uint64_t>(status.st_size) <= maximum;
  if (valid) output.reserve(static_cast<size_t>(status.st_size));
  std::array<char, kReadBufferBytes> buffer{};
  while (valid && output.size() < static_cast<size_t>(status.st_size)) {
    const size_t wanted = std::min(buffer.size(),
                                   static_cast<size_t>(status.st_size) - output.size());
    const ssize_t count = ::read(descriptor, buffer.data(), wanted);
    if (count <= 0 || static_cast<size_t>(count) > wanted) valid = false;
    else output.append(buffer.data(), static_cast<size_t>(count));
  }
  if (::close(descriptor) != 0) valid = false;
  return valid && output.size() == static_cast<size_t>(status.st_size);
}

bool validServerTaskId(const std::string& task_id) {
  if (task_id.size() < 7U || task_id.size() > 100U ||
      task_id.compare(0, 6U, "dtask-") != 0) return false;
  for (size_t at = 6U; at < task_id.size(); ++at) {
    const char ch = task_id[at];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-')) return false;
  }
  return true;
}

bool logicalAssetId(const std::string& task_id, const std::string& content_sha,
                    std::string& output) {
  if (!validServerTaskId(task_id)) {
    output = content_sha;
    return true;
  }
  Sha256 sha;
  const uint8_t separator = 0U;
  return sha.update(reinterpret_cast<const uint8_t*>(task_id.data()),
                    task_id.size()) &&
         sha.update(&separator, 1U) &&
         sha.update(reinterpret_cast<const uint8_t*>(content_sha.data()),
                    content_sha.size()) &&
         sha.finishHex(output);
}

}  // namespace

PosixAtomicAlbumStore::PosixAtomicAlbumStore(
    std::string mount_root, bool removable,
    AlbumCapacityBackend capacity_backend, std::string capacity_identity)
    : root_(std::move(mount_root)),
      removable_(removable),
      capacity_backend_(capacity_backend),
      capacity_identity_(std::move(capacity_identity)) {
  paths_valid_ = validRoot(root_);
  if (!paths_valid_) return;
  album_directory_ = root_ + "/inkloop-album";
  index_path_ = album_directory_ + "/index.json";
  next_path_ = album_directory_ + "/index.next";
  previous_path_ = album_directory_ + "/index.prev";
  part_path_ = album_directory_ + "/asset.part";
}

bool PosixAtomicAlbumStore::safeUnlink(const std::string& path) const {
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool PosixAtomicAlbumStore::serverTaskId(const std::string& task_id) {
  return validServerTaskId(task_id);
}

myai::Status PosixAtomicAlbumStore::begin(size_t maximum_bytes) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_) {
    return myai::Status(myai::ErrorCode::InvalidState, 0,
                        "album storage maintenance is active");
  }
  abort();
  if (!paths_valid_ || maximum_bytes < 45U ||
      maximum_bytes > kMaximumAlbumAssetBytes) {
    return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                        "invalid album staging request");
  }
  if (::mkdir(album_directory_.c_str(), 0700) != 0 && errno != EEXIST)
    return storageFailure("album directory unavailable");
  struct stat directory {};
  if (!inspectPath(album_directory_, directory) ||
      !S_ISDIR(directory.st_mode) || !safeUnlink(part_path_)) {
    return storageFailure("album directory invalid");
  }
  descriptor_ = ::open(part_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor_ < 0) return storageFailure("album staging open failed");
  maximum_bytes_ = maximum_bytes;
  written_bytes_ = 0;
  return myai::Status::success();
}

myai::Status PosixAtomicAlbumStore::append(const uint8_t* bytes, size_t length) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_ || descriptor_ < 0 || !bytes || length == 0 ||
      written_bytes_ > maximum_bytes_ || length > maximum_bytes_ - written_bytes_) {
    return myai::Status(myai::ErrorCode::InvalidState, 0,
                        "album staging is not writable");
  }
  size_t at = 0;
  while (at < length) {
    const ssize_t count = ::write(descriptor_, bytes + at, length - at);
    if (count <= 0 || static_cast<size_t>(count) > length - at) {
      abort();
      return storageFailure("album staging write failed");
    }
    at += static_cast<size_t>(count);
  }
  written_bytes_ += length;
  return myai::Status::success();
}

bool PosixAtomicAlbumStore::readAndValidateAsset(
    const std::string& path, size_t expected_bytes, bool expected_landscape,
    std::string& sha256) const {
  sha256.clear();
  if (expected_bytes < 45U || expected_bytes > kMaximumAlbumAssetBytes)
    return false;
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return false;
  struct stat status {};
  bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
               status.st_size >= 0 &&
               static_cast<uint64_t>(status.st_size) == expected_bytes;
  PaperColorPngValidator validator(expected_bytes);
  Sha256 hash;
  std::array<uint8_t, kReadBufferBytes> buffer{};
  size_t total = 0;
  while (valid && total < expected_bytes) {
    const size_t wanted = std::min(buffer.size(), expected_bytes - total);
    const ssize_t count = ::read(descriptor, buffer.data(), wanted);
    if (count <= 0 || static_cast<size_t>(count) > wanted ||
        !validator.append(buffer.data(), static_cast<size_t>(count)) ||
        !hash.update(buffer.data(), static_cast<size_t>(count))) {
      valid = false;
      break;
    }
    total += static_cast<size_t>(count);
  }
  if (::close(descriptor) != 0) valid = false;
  return valid && total == expected_bytes && validator.finish(expected_bytes) &&
         validator.landscape() == expected_landscape && hash.finishHex(sha256) &&
         validAlbumAssetId(sha256);
}

bool PosixAtomicAlbumStore::loadIndex(const std::string& path,
                                      AlbumIndex& index) const {
  std::string bytes;
  return readBounded(path, kMaximumAlbumIndexBytes, bytes) &&
         parseAlbumIndex(bytes, index) == AlbumIndexCode::Ok;
}

bool PosixAtomicAlbumStore::recoverIndex(AlbumIndex& index) {
  const bool current_exists = exists(index_path_);
  const bool next_exists = exists(next_path_);
  const bool previous_exists = exists(previous_path_);
  if (current_exists && loadIndex(index_path_, index)) {
    return !next_exists || safeUnlink(next_path_);
  }
  AlbumIndex recovered;
  if (next_exists && loadIndex(next_path_, recovered)) {
    if ((current_exists && !safeUnlink(index_path_)) ||
        ::rename(next_path_.c_str(), index_path_.c_str()) != 0) return false;
    index = std::move(recovered);
    return true;
  }
  if (previous_exists && loadIndex(previous_path_, recovered)) {
    if ((current_exists && !safeUnlink(index_path_)) ||
        (next_exists && !safeUnlink(next_path_)) ||
        ::rename(previous_path_.c_str(), index_path_.c_str()) != 0) return false;
    index = std::move(recovered);
    return true;
  }
  if (current_exists || next_exists || previous_exists) return false;
  index = AlbumIndex();
  return true;
}

bool PosixAtomicAlbumStore::writeAllFile(const std::string& path,
                                         const std::string& bytes) const {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor < 0) return false;
  size_t at = 0;
  bool valid = true;
  while (valid && at < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + at,
                                  bytes.size() - at);
    if (count <= 0 || static_cast<size_t>(count) > bytes.size() - at)
      valid = false;
    else at += static_cast<size_t>(count);
  }
  if (valid && ::fsync(descriptor) != 0) valid = false;
  if (::close(descriptor) != 0) valid = false;
  return valid && at == bytes.size();
}

bool PosixAtomicAlbumStore::commitIndex(const AlbumIndex& index) {
  std::string encoded;
  if (encodeAlbumIndex(index, encoded) != AlbumIndexCode::Ok ||
      !safeUnlink(next_path_) || !writeAllFile(next_path_, encoded)) {
    safeUnlink(next_path_);
    return false;
  }
  AlbumIndex verified;
  if (!loadIndex(next_path_, verified) || !safeUnlink(previous_path_)) {
    safeUnlink(next_path_);
    return false;
  }
  const bool had_current = exists(index_path_);
  if (had_current && ::rename(index_path_.c_str(), previous_path_.c_str()) != 0) {
    safeUnlink(next_path_);
    return false;
  }
  if (::rename(next_path_.c_str(), index_path_.c_str()) == 0) return true;
  if (had_current) ::rename(previous_path_.c_str(), index_path_.c_str());
  safeUnlink(next_path_);
  return false;
}

bool PosixAtomicAlbumStore::queryCapacity(uint64_t& total,
                                          uint64_t& free) const {
  total = 0;
  free = 0;
#if defined(ESP_PLATFORM)
  if (capacity_backend_ == AlbumCapacityBackend::EspLittleFs) {
    size_t littlefs_total = 0;
    size_t littlefs_used = 0;
    if (capacity_identity_.empty() ||
        esp_littlefs_info(capacity_identity_.c_str(), &littlefs_total,
                          &littlefs_used) != ESP_OK ||
        littlefs_total == 0 || littlefs_used > littlefs_total) {
      return false;
    }
    total = littlefs_total;
    free = littlefs_total - littlefs_used;
    return true;
  }
  if (capacity_backend_ == AlbumCapacityBackend::EspFatFs) {
    const char* identity =
        capacity_identity_.empty() ? root_.c_str() : capacity_identity_.c_str();
    return esp_vfs_fat_info(identity, &total, &free) == ESP_OK && total > 0 &&
           free <= total;
  }
#else
  if (capacity_backend_ != AlbumCapacityBackend::PosixVfs) return false;
#endif

  struct statvfs capacity {};
  if (::statvfs(root_.c_str(), &capacity) != 0 || capacity.f_frsize == 0)
    return false;
  const uint64_t blocks = capacity.f_bavail;
  const uint64_t block_size = capacity.f_frsize;
  if (blocks > std::numeric_limits<uint64_t>::max() / block_size) return false;
  free = blocks * block_size;
  if (capacity.f_blocks >
      std::numeric_limits<uint64_t>::max() / block_size) return false;
  total = capacity.f_blocks * block_size;
  return total > 0 && free <= total;
}

bool PosixAtomicAlbumStore::hasCommitCapacity(size_t index_bytes) const {
  uint64_t total = 0;
  uint64_t available = 0;
  if (!queryCapacity(total, available)) return false;
  const uint64_t reserve = removable_ ? kSdReserveBytes : kLittleFsReserveBytes;
  return index_bytes <= available && reserve <= available - index_bytes;
}

myai::Status PosixAtomicAlbumStore::commitValidated(
    const AlbumCommitRequest& request, AlbumCommitResult& result) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  result = AlbumCommitResult();
  if (maintenance_ || descriptor_ < 0 ||
      request.bytes != written_bytes_ ||
      request.bytes < 45U || request.bytes > maximum_bytes_ ||
      request.prompt_id.empty() || request.prompt_id.size() > 128U ||
      request.task_id.empty() || request.task_id.size() > 256U ||
      request.source_filename.empty() || request.source_filename.size() > 512U ||
      !validRenderStrategy(request.render_strategy)) {
    abort();
    return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                        "invalid album commit request");
  }
  bool durable = ::fsync(descriptor_) == 0;
  if (::close(descriptor_) != 0) durable = false;
  descriptor_ = -1;
  if (!durable) {
    safeUnlink(part_path_);
    return storageFailure("album staging durability failed");
  }
  std::string hash;
  if (!readAndValidateAsset(part_path_, request.bytes, request.landscape, hash)) {
    safeUnlink(part_path_);
    return myai::Status(myai::ErrorCode::Protocol, 0,
                        "album staging validation failed");
  }
  AlbumIndex index;
  if (!recoverIndex(index)) {
    safeUnlink(part_path_);
    return storageFailure("album index recovery required");
  }
  std::string logical_id;
  if (!logicalAssetId(request.task_id, hash, logical_id)) {
    safeUnlink(part_path_);
    return storageFailure("album logical id generation failed");
  }
  for (size_t ordinal = 0; ordinal < index.assets.size(); ++ordinal) {
    AlbumIndexAsset& existing = index.assets[ordinal];
    if (existing.id != logical_id) continue;
    std::string existing_hash;
    if (!readAndValidateAsset(root_ + existing.path, existing.bytes,
                              existing.landscape, existing_hash) ||
        existing_hash != hash || existing.content_sha256 != hash) {
      safeUnlink(part_path_);
      return storageFailure("album duplicate is corrupt");
    }
    safeUnlink(part_path_);
    if (serverTaskId(request.task_id) &&
        existing.render_strategy != request.render_strategy) {
      existing.render_strategy = request.render_strategy;
      if (!commitIndex(index))
        return storageFailure("album duplicate metadata commit failed");
    }
    result.asset_id = logical_id;
    result.content_sha256 = hash;
    result.path = existing.path;
    result.ordinal = ordinal;
    maximum_bytes_ = 0;
    written_bytes_ = 0;
    return myai::Status::success();
  }
  const size_t entry_limit = removable_ ? kSdEntryLimit : kLittleFsEntryLimit;
  if (index.assets.size() >= entry_limit) {
    safeUnlink(part_path_);
    return storageFailure("album entry limit reached");
  }
  AlbumIndex predicted = index;
  AlbumIndexAsset asset;
  asset.id = logical_id;
  asset.content_sha256 = hash;
  asset.path = "/inkloop-album/" + hash + ".png";
  asset.bytes = request.bytes;
  asset.landscape = request.landscape;
  const std::time_t now = std::time(nullptr);
  asset.created = now > 0 && static_cast<uint64_t>(now) <= 0xffffffffULL
                      ? static_cast<uint32_t>(now)
                      : 0U;
  asset.task_id = request.task_id;
  asset.render_strategy = request.render_strategy;
  predicted.assets.push_back(asset);
  std::string encoded;
  if (encodeAlbumIndex(predicted, encoded) != AlbumIndexCode::Ok ||
      !hasCommitCapacity(encoded.size())) {
    safeUnlink(part_path_);
    return storageFailure("album commit capacity unavailable");
  }
  const std::string final_path = root_ + asset.path;
  bool promoted_new = false;
  if (exists(final_path)) {
    std::string existing_hash;
    if (!readAndValidateAsset(final_path, request.bytes, request.landscape,
                              existing_hash) || existing_hash != hash) {
      safeUnlink(part_path_);
      return storageFailure("album final asset collision");
    }
    safeUnlink(part_path_);
  } else {
    if (::rename(part_path_.c_str(), final_path.c_str()) != 0) {
      safeUnlink(part_path_);
      return storageFailure("album asset promotion failed");
    }
    promoted_new = true;
  }
  if (!commitIndex(predicted)) {
    if (promoted_new) safeUnlink(final_path);
    return storageFailure("album index atomic commit failed");
  }
  result.asset_id = logical_id;
  result.content_sha256 = hash;
  result.path = asset.path;
  result.ordinal = predicted.assets.size() - 1U;
  maximum_bytes_ = 0;
  written_bytes_ = 0;
  return myai::Status::success();
}

void PosixAtomicAlbumStore::abort() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_) {
    // beginMaintenance() is admitted only with no open staging descriptor.
    // A failed producer admission may still call abort() unconditionally;
    // during format/remount that cleanup must remain memory-only and must not
    // touch the VFS (even to unlink a stale asset.part path).
    descriptor_ = -1;
    maximum_bytes_ = 0;
    written_bytes_ = 0;
    return;
  }
  if (descriptor_ >= 0) {
    ::close(descriptor_);
    descriptor_ = -1;
  }
  if (paths_valid_) safeUnlink(part_path_);
  maximum_bytes_ = 0;
  written_bytes_ = 0;
}

bool PosixAtomicAlbumStore::beginMaintenance() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_ || descriptor_ >= 0) return false;
  maintenance_ = true;
  return true;
}

void PosixAtomicAlbumStore::endMaintenance() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  maintenance_ = false;
}

myai::Status PosixAtomicAlbumStore::readCatalog(AlbumIndex& index) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  index = AlbumIndex();
  if (maintenance_ || !paths_valid_ || active()) {
    return myai::Status(myai::ErrorCode::InvalidState, 0,
                        "album catalog is busy");
  }
  return recoverIndex(index)
             ? myai::Status::success()
             : storageFailure("album index recovery required");
}

myai::Status PosixAtomicAlbumStore::markCurrent(
    const std::string& asset_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_ || !validAlbumAssetId(asset_id) || active()) {
    return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                        "invalid current album asset");
  }
  AlbumIndex index;
  if (!recoverIndex(index))
    return storageFailure("album index recovery required");
  const AlbumIndexAsset* found = nullptr;
  for (const AlbumIndexAsset& asset : index.assets) {
    if (asset.id == asset_id) {
      found = &asset;
      break;
    }
  }
  if (!found) {
    return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                        "album asset is not indexed");
  }
  if (index.current == asset_id &&
      index.current_render_strategy == found->render_strategy)
    return myai::Status::success();
  index.current = asset_id;
  index.current_render_strategy = found->render_strategy;
  return commitIndex(index)
             ? myai::Status::success()
             : storageFailure("album current index commit failed");
}

myai::Status PosixAtomicAlbumStore::updateRenderStrategy(
    const std::string& asset_id, const std::string& render_strategy) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_ || !validAlbumAssetId(asset_id) ||
      !validRenderStrategy(render_strategy) || active()) {
    return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                        "invalid album render strategy update");
  }
  AlbumIndex index;
  if (!recoverIndex(index))
    return storageFailure("album index recovery required");
  for (AlbumIndexAsset& asset : index.assets) {
    if (asset.id != asset_id) continue;
    if (asset.render_strategy == render_strategy)
      return myai::Status::success();
    asset.render_strategy = render_strategy;
    return commitIndex(index)
               ? myai::Status::success()
               : storageFailure("album render strategy commit failed");
  }
  return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                      "album asset is not indexed");
}

AlbumMutationCode PosixAtomicAlbumStore::removeAssetById(
    const std::string& asset_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_) return AlbumMutationCode::Busy;
  if (!validAlbumAssetId(asset_id)) return AlbumMutationCode::NotFound;
  if (active()) return AlbumMutationCode::Busy;
  AlbumIndex current;
  if (!recoverIndex(current)) return AlbumMutationCode::RecoveryRequired;
  size_t ordinal = current.assets.size();
  for (size_t at = 0; at < current.assets.size(); ++at) {
    if (current.assets[at].id == asset_id) {
      ordinal = at;
      break;
    }
  }
  if (ordinal == current.assets.size()) return AlbumMutationCode::NotFound;
  return removeAssetByOrdinal(ordinal);
}

AlbumMutationCode PosixAtomicAlbumStore::removeAssetByOrdinal(
    size_t ordinal) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (maintenance_ || active()) return AlbumMutationCode::Busy;
  AlbumIndex current;
  if (!recoverIndex(current)) return AlbumMutationCode::RecoveryRequired;
  if (ordinal >= current.assets.size()) return AlbumMutationCode::NotFound;
  const AlbumIndexAsset removed = current.assets[ordinal];
  AlbumIndex next = current;
  next.assets.erase(next.assets.begin() + static_cast<std::ptrdiff_t>(ordinal));
  if (next.current == removed.id) {
    next.current.clear();
    next.current_render_strategy.clear();
  }
  if (!commitIndex(next)) return AlbumMutationCode::PersistenceFailed;
  for (const AlbumIndexAsset& retained : next.assets) {
    if (retained.path == removed.path) return AlbumMutationCode::Ok;
  }
  return safeUnlink(root_ + removed.path) ? AlbumMutationCode::Ok
                                         : AlbumMutationCode::UnlinkFailed;
}

AlbumMutationCode PosixAtomicAlbumStore::clearAssets(size_t& removed) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  removed = 0;
  if (maintenance_ || active()) return AlbumMutationCode::Busy;
  AlbumIndex current;
  if (!recoverIndex(current)) return AlbumMutationCode::RecoveryRequired;
  removed = current.assets.size();
  if (current.assets.empty()) return AlbumMutationCode::Ok;
  AlbumIndex next;
  if (!commitIndex(next)) {
    removed = 0;
    return AlbumMutationCode::PersistenceFailed;
  }
  std::vector<std::string> unlinked;
  bool success = true;
  for (const AlbumIndexAsset& asset : current.assets) {
    if (std::find(unlinked.begin(), unlinked.end(), asset.path) !=
        unlinked.end()) {
      continue;
    }
    unlinked.push_back(asset.path);
    if (!safeUnlink(root_ + asset.path)) success = false;
  }
  return success ? AlbumMutationCode::Ok : AlbumMutationCode::UnlinkFailed;
}

myai::Status PosixAtomicAlbumStore::pruneTaskAssets(
    const std::vector<AlbumTaskBinding>& retained, size_t& removed) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  removed = 0;
  if (maintenance_ || active() || retained.size() > 128U) {
    return myai::Status(myai::ErrorCode::InvalidState, 0,
                        "album task prune is busy or oversized");
  }
  for (size_t at = 0; at < retained.size(); ++at) {
    if (!serverTaskId(retained[at].task_id) ||
        !validAlbumAssetId(retained[at].asset_id) ||
        !validRenderStrategy(retained[at].render_strategy)) {
      return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                          "invalid retained task asset");
    }
    for (size_t other = at + 1U; other < retained.size(); ++other) {
      if (retained[at].task_id == retained[other].task_id) {
        return myai::Status(myai::ErrorCode::InvalidArgument, 0,
                            "duplicate retained task");
      }
    }
  }
  AlbumIndex current;
  if (!recoverIndex(current))
    return storageFailure("album index recovery required");
  AlbumIndex next = current;
  std::vector<AlbumIndexAsset> stale;
  bool metadata_changed = false;
  next.assets.clear();
  for (AlbumIndexAsset asset : current.assets) {
    bool keep = !serverTaskId(asset.task_id);
    if (!keep) {
      for (const AlbumTaskBinding& binding : retained) {
        if (asset.task_id == binding.task_id &&
            asset.content_sha256 == binding.asset_id) {
          keep = true;
          if (asset.render_strategy != binding.render_strategy) {
            asset.render_strategy = binding.render_strategy;
            metadata_changed = true;
          }
          break;
        }
      }
    }
    if (keep)
      next.assets.push_back(asset);
    else
      stale.push_back(asset);
  }
  if (stale.empty() && !metadata_changed) return myai::Status::success();
  for (const AlbumIndexAsset& asset : stale) {
    if (next.current == asset.id) {
      next.current.clear();
      next.current_render_strategy.clear();
    }
  }
  if (!commitIndex(next)) return storageFailure("album task prune commit failed");
  bool unlinked = true;
  for (const AlbumIndexAsset& asset : stale) {
    bool shared = false;
    for (const AlbumIndexAsset& retained_asset : next.assets) {
      if (retained_asset.path == asset.path) {
        shared = true;
        break;
      }
    }
    if (!shared && !safeUnlink(root_ + asset.path)) unlinked = false;
    ++removed;
  }
  return unlinked ? myai::Status::success()
                  : storageFailure("album task asset unlink failed");
}

bool PosixAtomicAlbumStore::absoluteAssetPath(
    const AlbumIndexAsset& asset, std::string& path) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  path.clear();
  if (maintenance_ || !paths_valid_ || !validAlbumAssetId(asset.id) ||
      !validAlbumAssetId(asset.content_sha256) ||
      asset.path != std::string("/inkloop-album/") +
                        asset.content_sha256 + ".png") {
    return false;
  }
  path = root_ + asset.path;
  return true;
}

}  // namespace storage
}  // namespace inkloop
