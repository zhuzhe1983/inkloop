import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");

const harness = String.raw`
#include <array>
#include <cassert>
#include <string>

#include "EndpointPolicy.h"

using namespace inkloop::myai;

bool accepts(const std::string& url) {
  HttpsEndpoint endpoint;
  return EndpointPolicy::parseHttpsUrl(url, endpoint).ok();
}

int main() {
  HttpsEndpoint endpoint;
  assert(EndpointPolicy::parseHttpsUrl("https://myai.mess.host/healthz", endpoint).ok());
  assert(endpoint.host == "myai.mess.host" && endpoint.port == 443 && endpoint.tls);
  assert(EndpointPolicy::parsePublicUrl(
      "http://183.128.44.67:18090/gateway/v1/gateway-ping", true, endpoint).ok());
  assert(endpoint.host == "183.128.44.67" && endpoint.port == 18090 && !endpoint.tls);
  assert(!EndpointPolicy::parsePublicUrl(
      "http://183.128.44.67:18090/gateway/v1/gateway-ping", false, endpoint).ok());
  assert(!EndpointPolicy::parsePublicUrl("http://192.168.1.8/ping", true, endpoint).ok());
  assert(EndpointPolicy::parseHttpsUrl("https://Gateway.Example.COM.:8443/ping?x=1", endpoint).ok());
  assert(endpoint.host == "gateway.example.com" && endpoint.port == 8443);
  assert(accepts("https://1.1.1.1/path"));
  assert(accepts("https://[2606:4700:4700::1111]:443/ping"));

  const char* rejected[] = {
      "", "http://myai.mess.host", "HTTPS://myai.mess.host",
      "https://user:secret@myai.mess.host/", "https://myai.mess.host/#fragment",
      "https://myai.mess.host/ bad", "https://myai.mess.host\r\nX:1",
      "https://myai.mess.host:0/", "https://myai.mess.host:65536/",
      "https://myai.mess.host:notaport/", "https://localhost/",
      "https://inkloop.local/", "https://gateway.internal/", "https://x.home.arpa/",
      "https://gateway.example/", "https://gateway.test/", "https://-bad.example.com/",
      "https://bad-.example.com/", "https://10.0.0.1/", "https://100.64.0.1/",
      "https://127.0.0.1/", "https://169.254.1.1/", "https://172.31.0.1/",
      "https://192.168.1.1/", "https://192.0.2.1/", "https://198.18.0.1/",
      "https://198.51.100.1/", "https://203.0.113.1/", "https://224.0.0.1/",
      "https://999.1.1.1/", "https://1.2.3/", "https://[::1]/",
      "https://[fc00::1]/", "https://[fe80::1]/", "https://[ff02::1]/",
      "https://[2001:db8::1]/", "https://[2002:0808:0808::1]/",
      "https://[::ffff:8.8.8.8]/", "https://2001:4860:4860::8888/"};
  for (const char* url : rejected) assert(!accepts(url));

  assert(EndpointPolicy::isPublicIpv4({8, 8, 8, 8}));
  assert(!EndpointPolicy::isPublicIpv4({0, 0, 0, 0}));
  assert(!EndpointPolicy::isPublicIpv4({255, 255, 255, 255}));
  assert(EndpointPolicy::isPublicIpv6(
      {0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11}));
  assert(!EndpointPolicy::isPublicIpv6(
      {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-endpoint-policy-"));
  try {
    const source = join(scratch, "endpoint_policy.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(myai, "include/inkloop/myai"), source,
      join(myai, "EndpointPolicy.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
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

test("MyAI endpoint policy rejects SSRF and malformed HTTPS targets", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("native MyAI transport isolates TLS Center and public short-token Gateway policy", () => {
  const source = readFileSync(join(
    repo,
    "firmware/inkloop-idf/components/inkloop_myai_idf/esp_http_adapters.cpp",
  ), "utf8");
  assert.match(source, /CONFIG_MBEDTLS_CERTIFICATE_BUNDLE/);
  assert.match(source, /crt_bundle_attach = endpoint\.tls \? esp_crt_bundle_attach : nullptr/);
  assert.match(source, /plaintextPublicGatewayAllowed/);
  assert.match(source, /skip_cert_common_name_check = false/);
  assert.match(source, /disable_auto_redirect = true/);
  assert.match(source, /HTTP_EVENT_ON_CONNECTED[\s\S]+getpeername[\s\S]+publicSockaddr/);
  assert.match(source, /peerRejected[\s\S]+shutDownClientSocket/);
  assert.match(source, /kMaximumResponseBodyBytes/);
  assert.match(source, /context\.responseTooLarge/);
  assert.match(source, /esp_http_client_is_complete_data_received/);
  assert.match(source, /config\.is_async = true/);
  assert.match(source, /while \(pending > 0\)/);
  assert.match(source, /ESP_ERR_HTTP_EAGAIN/);
  assert.match(source, /boundedLatency\(startedMs, beforePass\) >= totalDeadlineMs/);
  assert.match(source, /results\.reserve\(candidates\.size\(\)\)/);
  assert.match(source, /results\.push_back\(slot\.result\)/);
  assert.match(source, /headers\.size\(\) != 1U/);
  assert.match(source, /"X-Gateway-ID"[\s\S]*candidates\[index\]\.id/);
  assert.doesNotMatch(source, /xTaskCreate|std::thread|HTTP_METHOD_GET[^\n]+ping/);
  assert.doesNotMatch(source, /ESP_LOG.|\bprintf\s*\(|\bputs\s*\(/);
  assert.doesNotMatch(source, /skip_cert_common_name_check\s*=\s*true|redirectsAllowed\s*=\s*true/);
});

const fakeEspHeader = String.raw`
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_HEAD } esp_http_client_method_t;
typedef enum {
  HTTP_EVENT_ERROR, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT,
  HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH,
  HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT
} esp_http_client_event_id_t;
struct FakeEspHttpClient;
typedef FakeEspHttpClient* esp_http_client_handle_t;
typedef struct esp_http_client_event {
  esp_http_client_event_id_t event_id;
  esp_http_client_handle_t client;
  void* data;
  int data_len;
  void* user_data;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t*);
