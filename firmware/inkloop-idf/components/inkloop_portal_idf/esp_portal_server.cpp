#include "inkloop/portal/esp_portal_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "inkloop/portal/bounded_queue_flow.hpp"

namespace inkloop {
namespace portal {
namespace {

constexpr size_t kReceiveChunkBytes = 2048U;
constexpr size_t kHeaderScratchBytes = 513U;
constexpr int64_t kQueueBackpressureDeadlineUs = 30000000LL;

const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Content";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

PortalResponse simpleError(int status, const char* error) {
  PortalResponse response;
  response.status = status;
  response.body = std::string("{\"ok\":false,\"error\":\"") + error + "\"}";
  return response;
}

PortalResponse resultError(PortalResult result, const char* fallback) {
  switch (result) {
    case PortalResult::Busy: {
      PortalResponse response = simpleError(409, "portal_queue_busy");
      response.retry_after_seconds = 1U;
      return response;
    }
    case PortalResult::TooLarge: return simpleError(413, "stream_too_large");
    case PortalResult::InvalidRequest:
    case PortalResult::InvalidData: return simpleError(422, fallback);
    case PortalResult::Unauthorized: return simpleError(401, "unauthorized");
    case PortalResult::Forbidden: return simpleError(403, "forbidden");
    case PortalResult::Unavailable:
    case PortalResult::InvalidConfiguration:
      return simpleError(503, "portal_stream_unavailable");
    case PortalResult::Ok: break;
  }
  return simpleError(500, fallback);
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
  const uint8_t second = static_cast<uint8_t>((host >> 16U) & 0xFFU);
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
        (bytes[0] == 0xFEU && (bytes[1] & 0xC0U) == 0x80U) ||
        (bytes[0] & 0xFEU) == 0xFCU) {
      return true;
    }
    if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
      uint32_t mapped = 0;
      std::memcpy(&mapped, bytes + 12U, sizeof(mapped));
      return privateIpv4(mapped);
    }
  }
  return false;
}

bool uploadRoute(const char* uri) {
  if (!uri) return false;
  static constexpr char kRoute[] = "/api/album/upload";
  const size_t length = sizeof(kRoute) - 1U;
  return std::strncmp(uri, kRoute, length) == 0 &&
         (uri[length] == '\0' || uri[length] == '?');
}

esp_err_t setCommonHeaders(httpd_req_t* request,
                           const PortalResponse& response) {
  esp_err_t status = httpd_resp_set_status(request, statusText(response.status));
  if (status == ESP_OK)
    status = httpd_resp_set_type(request, response.content_type.c_str());
  if (status == ESP_OK)
    status = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  if (status == ESP_OK)
    status = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  if (status == ESP_OK)
    status = httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  if (status == ESP_OK)
    status = httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  if (status == ESP_OK) {
    status = httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'self'; img-src 'self' blob:; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'; object-src 'none'; "
        "base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
  }
  if (status == ESP_OK && !response.set_cookie.empty())
    status = httpd_resp_set_hdr(request, "Set-Cookie", response.set_cookie.c_str());
  if (status == ESP_OK && response.retry_after_seconds != 0U) {
    const std::string retry = std::to_string(response.retry_after_seconds);
    status = httpd_resp_set_hdr(request, "Retry-After", retry.c_str());
  }
  return status;
}

esp_err_t sendImmediate(httpd_req_t* request,
                        const PortalResponse& response) {
  const esp_err_t headers = setCommonHeaders(request, response);
  if (headers != ESP_OK) return headers;
  return httpd_resp_send(request, response.body.data(), response.body.size());
}

}  // namespace

struct EspPortalServer::Impl {
  struct PreviewJob {
    httpd_req_t* request = nullptr;
    std::array<char, kMaximumAlbumIdBytes + 1U> asset_id{};
  };

  PortalCore& core;
  IPortalUploadQueue& uploads;
  IPortalPreviewSource& previews;
  EspPortalServerConfig config;
  httpd_handle_t server = nullptr;
  QueueHandle_t preview_queue = nullptr;
  SemaphoreHandle_t preview_stopped = nullptr;
  TaskHandle_t preview_task = nullptr;
  volatile bool stopping = false;

