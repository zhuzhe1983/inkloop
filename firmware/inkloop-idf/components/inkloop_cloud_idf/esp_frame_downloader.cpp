#include "inkloop/cloud/esp_frame_downloader.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "inkloop/myai/esp_http_adapters.hpp"

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Inkloop frame HTTPS requires the ESP-IDF trusted certificate bundle"
#endif

namespace inkloop {
namespace cloud {
namespace {

constexpr size_t kReadBufferBytes = 2048U;
constexpr uint32_t kMinimumTimeoutMs = 1000U;
constexpr uint32_t kMaximumTimeoutMs = 60000U;

InkloopCloudStatus failure(InkloopCloudCode code, const char* detail,
                           int http = 0) {
  InkloopCloudStatus output;
  output.code = code;
  output.http_status = http;
  output.detail = detail ? detail : "";
  return output;
}

InkloopCloudCode cloudCode(myai::ErrorCode code) {
  switch (code) {
    case myai::ErrorCode::Security:
      return InkloopCloudCode::Security;
    case myai::ErrorCode::Storage:
    case myai::ErrorCode::RecoveryRequired:
      return InkloopCloudCode::Storage;
    case myai::ErrorCode::TooLarge:
      return InkloopCloudCode::TooLarge;
    case myai::ErrorCode::InvalidArgument:
    case myai::ErrorCode::InvalidState:
      return InkloopCloudCode::InvalidArgument;
    case myai::ErrorCode::Unauthorized:
      return InkloopCloudCode::Unauthorized;
    case myai::ErrorCode::Conflict:
      return InkloopCloudCode::Conflict;
    case myai::ErrorCode::Protocol:
      return InkloopCloudCode::Protocol;
    default:
      return InkloopCloudCode::Transport;
  }
}

InkloopCloudStatus converted(const myai::Status& status,
                             const char* detail) {
  InkloopCloudStatus output;
  output.code = cloudCode(status.code);
  output.http_status = status.httpStatus;
  output.retry_after_ms = status.retryAfterMs;
  output.detail = detail ? detail : "";
  return output;
}

bool lowerHex(const std::string& value, size_t exact) {
  if (value.size() != exact) return false;
  for (const char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  }
  return true;
}

bool validDeviceId(const std::string& value) {
  if (value.size() < 20U || value.size() > 80U) return false;
  for (const char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-')) return false;
  }
  return true;
}

class ClientGuard final {
 public:
  explicit ClientGuard(esp_http_client_handle_t client) : client_(client) {}
  ~ClientGuard() {
    if (client_) {
      esp_http_client_close(client_);
      esp_http_client_cleanup(client_);
    }
  }
  esp_http_client_handle_t get() const { return client_; }

