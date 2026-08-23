import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "WssKeepAlive.h"
#include "WssIngress.h"
#include "inkloop/myai/esp_wss_transport.hpp"

using namespace inkloop::myai;

Status append(WssIngressAssembler& parser, uint8_t opcode, bool fin,
              size_t total, size_t offset, const uint8_t* data, size_t length,
              bool& complete, WssCompletedMessage& output) {
  WssIngressChunk chunk;
  chunk.opcode = opcode;
  chunk.finalFrame = fin;
  chunk.framePayloadBytes = total;
  chunk.frameOffset = offset;
  chunk.bytes = data;
  chunk.length = length;
  return parser.append(chunk, complete, output);
}

int main() {
  assert(detail::remainingWssReadTimeoutMs(100, 100) == 0);
  assert(detail::remainingWssReadTimeoutMs(100, 101) == 1);
  assert(detail::remainingWssReadTimeoutMs(100, 110) == 10);

  WssKeepAlive keep_alive;
  assert(!keep_alive.pingDue(0));
  keep_alive.start(1000);
  assert(!keep_alive.pingDue(20999));
  assert(keep_alive.pingDue(21000));
  const uint32_t first_ping = keep_alive.nextPingToken();
  assert(first_ping != 0);
  keep_alive.notePingSent(21000, first_ping);
  assert(keep_alive.awaitingPong());
  assert(!keep_alive.pingDue(41000));
  assert(!keep_alive.pongTimedOut(40999));
  assert(!keep_alive.notePong(first_ping + 1));
  assert(keep_alive.awaitingPong());
  assert(keep_alive.pongTimedOut(41000));
  assert(keep_alive.notePong(first_ping));
  assert(!keep_alive.awaitingPong());
  assert(!keep_alive.pingDue(40999));
  assert(keep_alive.pingDue(41000));
  // A monotonic-clock rollback is treated as due instead of suppressing
  // liveness forever.
  assert(keep_alive.pingDue(20000));
  keep_alive.stop();
  assert(!keep_alive.pingDue(999999));

  WssKeepAlive rebased;
  rebased.start(0);
  const uint32_t rebased_token = rebased.nextPingToken();
  rebased.notePingSent(0, rebased_token);
  rebased.rebase(5000);
  assert(rebased.awaitingPong());
  assert(!rebased.pongTimedOut(24999));
  assert(rebased.pongTimedOut(25000));
  assert(rebased.notePong(rebased_token));
  assert(rebased.nextPingToken() != rebased_token);

  uint16_t close_code = 0;
  assert(detail::decodeValidWssClosePayload(nullptr, 0, close_code));
  assert(close_code == 1000U);
  const uint8_t normal_close[] = {
      0x03, 0xe8, 'o', 'k', ' ', 0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x98, 0x80};
  assert(detail::decodeValidWssClosePayload(
      normal_close, sizeof(normal_close), close_code));
  assert(close_code == 1000U);
  const uint8_t private_close[] = {0x0f, 0xa0};
  assert(detail::decodeValidWssClosePayload(
      private_close, sizeof(private_close), close_code));
  assert(close_code == 4000U);

  const uint8_t one_byte_close[] = {0x03};
  const uint8_t forbidden_1005[] = {0x03, 0xed};
  const uint8_t extension_2000[] = {0x07, 0xd0};
  const uint8_t overlong_utf8[] = {0x03, 0xe8, 0xc0, 0x80};
  const uint8_t surrogate_utf8[] = {0x03, 0xe8, 0xed, 0xa0, 0x80};
  const uint8_t too_large_utf8[] = {0x03, 0xe8, 0xf4, 0x90, 0x80, 0x80};
  const uint8_t truncated_utf8[] = {0x03, 0xe8, 0xe4, 0xb8};
  assert(!detail::decodeValidWssClosePayload(
      one_byte_close, sizeof(one_byte_close), close_code));
  assert(!detail::decodeValidWssClosePayload(
      forbidden_1005, sizeof(forbidden_1005), close_code));
  assert(!detail::decodeValidWssClosePayload(
      extension_2000, sizeof(extension_2000), close_code));
  assert(!detail::decodeValidWssClosePayload(
      overlong_utf8, sizeof(overlong_utf8), close_code));
  assert(!detail::decodeValidWssClosePayload(
      surrogate_utf8, sizeof(surrogate_utf8), close_code));
  assert(!detail::decodeValidWssClosePayload(
      too_large_utf8, sizeof(too_large_utf8), close_code));
  assert(!detail::decodeValidWssClosePayload(
      truncated_utf8, sizeof(truncated_utf8), close_code));
  assert(!detail::decodeValidWssClosePayload(nullptr, 2U, close_code));

  WssIngressAssembler parser;
  bool complete = false;
  WssCompletedMessage output;
  const uint8_t hello[] = {'h','e','l','l','o'};
  assert(append(parser, 1, true, 5, 0, hello, 2, complete, output).ok());
  assert(!complete);
  assert(append(parser, 1, true, 5, 2, hello + 2, 3, complete, output).ok());
  assert(complete && output.kind == WssMessageKind::Text && output.length == 5);
  assert(std::memcmp(output.bytes, hello, 5) == 0);

  const uint8_t first[] = {'a','s','r'};
  const uint8_t second[] = {'.','f','i','n','a','l'};
  assert(append(parser, 1, false, 3, 0, first, 3, complete, output).ok());
  assert(!complete);
  assert(append(parser, 0, true, 6, 0, second, 1, complete, output).ok());
  assert(!complete);
  assert(append(parser, 0, true, 6, 1, second + 1, 5, complete, output).ok());
  assert(complete && output.kind == WssMessageKind::Text && output.length == 9);
  assert(std::string(reinterpret_cast<const char*>(output.bytes), output.length) ==
         "asr.final");

  std::vector<uint8_t> audio(WssIngressAssembler::kMaximumMessageBytes, 0x5a);
  for (size_t offset = 0; offset < audio.size(); offset += 997) {
    const size_t count = std::min<size_t>(997, audio.size() - offset);
    assert(append(parser, 2, true, audio.size(), offset, audio.data() + offset,
                  count, complete, output).ok());
  }
  assert(complete && output.kind == WssMessageKind::Binary &&
         output.length == audio.size());

  uint8_t byte = 1;
  assert(append(parser, 2, true, audio.size() + 1, 0, &byte, 1,
                complete, output).code == ErrorCode::TooLarge);
  assert(append(parser, 0, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);
  assert(append(parser, 9, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);

  assert(append(parser, 1, true, 2, 0, &byte, 1, complete, output).ok());
  assert(append(parser, 1, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);
  assert(append(parser, 1, true, 2, 0, &byte, 1, complete, output).ok());
  assert(append(parser, 1, true, 2, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);

  // A failure resets the parser and never exposes a partial message.
  assert(append(parser, 1, true, 1, 0, &byte, 1, complete, output).ok());
  assert(complete && output.length == 1);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-wss-ingress-"));
  try {
    const source = join(scratch, "wss.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"), source,
      "-I", join(native, "include"),
      join(myai, "WssIngress.cpp"), "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    }
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

const pollHarness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "esp_transport.h"
#include "esp_transport_ws.h"
#include "inkloop/myai/EndpointPolicy.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "inkloop/myai/esp_wss_transport.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace inkloop::myai;

struct FakeFrame {
  ws_transport_opcodes_t opcode;
  bool fin;
  std::vector<uint8_t> bytes;
  size_t payloadBytes = 0;
};

struct FakeSend {
  ws_transport_opcodes_t opcode;
  std::vector<uint8_t> bytes;
};

int64_t inkloop_test_time_us = 0;
esp_transport_item_t fake_websocket{};
std::deque<FakeFrame> fake_frames;
std::vector<FakeSend> fake_sends;
ws_transport_opcodes_t fake_last_opcode = WS_TRANSPORT_OPCODES_NONE;
bool fake_last_fin = true;
int fake_last_payload = 0;
size_t fake_poll_calls = 0;
size_t fake_read_calls = 0;

void resetFakeTransport() {
  fake_frames.clear();
  fake_sends.clear();
  fake_last_opcode = WS_TRANSPORT_OPCODES_NONE;
  fake_last_fin = true;
  fake_last_payload = 0;
  fake_poll_calls = 0;
  fake_read_calls = 0;
  inkloop_test_time_us = 0;
}

esp_transport_handle_t esp_transport_tcp_init() { return &fake_websocket; }
esp_transport_handle_t esp_transport_ssl_init() { return &fake_websocket; }
void esp_transport_ssl_crt_bundle_attach(
    esp_transport_handle_t, esp_err_t (*)(void*)) {}
void esp_transport_ssl_set_common_name(esp_transport_handle_t, const char*) {}
void esp_transport_set_default_port(esp_transport_handle_t, int) {}
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t) {
  return &fake_websocket;
}
esp_err_t esp_transport_ws_set_config(
    esp_transport_handle_t, const esp_transport_ws_config_t*) {
  return ESP_OK;
}
int esp_transport_connect(esp_transport_handle_t, const char*, int, int) {
  return 0;
}
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t) {
  return 101;
}
int esp_transport_get_socket(esp_transport_handle_t) { return 7; }
int esp_transport_close(esp_transport_handle_t) { return 0; }
void esp_transport_destroy(esp_transport_handle_t) {}

int esp_transport_poll_read(esp_transport_handle_t, int timeout_ms) {
  assert(timeout_ms == 0);
  ++fake_poll_calls;
  return fake_frames.empty() ? 0 : 1;
}

int esp_transport_read(esp_transport_handle_t, char* output, int capacity,
                       int timeout_ms) {
  assert(!fake_frames.empty());
  assert(timeout_ms > 0 && timeout_ms <= 10);
  const FakeFrame frame = fake_frames.front();
  fake_frames.pop_front();
  assert(frame.bytes.size() <= static_cast<size_t>(capacity));
  for (size_t index = 0; index < frame.bytes.size(); ++index) {
    output[index] = static_cast<char>(frame.bytes[index]);
  }
  fake_last_opcode = frame.opcode;
  fake_last_fin = frame.fin;
  fake_last_payload = static_cast<int>(
      frame.payloadBytes == 0U ? frame.bytes.size() : frame.payloadBytes);
  ++fake_read_calls;
  return static_cast<int>(frame.bytes.size());
}

ws_transport_opcodes_t esp_transport_ws_get_read_opcode(
    esp_transport_handle_t) {
  return fake_last_opcode;
}
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t) {
  return fake_last_fin;
}
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t) {
  return fake_last_payload;
}
int esp_transport_ws_send_raw(esp_transport_handle_t,
                              ws_transport_opcodes_t opcode,
                              const char* bytes, int length, int) {
  assert(length >= 0);
  FakeSend sent{opcode, {}};
  if (length != 0) {
    assert(bytes != nullptr);
    sent.bytes.assign(reinterpret_cast<const uint8_t*>(bytes),
                      reinterpret_cast<const uint8_t*>(bytes) + length);
  }
  fake_sends.push_back(sent);
  return length;
}

Status EndpointPolicy::parsePublicUrl(const std::string&, bool,
                                      HttpsEndpoint& endpoint) {
  endpoint.host = "example.com";
  endpoint.port = 443;
  endpoint.tls = true;
  return Status::success();
}
Status EspEndpointSecurity::validatePublicTlsEndpoint(const std::string&) {
  return Status::success();
}
Status EspEndpointSecurity::validatePublicEndpoint(const std::string&) {
  return Status::success();
}
Status EspEndpointSecurity::validateConnectedSocket(int) const {
  return Status::success();
}

class Listener final : public IWebSocketListener {
 public:
  void onWebSocketOpen() override { ++opens; }
  void onWebSocketText(const std::string&) override { ++texts; }
  void onWebSocketBinary(const uint8_t*, size_t) override {
    ++binaries;
    if (closeOnBinary) transport->close(1000, "listener_done");
  }
  void onWebSocketClosed(int code, const std::string& reason) override {
    closeCode = code;
    closeReason = reason;
  }

  int opens = 0;
  int texts = 0;
  int binaries = 0;
  int closeCode = 0;
  std::string closeReason;
  EspWssTransport* transport = nullptr;
  bool closeOnBinary = false;
};

struct Gate { bool ready = true; };

bool ingressReady(void* context) {
  return static_cast<Gate*>(context)->ready;
}

void activate(EspWssTransport& transport, Listener& listener, Gate& gate) {
  transport.websocket_ = &fake_websocket;
  transport.listener_ = &listener;
  transport.connected_ = true;
  transport.setIngressReadyGate(&ingressReady, &gate);
  transport.keep_alive_.start(0);
}

std::vector<uint8_t> tokenBytes(uint32_t token) {
  return {static_cast<uint8_t>(token >> 24U),
          static_cast<uint8_t>(token >> 16U),
          static_cast<uint8_t>(token >> 8U),
          static_cast<uint8_t>(token)};
}

void testContinuousIngressServicesKeepAlive() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate;
  EspWssTransport transport(security);
  activate(transport, listener, gate);

  inkloop_test_time_us = 20000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x11}});
  assert(transport.pollIngress().ok());
  assert(fake_read_calls == 1U);
  assert(listener.binaries == 1);
  assert(fake_sends.size() == 1U);
  assert(fake_sends[0].opcode ==
         (WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_PING));

  // Another readable data frame at the exact Pong deadline gets one read
  // opportunity, then timeout enforcement wins; data cannot postpone it.
  inkloop_test_time_us = 40000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x22}});
  const Status timed_out = transport.pollIngress();
  assert(timed_out.code == ErrorCode::Transport);
  assert(fake_read_calls == 2U);
  assert(listener.binaries == 1);
  assert(listener.closeCode == 1006);
  assert(listener.closeReason == "keepalive_pong_timeout");
}

