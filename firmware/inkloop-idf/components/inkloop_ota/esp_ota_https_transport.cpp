#include "inkloop/esp_ota_https_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Inkloop OTA HTTPS requires the ESP-IDF trusted certificate bundle"
#endif

namespace inkloop {
namespace {

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
  esp_http_client_handle_t client_ = nullptr;
};

std::uint64_t monotonicMs() {
  const std::int64_t microseconds = esp_timer_get_time();
  return microseconds > 0 ?
      static_cast<std::uint64_t>(microseconds) / 1000ULL : 0U;
}

bool remainingTimeout(std::uint64_t deadline_ms, int& output) {
  const std::uint64_t now = monotonicMs();
  if (now >= deadline_ms) return false;
  const std::uint64_t remaining = deadline_ms - now;
  output = static_cast<int>(std::min<std::uint64_t>(
      remaining, static_cast<std::uint64_t>(
                     std::numeric_limits<int>::max())));
  return output > 0;
}

bool publicIpv4Bytes(const std::uint8_t* bytes) {
  if (!bytes) return false;
  const std::uint8_t first = bytes[0];
  const std::uint8_t second = bytes[1];
  const std::uint8_t third = bytes[2];
  if (first == 0U || first == 10U || first == 127U || first >= 224U)
    return false;
  if (first == 100U && (second & 0xC0U) == 0x40U) return false;
  if (first == 169U && second == 254U) return false;
  if (first == 172U && second >= 16U && second <= 31U) return false;
  if (first == 192U && second == 0U && third == 0U) return false;
  if (first == 192U && second == 0U && third == 2U) return false;
  if (first == 192U && second == 88U && third == 99U) return false;
  if (first == 192U && second == 168U) return false;
  if (first == 198U && (second == 18U || second == 19U)) return false;
  if (first == 198U && second == 51U && third == 100U) return false;
  if (first == 203U && second == 0U && third == 113U) return false;
  return true;
}

bool allZero(const std::uint8_t* bytes, std::size_t length) {
  if (!bytes) return true;
  for (std::size_t at = 0U; at < length; ++at) {
    if (bytes[at] != 0U) return false;
  }
  return true;
}

bool publicIpv6Bytes(const std::uint8_t* bytes) {
  if (!bytes || allZero(bytes, 16U)) return false;
  bool mapped = true;
  for (std::size_t at = 0U; at < 10U; ++at)
    mapped = mapped && bytes[at] == 0U;
  mapped = mapped && bytes[10] == 0xFFU && bytes[11] == 0xFFU;
  if (mapped) return publicIpv4Bytes(bytes + 12U);
  if ((bytes[0] & 0xE0U) != 0x20U) return false;
  if (bytes[0] == 0x20U && bytes[1] == 0x01U &&
      bytes[2] == 0x0DU && bytes[3] == 0xB8U)
    return false;
  if (bytes[0] == 0x20U && bytes[1] == 0x01U &&
      bytes[2] == 0x00U && (bytes[3] & 0xF0U) == 0x10U)
    return false;
  if (bytes[0] == 0x20U && bytes[1] == 0x01U &&
      bytes[2] == 0x00U && bytes[3] == 0x02U)
    return false;
  return true;
}

bool connectedPeerPublic(esp_http_client_handle_t client) {
  sockaddr_storage peer{};
  socklen_t length = sizeof(peer);
  const int socket = esp_http_client_get_socket(client);
  if (socket < 0 ||
      getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) != 0)
    return false;
  if (peer.ss_family == AF_INET && length >= sizeof(sockaddr_in)) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&peer);
    const std::uint32_t address = ntohl(ipv4->sin_addr.s_addr);
    const std::array<std::uint8_t, 4U> bytes{
        static_cast<std::uint8_t>(address >> 24U),
        static_cast<std::uint8_t>(address >> 16U),
        static_cast<std::uint8_t>(address >> 8U),
        static_cast<std::uint8_t>(address)};
    return publicIpv4Bytes(bytes.data());
  }
  if (peer.ss_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&peer);
    return publicIpv6Bytes(ipv6->sin6_addr.s6_addr);
  }
  return false;
}

OtaHttpsFetchObservation failed(OtaHttpsFetchCode code,
                                int system_status = 0,
                                int http_status = 0) {
  OtaHttpsFetchObservation output;
  output.code = code;
  output.system_status = system_status;
  output.http_status = http_status;
  return output;
}

}  // namespace

std::uint64_t EspOtaMonotonicClock::nowMs() const {
  return monotonicMs();
}

