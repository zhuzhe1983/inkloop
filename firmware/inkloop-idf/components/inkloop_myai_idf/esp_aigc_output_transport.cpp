#include "inkloop/myai/esp_aigc_output_transport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "inkloop/myai/AigcStreamDecoder.h"

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Inkloop MyAI AIGC requires the ESP-IDF trusted certificate bundle"
#endif

namespace inkloop {
namespace myai {
namespace {

constexpr size_t kMaximumRequestBodyBytes = 64U * 1024U;
constexpr size_t kMaximumHeaderCount = 32U;
constexpr size_t kMaximumHeaderNameBytes = 64U;
constexpr size_t kMaximumHeaderValueBytes = 2048U;
constexpr size_t kMaximumAggregateHeaderBytes = 16U * 1024U;
constexpr size_t kMaximumMetadataOverheadBytes = 8192U;
constexpr uint32_t kMaximumTimeoutMs = 120000U;
constexpr size_t kReadBufferBytes = 2048U;

Status failure(ErrorCode code, const char* detail, int http = 0) {
  return Status(code, http, detail);
}

bool lineSafe(const std::string& value) {
  for (unsigned char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == 0) return false;
  }
  return true;
}

Status validateHeaders(const std::map<std::string, std::string>& headers) {
  if (headers.size() > kMaximumHeaderCount) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid bounded AIGC HTTP headers");
  }
  size_t aggregate = 0;
  for (const auto& header : headers) {
    if (header.first.empty() || header.first.size() > kMaximumHeaderNameBytes ||
        header.second.size() > kMaximumHeaderValueBytes ||
        !lineSafe(header.first) || !lineSafe(header.second) ||
        aggregate > kMaximumAggregateHeaderBytes - header.first.size() ||
        aggregate + header.first.size() >
            kMaximumAggregateHeaderBytes - header.second.size()) {
      return failure(ErrorCode::InvalidArgument,
                     "invalid bounded AIGC HTTP headers");
    }
    aggregate += header.first.size() + header.second.size();
  }
  return Status::success();
}

bool extractJsonString(const std::string& json, const char* key,
                       std::string& output, size_t maximum) {
  const std::string marker = std::string("\"") + key + "\"";
  const size_t key_at = json.find(marker);
  if (key_at == std::string::npos) return false;
  size_t at = key_at + marker.size();
  while (at < json.size() &&
         std::isspace(static_cast<unsigned char>(json[at]))) ++at;
  if (at >= json.size() || json[at++] != ':') return false;
  while (at < json.size() &&
         std::isspace(static_cast<unsigned char>(json[at]))) ++at;
  if (at >= json.size() || json[at++] != '"') return false;
  output.clear();
  bool escaped = false;
  for (; at < json.size(); ++at) {
    const char ch = json[at];
    if (escaped) {
      if (ch != '"' && ch != '\\' && ch != '/') return false;
      output.push_back(ch);
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return !output.empty();
    } else if (static_cast<unsigned char>(ch) < 0x20U) {
      return false;
    } else {
      output.push_back(ch);
    }
    if (output.size() > maximum) return false;
  }
  return false;
}

class ClientGuard {
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

Status aborting(IImageSink& sink, const Status& status) {
  sink.abort();
  return status;
}

}  // namespace

EspAigcOutputTransport::EspAigcOutputTransport(
    EspEndpointSecurity& endpointSecurity)
    : endpoint_security_(endpointSecurity) {}

Status EspAigcOutputTransport::postAndDecodeBase64(
    const HttpRequest& request, size_t maxEncodedBytes,
    size_t maxDecodedBytes, IImageSink& sink, AigcOutputMetadata& metadata) {
  metadata.contentType.clear();
  metadata.decodedBytes = 0;
  if (request.method != "POST" || request.body.empty() ||
      request.body.size() > kMaximumRequestBodyBytes ||
      request.maxResponseBytes != 0 || request.timeoutMs == 0 ||
      request.timeoutMs > kMaximumTimeoutMs ||
      !request.tlsPeerVerificationRequired ||
      !request.rejectPrivateResolvedAddresses || request.redirectsAllowed ||
      maxEncodedBytes == 0 || maxDecodedBytes == 0 ||
      maxEncodedBytes > 8U * 1024U * 1024U ||
      maxDecodedBytes > 6U * 1024U * 1024U) {
    return aborting(sink, failure(ErrorCode::InvalidArgument,
                                  "invalid bounded AIGC output request"));
  }
  Status valid = validateHeaders(request.headers);
  if (!valid.ok()) return aborting(sink, valid);
  valid = endpoint_security_.validatePublicTlsEndpoint(request.url);
  if (!valid.ok()) return aborting(sink, valid);

  std::string prompt_id;
  std::string filename;
  if (!extractJsonString(request.body, "prompt_id", prompt_id, 128U) ||
      !extractJsonString(request.body, "filename", filename, 512U)) {
    return aborting(sink, failure(ErrorCode::Protocol,
                                  "invalid AIGC output request metadata"));
  }
  metadata.promptId = prompt_id;
  metadata.filename = filename;

  esp_http_client_config_t config{};
  config.url = request.url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = static_cast<int>(request.timeoutMs);
  config.disable_auto_redirect = true;
  config.max_redirection_count = 0;
  config.max_authorization_retries = -1;
  config.buffer_size = static_cast<int>(kReadBufferBytes);
  config.buffer_size_tx = 2048;
  config.skip_cert_common_name_check = false;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  ClientGuard client(esp_http_client_init(&config));
  if (!client.get()) {
    return aborting(sink, failure(ErrorCode::Transport,
                                  "AIGC HTTP client allocation failed"));
  }
  for (const auto& header : request.headers) {
    const esp_err_t set = esp_http_client_set_header(
        client.get(), header.first.c_str(), header.second.c_str());
    if (set != ESP_OK) {
      return aborting(sink, failure(ErrorCode::Transport,
                                    "AIGC HTTP header setup failed"));
    }
  }
  if (esp_http_client_open(client.get(), request.body.size()) != ESP_OK) {
    return aborting(sink, failure(ErrorCode::Transport,
                                  "AIGC HTTPS connection failed"));
  }
  valid = endpoint_security_.validateConnectedSocket(
      esp_http_client_get_socket(client.get()));
  if (!valid.ok()) return aborting(sink, valid);

  size_t written = 0;
  while (written < request.body.size()) {
    const size_t remaining = request.body.size() - written;
    const int chunk = esp_http_client_write(
        client.get(), request.body.data() + written,
        static_cast<int>(std::min<size_t>(
            remaining, static_cast<size_t>(std::numeric_limits<int>::max()))));
    if (chunk <= 0 || static_cast<size_t>(chunk) > remaining) {
      return aborting(sink, failure(ErrorCode::Transport,
                                    "AIGC HTTP request write failed"));
    }
    written += static_cast<size_t>(chunk);
  }

  const int64_t content_length = esp_http_client_fetch_headers(client.get());
  const int http_status = esp_http_client_get_status_code(client.get());
  if (content_length < -1 || http_status < 200 || http_status >= 300) {
    return aborting(sink, failure(ErrorCode::Protocol,
                                  "AIGC output HTTP rejected", http_status));
  }
  if (maxEncodedBytes >
      std::numeric_limits<size_t>::max() - kMaximumMetadataOverheadBytes) {
    return aborting(sink, failure(ErrorCode::TooLarge,
                                  "AIGC output cap overflow"));
  }
  const size_t maximum_envelope =
      maxEncodedBytes + kMaximumMetadataOverheadBytes;
  if (content_length >= 0 &&
      static_cast<uint64_t>(content_length) > maximum_envelope) {
    return aborting(sink, failure(ErrorCode::TooLarge,
                                  "AIGC output envelope exceeds cap"));
  }

  AigcStreamDecoder decoder(maxEncodedBytes, maxDecodedBytes, sink, metadata);
  std::array<uint8_t, kReadBufferBytes> buffer{};
  while (true) {
    const int count = esp_http_client_read(
        client.get(), reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (count < 0) {
      return aborting(sink, failure(ErrorCode::Transport,
                                    "AIGC output read failed"));
    }
    if (count == 0) break;
    const Status consumed = decoder.append(buffer.data(),
                                           static_cast<size_t>(count));
    if (!consumed.ok()) return aborting(sink, consumed);
  }
  if (!esp_http_client_is_complete_data_received(client.get())) {
    return aborting(sink, failure(ErrorCode::Transport,
                                  "AIGC output response was truncated"));
  }
  valid = decoder.finish();
  if (!valid.ok()) return aborting(sink, valid);
  valid = sink.commit(metadata);
  if (!valid.ok()) return aborting(sink, valid);
  return Status::success();
}

}  // namespace myai
}  // namespace inkloop