void testDeferredBackpressurePreservesQueuedPong() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate{false};
  EspWssTransport transport(security);
  activate(transport, listener, gate);
  const uint32_t token = transport.keep_alive_.nextPingToken();
  transport.keep_alive_.notePingSent(0, token);

  inkloop_test_time_us = 20000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x33}});
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_PONG, true, tokenBytes(token)});
  assert(transport.pollIngress().ok());
  assert(transport.deferredDataPending_);
  assert(fake_read_calls == 1U);
  assert(listener.binaries == 0);
  assert(fake_sends.empty());
  assert(listener.closeCode == 0);

  inkloop_test_time_us = 22000LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(fake_read_calls == 1U);
  gate.ready = true;
  assert(transport.pollIngress().ok());
  assert(fake_read_calls == 1U);
  assert(listener.binaries == 1);
  assert(transport.postBackpressureControlGrace_);
  assert(transport.pollIngress().ok());
  assert(fake_read_calls == 2U);
  assert(!transport.keep_alive_.awaitingPong());
  assert(listener.closeCode == 0);
  assert(fake_sends.empty());
}

void testPartialBackpressurePreservesQueuedPong() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener partial_listener;
  Gate gate{true};
  EspWssTransport partial(security);
  activate(partial, partial_listener, gate);
  const uint32_t token = partial.keep_alive_.nextPingToken();
  partial.keep_alive_.notePingSent(0, token);

  inkloop_test_time_us = 1000LL * 1000LL;
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_BINARY, true, {0x41}, 3U});
  assert(partial.pollIngress().ok());
  assert(partial.currentFrameOffset_ == 1U);

  gate.ready = false;
  inkloop_test_time_us = 20000LL * 1000LL;
  assert(partial.pollIngress().ok());
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_BINARY, true, {0x42, 0x43}, 3U});
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_PONG, true, tokenBytes(token)});

  gate.ready = true;
  inkloop_test_time_us = 21000LL * 1000LL;
  assert(partial.pollIngress().ok());
  assert(fake_read_calls == 2U);
  assert(partial.currentFrameOffset_ == 0U);
  assert(partial_listener.binaries == 1);
  assert(partial.postBackpressureControlGrace_);
  assert(partial.pollIngress().ok());
  assert(fake_read_calls == 3U);
  assert(!partial.keep_alive_.awaitingPong());
  assert(partial_listener.closeCode == 0);
  assert(fake_sends.empty());
}

