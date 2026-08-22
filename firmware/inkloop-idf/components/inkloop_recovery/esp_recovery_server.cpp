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

namespace inkloop {
namespace recovery {
namespace {

constexpr size_t kHeaderScratchBytes = 257U;
constexpr size_t kMaximumUriBytes = 128U;

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
    return sendResponse(native, core.handle(request));
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
