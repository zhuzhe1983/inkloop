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
constexpr size_t kMaximumControlFrameBytes = 125U;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 250;
// pollIngress() shares a cooperative owner with latency-sensitive controls.
// Ten milliseconds is long enough for an already-readable TLS record tail,
// while remaining below the 20 ms physical-button acceptance budget.
constexpr uint64_t kIngressReadBudgetMs = 10U;
// A healthy voice owner drains the 12 KiB admission headroom in well under a
// second even at the minimum supported PCM rate. Five seconds distinguishes a
// stuck local consumer from a delayed network Pong without conflating them.
constexpr uint64_t kIngressBackpressureTimeoutMs = 5000U;

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

uint32_t decodePingToken(const uint8_t* bytes, size_t length) {
  if (!bytes || length != 4U) return 0U;
  return (static_cast<uint32_t>(bytes[0]) << 24U) |
         (static_cast<uint32_t>(bytes[1]) << 16U) |
         (static_cast<uint32_t>(bytes[2]) << 8U) |
         static_cast<uint32_t>(bytes[3]);
}

std::array<uint8_t, 4> encodePingToken(uint32_t token) {
  return {{static_cast<uint8_t>(token >> 24U),
           static_cast<uint8_t>(token >> 16U),
           static_cast<uint8_t>(token >> 8U),
           static_cast<uint8_t>(token)}};
}

}  // namespace

EspWssTransport::EspWssTransport(EspEndpointSecurity& endpointSecurity)
    : endpointSecurity_(endpointSecurity), network_(nullptr), websocket_(nullptr),
      listener_(nullptr), port_(443), currentFrameOffset_(0),
      deferredDataLength_(0), deferredFramePayloadBytes_(0),
      deferredFrameOffset_(0), deferredOpcode_(0), deferredFinalFrame_(false),
      deferredDataPending_(false),
      ingressBackpressureStartedMs_(0), ingressBackpressureActive_(false),
      ingressBackpressureFrameDrained_(false),
      postBackpressureControlGrace_(false),
      episodeControlGraceUsed_(false),
      pendingPongBackpressureRebaseUsed_(false),
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
  // ESP-IDF otherwise consumes Pong internally without exposing whether our
  // own Ping was acknowledged. Explicit propagation lets the cooperative
  // owner enforce a bounded missing-Pong deadline without another task.
  config.propagate_control_frames = true;
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
  if (keep_alive_.pongTimedOut(now_ms)) {
    notifyClosed(1006, "keepalive_pong_timeout");
    return transportError("MyAI WSS keepalive Pong timed out");
  }
  if (!keep_alive_.pingDue(now_ms)) return Status::success();
  const uint32_t token = keep_alive_.nextPingToken();
  if (token == 0U) {
    notifyClosed(1006, "keepalive_state_failed");
    return transportError("MyAI WSS keepalive state failed");
  }
  // This per-connection sequence is credential-free. Requiring the same four
  // bytes in Pong prevents unrelated/unsolicited Pong traffic from extending
  // a dead connection indefinitely.
  const std::array<uint8_t, 4> payload = encodePingToken(token);
  const int sent = esp_transport_ws_send_raw(
      websocket_, static_cast<ws_transport_opcodes_t>(
                      WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_PING),
      reinterpret_cast<const char*>(payload.data()),
      static_cast<int>(payload.size()), kSendTimeoutMs);
  if (sent != static_cast<int>(payload.size())) {
    notifyClosed(1006, "keepalive_ping_failed");
    return transportError("MyAI WSS keepalive ping failed");
  }
  keep_alive_.notePingSent(now_ms, token);
  pendingPongBackpressureRebaseUsed_ = false;
  return Status::success();
}