void testPermanentBackpressureUsesIndependentWatchdog() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate{false};
  EspWssTransport transport(security);
  activate(transport, listener, gate);

  inkloop_test_time_us = 20000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x51}});
  assert(transport.pollIngress().ok());
  assert(fake_sends.empty());
  inkloop_test_time_us = 24999LL * 1000LL;
  assert(transport.pollIngress().ok());
  inkloop_test_time_us = 25000LL * 1000LL;
  const Status timed_out = transport.pollIngress();
  assert(timed_out.code == ErrorCode::Transport);
  assert(timed_out.detail == "MyAI WSS ingress backpressure timed out");
  assert(listener.closeCode == 1006);
  assert(listener.closeReason == "ingress_backpressure_timeout");
  assert(fake_sends.empty());
}

void testRepeatedBackpressureDoesNotRebaseSamePong() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate{false};
  EspWssTransport transport(security);
  activate(transport, listener, gate);
  const uint32_t token = transport.keep_alive_.nextPingToken();
  transport.keep_alive_.notePingSent(0, token);

  inkloop_test_time_us = 20000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x61}});
  assert(transport.pollIngress().ok());
  gate.ready = true;
  inkloop_test_time_us = 22000LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(transport.postBackpressureControlGrace_);
  assert(transport.pollIngress().ok());
  assert(transport.pendingPongBackpressureRebaseUsed_);

  gate.ready = false;
  inkloop_test_time_us = 40000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x62}});
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_PONG, true, tokenBytes(token)});
  assert(transport.pollIngress().ok());
  assert(transport.postBackpressureControlGrace_);
  gate.ready = true;
  inkloop_test_time_us = 41000LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(transport.postBackpressureControlGrace_);
  assert(transport.keep_alive_.awaitingPong());
  assert(transport.keep_alive_.last_ping_ms_ == 22000ULL);

  inkloop_test_time_us = 41999LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(!transport.keep_alive_.awaitingPong());
  assert(transport.keep_alive_.last_ping_ms_ == 22000ULL);
  assert(listener.closeCode == 0);
  assert(fake_sends.empty());
}

