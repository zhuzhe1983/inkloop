#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "inkloop/portal/portal_core.hpp"

namespace inkloop {
namespace portal {

// All try* methods run in the HTTP server task. They must only copy into a
// bounded owner queue and return immediately; storage mutation belongs to the
// consumer task. finish() is the typed commit marker for the upload.
class IPortalUploadQueue {
 public:
  virtual ~IPortalUploadQueue() = default;
  virtual PortalResult tryBegin(const PortalStreamRequest& request) = 0;
  virtual PortalResult tryWrite(uint64_t request_id, const uint8_t* bytes,
                                size_t length) = 0;
  virtual PortalResult tryFinish(uint64_t request_id) = 0;
  virtual void tryAbort(uint64_t request_id) = 0;
};

struct PortalPreviewInfo {
  uint64_t handle = 0;
  size_t bytes = 0;
  std::string content_type;
};

// Preview reads execute only on the low-priority preview worker, never inside
// the esp_http_server handler. The owner may map handle to its storage object.
class IPortalPreviewSource {
 public:
  virtual ~IPortalPreviewSource() = default;
  virtual PortalResult open(const std::string& asset_id,
                            PortalPreviewInfo& output) = 0;
  virtual PortalResult read(uint64_t handle, uint8_t* output, size_t capacity,
                            size_t& bytes_read) = 0;
  virtual void close(uint64_t handle) = 0;
};

struct EspPortalServerConfig {
  uint16_t port = 80U;
  // PortalCore renders a bounded but rich state document. ESP-IDF's 4 KiB
  // HTTPD default is too small once the request, response stream and C++
  // formatter frames coexist, so keep an explicit measured margin here.
  uint32_t http_task_stack_bytes = 8192U;
  size_t preview_queue_length = 2U;
  uint32_t preview_task_stack_bytes = 6144U;
  uint8_t preview_task_priority = 1U;
};

class EspPortalServer {
 public:
  EspPortalServer(PortalCore& core, IPortalUploadQueue& uploads,
                  IPortalPreviewSource& previews,
                  const EspPortalServerConfig& config = {});
  ~EspPortalServer();

  EspPortalServer(const EspPortalServer&) = delete;
  EspPortalServer& operator=(const EspPortalServer&) = delete;

  esp_err_t start();
  esp_err_t stop();
  bool running() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace portal
}  // namespace inkloop