typedef struct {
  const char* url;
  esp_http_client_method_t method;
  int timeout_ms;
  bool disable_auto_redirect;
  int max_redirection_count;
  int max_authorization_retries;
  http_event_handle_cb event_handler;
  void* user_data;
  bool is_async;
  int buffer_size;
  int buffer_size_tx;
  bool skip_cert_common_name_check;
  esp_err_t (*crt_bundle_attach)(void*);
  bool keep_alive_enable;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_get_socket(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
`;

const nativeHarness = String.raw`
#include <cassert>
#include <cstring>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "inkloop/myai/esp_http_adapters.hpp"
#include "esp_http_client.h"

using namespace inkloop::myai;

struct FakeEspHttpClient {
  std::string url;
  esp_http_client_config_t config{};
  int calls = 0;
  int status = 0;
  int headers = 0;
};

static int g_expected_clients = 0;
static int g_initialized_clients = 0;
static bool g_first_perform_saw_all = false;
static bool g_dns_private = false;
static bool g_dns_mixed = false;
static bool g_peer_private = false;
static bool g_socket_shutdown = false;
static bool g_wire_headers_sent = false;
static bool g_complete_response = true;
static std::string g_response_body = "{}";

extern "C" int fake_getaddrinfo(const char*, const char*, const addrinfo*,
                                 addrinfo** output) {
  static addrinfo records[2];
  static sockaddr_in addresses[2];
  std::memset(records, 0, sizeof(records));
  std::memset(addresses, 0, sizeof(addresses));
  const int count = g_dns_mixed ? 2 : 1;
  for (int index = 0; index < count; ++index) {
    addresses[index].sin_family = AF_INET;
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&addresses[index].sin_addr.s_addr);
    const bool privateAddress = g_dns_private || (g_dns_mixed && index == 1);
    bytes[0] = privateAddress ? 127 : 8;
    bytes[1] = privateAddress ? 0 : 8;
    bytes[2] = privateAddress ? 0 : 8;
    bytes[3] = privateAddress ? 1 : 8;
    records[index].ai_family = AF_INET;
    records[index].ai_socktype = SOCK_STREAM;
    records[index].ai_addr = reinterpret_cast<sockaddr*>(&addresses[index]);
    records[index].ai_addrlen = sizeof(sockaddr_in);
    records[index].ai_next = index + 1 < count ? &records[index + 1] : nullptr;
  }
  *output = &records[0];
  return 0;
}
extern "C" void fake_freeaddrinfo(addrinfo*) {}
extern "C" int fake_getpeername(int, sockaddr* output, socklen_t* length) {
  if (!output || !length || *length < sizeof(sockaddr_in)) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  unsigned char* bytes = reinterpret_cast<unsigned char*>(&address.sin_addr.s_addr);
  bytes[0] = g_peer_private ? 192 : 8;
  bytes[1] = g_peer_private ? 168 : 8;
  bytes[2] = g_peer_private ? 1 : 4;
  bytes[3] = g_peer_private ? 1 : 4;
  std::memcpy(output, &address, sizeof(address));
  *length = sizeof(address);
  return 0;
}
extern "C" int fake_shutdown(int, int) {
  g_socket_shutdown = true;
  return 0;
}

extern "C" esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
extern "C" int64_t esp_timer_get_time(void) { return 1000000; }
extern "C" void vTaskDelay(unsigned int) {}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
  assert(config && config->url);
  if (config->is_async) assert(config->method == HTTP_METHOD_HEAD);
  else assert(config->method == HTTP_METHOD_GET || config->method == HTTP_METHOD_POST);
  assert(config->disable_auto_redirect && !config->skip_cert_common_name_check);
  assert(config->crt_bundle_attach == esp_crt_bundle_attach);
  auto* client = new FakeEspHttpClient();
  client->url = config->url;
  client->config = *config;
  if (config->is_async) ++g_initialized_clients;
  return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char* key, const char* value) {
  assert(client && key && value);
  ++client->headers;
  return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int) {
  return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int timeout) {
  assert(timeout > 0);
  return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
  assert(client);
  if (!client->config.is_async) {
    g_socket_shutdown = false;
    esp_http_client_event_t connected{};
    connected.event_id = HTTP_EVENT_ON_CONNECTED;
    connected.client = client;
    connected.user_data = client->config.user_data;
    assert(client->config.event_handler(&connected) == ESP_OK);
    if (g_socket_shutdown) return ESP_FAIL;
    g_wire_headers_sent = true;
    if (!g_response_body.empty()) {
      esp_http_client_event_t data{};
      data.event_id = HTTP_EVENT_ON_DATA;
      data.client = client;
      data.user_data = client->config.user_data;
      data.data = g_response_body.data();
      data.data_len = static_cast<int>(g_response_body.size());
      assert(client->config.event_handler(&data) == ESP_OK);
      if (g_socket_shutdown) return ESP_FAIL;
    }
    client->status = 200;
    return ESP_OK;
  }
  if (!g_first_perform_saw_all) {
    assert(g_initialized_clients == g_expected_clients);
    g_first_perform_saw_all = true;
  }
  ++client->calls;
  if (client->url.find("timeout") != std::string::npos) return ESP_ERR_HTTP_EAGAIN;
  if (client->url.find("fast") != std::string::npos && client->calls >= 2) {
    client->status = 204;
    return ESP_OK;
  }
  if (client->url.find("slow") != std::string::npos && client->calls >= 4) {
    client->status = 200;
    return ESP_OK;
  }
  if (client->url.find("offline") != std::string::npos && client->calls >= 3) {
    client->status = 503;
    return ESP_FAIL;
  }
  return ESP_ERR_HTTP_EAGAIN;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) {
  return client ? client->status : 0;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) {
  return g_complete_response;
}
int esp_http_client_get_socket(esp_http_client_handle_t) { return 42; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
  delete client;
  return ESP_OK;
}