void testDataGraceCannotRenewWithinSameEpisode() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate{false};
  EspWssTransport transport(security);
  activate(transport, listener, gate);
  const uint32_t token = transport.keep_alive_.nextPingToken();
  transport.keep_alive_.notePingSent(0, token);

  inkloop_test_time_us = 1000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x70}});
  assert(transport.pollIngress().ok());
  gate.ready = true;
  inkloop_test_time_us = 2000LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(transport.postBackpressureControlGrace_);

  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true,
                         std::vector<uint8_t>(2048U, 0x71), 4096U});
  inkloop_test_time_us = 3000LL * 1000LL;
  assert(transport.pollIngress().ok());
  assert(transport.currentFrameOffset_ == 2048U);
  assert(transport.episodeControlGraceUsed_);
  assert(!transport.postBackpressureControlGrace_);

  gate.ready = false;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true,
                         std::vector<uint8_t>(2048U, 0x72), 4096U});
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_PONG, true, tokenBytes(token)});
  assert(transport.pollIngress().ok());
  assert(!transport.postBackpressureControlGrace_);
  assert(fake_read_calls == 2U);

  gate.ready = true;
  inkloop_test_time_us = 22000LL * 1000LL;
  assert(transport.pollIngress().code == ErrorCode::Transport);
  assert(listener.closeReason == "keepalive_pong_timeout");
  assert(fake_read_calls == 3U);
  assert(fake_frames.size() == 1U);
}