OtaHttpsFetchObservation EspOtaHttpsTransport::get(
    const OtaHttpsFetchRequest& request, IOtaHttpsBodySink& sink) {
  ParsedOtaHttpsUrl parsed;
  if (parseOtaHttpsUrl(request.url, parsed) != OtaHttpsUrlCode::Ok)
    return failed(OtaHttpsFetchCode::UrlRejected);
  if (request.deadline_ms == 0U || request.maximum_content_length == 0U ||
      request.maximum_content_length > kMaximumOtaImageBytes ||
      request.expected_content_length > request.maximum_content_length ||
      request.maximum_chunk_bytes == 0U ||
      request.maximum_chunk_bytes > kMaximumOtaHttpsTransportChunkBytes)
    return failed(OtaHttpsFetchCode::InvalidRequest);

  int timeout_ms = 0;
  if (!remainingTimeout(request.deadline_ms, timeout_ms))
    return failed(OtaHttpsFetchCode::DeadlineExceeded);
  std::array<char, kMaximumOtaUrlBytes + 1U> url{};
  std::memcpy(url.data(), request.url.data, request.url.length);

  esp_http_client_config_t config{};
  config.url = url.data();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = timeout_ms;
  config.disable_auto_redirect = true;
  config.max_redirection_count = 0;
  config.max_authorization_retries = -1;
  config.buffer_size = static_cast<int>(kMaximumOtaHttpsTransportChunkBytes);
  config.buffer_size_tx = 1024;
  config.skip_cert_common_name_check = false;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  ClientGuard client(esp_http_client_init(&config));
  if (!client.get()) return failed(OtaHttpsFetchCode::ClientUnavailable);
  const esp_err_t opened = esp_http_client_open(client.get(), 0);
  if (opened != ESP_OK)
    return failed(OtaHttpsFetchCode::ConnectionFailed, opened);
  if (!connectedPeerPublic(client.get()))
    return failed(OtaHttpsFetchCode::PeerRejected);

  if (!remainingTimeout(request.deadline_ms, timeout_ms))
    return failed(OtaHttpsFetchCode::DeadlineExceeded);
  const esp_err_t timeout_status =
      esp_http_client_set_timeout_ms(client.get(), timeout_ms);
  if (timeout_status != ESP_OK)
    return failed(OtaHttpsFetchCode::ConnectionFailed, timeout_status);
  const std::int64_t content_length =
      esp_http_client_fetch_headers(client.get());
  const int http_status = esp_http_client_get_status_code(client.get());
  if (http_status >= 300 && http_status < 400)
    return failed(OtaHttpsFetchCode::RedirectRejected, 0, http_status);
  if (http_status != 200)
    return failed(OtaHttpsFetchCode::HttpStatusRejected, 0, http_status);
  if (content_length <= 0)
    return failed(OtaHttpsFetchCode::ContentLengthRequired, 0, http_status);
  const std::uint64_t exact_length =
      static_cast<std::uint64_t>(content_length);
  if (exact_length > request.maximum_content_length)
    return failed(OtaHttpsFetchCode::ResponseTooLarge, 0, http_status);
  if (request.expected_content_length != 0U &&
      exact_length != request.expected_content_length)
    return failed(OtaHttpsFetchCode::ContentLengthMismatch, 0, http_status);

  OtaHttpsFetchObservation output;
  output.http_status = http_status;
  output.content_length = exact_length;
  std::array<std::uint8_t, kMaximumOtaHttpsTransportChunkBytes> buffer{};
  while (output.bytes_received < exact_length) {
    if (!remainingTimeout(request.deadline_ms, timeout_ms))
      return failed(OtaHttpsFetchCode::DeadlineExceeded, 0, http_status);
    const esp_err_t set_timeout =
        esp_http_client_set_timeout_ms(client.get(), timeout_ms);
    if (set_timeout != ESP_OK)
      return failed(OtaHttpsFetchCode::ReadFailed, set_timeout, http_status);
    const std::uint64_t remaining = exact_length - output.bytes_received;
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            remaining,
            std::min<std::size_t>(request.maximum_chunk_bytes,
                                  buffer.size())));
    const int count = esp_http_client_read(
        client.get(), reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(wanted));
    if (count < 0)
      return failed(OtaHttpsFetchCode::ReadFailed, count, http_status);
    if (count == 0) return failed(OtaHttpsFetchCode::Truncated, 0,
                                  http_status);
    if (static_cast<std::size_t>(count) > wanted)
      return failed(OtaHttpsFetchCode::ContentLengthMismatch, 0,
                    http_status);
    if (!remainingTimeout(request.deadline_ms, timeout_ms))
      return failed(OtaHttpsFetchCode::DeadlineExceeded, 0, http_status);
    if (!sink.append(buffer.data(), static_cast<std::size_t>(count)))
      return failed(OtaHttpsFetchCode::SinkRejected, 0, http_status);
    output.bytes_received += static_cast<std::uint64_t>(count);
  }
  if (!esp_http_client_is_complete_data_received(client.get()))
    return failed(OtaHttpsFetchCode::Truncated, 0, http_status);
  if (!remainingTimeout(request.deadline_ms, timeout_ms))
    return failed(OtaHttpsFetchCode::DeadlineExceeded, 0, http_status);
  output.code = OtaHttpsFetchCode::Ok;
  return output;
}

}  // namespace inkloop