  Impl(PortalCore& core_value, IPortalUploadQueue& upload_value,
       IPortalPreviewSource& preview_value,
       const EspPortalServerConfig& config_value)
      : core(core_value),
        uploads(upload_value),
        previews(preview_value),
        config(config_value) {}

  static void previewTaskEntry(void* context) {
    static_cast<Impl*>(context)->previewLoop();
  }

  static esp_err_t requestHandler(httpd_req_t* request) {
    if (!request || !request->user_ctx) return ESP_ERR_INVALID_ARG;
    return static_cast<Impl*>(request->user_ctx)->handleRequest(request);
  }

  PortalResponse readRequest(httpd_req_t* native, PortalRequest& request) {
    if (std::strlen(native->uri) > 512U)
      return simpleError(400, "invalid_uri");
    request.method = native->method == HTTP_GET ? "GET" :
                     native->method == HTTP_POST ? "POST" : "UNSUPPORTED";
    request.path = native->uri;
    request.peer_is_local = localPeer(native);
    request.content_length = native->content_len;
    request.now_seconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL;
    if (!readHeader(native, "Host", 128U, request.host) ||
        !readHeader(native, "Origin", 160U, request.origin) ||
        !readHeader(native, "Cookie", 512U, request.cookie) ||
        !readHeader(native, "X-Inkloop-CSRF", 128U, request.csrf_token) ||
        !readHeader(native, "Content-Type", 128U, request.content_type)) {
      return simpleError(400, "request_header_too_large");
    }
    if (native->method == HTTP_POST && !uploadRoute(native->uri)) {
      if (native->content_len > kMaximumPortalRequestBodyBytes)
        return simpleError(413, "request_body_too_large");
      request.body.resize(native->content_len);
      size_t received = 0;
      unsigned int timeouts = 0;
      while (received < native->content_len) {
        const int count = httpd_req_recv(
            native, &request.body[received], native->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2U) continue;
        if (count <= 0) return simpleError(400, "request_body_incomplete");
        received += static_cast<size_t>(count);
      }
    }
    PortalResponse ok;
    ok.status = 0;
    return ok;
  }

  esp_err_t handleRequest(httpd_req_t* request) {
    if (stopping) return sendImmediate(request, simpleError(503, "portal_stopping"));
    PortalRequest portable;
    const PortalResponse read = readRequest(request, portable);
    if (read.status != 0) return sendImmediate(request, read);
    const PortalResponse output = core.handle(portable);
    if (output.disposition == ResponseDisposition::Immediate)
      return sendImmediate(request, output);
    if (output.disposition == ResponseDisposition::StreamAlbumUpload)
      return streamUpload(request, output);
    return queuePreview(request, output);
  }