void testDeadlinePongWinsOneBoundedRead() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate{false};
  EspWssTransport transport(security);
  activate(transport, listener, gate);
  const uint32_t token = transport.keep_alive_.nextPingToken();
  transport.keep_alive_.notePingSent(0, token);

  inkloop_test_time_us = 20000LL * 1000LL;
  fake_frames.push_back(
      {WS_TRANSPORT_OPCODES_PONG, true, tokenBytes(token)});
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x44}});
  assert(transport.pollIngress().ok());
  assert(listener.closeCode == 0);
  assert(fake_read_calls == 1U);
  assert(fake_frames.size() == 1U);
  // Acknowledging the deadline Pong allows the next scheduled Ping instead
  // of synthesizing a timeout. The data frame remains for the next turn.
  assert(fake_sends.size() == 1U);
  assert(fake_sends[0].opcode ==
         (WS_TRANSPORT_OPCODES_FIN | WS_TRANSPORT_OPCODES_PING));

  assert(transport.pollIngress().ok());
  assert(fake_read_calls == 2U);
  assert(transport.deferredDataPending_);
}

void testListenerCloseRemainsSuccessful() {
  resetFakeTransport();
  EspEndpointSecurity security;
  Listener listener;
  Gate gate;
  EspWssTransport transport(security);
  activate(transport, listener, gate);
  listener.transport = &transport;
  listener.closeOnBinary = true;

  inkloop_test_time_us = 1000LL * 1000LL;
  fake_frames.push_back({WS_TRANSPORT_OPCODES_BINARY, true, {0x55}});
  assert(transport.pollIngress().ok());
  assert(listener.binaries == 1);
  assert(!transport.connected());
}

