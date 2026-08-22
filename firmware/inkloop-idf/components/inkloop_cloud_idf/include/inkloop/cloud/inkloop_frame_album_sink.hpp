#pragma once

#include "inkloop/cloud/inkloop_frame_stream.hpp"
#include "inkloop/storage/aigc_album_sink.hpp"

namespace inkloop {
namespace cloud {

// Concrete streaming bridge to the existing atomic album staging owner. The
// exact server task id is persisted as AlbumCommitRequest::task_id.
class InkloopFrameAlbumSink final : public IInkloopFrameStagingSink {
 public:
  explicit InkloopFrameAlbumSink(storage::IAlbumStagingStore& store);
  ~InkloopFrameAlbumSink() override { abort(); }

  InkloopCloudStatus begin(
      const InkloopFrameStagingRequest& request) override;
  InkloopCloudStatus append(
      const uint8_t* bytes, size_t length) override;
  InkloopCloudStatus commit(
      const InkloopFrameMetadata& metadata) override;
  void abort() override;

  bool takeCommittedAsset(storage::AlbumCommitResult& result);
  bool active() const { return active_; }

 private:
  InkloopCloudStatus fail(InkloopCloudCode code, const char* detail,
                          bool abort_store = true);

  storage::IAlbumStagingStore& store_;
  InkloopFrameStagingRequest request_;
  storage::AlbumCommitResult committed_;
  size_t written_bytes_ = 0;
  bool active_ = false;
  bool committed_ready_ = false;
};

}  // namespace cloud
}  // namespace inkloop