struct Clock final : IClock {
  mutable uint64_t now = 0;
  bool ready = true;
  uint64_t monotonicMs() const override { return ++now; }
  std::string utcIso8601() const override {
    return ready ? "2026-08-22T04:30:00Z" : std::string();
  }
};

GatewayCandidate gateway(const char* id) {
  GatewayCandidate value;
  value.id = id;
  value.baseUrl = std::string("https://") + id + ".example.com";
  value.pingUrl = value.baseUrl + "/ping";
  return value;
}

int main() {
  EspEndpointSecurity security;
  EspHttpTransport http(security);
  HttpRequest request;
  request.method = "GET";
  request.url = "https://myai.mess.host/healthz";
  request.headers["Authorization"] = "Bearer opaque";
  HttpResponse response;
  g_dns_private = false;
  g_dns_mixed = false;
  g_peer_private = false;
  g_wire_headers_sent = false;
  g_response_body = "{\"ok\":true}";
  assert(http.perform(request, response).ok());
  assert(response.status == 200 && response.body == g_response_body);
  assert(g_wire_headers_sent);

  g_peer_private = true;
  g_wire_headers_sent = false;
  assert(http.perform(request, response).code == ErrorCode::Security);
  assert(!g_wire_headers_sent);
  g_peer_private = false;

  g_dns_private = true;
  assert(http.perform(request, response).code == ErrorCode::Security);
  g_dns_private = false;
  g_dns_mixed = true;
  assert(http.perform(request, response).code == ErrorCode::Security);
  g_dns_mixed = false;

  request.maxResponseBytes = 3;
  g_response_body = "four";
  assert(http.perform(request, response).code == ErrorCode::TooLarge);
  request.maxResponseBytes = 64;
  g_response_body = "ok";
  g_complete_response = false;
  assert(http.perform(request, response).code == ErrorCode::Transport);
  g_complete_response = true;
  request.headers["bad\r\n"] = "value";
  assert(http.perform(request, response).code == ErrorCode::InvalidArgument);
  request.headers.erase("bad\r\n");
  request.tlsPeerVerificationRequired = false;
  assert(http.perform(request, response).code == ErrorCode::Security);

  g_initialized_clients = 0;
  g_first_perform_saw_all = false;
  Clock clock;
  EspGatewayProbeSet probes(clock);
  std::map<std::string, std::string> headers{{"Authorization", "Bearer opaque"}};
  std::vector<GatewayCandidate> candidates{
      gateway("slow"), gateway("fast"), gateway("offline")};
  std::vector<GatewayProbe> results;
  g_expected_clients = 3;
  assert(probes.probeConcurrent(candidates, headers, 100, results).ok());
  assert(g_first_perform_saw_all && results.size() == candidates.size());
  assert(results[0].gatewayId == "slow" && results[0].ok);
  assert(results[1].gatewayId == "fast" && results[1].ok);
  assert(results[1].latencyMs < results[0].latencyMs);
  assert(results[2].gatewayId == "offline" && !results[2].ok);
  assert(results[2].error == "transport");
  for (const GatewayProbe& result : results) {
    assert(result.checkedAt == "2026-08-22T04:30:00Z");
  }

  headers["X-Device-ID"] = "must-not-cross-control-plane";
  assert(probes.probeConcurrent(candidates, headers, 100, results).code ==
         ErrorCode::Security);
  headers.erase("X-Device-ID");
  headers["X-Gateway-ID"] = "caller-must-not-override-candidate";
  assert(probes.probeConcurrent(candidates, headers, 100, results).code ==
         ErrorCode::Security);
  headers.erase("X-Gateway-ID");

  g_initialized_clients = 0;
  g_first_perform_saw_all = false;
  g_expected_clients = 2;
  candidates = {gateway("timeout-one"), gateway("timeout-two")};
  assert(probes.probeConcurrent(candidates, headers, 5, results).ok());
  assert(results.size() == 2);
  for (const GatewayProbe& result : results) {
    assert(!result.ok && result.error == "timeout" && result.latencyMs <= 5);
  }

  candidates = {gateway("fast")};
  candidates[0].pingUrl = "https://127.0.0.1/ping";
  assert(probes.probeConcurrent(candidates, headers, 100, results).code ==
         ErrorCode::Security);
  clock.ready = false;
  candidates[0] = gateway("fast");
  assert(probes.probeConcurrent(candidates, headers, 100, results).code ==
         ErrorCode::InvalidState);
  return 0;
}
`;

function buildAndRunNativeHarness(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-native-http-"));
  try {
    mkdirSync(join(scratch, "lwip"));
    mkdirSync(join(scratch, "freertos"));
    writeFileSync(join(scratch, "esp_http_client.h"), fakeEspHeader);
    writeFileSync(join(scratch, "esp_crt_bundle.h"),
      "#pragma once\n#ifdef __cplusplus\nextern \"C\" {\n#endif\ntypedef int esp_err_t; esp_err_t esp_crt_bundle_attach(void*);\n#ifdef __cplusplus\n}\n#endif\n");
    writeFileSync(join(scratch, "esp_timer.h"),
      "#pragma once\n#include <stdint.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nint64_t esp_timer_get_time(void);\n#ifdef __cplusplus\n}\n#endif\n");
    writeFileSync(join(scratch, "lwip", "netdb.h"),
      "#pragma once\n#include <netdb.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nint fake_getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);\nvoid fake_freeaddrinfo(struct addrinfo*);\n#ifdef __cplusplus\n}\n#endif\n#define getaddrinfo fake_getaddrinfo\n#define freeaddrinfo fake_freeaddrinfo\n");
    writeFileSync(join(scratch, "lwip", "sockets.h"),
      "#pragma once\n#include <sys/socket.h>\n#include <netinet/in.h>\n#include <unistd.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nint fake_getpeername(int, struct sockaddr*, socklen_t*);\nint fake_shutdown(int, int);\n#ifdef __cplusplus\n}\n#endif\n#define getpeername fake_getpeername\n#define shutdown fake_shutdown\n");
    writeFileSync(join(scratch, "freertos", "FreeRTOS.h"), "#pragma once\n");
    writeFileSync(join(scratch, "freertos", "task.h"),
      "#pragma once\n#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid vTaskDelay(unsigned int);\n#ifdef __cplusplus\n}\n#endif\n");
    const source = join(scratch, "native_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, nativeHarness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1",
      "-I", scratch,
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      "-I", join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf/include"),
      source,
      join(myai, "EndpointPolicy.cpp"),
      join(myai, "GatewayProbeContract.cpp"),
      join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf/esp_http_adapters.cpp"),
      "-o", binary,
    ];
    if (sanitized) args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
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

test("native async gateway probe set is one bounded concurrent deadline", () => {
  buildAndRunNativeHarness(false);
  buildAndRunNativeHarness(true);
});
