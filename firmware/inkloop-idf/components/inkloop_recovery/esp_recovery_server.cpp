#include "inkloop/recovery/esp_recovery_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace inkloop {
namespace recovery {
namespace {

constexpr size_t kHeaderScratchBytes = 257U;
constexpr size_t kMaximumUriBytes = 128U;
constexpr size_t kRecoveryHttpTaskStackBytes = 16U * 1024U;
constexpr TickType_t kRecoveryExportChunkDelay =
    pdMS_TO_TICKS(4U) > 0U ? pdMS_TO_TICKS(4U) : 1U;

const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Content";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

RecoveryResponse nativeError(int status, const char* error) {
  RecoveryResponse output;
  output.status = status;
  output.body = std::string("{\"ok\":false,\"error\":\"") + error + "\"}";
  return output;
}

template <size_t Size>
std::string lowerHex(const std::array<uint8_t, Size>& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output(Size * 2U, '0');
  for (size_t at = 0U; at < Size; ++at) {
    output[at * 2U] = kHex[value[at] >> 4U];
    output[at * 2U + 1U] = kHex[value[at] & 0x0fU];
  }
  return output;
}

bool readHeader(httpd_req_t* request, const char* name, size_t maximum,
                std::string& output) {
  output.clear();
  const size_t length = httpd_req_get_hdr_value_len(request, name);
  if (length == 0U) return true;
  if (length > maximum || length + 1U > kHeaderScratchBytes) return false;
  std::array<char, kHeaderScratchBytes> scratch{};
  if (httpd_req_get_hdr_value_str(request, name, scratch.data(),
                                  length + 1U) != ESP_OK) {
    return false;
  }
  output.assign(scratch.data(), length);
  return true;
}

bool privateIpv4(uint32_t network_order) {
  const uint32_t host = ntohl(network_order);
  const uint8_t first = static_cast<uint8_t>(host >> 24U);
  const uint8_t second = static_cast<uint8_t>((host >> 16U) & 0xffU);
  return first == 10U || first == 127U ||
         (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) ||
         (first == 169U && second == 254U);
}

bool localPeer(httpd_req_t* request) {
  const int socket = httpd_req_to_sockfd(request);
  if (socket < 0) return false;
  sockaddr_storage peer{};
  socklen_t length = sizeof(peer);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) != 0)
    return false;
  if (peer.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&peer);
    return privateIpv4(ipv4->sin_addr.s_addr);
  }
  if (peer.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&peer);
    const uint8_t* bytes = ipv6->sin6_addr.s6_addr;
    if (IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) ||
        (bytes[0] == 0xfeU && (bytes[1] & 0xc0U) == 0x80U) ||
        (bytes[0] & 0xfeU) == 0xfcU) {
      return true;
    }
    if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
      uint32_t mapped = 0U;
      std::memcpy(&mapped, bytes + 12U, sizeof(mapped));
      return privateIpv4(mapped);
    }
  }
  return false;
}

esp_err_t sendResponse(httpd_req_t* request,
                       const RecoveryResponse& response) {
  esp_err_t result = httpd_resp_set_status(request, statusText(response.status));
  if (result == ESP_OK)
    result = httpd_resp_set_type(request, response.content_type.c_str());
  if (result == ESP_OK)
    result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  if (result == ESP_OK)
    result = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  if (result == ESP_OK)
    result = httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  if (result == ESP_OK)
    result = httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'self'; style-src 'unsafe-inline'; script-src "
        "'unsafe-inline'; connect-src 'self'; img-src 'none'; object-src "
        "'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
  }
  if (result == ESP_OK && !response.set_cookie.empty()) {
    result = httpd_resp_set_hdr(request, "Set-Cookie",
                                response.set_cookie.c_str());
  }
  if (result != ESP_OK) return result;
  return httpd_resp_send(request, response.body.data(), response.body.size());
}

}  // namespace

struct EspRecoveryServer::Impl {
  RecoveryPortalCore& core;
  EspRecoveryServerConfig config;
  httpd_handle_t server = nullptr;
  bool stopping = false;

