#include "inkloop/cloud/inkloop_frame_album_sink.hpp"

#include <utility>

namespace inkloop {
namespace cloud {
namespace {

InkloopCloudCode cloudCode(myai::ErrorCode code) {
  switch (code) {
    case myai::ErrorCode::None:
      return InkloopCloudCode::Ok;
    case myai::ErrorCode::InvalidArgument:
    case myai::ErrorCode::InvalidState:
      return InkloopCloudCode::InvalidArgument;
    case myai::ErrorCode::Storage:
    case myai::ErrorCode::RecoveryRequired:
      return InkloopCloudCode::Storage;
    case myai::ErrorCode::Security:
      return InkloopCloudCode::Security;
    case myai::ErrorCode::Transport:
      return InkloopCloudCode::Transport;
    case myai::ErrorCode::Unauthorized:
      return InkloopCloudCode::Unauthorized;
    case myai::ErrorCode::Conflict:
      return InkloopCloudCode::Conflict;
    case myai::ErrorCode::TooLarge:
      return InkloopCloudCode::TooLarge;
    default:
      return InkloopCloudCode::Protocol;
  }
}

InkloopCloudStatus converted(const myai::Status& status,
                             const char* fallback) {
  InkloopCloudStatus output;
  output.code = cloudCode(status.code);
  output.http_status = status.httpStatus;
  output.retry_after_ms = status.retryAfterMs;
  output.detail = fallback ? fallback : "";
  return output;
}

}  // namespace

InkloopFrameAlbumSink::InkloopFrameAlbumSink(
    storage::IAlbumStagingStore& store)
    : store_(store) {}

InkloopCloudStatus InkloopFrameAlbumSink::fail(InkloopCloudCode code,
                                                const char* detail,
                                                bool abort_store) {
  if (abort_store) store_.abort();
  active_ = false;
  request_ = InkloopFrameStagingRequest();
  written_bytes_ = 0;
  InkloopCloudStatus output;
  output.code = code;
  output.detail = detail ? detail : "";
  return output;
}

InkloopCloudStatus InkloopFrameAlbumSink::begin(
    const InkloopFrameStagingRequest& request) {
  abort();
  committed_ready_ = false;
  committed_ = storage::AlbumCommitResult();
  if (request.task_id.empty() || request.task_id.size() > 128U ||
      request.render_strategy.empty() || request.render_strategy.size() > 64U ||
      request.content_length < 45U ||
      request.content_length > kMaximumInkloopFrameBytes) {
    return fail(InkloopCloudCode::InvalidArgument,
                "invalid_frame_album_request", false);
  }
  const myai::Status opened = store_.begin(kMaximumInkloopFrameBytes);
  if (!opened.ok()) {
    store_.abort();
    return converted(opened, "frame_album_staging_open_failed");
  }
  request_ = request;
  written_bytes_ = 0;
  active_ = true;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopFrameAlbumSink::append(const uint8_t* bytes,
                                                 size_t length) {
  if (!active_ || !bytes || length == 0U ||
      written_bytes_ > request_.content_length ||
      length > request_.content_length - written_bytes_) {
    return fail(InkloopCloudCode::TooLarge, "frame_album_append_invalid");
  }
  const myai::Status written = store_.append(bytes, length);
  if (!written.ok()) {
    store_.abort();
    active_ = false;
    request_ = InkloopFrameStagingRequest();
    written_bytes_ = 0;
    return converted(written, "frame_album_staging_write_failed");
  }
  written_bytes_ += length;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopFrameAlbumSink::commit(
    const InkloopFrameMetadata& metadata) {
  if (!active_ || written_bytes_ != request_.content_length ||
      metadata.bytes != written_bytes_ || metadata.sha256.size() != 64U) {
    return fail(InkloopCloudCode::Protocol,
                "frame_album_commit_metadata_invalid");
  }
  storage::AlbumCommitRequest request;
  // prompt_id remains populated for the current store schema, but task_id is
  // the authoritative ownership field and is never rewritten or prefixed.
  request.prompt_id = request_.task_id;
  request.task_id = request_.task_id;
  request.source_filename = "inkloop-frame.png";
  request.render_strategy = request_.render_strategy;
  request.bytes = metadata.bytes;
  request.landscape = metadata.landscape;
  storage::AlbumCommitResult committed;
  const myai::Status stored = store_.commitValidated(request, committed);
  if (!stored.ok()) {
    store_.abort();
    active_ = false;
    request_ = InkloopFrameStagingRequest();
    written_bytes_ = 0;
    return converted(stored, "frame_album_atomic_commit_failed");
  }
  if (committed.content_sha256 != metadata.sha256 ||
      committed.asset_id.empty() || committed.path.empty()) {
    return fail(InkloopCloudCode::Storage,
                "frame_album_commit_verification_failed");
  }
  committed_ = std::move(committed);
  committed_ready_ = true;
  active_ = false;
  request_ = InkloopFrameStagingRequest();
  written_bytes_ = 0;
  return InkloopCloudStatus::success();
}

void InkloopFrameAlbumSink::abort() {
  if (active_) store_.abort();
  active_ = false;
  request_ = InkloopFrameStagingRequest();
  written_bytes_ = 0;
}

bool InkloopFrameAlbumSink::takeCommittedAsset(
    storage::AlbumCommitResult& result) {
  if (!committed_ready_) return false;
  result = committed_;
  committed_ = storage::AlbumCommitResult();
  committed_ready_ = false;
  return true;
}

}  // namespace cloud
}  // namespace inkloop
