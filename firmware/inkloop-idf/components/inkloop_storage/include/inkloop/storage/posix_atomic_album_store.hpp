#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "inkloop/storage/aigc_album_sink.hpp"
#include "inkloop/storage/album_index.hpp"

namespace inkloop {
namespace storage {

struct AlbumTaskBinding {
  std::string task_id;
  // Expected physical frame SHA from the authoritative task revision. The
  // historical member name is kept to avoid widening the migration surface.
  std::string asset_id;
  // A task replacement is authoritative for cached rendering too, so a
  // strategy-only edit does not require downloading identical PNG bytes.
  std::string render_strategy = "official-quality";
};

enum class AlbumMutationCode : uint8_t {
  Ok,
  Busy,
  NotFound,
  RecoveryRequired,
  PersistenceFailed,
  UnlinkFailed,
};

// POSIX VFS implementation used by the sole storage owner after LittleFS or SD
// has been mounted. It never mounts, formats, erases or selects a filesystem.
class PosixAtomicAlbumStore final : public IAlbumStagingStore {
 public:
  PosixAtomicAlbumStore(std::string mount_root, bool removable);
  ~PosixAtomicAlbumStore() override { abort(); }

  myai::Status begin(size_t maximum_bytes) override;
  myai::Status append(const uint8_t* bytes, size_t length) override;
  myai::Status commitValidated(const AlbumCommitRequest& request,
                               AlbumCommitResult& result) override;
  void abort() override;

  // Composition-owned destructive maintenance gate. Unlike active(), this
  // covers every catalog/staging operation, not just an open asset.part file.
  // The caller must still quiesce higher-level owners first so queued work can
  // be rejected cleanly instead of waiting on this mutex.
  bool beginMaintenance();
  void endMaintenance();

  // Read/display operations are admitted only while no staged mutation is
  // active. Recovery may promote an already durable index.next/index.prev but
  // never invents or deletes a committed asset.
  myai::Status readCatalog(AlbumIndex& index);
  myai::Status markCurrent(const std::string& asset_id);
  myai::Status updateRenderStrategy(const std::string& asset_id,
                                    const std::string& render_strategy);
  // User/Portal mutations commit the new index before unlinking physical
  // bytes. Multiple logical owners can share one content-addressed PNG; bytes
  // are removed only after the last owner disappears. A failed post-commit
  // unlink leaves a harmless orphan and never a dangling catalog entry.
  AlbumMutationCode removeAssetById(const std::string& asset_id);
  AlbumMutationCode removeAssetByOrdinal(size_t zero_based_ordinal);
  AlbumMutationCode clearAssets(size_t& removed);
  // Removes cache entries owned by deleted or superseded Inkloop tasks and
  // applies retained render-strategy edits in the same atomic index commit. It
  // never touches AIGC/upload assets. Index removal commits before unlink, so
  // power loss may leave a harmless orphan but never a dangling index entry.
  myai::Status pruneTaskAssets(
      const std::vector<AlbumTaskBinding>& retained, size_t& removed);
  bool absoluteAssetPath(const AlbumIndexAsset& asset,
                         std::string& path) const;

  bool pathsValid() const { return paths_valid_; }
  bool active() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return descriptor_ >= 0;
  }

 private:
  bool readAndValidateAsset(const std::string& path, size_t expected_bytes,
                            bool expected_landscape,
                            std::string& sha256) const;
  bool loadIndex(const std::string& path, AlbumIndex& index) const;
  bool recoverIndex(AlbumIndex& index);
  bool commitIndex(const AlbumIndex& index);
  bool writeAllFile(const std::string& path, const std::string& bytes) const;
  bool hasCommitCapacity(size_t index_bytes) const;
  bool safeUnlink(const std::string& path) const;
  static bool serverTaskId(const std::string& task_id);

  std::string root_;
  std::string album_directory_;
  std::string index_path_;
  std::string next_path_;
  std::string previous_path_;
  std::string part_path_;
  bool removable_ = false;
  bool paths_valid_ = false;
  int descriptor_ = -1;
  size_t maximum_bytes_ = 0;
  size_t written_bytes_ = 0;
  bool maintenance_ = false;
  // Display, Portal, cloud delivery and local tools live on different tasks.
  // Serialize every catalog/staging transition in the store itself instead of
  // relying on a check-then-act active() observation in a caller. Recursive is
  // intentional because a few public operations share another public helper
  // (begin->abort and remove-by-id->remove-by-ordinal).
  mutable std::recursive_mutex mutex_;
};

}  // namespace storage
}  // namespace inkloop