  Impl(RecoveryPortalCore& core_value,
       const EspRecoveryServerConfig& config_value)
      : core(core_value), config(config_value) {}

  static esp_err_t requestHandler(httpd_req_t* request) {
    if (!request || !request->user_ctx) return ESP_ERR_INVALID_ARG;
    return static_cast<Impl*>(request->user_ctx)->handleRequest(request);
  }

  RecoveryResponse readRequest(httpd_req_t* native,
                               RecoveryRequest& request) const {
    if (std::strlen(native->uri) > kMaximumUriBytes)
      return nativeError(400, "uri_invalid");
    request.method = native->method == HTTP_GET
                         ? "GET"
                         : native->method == HTTP_POST ? "POST" : "UNSUPPORTED";
    request.path = native->uri;
    request.peer_is_local = localPeer(native);
    request.content_length = native->content_len;
    request.now_seconds =
        static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL;
    if (!readHeader(native, "Host", 128U, request.host) ||
        !readHeader(native, "Origin", 160U, request.origin) ||
        !readHeader(native, "Cookie", 256U, request.cookie) ||
        !readHeader(native, "X-Inkloop-CSRF", kMaximumRecoveryTokenBytes,
                    request.csrf_token) ||
        !readHeader(native, "Content-Type", 64U, request.content_type)) {
      return nativeError(400, "request_header_too_large");
    }
    if (native->content_len > kMaximumRecoveryRequestBodyBytes)
      return nativeError(413, "request_body_too_large");
    if (native->content_len != 0U) {
      request.body.resize(native->content_len);
      size_t received = 0U;
      unsigned int timeouts = 0U;
      while (received < native->content_len) {
        const int count = httpd_req_recv(
            native, &request.body[received], native->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2U) continue;
        if (count <= 0) return nativeError(400, "request_body_incomplete");
        received += static_cast<size_t>(count);
      }
    }
    RecoveryResponse ok;
    ok.status = 0;
    return ok;
  }

  esp_err_t handleRequest(httpd_req_t* native) {
    if (stopping)
      return sendResponse(native, nativeError(503, "recovery_portal_stopping"));
    RecoveryRequest request;
    const RecoveryResponse read = readRequest(native, request);
    if (read.status != 0) return sendResponse(native, read);
    if (request.method == "GET" && request.path.rfind(
            "/api/recovery/export/file/", 0U) == 0U) {
      RecoveryExportStream stream;
      const RecoveryResponse opened = core.openRecoveryExportFile(
          request, stream);
      if (opened.status != 200) return sendResponse(native, opened);
      return sendExport(native, stream);
    }
    return sendResponse(native, core.handle(request));
  }

