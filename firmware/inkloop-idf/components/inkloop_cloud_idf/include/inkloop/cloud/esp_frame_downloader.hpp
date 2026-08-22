#pragma once

#include <cstdint>
#include <string>

#include "inkloop/cloud/inkloop_frame_album_sink.hpp"
#include "inkloop/cloud/inkloop_frame_stream.hpp"

namespace inkloop {
namespace myai {
class EspEndpointSecurity;
}
namespace cloud {

struct InkloopFrameDownloadRequest {
  std::string task_id;
  std::string url;
  std::string expected_sha256;
  std::string render_strategy = "official-quality";
  uint32_t timeout_ms = 30000U;
};

// Blocking native HTTPS adapter intended for the sole slow network owner. It
// requires exact Content-Length and streams each 2 KiB read directly to sink.
class EspInkloopFrameDownloader final : public IInkloopFrameDownloader {
 public:
  explicit EspInkloopFrameDownloader(
      myai::EspEndpointSecurity& endpoint_security);

  InkloopCloudStatus download(
      const InkloopFrameDownloadRequest& request,
      const InkloopIdentitySnapshot& identity,
      IInkloopFrameStagingSink& sink,
      InkloopFrameMetadata& metadata);

  // Product-facing convenience path: the scheduled task is the authoritative
  // source of URL/hash/render metadata and its exact id owns the cache entry.
  InkloopCloudStatus download(
      const InkloopIdentitySnapshot& identity,
      const storage::InkloopTaskRecord& task,
      storage::IAlbumStagingStore& store,
      storage::AlbumCommitResult& result) override;

 private:
  myai::EspEndpointSecurity& endpoint_security_;
};

}  // namespace cloud
}  // namespace inkloop