int main() {
  testContinuousIngressServicesKeepAlive();
  testDeferredBackpressurePreservesQueuedPong();
  testPartialBackpressurePreservesQueuedPong();
  testPermanentBackpressureUsesIndependentWatchdog();
  testRepeatedBackpressureDoesNotRebaseSamePong();
  testDataGraceCannotRenewWithinSameEpisode();
  testDeadlinePongWinsOneBoundedRead();
  testListenerCloseRemainsSuccessful();
  return 0;
}
`;

function buildAndRunPollHarness() {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-wss-poll-"));
  try {
    const stubs = join(scratch, "stubs");
    const source = join(scratch, "poll.cpp");
    const binary = join(scratch, "poll");
    const stubFiles = new Map([
      ["esp_err.h", String.raw`
#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
`],
      ["esp_crt_bundle.h", String.raw`
#pragma once
#include "esp_err.h"
inline esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
`],
      ["esp_transport.h", String.raw`
#pragma once
struct esp_transport_item_t {};
using esp_transport_handle_t = esp_transport_item_t*;
int esp_transport_connect(esp_transport_handle_t, const char*, int, int);
int esp_transport_get_socket(esp_transport_handle_t);
void esp_transport_set_default_port(esp_transport_handle_t, int);
int esp_transport_close(esp_transport_handle_t);
void esp_transport_destroy(esp_transport_handle_t);
int esp_transport_poll_read(esp_transport_handle_t, int);
int esp_transport_read(esp_transport_handle_t, char*, int, int);
`],
      ["esp_transport_tcp.h", String.raw`
#pragma once
#include "esp_transport.h"
esp_transport_handle_t esp_transport_tcp_init();
`],
      ["esp_transport_ssl.h", String.raw`
#pragma once
#include "esp_err.h"
#include "esp_transport.h"
esp_transport_handle_t esp_transport_ssl_init();
void esp_transport_ssl_crt_bundle_attach(
    esp_transport_handle_t, esp_err_t (*)(void*));
void esp_transport_ssl_set_common_name(esp_transport_handle_t, const char*);
`],
      ["esp_transport_ws.h", String.raw`
#pragma once
#include <cstddef>
#include "esp_err.h"
#include "esp_transport.h"
enum ws_transport_opcodes_t {
  WS_TRANSPORT_OPCODES_CONT = 0x00,
  WS_TRANSPORT_OPCODES_TEXT = 0x01,
  WS_TRANSPORT_OPCODES_BINARY = 0x02,
  WS_TRANSPORT_OPCODES_CLOSE = 0x08,
  WS_TRANSPORT_OPCODES_PING = 0x09,
  WS_TRANSPORT_OPCODES_PONG = 0x0a,
  WS_TRANSPORT_OPCODES_FIN = 0x80,
  WS_TRANSPORT_OPCODES_NONE = 0x100,
};
struct esp_transport_ws_config_t {
  const char* ws_path = nullptr;
  const char* sub_protocol = nullptr;
  const char* user_agent = nullptr;
  const char* headers = nullptr;
  void* header_hook = nullptr;
  void* header_user_context = nullptr;
  const char* auth = nullptr;
  char* response_headers = nullptr;
  size_t response_headers_len = 0;
  bool propagate_control_frames = false;
};
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t);
esp_err_t esp_transport_ws_set_config(
    esp_transport_handle_t, const esp_transport_ws_config_t*);
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t);
int esp_transport_ws_send_raw(esp_transport_handle_t,
                              ws_transport_opcodes_t, const char*, int, int);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(
    esp_transport_handle_t);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t);
`],
      ["esp_timer.h", String.raw`
#pragma once
#include <cstdint>
extern int64_t inkloop_test_time_us;
inline int64_t esp_timer_get_time() { return inkloop_test_time_us; }
`],
      ["freertos/task.h", String.raw`