  esp_err_t streamUpload(httpd_req_t* request,
                         const PortalResponse& authorized) {
    const uint64_t request_id = authorized.stream.request_id;
    const int64_t deadline =
        esp_timer_get_time() + kQueueBackpressureDeadlineUs;
    const auto clock = [] { return esp_timer_get_time(); };
    const auto yield = [] { vTaskDelay(1); };
    PortalResult result = retryBusyUntil(
        deadline, [&] { return uploads.tryBegin(authorized.stream); },
        clock, yield);
    if (result != PortalResult::Ok)
      return sendImmediate(request, resultError(result, "upload_begin_failed"));
    std::array<uint8_t, kReceiveChunkBytes> buffer{};
    size_t received = 0;
    unsigned int timeouts = 0;
    while (received < authorized.stream.content_length) {
      const size_t wanted = std::min(buffer.size(),
                                     authorized.stream.content_length - received);
      const int count = httpd_req_recv(request, reinterpret_cast<char*>(buffer.data()),
                                       wanted);
      if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2U) continue;
      if (count <= 0) {
        uploads.tryAbort(request_id);
        return sendImmediate(request, simpleError(400, "upload_incomplete"));
      }
      timeouts = 0;
      result = retryBusyUntil(
          deadline,
          [&] {
            return uploads.tryWrite(request_id, buffer.data(),
                                    static_cast<size_t>(count));
          },
          clock, yield);
      if (result != PortalResult::Ok) {
        uploads.tryAbort(request_id);
        return sendImmediate(request, resultError(result, "upload_queue_failed"));
      }
      received += static_cast<size_t>(count);
    }
    result = retryBusyUntil(
        deadline, [&] { return uploads.tryFinish(request_id); }, clock,
        yield);
    if (result != PortalResult::Ok) {
      uploads.tryAbort(request_id);
      return sendImmediate(request, resultError(result, "upload_finish_failed"));
    }
    PortalResponse accepted;
    accepted.status = 202;
    accepted.body = std::string("{\"ok\":true,\"state\":\"queued\",\"requestId\":") +
                    std::to_string(request_id) +
                    ",\"command\":\"COMMIT_ALBUM_UPLOAD\"}";
    return sendImmediate(request, accepted);
  }

  esp_err_t queuePreview(httpd_req_t* request,
                         const PortalResponse& authorized) {
    httpd_req_t* asynchronous = nullptr;
    esp_err_t status = httpd_req_async_handler_begin(request, &asynchronous);
    if (status != ESP_OK) return status;
    PreviewJob job;
    job.request = asynchronous;
    std::copy(authorized.stream.asset_id.begin(), authorized.stream.asset_id.end(),
              job.asset_id.begin());
    if (!preview_queue || xQueueSend(preview_queue, &job, 0) != pdTRUE) {
      const esp_err_t sent = sendImmediate(
          asynchronous, resultError(PortalResult::Busy, "preview_queue_busy"));
      httpd_req_async_handler_complete(asynchronous);
      return sent == ESP_OK ? ESP_OK : sent;
    }
    return ESP_OK;
  }

  void previewLoop() {
    PreviewJob job;
    while (xQueueReceive(preview_queue, &job, portMAX_DELAY) == pdTRUE) {
      if (!job.request) break;
      streamPreview(job);
      httpd_req_async_handler_complete(job.request);
    }
    if (preview_stopped) xSemaphoreGive(preview_stopped);
    preview_task = nullptr;
    vTaskDelete(nullptr);
  }

  void streamPreview(const PreviewJob& job) {
    PortalPreviewInfo info;
    PortalResult result = previews.open(job.asset_id.data(), info);
    if (result != PortalResult::Ok) {
      sendImmediate(job.request, resultError(result, "preview_open_failed"));
      return;
    }
    if (info.handle == 0U || info.bytes == 0U ||
        info.bytes > kMaximumAlbumUploadBytes || info.content_type != "image/png") {
      previews.close(info.handle);
      sendImmediate(job.request, simpleError(422, "preview_metadata_invalid"));
      return;
    }
    PortalResponse headers;
    headers.status = 200;
    headers.content_type = info.content_type;
    if (setCommonHeaders(job.request, headers) != ESP_OK) {
      previews.close(info.handle);
      return;
    }
    std::array<uint8_t, kReceiveChunkBytes> buffer{};
    size_t sent = 0;
    bool success = true;
    while (sent < info.bytes) {
      size_t read = 0;
      const size_t capacity = std::min(buffer.size(), info.bytes - sent);
      result = previews.read(info.handle, buffer.data(), capacity, read);
      if (result != PortalResult::Ok || read == 0U || read > capacity ||
          httpd_resp_send_chunk(job.request,
                                reinterpret_cast<const char*>(buffer.data()),
                                read) != ESP_OK) {
        success = false;
        break;
      }
      sent += read;
    }
    previews.close(info.handle);
    if (success && sent == info.bytes)
      httpd_resp_send_chunk(job.request, nullptr, 0U);
  }

  esp_err_t start() {
    if (server) return ESP_ERR_INVALID_STATE;
    if (!core.ready() || config.port == 0U || config.preview_queue_length == 0U ||
        config.preview_queue_length > 8U ||
        config.http_task_stack_bytes < 6144U ||
        config.http_task_stack_bytes > 16384U ||
        config.preview_task_stack_bytes < 4096U ||
        config.preview_task_stack_bytes > 16384U ||
        config.preview_task_priority > static_cast<uint8_t>(configMAX_PRIORITIES - 1)) {
      return ESP_ERR_INVALID_ARG;
    }
    stopping = false;
    preview_queue = xQueueCreate(config.preview_queue_length, sizeof(PreviewJob));
    preview_stopped = xSemaphoreCreateBinary();
    if (!preview_queue || !preview_stopped) {
      cleanupRtos();
      return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(&Impl::previewTaskEntry, "ink_portal_preview",
                    config.preview_task_stack_bytes, this,
                    config.preview_task_priority, &preview_task) != pdPASS) {
      cleanupRtos();
      return ESP_ERR_NO_MEM;
    }
    httpd_config_t native = HTTPD_DEFAULT_CONFIG();
    native.server_port = config.port;
    native.stack_size = config.http_task_stack_bytes;
    native.uri_match_fn = httpd_uri_match_wildcard;
    native.max_uri_handlers = std::max(native.max_uri_handlers, static_cast<uint16_t>(4U));
    native.lru_purge_enable = true;
    native.recv_wait_timeout = 5;
    native.send_wait_timeout = 5;
    esp_err_t status = httpd_start(&server, &native);
    if (status != ESP_OK) {
      stopWorker();
      cleanupRtos();
      return status;
    }
    httpd_uri_t get{};
    get.uri = "/*";
    get.method = HTTP_GET;
    get.handler = &Impl::requestHandler;
    get.user_ctx = this;
    status = httpd_register_uri_handler(server, &get);
    httpd_uri_t post = get;
    post.method = HTTP_POST;
    if (status == ESP_OK) status = httpd_register_uri_handler(server, &post);
    if (status != ESP_OK) {
      httpd_stop(server);
      server = nullptr;
      stopWorker();
      cleanupRtos();
    }
    return status;
  }

  esp_err_t stopWorker() {
    if (!preview_task) return ESP_OK;
    PreviewJob stop_job;
    if (xQueueSend(preview_queue, &stop_job, pdMS_TO_TICKS(1000U)) != pdTRUE)
      return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(preview_stopped, pdMS_TO_TICKS(5000U)) != pdTRUE)
      return ESP_ERR_TIMEOUT;
    return ESP_OK;
  }

  esp_err_t stop() {
    if (!server && !preview_task) return ESP_ERR_INVALID_STATE;
    stopping = true;
    const esp_err_t worker = stopWorker();
    if (worker != ESP_OK) return worker;
    esp_err_t status = ESP_OK;
    if (server) {
      status = httpd_stop(server);
      if (status == ESP_OK) server = nullptr;
    }
    if (status == ESP_OK) cleanupRtos();
    return status;
  }

  void cleanupRtos() {
    if (preview_queue && !preview_task) {
      vQueueDelete(preview_queue);
      preview_queue = nullptr;
    }
    if (preview_stopped && !preview_task) {
      vSemaphoreDelete(preview_stopped);
      preview_stopped = nullptr;
    }
  }
};

EspPortalServer::EspPortalServer(PortalCore& core, IPortalUploadQueue& uploads,
                                 IPortalPreviewSource& previews,
                                 const EspPortalServerConfig& config)
    : impl_(new (std::nothrow) Impl(core, uploads, previews, config)) {}

EspPortalServer::~EspPortalServer() {
  if (!impl_) return;
  if ((impl_->server || impl_->preview_task) && impl_->stop() != ESP_OK) {
    // A slow preview source may still be using Impl. Leaking this exceptional
    // shutdown object is safer than freeing memory still referenced by a task.
    impl_ = nullptr;
    return;
  }
  delete impl_;
  impl_ = nullptr;
}

esp_err_t EspPortalServer::start() {
  return impl_ ? impl_->start() : ESP_ERR_NO_MEM;
}

esp_err_t EspPortalServer::stop() {
  return impl_ ? impl_->stop() : ESP_ERR_INVALID_STATE;
}

bool EspPortalServer::running() const {
  return impl_ && impl_->server != nullptr && !impl_->stopping;
}

}  // namespace portal
}  // namespace inkloop
