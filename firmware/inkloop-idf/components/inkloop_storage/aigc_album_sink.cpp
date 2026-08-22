#include "inkloop/storage/aigc_album_sink.hpp"

#include <utility>

namespace inkloop {
namespace storage {
namespace {

bool stableAssetId(const std::string& value) {
  if (value.size() != 64U) return false;
  for (char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  }
  return true;
}

}  // namespace

AigcAlbumSink::AigcAlbumSink(IAlbumStagingStore& store, size_t maximum_bytes,
                             std::string render_strategy)
    : store_(store),
      maximum_bytes_(maximum_bytes),
      render_strategy_(std::move(render_strategy)),
      validator_(maximum_bytes) {}

myai::Status AigcAlbumSink::fail(myai::ErrorCode code, const char* detail,
                                 bool abort_store) {
  if (abort_store) store_.abort();
  active_ = false;
  bytes_ = 0;
  validator_.reset(maximum_bytes_);
  metadata_ = myai::AigcOutputMetadata();
  return myai::Status(code, 0, detail);
}

myai::Status AigcAlbumSink::begin(
    const myai::AigcOutputMetadata& metadata) {
  abort();
  committed_ready_ = false;
  committed_ = AlbumCommitResult();
  if (maximum_bytes_ < 45U || maximum_bytes_ > 6U * 1024U * 1024U ||
      render_strategy_.empty() || render_strategy_.size() > 64U ||
      (metadata.contentType != "image/png" &&
       metadata.contentType != "image/x-png") ||
      metadata.promptId.empty() || metadata.promptId.size() > 128U ||
      metadata.filename.empty() || metadata.filename.size() > 512U) {
    return fail(myai::ErrorCode::InvalidArgument,
                "invalid AIGC album metadata");
  }
  const myai::Status opened = store_.begin(maximum_bytes_);
  if (!opened.ok()) return fail(opened.code, "AIGC album staging open failed");
  metadata_ = metadata;
  bytes_ = 0;
  validator_.reset(maximum_bytes_);
  active_ = true;
  return myai::Status::success();
}

myai::Status AigcAlbumSink::write(const uint8_t* bytes, size_t length) {
  if (!active_ || !bytes || length == 0 || bytes_ > maximum_bytes_ ||
      length > maximum_bytes_ - bytes_) {
    return fail(myai::ErrorCode::TooLarge,
                "AIGC album stream exceeds cap");
  }
  if (!validator_.append(bytes, length)) {
    return fail(myai::ErrorCode::Protocol, "invalid PaperColor PNG stream");
  }
  const myai::Status written = store_.append(bytes, length);
  if (!written.ok()) {
    return fail(written.code, "AIGC album staging write failed");
  }
  bytes_ += length;
  return myai::Status::success();
}

myai::Status AigcAlbumSink::commit(myai::AigcOutputMetadata& metadata) {
  if (!active_ || bytes_ < 45U || metadata.decodedBytes != bytes_ ||
      metadata.promptId != metadata_.promptId ||
      metadata.filename != metadata_.filename ||
      !validator_.finish(bytes_)) {
    return fail(myai::ErrorCode::Protocol,
                "AIGC album payload is incomplete or inconsistent");
  }
  AlbumCommitRequest request;
  request.prompt_id = metadata_.promptId;
  request.task_id = "aigc:" + metadata_.promptId;
  request.source_filename = metadata_.filename;
  request.render_strategy = render_strategy_;
  request.bytes = bytes_;
  request.landscape = validator_.landscape();
  AlbumCommitResult committed;
  const myai::Status stored = store_.commitValidated(request, committed);
  if (!stored.ok() || !stableAssetId(committed.asset_id) ||
      committed.path.empty()) {
    return fail(stored.ok() ? myai::ErrorCode::Storage : stored.code,
                "AIGC album atomic commit failed");
  }
  committed_ = std::move(committed);
  committed_ready_ = true;
  active_ = false;
  bytes_ = 0;
  validator_.reset(maximum_bytes_);
  metadata_ = myai::AigcOutputMetadata();
  return myai::Status::success();
}

void AigcAlbumSink::abort() {
  if (active_) store_.abort();
  active_ = false;
  bytes_ = 0;
  validator_.reset(maximum_bytes_);
  metadata_ = myai::AigcOutputMetadata();
}

bool AigcAlbumSink::takeCommittedAsset(AlbumCommitResult& result) {
  if (!committed_ready_) return false;
  result = committed_;
  committed_ = AlbumCommitResult();
  committed_ready_ = false;
  return true;
}

}  // namespace storage
}  // namespace inkloop