 private:
  esp_http_client_handle_t client_;
};

InkloopCloudStatus aborting(IInkloopFrameStagingSink& sink,
                            const InkloopCloudStatus& status) {
  sink.abort();
  return status;
}

}  // namespace

EspInkloopFrameDownloader::EspInkloopFrameDownloader(
    myai::EspEndpointSecurity& endpoint_security)
    : endpoint_security_(endpoint_security) {}

InkloopCloudStatus EspInkloopFrameDownloader::download(
    const InkloopFrameDownloadRequest& request,
    const InkloopIdentitySnapshot& identity,
    IInkloopFrameStagingSink& sink,
    InkloopFrameMetadata& metadata) {
  sink.abort();
  metadata = InkloopFrameMetadata();
  if (request.task_id.empty() || request.task_id.size() > 128U ||
      request.url.size() < 12U || request.url.size() > 1024U ||
      !lowerHex(request.expected_sha256, 64U) ||
      request.timeout_ms < kMinimumTimeoutMs ||
      request.timeout_ms > kMaximumTimeoutMs ||
      !validDeviceId(identity.device_id) ||
      !lowerHex(identity.secret, 64U)) {
    return failure(InkloopCloudCode::InvalidArgument,
                   "invalid_frame_download_request");
  }

  myai::Status secured =
      endpoint_security_.validatePublicTlsEndpoint(request.url);
  if (!secured.ok())
    return aborting(sink, converted(secured, "frame_endpoint_rejected"));

  esp_http_client_config_t config{};
  config.url = request.url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = static_cast<int>(request.timeout_ms);
  config.disable_auto_redirect = true;
  config.max_redirection_count = 0;
  config.max_authorization_retries = -1;
  config.buffer_size = static_cast<int>(kReadBufferBytes);
  config.buffer_size_tx = 2048;
  config.skip_cert_common_name_check = false;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  ClientGuard client(esp_http_client_init(&config));
  if (!client.get())
    return aborting(sink, failure(InkloopCloudCode::Transport,
                                  "frame_http_client_allocation_failed"));
  std::string authorization =
      "InkloopDevice " + identity.device_id + ":" + identity.secret;
  const esp_err_t header = esp_http_client_set_header(
      client.get(), "Authorization", authorization.c_str());
  std::fill(authorization.begin(), authorization.end(), '\0');
  authorization.clear();
  if (header != ESP_OK)
    return aborting(sink, failure(InkloopCloudCode::Transport,
                                  "frame_authorization_setup_failed"));

  if (esp_http_client_open(client.get(), 0) != ESP_OK)
    return aborting(sink, failure(InkloopCloudCode::Transport,
                                  "frame_https_connection_failed"));
  secured = endpoint_security_.validateConnectedSocket(
      esp_http_client_get_socket(client.get()));
  if (!secured.ok())
    return aborting(sink, converted(secured, "frame_peer_rejected"));

  const int64_t content_length = esp_http_client_fetch_headers(client.get());
  const int http_status = esp_http_client_get_status_code(client.get());
  if (http_status != 200)
    return aborting(sink, failure(
        http_status == 401 || http_status == 403
            ? InkloopCloudCode::Unauthorized
            : (http_status == 413 ? InkloopCloudCode::TooLarge
                                  : InkloopCloudCode::Protocol),
        "frame_http_rejected", http_status));
  if (esp_http_client_is_chunked_response(client.get()) ||
      content_length < 45 ||
      static_cast<uint64_t>(content_length) > kMaximumInkloopFrameBytes ||
      static_cast<uint64_t>(content_length) >
          std::numeric_limits<size_t>::max()) {
    return aborting(sink, failure(
        content_length > static_cast<int64_t>(kMaximumInkloopFrameBytes)
            ? InkloopCloudCode::TooLarge
            : InkloopCloudCode::Protocol,
        "frame_content_length_invalid", http_status));
  }

  InkloopFrameStagingRequest staging;
  staging.task_id = request.task_id;
  staging.render_strategy = request.render_strategy;
  staging.content_length = static_cast<size_t>(content_length);
  InkloopFrameStream stream(sink);
  InkloopCloudStatus status = stream.begin(staging, request.expected_sha256);
  if (!status.ok()) return status;

  std::array<uint8_t, kReadBufferBytes> buffer{};
  size_t remaining = staging.content_length;
  while (remaining > 0U) {
    const size_t requested = std::min(remaining, buffer.size());
    const int count = esp_http_client_read(
        client.get(), reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(requested));
    if (count <= 0 || static_cast<size_t>(count) > requested) {
      stream.abort();
      return failure(InkloopCloudCode::Transport,
                     "frame_response_truncated", http_status);
    }
    status = stream.append(buffer.data(), static_cast<size_t>(count));
    if (!status.ok()) return status;
    remaining -= static_cast<size_t>(count);
  }
  if (!esp_http_client_is_complete_data_received(client.get())) {
    stream.abort();
    return failure(InkloopCloudCode::Transport,
                   "frame_response_incomplete", http_status);
  }
  status = stream.finish(metadata);
  if (!status.ok()) return status;
  return InkloopCloudStatus::success();
}

InkloopCloudStatus EspInkloopFrameDownloader::download(
    const InkloopIdentitySnapshot& identity,
    const storage::InkloopTaskRecord& task,
    storage::IAlbumStagingStore& store,
    storage::AlbumCommitResult& result) {
  result = storage::AlbumCommitResult();
  InkloopFrameDownloadRequest request;
  request.task_id = task.id;
  request.url = task.frame_url;
  request.expected_sha256 = task.frame_hash;
  request.render_strategy = task.render_strategy;
  InkloopFrameAlbumSink sink(store);
  InkloopFrameMetadata metadata;
  InkloopCloudStatus status = download(request, identity, sink, metadata);
  if (!status.ok()) return status;
  if (!sink.takeCommittedAsset(result))
    return failure(InkloopCloudCode::Storage,
                   "frame_album_result_unavailable");
  return InkloopCloudStatus::success();
}

}  // namespace cloud
}  // namespace inkloop