#pragma once
#include <cstdint>
inline void vTaskDelay(uint32_t) {}
`],
    ]);
    for (const [relative, contents] of stubFiles) {
      const target = join(stubs, relative);
      mkdirSync(target.slice(0, target.lastIndexOf("/")), { recursive: true });
      writeFileSync(target, contents);
    }
    writeFileSync(source, pollHarness);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1",
      "-DCONFIG_LOG_MAXIMUM_LEVEL=3",
      "-I", stubs,
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      "-I", join(native, "include"),
      source,
      join(native, "esp_wss_transport.cpp"),
      join(myai, "WssIngress.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("WSS ingress is bounded and fragment-safe under strict C++17", () => {
  buildAndRun(false);
});

test("WSS ingress survives adversarial fragmentation under ASan and UBSan", () => {
  buildAndRun(true);
});

test("WSS poll services keepalive under continuous ingress and backpressure", () => {
  buildAndRunPollHarness();
});

test("native WS/WSS adapter rejects redirects and checks the connected public peer", () => {
  const source = readFileSync(join(native, "esp_wss_transport.cpp"), "utf8");
  const cmake = readFileSync(join(native, "CMakeLists.txt"), "utf8");
  const defaults = readFileSync(join(repo, "firmware/inkloop-idf/sdkconfig.defaults"), "utf8");
  assert.match(source, /endpoint\.tls \? esp_transport_ssl_init\(\) : esp_transport_tcp_init\(\)/);
  assert.match(source, /esp_transport_ssl_crt_bundle_attach\(network_, esp_crt_bundle_attach\)/);
  assert.match(source, /esp_transport_ssl_set_common_name\(network_, host_\.c_str\(\)\)/);
  assert.match(source, /"ws:\/\/"/);
  assert.match(source, /result != 0[\s\S]+redirect[\s\S]+Security/);
  assert.match(source, /get_upgrade_request_status\(websocket_\) != 101/);
  assert.match(source, /validateConnectedSocket[\s\S]+esp_transport_get_socket/);
  assert.match(source, /EspNetworkOperationLease network_lease\(kConnectTimeoutMs\)/);
  assert.match(source, /network_lease\.acquired\(\)/);
  assert.match(source, /esp_transport_poll_read\(websocket_, 0\)/);
  assert.match(source, /std::array<uint8_t, 2048> chunk/);
  assert.match(source, /remainingWssReadTimeoutMs[\s\S]+read_timeout_ms/);
  assert.match(source, /esp_transport_read\([\s\S]+read_timeout_ms\)/);
  assert.doesNotMatch(source, /esp_transport_read\([\s\S]{0,180}, 0\)/);
  assert.match(source, /currentFrameOffset_/);
  assert.match(source, /WS_TRANSPORT_OPCODES_FIN \| WS_TRANSPORT_OPCODES_PING/);
  assert.match(source, /WS_TRANSPORT_OPCODES_FIN \| WS_TRANSPORT_OPCODES_PONG/);
  assert.match(source, /keep_alive_\.pingDue\(now_ms\)/);
  assert.match(source, /keep_alive_\.pongTimedOut\(now_ms\)/);
  assert.match(source, /keep_alive_\.notePingSent\(now_ms, token\)/);
  assert.match(source, /keep_alive_\.notePong\(decodePingToken/);
  assert.match(source, /decodeValidWssClosePayload/);
  assert.match(source, /invalid_close_frame/);
  assert.match(source, /config\.propagate_control_frames = true/);
  const socketPoll = source.indexOf("esp_transport_poll_read(websocket_, 0)");
  const controlDispatch = source.indexOf(
    "const Status control = handleControlFrame(", socketPoll);
  const dataGate = source.indexOf("const bool data_ready", controlDispatch);
  const dataKeepAlive = source.indexOf(
    "const Status keep_alive = serviceKeepAliveAfterIngress()", dataGate);
  assert.ok(socketPoll >= 0 && controlDispatch > socketPoll &&
            dataGate > controlDispatch && dataKeepAlive > dataGate);
  assert.match(source, /ingress_backpressure_timeout/);
  assert.match(source, /keep_alive_\.rebase\(monotonicMs\(\)\)/);
  assert.match(source, /CONFIG_LOG_MAXIMUM_LEVEL > 3/);
  assert.match(cmake, /tcp_transport/);
  assert.match(defaults, /CONFIG_WS_BUFFER_SIZE=4096/);
  assert.doesNotMatch(source, /esp_websocket_client|xTaskCreate|std::thread/);
  assert.doesNotMatch(source, /ESP_LOG|printf\s*\(|puts\s*\(/);
});
