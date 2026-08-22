#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/storage/papercolor_png.hpp"

namespace inkloop {
namespace storage {

struct AlbumCommitRequest {
  std::string prompt_id;
  // Stable source ownership key. AIGC uses `aigc:<prompt_id>`; an Inkloop
  // scheduled frame uses the exact server task id so deletions can prune only
  // task-owned cache entries without touching uploads or generated images.
  std::string task_id;
  std::string source_filename;
  std::string render_strategy;
  size_t bytes = 0;
  bool landscape = false;
};

struct AlbumCommitResult {
  // Logical album entry id used by display/current/Portal operations.
  std::string asset_id;
  // Verified SHA-256 of the physical PNG. It may differ from asset_id for a
  // task-owned logical entry that shares bytes with another source.
  std::string content_sha256;
  std::string path;
  size_t ordinal = 0;
};

// Implemented by the sole storage owner. commitValidated() must fsync/close,
// re-read the staged file through the same PNG validator, compute SHA-256,
// reserve final capacity, atomically promote the asset, then atomically commit
// the album index. A failure must leave no committed index entry.
class IAlbumStagingStore {
 public:
  virtual ~IAlbumStagingStore() = default;
  virtual myai::Status begin(size_t maximum_bytes) = 0;
  virtual myai::Status append(const uint8_t* bytes, size_t length) = 0;
  virtual myai::Status commitValidated(const AlbumCommitRequest& request,
                                       AlbumCommitResult& result) = 0;
  virtual void abort() = 0;
};

class AigcAlbumSink final : public myai::IImageSink {
 public:
  AigcAlbumSink(IAlbumStagingStore& store, size_t maximum_bytes,
                std::string render_strategy);
  ~AigcAlbumSink() override { abort(); }

  myai::Status begin(const myai::AigcOutputMetadata& metadata) override;
  myai::Status write(const uint8_t* bytes, size_t length) override;
  myai::Status commit(myai::AigcOutputMetadata& metadata) override;
  void abort() override;

  bool takeCommittedAsset(AlbumCommitResult& result);
  bool active() const { return active_; }

 private:
  myai::Status fail(myai::ErrorCode code, const char* detail,
                    bool abort_store = true);

  IAlbumStagingStore& store_;
  size_t maximum_bytes_;
  std::string render_strategy_;
  PaperColorPngValidator validator_;
  myai::AigcOutputMetadata metadata_;
  AlbumCommitResult committed_;
  size_t bytes_ = 0;
  bool active_ = false;
  bool committed_ready_ = false;
};

}  // namespace storage
}  // namespace inkloop
