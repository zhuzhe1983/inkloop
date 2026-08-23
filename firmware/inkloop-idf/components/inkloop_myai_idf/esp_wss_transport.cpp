#include "inkloop/myai/esp_wss_transport.hpp"

#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_timer.h"
#include "inkloop/myai/EndpointPolicy.h"
#include "inkloop/myai/esp_network_operation_gate.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Inkloop MyAI WSS requires the ESP-IDF trusted certificate bundle"
#endif

#if CONFIG_LOG_MAXIMUM_LEVEL > 3
#error "Inkloop MyAI WSS forbids debug/verbose transport logs containing headers"
#endif

namespace inkloop {
namespace myai {
namespace {

constexpr size_t kMaximumHeaderCount = 16;
constexpr size_t kMaximumHeaderNameBytes = 64;
constexpr size_t kMaximumHeaderValueBytes = 2048;
constexpr size_t kMaximumHeaderBlockBytes = 3072;
constexpr size_t kMaximumOutboundTextBytes = 12U * 1024U;
constexpr size_t kMaximumOutboundAudioBytes = 12U * 1024U;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 250;

uint64_t monotonicMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

Status transportError(const char* detail) {
  return Status(ErrorCode::Transport, 0, detail);
}

bool lineSafe(const std::string& value) {
  for (unsigned char ch : value) {
    if (ch == 0 || ch == '\r' || ch == '\n') return false;
  }
  return true;
}

Status parseWebSocketUrl(const std::string& url, HttpsEndpoint& endpoint,
                         std::string& path, std::string& httpUrl) {
  size_t authorityOffset = 0;
  if (url.compare(0, 6, "wss://") == 0) {
    httpUrl = std::string("https://") + url.substr(6);
    authorityOffset = 6;
  } else if (url.compare(0, 5, "ws://") == 0) {
    httpUrl = std::string("http://") + url.substr(5);
    authorityOffset = 5;
  } else {
    return Status(ErrorCode::Security, 0,
                  "MyAI WebSocket requires public WS or WSS");
  }
  Status status = EndpointPolicy::parsePublicUrl(httpUrl, true, endpoint);
  if (!status.ok()) return status;
  const size_t pathOffset = url.find_first_of("/?", authorityOffset);
  if (pathOffset == std::string::npos) path = "/";
  else if (url[pathOffset] == '?') path = "/" + url.substr(pathOffset);
  else path = url.substr(pathOffset);
  return Status::success();
}

Status buildHeaderBlock(const std::map<std::string, std::string>& headers,
                        std::string& output) {
  output.clear();
  if (headers.size() > kMaximumHeaderCount) {
    return Status(ErrorCode::InvalidArgument, 0,
                  "invalid bounded MyAI WSS headers");
  }
  for (const auto& header : headers) {
    if (header.first.empty() || header.first.size() > kMaximumHeaderNameBytes ||
        header.second.size() > kMaximumHeaderValueBytes ||
        header.first.find(':') != std::string::npos ||
        !lineSafe(header.first) || !lineSafe(header.second)) {
      output.clear();
      return Status(ErrorCode::InvalidArgument, 0,
                    "invalid bounded MyAI WSS headers");
    }
    const size_t required = header.first.size() + header.second.size() + 4U;
    if (required > kMaximumHeaderBlockBytes - output.size()) {
      output.clear();
      return Status(ErrorCode::TooLarge, 0,
                    "MyAI WSS headers exceed byte limit");
    }
    output += header.first;
    output += ": ";
    output += header.second;
    output += "\r\n";
  }
  return Status::success();
}

}  // namespace

EspWssTransport::EspWssTransport(EspEndpointSecurity& endpointSecurity)
    : endpointSecurity_(endpointSecurity), network_(nullptr), websocket_(nullptr),
      listener_(nullptr), port_(443), currentFrameOffset_(0),
      connected_(false), ingressReady_(nullptr),
      ingressReadyContext_(nullptr) {}

EspWssTransport::~EspWssTransport() { releaseTransport(); }

Status EspWssTransport::configure(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {
  HttpsEndpoint endpoint;
  std::string httpUrl;
  Status status = parseWebSocketUrl(url, endpoint, path_, httpUrl);
  if (!status.ok()) return status;
  status = endpointSecurity_.validatePublicEndpoint(httpUrl);
  if (!status.ok()) return status;
  status = buildHeaderBlock(headers, headerBlock_);
  if (!status.ok()) return status;
  host_ = endpoint.host;
  port_ = endpoint.port;

  network_ = endpoint.tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
  if (!network_) return transportError("MyAI WebSocket allocation failed");
  if (endpoint.tls) {
    esp_transport_ssl_crt_bundle_attach(network_, esp_crt_bundle_attach);
    esp_transport_ssl_set_common_name(network_, host_.c_str());
  }
  esp_transport_set_default_port(network_, endpoint.port);
  websocket_ = esp_transport_ws_init(network_);
  if (!websocket_) {
    releaseTransport();
    return transportError("MyAI WSS transport allocation failed");
  }
  esp_transport_set_default_port(websocket_, endpoint.port);
  esp_transport_ws_config_t config{};
  config.ws_path = path_.c_str();
  config.user_agent = "inkloop-esp-idf/1";
  config.headers = headerBlock_.empty() ? nullptr : headerBlock_.c_str();
  config.propagate_control_frames = false;
  if (esp_transport_ws_set_config(websocket_, &config) != ESP_OK) {
    releaseTransport();
    return transportError("MyAI WSS configuration failed");
  }
  return Status::success();
}

Status EspWssTransport::connect(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    IWebSocketListener& listener) {
  if (connected_ || websocket_ || listener_) {
    return Status(ErrorCode::InvalidState, 0,
                  "MyAI WSS is already active");
  }
  Status status = configure(url, headers);
  if (!status.ok()) return status;
  EspNetworkOperationLease network_lease(kConnectTimeoutMs);
  if (!network_lease.acquired()) {
    releaseTransport();
    return transportError("MyAI WSS operation gate timed out");
  }
  const int result = esp_transport_connect(websocket_, host_.c_str(), port_,
                                           kConnectTimeoutMs);
  // esp_transport_ws returns zero only for the exact 101 upgrade. Positive
  // 3xx values are redirects and are deliberately rejected before any retry.
  if (result != 0 ||
      esp_transport_ws_get_upgrade_request_status(websocket_) != 101) {
    const bool redirect = result > 0;
    releaseTransport();
    return Status(redirect ? ErrorCode::Security : ErrorCode::Transport, 0,
                  redirect ? "MyAI WSS redirect rejected"
                           : "MyAI WSS handshake failed");
  }
  status = endpointSecurity_.validateConnectedSocket(
      esp_transport_get_socket(websocket_));
  if (!status.ok()) {
    releaseTransport();
    return status;
  }
  connected_ = true;
  listener_ = &listener;
  ingress_.reset();
  currentFrameOffset_ = 0;
  keep_alive_.start(monotonicMs());
  listener_->onWebSocketOpen();
  return Status::success();
}

Status EspWssTransport::send(uint8_t opcode, const uint8_t* bytes,
                             size_t length) {
  if (!connected_ || !websocket_ || !bytes || length == 0 ||
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return Status(ErrorCode::InvalidState, 0,
                  "MyAI WSS send rejected");
  }
  const ws_transport_opcodes_t wireOpcode = static_cast<ws_transport_opcodes_t>(
      WS_TRANSPORT_OPCODES_FIN | opcode);
  const int sent = esp_transport_ws_send_raw(
      websocket_, wireOpcode, reinterpret_cast<const char*>(bytes),
      static_cast<int>(length), kSendTimeoutMs);
  if (sent != static_cast<int>(length)) {
    notifyClosed(1006, "transport_send_failed");
    return transportError("MyAI WSS send failed");
  }
  return Status::success();
}

Status EspWssTransport::sendText(const std::string& message) {
  if (message.empty() || message.size() > kMaximumOutboundTextBytes) {
    return Status(ErrorCode::TooLarge, 0,
                  "MyAI WSS text exceeds byte limit");
  }
  return send(WS_TRANSPORT_OPCODES_TEXT,
              reinterpret_cast<const uint8_t*>(message.data()),
              message.size());
}

Status EspWssTransport::sendBinary(const uint8_t* bytes, size_t length) {
  if (length > kMaximumOutboundAudioBytes) {
    return Status(ErrorCode::TooLarge, 0,
                  "MyAI WSS audio exceeds byte limit");
  }
  return send(WS_TRANSPORT_OPCODES_BINARY, bytes, length);
}

Status EspWssTransport::serviceKeepAlive() {
  if (!connected_ || !websocket_) {
    return Status(ErrorCode::InvalidState, 0, "MyAI WSS is not open");
  }
  const uint64_t now_ms = monotonicMs();
  if (!keep_alive_.pingDue(now_ms)) return Status::success();
  // A one-byte, credential-free payload avoids SDK-specific zero-length frame
  // handling while remaining a valid RFC 6455 control frame. The transport
  // consumes Pong/control frames internally (`propagate_control_frames=false`).
  const uint8_t payload = 0U;
  const int sent = esp_transport_ws_send_raw(
      websocket_, static_cast<ws_transport_opcodes_t>(
                      WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_PING),
      reinterpret_cast<const char*>(&payload), 1, kSendTimeoutMs);
  if (sent != 1) {
    notifyClosed(1006, "keepalive_ping_failed");
    return transportError("MyAI WSS keepalive ping failed");
  }
  keep_alive_.notePingSent(now_ms);
  return Status::success();
}

Status EspWssTransport::pollIngress() {
  if (!connected_ || !websocket_ || !listener_) {
    return Status(ErrorCode::InvalidState, 0, "MyAI WSS is not open");
  }
  const Status keep_alive = serviceKeepAlive();
  if (!keep_alive.ok()) return keep_alive;
  // Backpressure is evaluated before touching the socket. The existing
  // partially assembled frame may continue only when one maximum legal audio
  // message fits in the playback bridge; this prevents callback-side loss.
  if (ingressReady_ && !ingressReady_(ingressReadyContext_)) {
    return Status::success();
  }
  const int readable = esp_transport_poll_read(websocket_, 0);
  if (readable == 0) return Status::success();
  if (readable < 0) {
    notifyClosed(1006, "transport_poll_failed");
    return transportError("MyAI WSS poll failed");
  }

  std::array<uint8_t, 2048> chunk{};
  const int received = esp_transport_read(
      websocket_, reinterpret_cast<char*>(chunk.data()),
      static_cast<int>(chunk.size()), 0);
  if (received < 0) {
    notifyClosed(1006, "transport_read_failed");
    return transportError("MyAI WSS read failed");
  }
  const uint8_t opcode = static_cast<uint8_t>(
      esp_transport_ws_get_read_opcode(websocket_) & 0x0fU);
  if (received == 0) {
    if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
      notifyClosed(1000, "server_closed");
    }
    return Status::success();
  }
  const int payload = esp_transport_ws_get_read_payload_len(websocket_);
  if (payload <= 0) {
    notifyClosed(1002, "invalid_frame_length");
    return Status(ErrorCode::Protocol, 0,
                  "invalid MyAI WSS frame length");
  }
  WssIngressChunk input;
  input.opcode = opcode;
  input.finalFrame = esp_transport_ws_get_fin_flag(websocket_);
  input.framePayloadBytes = static_cast<size_t>(payload);
  input.frameOffset = currentFrameOffset_;
  input.bytes = chunk.data();
  input.length = static_cast<size_t>(received);
  bool complete = false;
  WssCompletedMessage message;
  Status status = ingress_.append(input, complete, message);
  if (!status.ok()) {
    notifyClosed(status.code == ErrorCode::TooLarge ? 1009 : 1002,
                 "invalid_ingress_frame");
    return status;
  }
  currentFrameOffset_ += static_cast<size_t>(received);
  if (currentFrameOffset_ == static_cast<size_t>(payload)) {
    currentFrameOffset_ = 0;
  }
  if (!complete || !listener_) return Status::success();
  if (message.kind == WssMessageKind::Text) {
    listener_->onWebSocketText(std::string(
        reinterpret_cast<const char*>(message.bytes), message.length));
  } else if (message.kind == WssMessageKind::Binary) {
    listener_->onWebSocketBinary(message.bytes, message.length);
  }
  return Status::success();
}

void EspWssTransport::close(uint16_t code, const std::string& reason) {
  if (connected_ && websocket_) {
    std::array<uint8_t, 125> payload{};
    payload[0] = static_cast<uint8_t>(code >> 8U);
    payload[1] = static_cast<uint8_t>(code & 0xffU);
    const size_t reasonLength = std::min<size_t>(reason.size(), 123U);
    if (reasonLength != 0) {
      std::memcpy(payload.data() + 2U, reason.data(), reasonLength);
    }
    (void)esp_transport_ws_send_raw(
        websocket_, static_cast<ws_transport_opcodes_t>(
                        WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_CLOSE),
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(reasonLength + 2U), kSendTimeoutMs);
  }
  listener_ = nullptr;
  releaseTransport();
}

void EspWssTransport::notifyClosed(int code, const char* reason) {
  IWebSocketListener* listener = listener_;
  listener_ = nullptr;
  releaseTransport();
  if (listener) listener->onWebSocketClosed(code, reason);
}

void EspWssTransport::releaseTransport() {
  connected_ = false;
  keep_alive_.stop();
  currentFrameOffset_ = 0;
  ingress_.reset();
  if (websocket_) {
    (void)esp_transport_close(websocket_);
    esp_transport_destroy(websocket_);
    websocket_ = nullptr;
  }
  if (network_) {
    esp_transport_destroy(network_);
    network_ = nullptr;
  }
  host_.clear();
  path_.clear();
  headerBlock_.clear();
  port_ = 443;
}

}  // namespace myai
}  // namespace inkloop