Status EspWssTransport::pauseForIngressBackpressure(
    bool allow_pong_grace) {
  const uint64_t now_ms = monotonicMs();
  if (!ingressBackpressureActive_) {
    ingressBackpressureActive_ = true;
    ingressBackpressureStartedMs_ = now_ms;
    episodeControlGraceUsed_ = false;
  }
  ingressBackpressureFrameDrained_ = false;
  if (allow_pong_grace && keep_alive_.awaitingPong() &&
      !episodeControlGraceUsed_) {
    postBackpressureControlGrace_ = true;
  }
  if (now_ms < ingressBackpressureStartedMs_ ||
      now_ms - ingressBackpressureStartedMs_ >=
          kIngressBackpressureTimeoutMs) {
    notifyClosed(1006, "ingress_backpressure_timeout");
    return transportError("MyAI WSS ingress backpressure timed out");
  }
  return Status::success();
}

Status EspWssTransport::serviceKeepAliveAfterIngress() {
  return serviceKeepAlive();
}

void EspWssTransport::noteIngressBackpressureFrameDrained() {
  if (!ingressBackpressureActive_) return;
  ingressBackpressureFrameDrained_ = true;
  if (keep_alive_.awaitingPong() && postBackpressureControlGrace_ &&
      !pendingPongBackpressureRebaseUsed_) {
    keep_alive_.rebase(monotonicMs());
    pendingPongBackpressureRebaseUsed_ = true;
  }
}

void EspWssTransport::finishIngressBackpressureEpisode() {
  ingressBackpressureStartedMs_ = 0;
  ingressBackpressureActive_ = false;
  ingressBackpressureFrameDrained_ = false;
  episodeControlGraceUsed_ = false;
}

Status EspWssTransport::handleControlFrame(
    uint8_t opcode, bool final_frame, const uint8_t* bytes, size_t length,
    size_t frame_payload_bytes) {
  if (!final_frame || frame_payload_bytes > kMaximumControlFrameBytes ||
      length != frame_payload_bytes) {
    notifyClosed(1002, "invalid_control_frame");
    return Status(ErrorCode::Protocol, 0,
                  "invalid MyAI WSS control frame");
  }
  if (opcode == WS_TRANSPORT_OPCODES_PING) {
    const int sent = esp_transport_ws_send_raw(
        websocket_, static_cast<ws_transport_opcodes_t>(
                        WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_PONG),
        reinterpret_cast<const char*>(bytes), static_cast<int>(length),
        kSendTimeoutMs);
    if (sent != static_cast<int>(length)) {
      notifyClosed(1006, "keepalive_pong_send_failed");
      return transportError("MyAI WSS Pong send failed");
    }
    return Status::success();
  }
  if (opcode == WS_TRANSPORT_OPCODES_PONG) {
    // RFC 6455 permits unsolicited Pong. Ignore it unless it acknowledges the
    // exact credential-free token currently outstanding.
    if (keep_alive_.notePong(decodePingToken(bytes, length))) {
      pendingPongBackpressureRebaseUsed_ = false;
    }
    return Status::success();
  }
  if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
    uint16_t close_code = 1000U;
    if (!detail::decodeValidWssClosePayload(bytes, length, close_code)) {
      notifyClosed(1002, "invalid_close_frame");
      return Status(ErrorCode::Protocol, 0,
                    "invalid MyAI WSS close frame");
    }
    (void)esp_transport_ws_send_raw(
        websocket_, static_cast<ws_transport_opcodes_t>(
                        WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_CLOSE),
        reinterpret_cast<const char*>(bytes), static_cast<int>(length),
        kSendTimeoutMs);
    notifyClosed(close_code, "server_closed");
    return Status::success();
  }
  notifyClosed(1002, "unsupported_control_frame");
  return Status(ErrorCode::Protocol, 0,
                "unsupported MyAI WSS control frame");
}

bool EspWssTransport::ingressDataReady() const {
  return !ingressReady_ || ingressReady_(ingressReadyContext_);
}

