#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "inkloop/inkloop_cloud_client.hpp"
#include "inkloop/storage/papercolor_png.hpp"
#include "inkloop/storage/sha256.hpp"

namespace inkloop {
namespace cloud {

inline constexpr size_t kMaximumInkloopFrameBytes = 1500000U;

struct InkloopFrameStagingRequest {
  std::string task_id;
  std::string render_strategy;
  size_t content_length = 0;
};

struct InkloopFrameMetadata {
  size_t bytes = 0;
  bool landscape = false;
  std::string sha256;
};

// The storage owner supplies this sink. append() receives bounded network
// chunks; commit() is called only after exact-length PNG and SHA validation.
class IInkloopFrameStagingSink {
 public:
  virtual ~IInkloopFrameStagingSink() = default;
  virtual InkloopCloudStatus begin(
      const InkloopFrameStagingRequest& request) = 0;
  virtual InkloopCloudStatus append(
      const uint8_t* bytes, size_t length) = 0;
  virtual InkloopCloudStatus commit(
      const InkloopFrameMetadata& metadata) = 0;
  virtual void abort() = 0;
};

// Incremental fail-closed verifier. It owns no image-sized buffer.
class InkloopFrameStream final {
 public:
  explicit InkloopFrameStream(IInkloopFrameStagingSink& sink);
  ~InkloopFrameStream();

  InkloopFrameStream(const InkloopFrameStream&) = delete;
  InkloopFrameStream& operator=(const InkloopFrameStream&) = delete;

  InkloopCloudStatus begin(const InkloopFrameStagingRequest& request,
                           const std::string& expected_sha256);
  InkloopCloudStatus append(const uint8_t* bytes, size_t length);
  InkloopCloudStatus finish(InkloopFrameMetadata& metadata);
  void abort();

  bool active() const { return active_; }
  size_t receivedBytes() const { return received_bytes_; }

 private:
  InkloopCloudStatus fail(InkloopCloudCode code, const char* detail);

  IInkloopFrameStagingSink& sink_;
  storage::PaperColorPngValidator png_;
  storage::Sha256 sha_;
  std::string expected_sha256_;
  size_t expected_bytes_ = 0;
  size_t received_bytes_ = 0;
  bool active_ = false;
};

}  // namespace cloud
}  // namespace inkloop