  esp_err_t sendExport(httpd_req_t* request, RecoveryExportStream stream) {
    const std::string digest = lowerHex(stream.digest);
    const std::string bytes = std::to_string(stream.byte_count);
    const std::string filename = stream.item == 0U
        ? "attachment; filename=\"index.json\""
        : stream.item == 1U
            ? "attachment; filename=\"index.next\""
            : stream.item == 2U
                ? "attachment; filename=\"index.prev\""
                : "attachment; filename=\"" + digest + ".png\"";
    esp_err_t result = httpd_resp_set_status(request, "200 OK");
    if (result == ESP_OK)
      result = httpd_resp_set_type(request, "application/octet-stream");
    if (result == ESP_OK)
      result = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (result == ESP_OK)
      result = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    if (result == ESP_OK)
      result = httpd_resp_set_hdr(request, "Content-Disposition",
                                  filename.c_str());
    if (result == ESP_OK)
      result = httpd_resp_set_hdr(request, "X-Inkloop-SHA256",
                                  digest.c_str());
    if (result == ESP_OK)
      result = httpd_resp_set_hdr(request, "X-Inkloop-Bytes",
                                  bytes.c_str());
    if (result != ESP_OK) {
      core.closeRecoveryExportFile(stream.handle);
      return result;
    }

    std::array<uint8_t, kMaximumRecoveryExportChunkBytes> chunk{};
    uint64_t sent = 0U;
    for (;;) {
      size_t count = 0U;
      const RecoveryExportResult read = core.readRecoveryExportFile(
          stream.handle, chunk.data(), chunk.size(), count);
      if (read == RecoveryExportResult::Complete && count == 0U &&
          sent == stream.byte_count) {
        result = httpd_resp_send_chunk(request, nullptr, 0U);
        break;
      }
      if (read != RecoveryExportResult::Ok || count == 0U ||
          count > chunk.size() || sent > stream.byte_count ||
          count > stream.byte_count - sent) {
        result = ESP_FAIL;
        break;
      }
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char*>(chunk.data()), count);
      if (result != ESP_OK) break;
      sent += count;
      // One 4 KiB chunk per 4 ms caps a single authenticated export at
      // roughly 1 MiB/s and yields to Wi-Fi/system tasks between reads.
      vTaskDelay(kRecoveryExportChunkDelay);
    }
    core.closeRecoveryExportFile(stream.handle);
    std::fill(chunk.begin(), chunk.end(), 0U);
    return result;
  }

  esp_err_t start() {
    if (server) return ESP_ERR_INVALID_STATE;
    if (!core.ready() || config.port == 0U ||
        config.maximum_open_sockets < 1U ||
        config.maximum_open_sockets > 7U) {
      return ESP_ERR_INVALID_ARG;
    }
    stopping = false;
    httpd_config_t native = HTTPD_DEFAULT_CONFIG();
    native.server_port = config.port;
    // Recovery action inspection verifies up to four transactional snapshots
    // (including bounded SHA-256 state) in one authenticated request. The IDF
    // 4 KiB default overflows before it can return the read-only inventory.
    // Match the normal Portal task while keeping the owner caller-driven.
    native.stack_size = kRecoveryHttpTaskStackBytes;
    native.max_open_sockets = config.maximum_open_sockets;
    native.uri_match_fn = httpd_uri_match_wildcard;
    native.max_uri_handlers =
        std::max(native.max_uri_handlers, static_cast<uint16_t>(2U));
    native.lru_purge_enable = true;
    native.recv_wait_timeout = 5;
    native.send_wait_timeout = 5;
    esp_err_t result = httpd_start(&server, &native);
    if (result != ESP_OK) return result;
    httpd_uri_t route{};
    route.uri = "/*";
    route.method = HTTP_GET;
    route.handler = &Impl::requestHandler;
    route.user_ctx = this;
    result = httpd_register_uri_handler(server, &route);
    route.method = HTTP_POST;
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &route);
    if (result != ESP_OK) {
      httpd_stop(server);
      server = nullptr;
    }
    return result;
  }

  esp_err_t stop() {
    if (!server) return ESP_ERR_INVALID_STATE;
    stopping = true;
    const esp_err_t result = httpd_stop(server);
    if (result == ESP_OK) {
      server = nullptr;
    } else {
      // Preserve an accurate running state so an owner can retry stop without
      // destroying the core referenced by the still-live native server.
      stopping = false;
    }
    return result;
  }
};

EspRecoveryServer::EspRecoveryServer(
    RecoveryPortalCore& core, const EspRecoveryServerConfig& config)
    : impl_(new (std::nothrow) Impl(core, config)) {}

EspRecoveryServer::~EspRecoveryServer() {
  if (!impl_) return;
  if (impl_->server) impl_->stop();
  delete impl_;
  impl_ = nullptr;
}

esp_err_t EspRecoveryServer::start() {
  return impl_ ? impl_->start() : ESP_ERR_NO_MEM;
}

esp_err_t EspRecoveryServer::stop() {
  return impl_ ? impl_->stop() : ESP_ERR_INVALID_STATE;
}

bool EspRecoveryServer::running() const {
  return impl_ && impl_->server != nullptr && !impl_->stopping;
}

}  // namespace recovery
}  // namespace inkloop