Status EspWssTransport::consumeDataChunk(
    uint8_t opcode, bool final_frame, size_t frame_payload_bytes,
    size_t frame_offset, const uint8_t* bytes, size_t length) {
  WssIngressChunk input;
  input.opcode = opcode;
  input.finalFrame = final_frame;
  input.framePayloadBytes = frame_payload_bytes;
  input.frameOffset = frame_offset;
  input.bytes = bytes;
  input.length = length;
  bool complete = false;
  WssCompletedMessage message;
  Status status = ingress_.append(input, complete, message);
  if (!status.ok()) {
    notifyClosed(status.code == ErrorCode::TooLarge ? 1009 : 1002,
                 "invalid_ingress_frame");
    return status;
  }
  currentFrameOffset_ = frame_offset + length;
  if (currentFrameOffset_ == frame_payload_bytes) currentFrameOffset_ = 0;
  if (!complete || !listener_) return Status::success();
  if (message.kind == WssMessageKind::Text) {
    listener_->onWebSocketText(std::string(
        reinterpret_cast<const char*>(message.bytes), message.length));
  } else if (message.kind == WssMessageKind::Binary) {
    listener_->onWebSocketBinary(message.bytes, message.length);
  }
  return Status::success();
}

Status EspWssTransport::pollIngress() {
  if (!connected_ || !websocket_ || !listener_) {
    return Status(ErrorCode::InvalidState, 0, "MyAI WSS is not open");
  }

  // Once a data chunk has been identified, retain it until playback can
  // accept the corresponding message. Keepalive is suspended while local
  // backpressure owns a started frame: a valid Pong may already be queued
  // behind bytes that RFC 6455 forbids it from interleaving with.
  if (deferredDataPending_) {
    if (!ingressDataReady()) return pauseForIngressBackpressure(true);
    deferredDataPending_ = false;
    const Status consumed = consumeDataChunk(
        deferredOpcode_, deferredFinalFrame_, deferredFramePayloadBytes_,
        deferredFrameOffset_, deferredDataChunk_.data(), deferredDataLength_);
    if (!consumed.ok() || !connected_) return consumed;
    if (currentFrameOffset_ != 0U) return consumed;
    noteIngressBackpressureFrameDrained();
    if (postBackpressureControlGrace_) return consumed;
    finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  // A control frame cannot interleave inside a data-frame payload. Pausing a
  // known continuation therefore cannot expose a queued Pong until that frame
  // is drained. The independent local watchdog bounds a permanently closed
  // gate without misreporting the failure as a missing Pong.
  if (currentFrameOffset_ != 0U && !ingressDataReady()) {
    return pauseForIngressBackpressure(true);
  }

  // Drain one already-buffered frame/chunk before enforcing the Pong deadline.
  // After backpressure reaches a frame boundary this is the sole grace read;
  // a continuous sequence of new data frames cannot postpone timeout again.
  const int readable = esp_transport_poll_read(websocket_, 0);
  if (readable == 0) {
    if (ingressBackpressureActive_ &&
        !ingressBackpressureFrameDrained_)
      return pauseForIngressBackpressure(true);
    if (postBackpressureControlGrace_) {
      postBackpressureControlGrace_ = false;
      episodeControlGraceUsed_ = true;
    }
    if (ingressBackpressureActive_) finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  if (readable < 0) {
    notifyClosed(1006, "transport_poll_failed");
    return transportError("MyAI WSS poll failed");
  }

  const bool frame_boundary_before_read = currentFrameOffset_ == 0U;
  std::array<uint8_t, 2048> chunk{};
  const uint64_t read_deadline_ms = monotonicMs() + kIngressReadBudgetMs;
  const int read_timeout_ms = detail::remainingWssReadTimeoutMs(
      monotonicMs(), read_deadline_ms);
  if (read_timeout_ms <= 0) {
    if (ingressBackpressureActive_ &&
        !ingressBackpressureFrameDrained_)
      return pauseForIngressBackpressure(true);
    if (postBackpressureControlGrace_) {
      postBackpressureControlGrace_ = false;
      episodeControlGraceUsed_ = true;
    }
    if (ingressBackpressureActive_) finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  const int received = esp_transport_read(
      websocket_, reinterpret_cast<char*>(chunk.data()),
      static_cast<int>(chunk.size()), read_timeout_ms);
  if (received < 0) {
    notifyClosed(1006, "transport_read_failed");
    return transportError("MyAI WSS read failed");
  }
  const uint8_t opcode = static_cast<uint8_t>(
      esp_transport_ws_get_read_opcode(websocket_) & 0x0fU);
  const bool final_frame = esp_transport_ws_get_fin_flag(websocket_);
  const int payload = esp_transport_ws_get_read_payload_len(websocket_);
  if (opcode == WS_TRANSPORT_OPCODES_CLOSE ||
      opcode == WS_TRANSPORT_OPCODES_PING ||
      opcode == WS_TRANSPORT_OPCODES_PONG) {
    const bool grace_read = postBackpressureControlGrace_;
    const Status control = handleControlFrame(
        opcode, final_frame, chunk.data(), static_cast<size_t>(received),
        payload < 0 ? std::numeric_limits<size_t>::max()
                    : static_cast<size_t>(payload));
    if (grace_read) {
      postBackpressureControlGrace_ = false;
      episodeControlGraceUsed_ = true;
    }
    if (!control.ok() || !connected_) return control;
    if (ingressBackpressureActive_) finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  if (received == 0) {
    if (ingressBackpressureActive_ &&
        !ingressBackpressureFrameDrained_)
      return pauseForIngressBackpressure(true);
    if (postBackpressureControlGrace_) {
      postBackpressureControlGrace_ = false;
      episodeControlGraceUsed_ = true;
    }
    if (ingressBackpressureActive_) finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  if (payload <= 0) {
    notifyClosed(1002, "invalid_frame_length");
    return Status(ErrorCode::Protocol, 0,
                  "invalid MyAI WSS frame length");
  }
  const bool grace_data_frame =
      frame_boundary_before_read && postBackpressureControlGrace_;
  const bool data_ready =
      !frame_boundary_before_read || ingressDataReady();
  if (!data_ready) {
    if (grace_data_frame) {
      postBackpressureControlGrace_ = false;
      episodeControlGraceUsed_ = true;
    }
    deferredOpcode_ = opcode;
    deferredFinalFrame_ = final_frame;
    deferredFramePayloadBytes_ = static_cast<size_t>(payload);
    deferredFrameOffset_ = currentFrameOffset_;
    deferredDataLength_ = static_cast<size_t>(received);
    std::memcpy(deferredDataChunk_.data(), chunk.data(), deferredDataLength_);
    deferredDataPending_ = true;
    return pauseForIngressBackpressure(!grace_data_frame);
  }
  if (grace_data_frame) {
    postBackpressureControlGrace_ = false;
    episodeControlGraceUsed_ = true;
  }
  if (grace_data_frame) {
    ingressBackpressureFrameDrained_ = false;
  } else if (frame_boundary_before_read &&
             ingressBackpressureFrameDrained_) {
    finishIngressBackpressureEpisode();
  }

  const bool draining_backpressured_frame = ingressBackpressureActive_;
  if (!draining_backpressured_frame) {
    const Status keep_alive = serviceKeepAliveAfterIngress();
    if (!keep_alive.ok()) return keep_alive;
  }
  const Status consumed = consumeDataChunk(
      opcode, final_frame, static_cast<size_t>(payload), currentFrameOffset_,
      chunk.data(), static_cast<size_t>(received));
  if (!consumed.ok() || !connected_) return consumed;
  if (ingressBackpressureActive_ && currentFrameOffset_ == 0U) {
    noteIngressBackpressureFrameDrained();
  }
  if (ingressBackpressureActive_ && ingressBackpressureFrameDrained_ &&
      !postBackpressureControlGrace_) {
    finishIngressBackpressureEpisode();
    return serviceKeepAliveAfterIngress();
  }
  if (ingressBackpressureActive_ || postBackpressureControlGrace_ ||
      !draining_backpressured_frame) {
    return consumed;
  }
  return serviceKeepAliveAfterIngress();
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
  deferredDataLength_ = 0;
  deferredFramePayloadBytes_ = 0;
  deferredFrameOffset_ = 0;
  deferredOpcode_ = 0;
  deferredFinalFrame_ = false;
  deferredDataPending_ = false;
  ingressBackpressureStartedMs_ = 0;
  ingressBackpressureActive_ = false;
  ingressBackpressureFrameDrained_ = false;
  postBackpressureControlGrace_ = false;
  episodeControlGraceUsed_ = false;
  pendingPongBackpressureRebaseUsed_ = false;
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
