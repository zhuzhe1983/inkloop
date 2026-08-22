#include "inkloop/cloud/inkloop_frame_stream.hpp"

#include <utility>

namespace inkloop {
namespace cloud {
namespace {

InkloopCloudStatus status(InkloopCloudCode code, const char* detail) {
  InkloopCloudStatus output;
  output.code = code;
  output.detail = detail ? detail : "";
  return output;
}

bool lowerSha256(const std::string& value) {
  if (value.size() != 64U) return false;
  for (const char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  }
  return true;
}

bool validTaskId(const std::string& value) {
  if (value.empty() || value.size() > 256U) return false;
  for (const unsigned char ch : value) {
    if (ch < 0x20U || ch == 0x7fU) return false;
  }
  return true;
}

bool validRenderStrategy(const std::string& value) {
  return value == "official-quality" || value == "classic-six-color" ||
         value == "reflectance-photo" || value == "solid-clean";
}

}  // namespace

InkloopFrameStream::InkloopFrameStream(IInkloopFrameStagingSink& sink)
    : sink_(sink), png_(kMaximumInkloopFrameBytes) {}

InkloopFrameStream::~InkloopFrameStream() { abort(); }

InkloopCloudStatus InkloopFrameStream::fail(InkloopCloudCode code,
                                             const char* detail) {
  sink_.abort();
  active_ = false;
  expected_sha256_.clear();
  expected_bytes_ = 0;
  received_bytes_ = 0;
  png_.reset(kMaximumInkloopFrameBytes);
  sha_ = storage::Sha256();
  return status(code, detail);
}

InkloopCloudStatus InkloopFrameStream::begin(
    const InkloopFrameStagingRequest& request,
    const std::string& expected_sha256) {
  abort();
  if (!validTaskId(request.task_id) ||
      !validRenderStrategy(request.render_strategy) ||
      request.content_length < 45U ||
      request.content_length > kMaximumInkloopFrameBytes ||
      !lowerSha256(expected_sha256)) {
    return status(request.content_length > kMaximumInkloopFrameBytes
                      ? InkloopCloudCode::TooLarge
                      : InkloopCloudCode::InvalidArgument,
                  "invalid_frame_stream_request");
  }
  InkloopCloudStatus opened = sink_.begin(request);
  if (!opened.ok()) {
    sink_.abort();
    return opened;
  }
  png_.reset(kMaximumInkloopFrameBytes);
  sha_ = storage::Sha256();
  expected_sha256_ = expected_sha256;
  expected_bytes_ = request.content_length;
  received_bytes_ = 0;
  active_ = true;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopFrameStream::append(const uint8_t* bytes,
                                               size_t length) {
  if (!active_ || !bytes || length == 0U)
    return fail(InkloopCloudCode::Protocol, "invalid_frame_chunk");
  if (received_bytes_ > expected_bytes_ ||
      length > expected_bytes_ - received_bytes_) {
    return fail(InkloopCloudCode::TooLarge, "frame_exceeds_content_length");
  }
  if (!png_.append(bytes, length))
    return fail(InkloopCloudCode::Protocol, "invalid_frame_png");
  if (!sha_.update(bytes, length))
    return fail(InkloopCloudCode::Protocol, "frame_sha_update_failed");
  const InkloopCloudStatus written = sink_.append(bytes, length);
  if (!written.ok()) return fail(written.code, "frame_staging_write_failed");
  received_bytes_ += length;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus InkloopFrameStream::finish(
    InkloopFrameMetadata& metadata) {
  metadata = InkloopFrameMetadata();
  if (!active_ || received_bytes_ != expected_bytes_ ||
      !png_.finish(received_bytes_)) {
    return fail(InkloopCloudCode::Protocol, "frame_incomplete_or_invalid");
  }
  std::string actual_sha256;
  if (!sha_.finishHex(actual_sha256) || actual_sha256 != expected_sha256_)
    return fail(InkloopCloudCode::Security, "frame_sha256_mismatch");

  InkloopFrameMetadata completed;
  completed.bytes = received_bytes_;
  completed.landscape = png_.landscape();
  completed.sha256 = std::move(actual_sha256);
  const InkloopCloudStatus committed = sink_.commit(completed);
  if (!committed.ok())
    return fail(committed.code, "frame_staging_commit_failed");

  metadata = completed;
  active_ = false;
  expected_sha256_.clear();
  expected_bytes_ = 0;
  received_bytes_ = 0;
  png_.reset(kMaximumInkloopFrameBytes);
  sha_ = storage::Sha256();
  return InkloopCloudStatus::success();
}

void InkloopFrameStream::abort() {
  if (active_) sink_.abort();
  active_ = false;
  expected_sha256_.clear();
  expected_bytes_ = 0;
  received_bytes_ = 0;
  png_.reset(kMaximumInkloopFrameBytes);
  sha_ = storage::Sha256();
}

}  // namespace cloud
}  // namespace inkloop
