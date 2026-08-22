#include "inkloop/esp_wifi_provisioning_server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#include "esp_http_server.h"

namespace inkloop {
namespace {

const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Content";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

}  // namespace

struct EspWifiProvisioningServer::Impl {
  explicit Impl(WifiProvisioningPortal& value) : portal(value) {}

  static esp_err_t handler(httpd_req_t* request) {
    if (!request || !request->user_ctx) return ESP_ERR_INVALID_ARG;
    return static_cast<Impl*>(request->user_ctx)->handle(request);
  }

  esp_err_t handle(httpd_req_t* native) {
    WifiProvisioningRequest request;
    request.method = native->method == HTTP_GET ? "GET" :
                     native->method == HTTP_POST ? "POST" : "UNSUPPORTED";
    request.path = native->uri;
    if (native->method == HTTP_POST) {
      if (native->content_len > kMaximumWifiProvisioningBodyBytes) {
        const WifiProvisioningResponse response{
            413, "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":\"body_too_large\"}"};
        return send(native, response);
      }
      const size_t content_type_length =
          httpd_req_get_hdr_value_len(native, "Content-Type");
      if (content_type_length == 0U || content_type_length > 63U) {
        const WifiProvisioningResponse response{
            415, "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":\"form_required\"}"};
        return send(native, response);
      }
      std::array<char, 64> content_type{};
      if (httpd_req_get_hdr_value_str(native, "Content-Type",
                                      content_type.data(),
                                      content_type_length + 1U) != ESP_OK) {
        return ESP_FAIL;
      }
      request.content_type.assign(content_type.data(), content_type_length);
      request.body.resize(native->content_len);
      size_t received = 0;
      unsigned int timeouts = 0;
      while (received < native->content_len) {
        const int count = httpd_req_recv(
            native, request.body.data() + received,
            native->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2U) continue;
        if (count <= 0) return ESP_FAIL;
        received += static_cast<size_t>(count);
      }
    }
    const WifiProvisioningResponse response = portal.handle(request);
    std::fill(request.body.begin(), request.body.end(), '\0');
    return send(native, response);
  }

  static esp_err_t send(httpd_req_t* native,
                        const WifiProvisioningResponse& response) {
    esp_err_t status = httpd_resp_set_status(native, statusText(response.status));
    if (status == ESP_OK)
      status = httpd_resp_set_type(native, response.content_type.c_str());
    if (status == ESP_OK)
      status = httpd_resp_set_hdr(native, "Cache-Control", "no-store");
    if (status == ESP_OK)
      status = httpd_resp_set_hdr(native, "X-Content-Type-Options", "nosniff");
    if (status != ESP_OK) return status;
    return httpd_resp_send(native, response.body.data(), response.body.size());
  }

  esp_err_t start() {
    if (server) return ESP_ERR_INVALID_STATE;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    esp_err_t status = httpd_start(&server, &config);
    if (status != ESP_OK) return status;
    httpd_uri_t root{};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = &Impl::handler;
    root.user_ctx = this;
    status = httpd_register_uri_handler(server, &root);
    httpd_uri_t configure = root;
    configure.uri = "/configure";
    configure.method = HTTP_POST;
    if (status == ESP_OK)
      status = httpd_register_uri_handler(server, &configure);
    if (status != ESP_OK) {
      httpd_stop(server);
      server = nullptr;
    }
    return status;
  }

  esp_err_t stop() {
    if (!server) return ESP_ERR_INVALID_STATE;
    const esp_err_t status = httpd_stop(server);
    if (status == ESP_OK) server = nullptr;
    return status;
  }

  WifiProvisioningPortal& portal;
  httpd_handle_t server = nullptr;
};

EspWifiProvisioningServer::EspWifiProvisioningServer(
    WifiProvisioningPortal& portal)
    : impl_(new (std::nothrow) Impl(portal)) {}

EspWifiProvisioningServer::~EspWifiProvisioningServer() {
  if (!impl_) return;
  if (impl_->server) impl_->stop();
  delete impl_;
}

esp_err_t EspWifiProvisioningServer::start() {
  return impl_ ? impl_->start() : ESP_ERR_NO_MEM;
}

esp_err_t EspWifiProvisioningServer::stop() {
  return impl_ ? impl_->stop() : ESP_ERR_INVALID_STATE;
}

bool EspWifiProvisioningServer::running() const {
  return impl_ && impl_->server != nullptr;
}

}  // namespace inkloop
