#include "inkloop/myai/esp_http_adapters.hpp"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "inkloop/myai/EndpointPolicy.h"
#include "inkloop/myai/GatewayProbeContract.h"
#include "inkloop/myai/esp_network_operation_gate.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Inkloop MyAI HTTPS requires the ESP-IDF trusted certificate bundle"
#endif

namespace inkloop {
namespace myai {
namespace {

constexpr size_t kMaximumRequestBodyBytes = 64U * 1024U;
constexpr size_t kMaximumResponseBodyBytes = 256U * 1024U;
constexpr size_t kMaximumHeaderCount = 32;
constexpr size_t kMaximumHeaderNameBytes = 64;
constexpr size_t kMaximumHeaderValueBytes = 2048;
constexpr size_t kMaximumAggregateHeaderBytes = 16U * 1024U;
constexpr uint32_t kMaximumTimeoutMs = 120000;
constexpr size_t kMaximumResolvedAddresses = 16;

bool lineSafe(const std::string& value) {
  for (unsigned char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == 0) return false;
  }
  return true;
}

bool publicSockaddr(const sockaddr* address, socklen_t length) {
  if (!address) return false;
  if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ipv4->sin_addr.s_addr);
    return EndpointPolicy::isPublicIpv4({bytes[0], bytes[1], bytes[2], bytes[3]});
  }
  if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
    std::array<uint8_t, 16> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = ipv6->sin6_addr.s6_addr[index];
    }
    return EndpointPolicy::isPublicIpv6(bytes);
  }
  return false;
}

void shutDownClientSocket(esp_http_client_handle_t client) {
  const int socket = esp_http_client_get_socket(client);
  if (socket >= 0) shutdown(socket, SHUT_RDWR);
}

struct HttpEventContext {
  std::string* responseBody = nullptr;
  size_t maximumResponseBytes = 0;
  bool peerRejected = false;
  bool responseTooLarge = false;
};

esp_err_t httpEvent(esp_http_client_event_t* event) {
  if (!event || !event->user_data) return ESP_ERR_INVALID_ARG;
  auto* context = static_cast<HttpEventContext*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    const int socket = esp_http_client_get_socket(event->client);
    if (socket < 0 ||
        getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) != 0 ||
        !publicSockaddr(reinterpret_cast<const sockaddr*>(&peer), length)) {
      context->peerRejected = true;
      shutDownClientSocket(event->client);
    }
  } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    if (!context->responseBody || !event->data ||
        static_cast<size_t>(event->data_len) >
            context->maximumResponseBytes - context->responseBody->size()) {
      context->responseTooLarge = true;
      shutDownClientSocket(event->client);
    } else {
      context->responseBody->append(static_cast<const char*>(event->data),
                                    static_cast<size_t>(event->data_len));
    }
  }
  return ESP_OK;
}

bool supportedMethod(const std::string& method,
                     esp_http_client_method_t& output) {
  if (method == "GET") output = HTTP_METHOD_GET;
  else if (method == "POST") output = HTTP_METHOD_POST;
  else if (method == "HEAD") output = HTTP_METHOD_HEAD;
  else return false;
  return true;
}

Status validateRequest(const HttpRequest& request,
                       esp_http_client_method_t& method) {
  HttpsEndpoint endpoint;
  const Status parsed = EndpointPolicy::parsePublicUrl(
      request.url, request.plaintextPublicGatewayAllowed, endpoint);
  const bool transportPolicy = parsed.ok() &&
      ((endpoint.tls && request.tlsPeerVerificationRequired &&
        !request.plaintextPublicGatewayAllowed) ||
       (!endpoint.tls && !request.tlsPeerVerificationRequired &&
        request.plaintextPublicGatewayAllowed));
  if (!transportPolicy || !request.rejectPrivateResolvedAddresses ||
      request.redirectsAllowed) {
    return Status(ErrorCode::Security, 0,
                  "MyAI HTTP transport policy was weakened");
  }
  if (!supportedMethod(request.method, method) ||
      request.body.size() > kMaximumRequestBodyBytes ||
      request.maxResponseBytes == 0 ||
      request.maxResponseBytes > kMaximumResponseBodyBytes ||
      request.timeoutMs == 0 || request.timeoutMs > kMaximumTimeoutMs) {
    return Status(ErrorCode::InvalidArgument, 0,
                  "invalid bounded MyAI HTTP request");
  }
  return Status::success();
}

Status validateHeaders(
    const std::map<std::string, std::string>& headers) {
  if (headers.size() > kMaximumHeaderCount) {
    return Status(ErrorCode::InvalidArgument, 0,
                  "invalid bounded MyAI HTTP headers");
  }
  size_t aggregate = 0;
  for (const auto& header : headers) {
    if (header.first.empty() || header.first.size() > kMaximumHeaderNameBytes ||
        header.second.size() > kMaximumHeaderValueBytes ||
        !lineSafe(header.first) || !lineSafe(header.second) ||
        aggregate > kMaximumAggregateHeaderBytes - header.first.size() ||
        aggregate + header.first.size() >
            kMaximumAggregateHeaderBytes - header.second.size()) {
      return Status(ErrorCode::InvalidArgument, 0,
                    "invalid bounded MyAI HTTP headers");
    }
    aggregate += header.first.size() + header.second.size();
  }
  return Status::success();
}

uint32_t boundedLatency(uint64_t startedMs, uint64_t nowMs) {
  const uint64_t elapsed = nowMs >= startedMs ? nowMs - startedMs : 0;
  return static_cast<uint32_t>(std::min<uint64_t>(
      elapsed, std::numeric_limits<uint32_t>::max()));
}

}  // namespace

Status EspEndpointSecurity::validatePublicTlsEndpoint(
    const std::string& httpsUrl) {
  HttpsEndpoint endpoint;
  Status parsed = EndpointPolicy::parseHttpsUrl(httpsUrl, endpoint);
  if (!parsed.ok()) return parsed;

  return validatePublicEndpoint(httpsUrl);
}

Status EspEndpointSecurity::validatePublicEndpoint(const std::string& url) {
  HttpsEndpoint endpoint;
  Status parsed = EndpointPolicy::parsePublicUrl(url, true, endpoint);
  if (!parsed.ok()) return parsed;

  char port[6] = {};
  std::snprintf(port, sizeof(port), "%u", static_cast<unsigned>(endpoint.port));
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  if (getaddrinfo(endpoint.host.c_str(), port, &hints, &addresses) != 0 ||
      !addresses) {
    if (addresses) freeaddrinfo(addresses);
    return Status(ErrorCode::Transport, 0,
                  "MyAI endpoint DNS resolution failed");
  }

  bool any = false;
  bool allPublic = true;
  size_t count = 0;
  for (const addrinfo* current = addresses; current; current = current->ai_next) {
    ++count;
    if (count > kMaximumResolvedAddresses || !current->ai_addr ||
        !publicSockaddr(current->ai_addr,
                        static_cast<socklen_t>(current->ai_addrlen))) {
      allPublic = false;
      break;
    }
    any = true;
  }
  freeaddrinfo(addresses);
  if (!any || !allPublic) {
    return Status(ErrorCode::Security, 0,
                  "MyAI endpoint resolved to a non-public address");
  }
  return Status::success();
}

Status EspEndpointSecurity::validateConnectedSocket(int socket) const {
  sockaddr_storage peer{};
  socklen_t length = sizeof(peer);
  if (socket < 0 ||
      getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) != 0) {
    return Status(ErrorCode::Transport, 0,
                  "MyAI connected peer is unavailable");
  }
  if (!publicSockaddr(reinterpret_cast<const sockaddr*>(&peer), length)) {
    return Status(ErrorCode::Security, 0,
                  "MyAI connected peer was not public");
  }
  return Status::success();
}

EspHttpTransport::EspHttpTransport(EspEndpointSecurity& endpointSecurity)
    : endpointSecurity_(endpointSecurity) {}

Status EspHttpTransport::perform(const HttpRequest& request,
                                 HttpResponse& response) {
  response = HttpResponse();
  esp_http_client_method_t method = HTTP_METHOD_GET;
  Status valid = validateRequest(request, method);
  if (!valid.ok()) return valid;
  valid = validateHeaders(request.headers);
  if (!valid.ok()) return valid;
  HttpsEndpoint endpoint;
  valid = EndpointPolicy::parsePublicUrl(
      request.url, request.plaintextPublicGatewayAllowed, endpoint);
  if (!valid.ok()) return valid;
  valid = endpointSecurity_.validatePublicEndpoint(request.url);
  if (!valid.ok()) return valid;

  EspNetworkOperationLease network_lease(request.timeoutMs);
  if (!network_lease.acquired()) {
    return Status(ErrorCode::Transport, 0,
                  "MyAI network operation gate timed out");
  }

  HttpEventContext context;
  context.responseBody = &response.body;
  context.maximumResponseBytes = request.maxResponseBytes;

  esp_http_client_config_t config{};
  config.url = request.url.c_str();
  config.method = method;
  config.timeout_ms = static_cast<int>(request.timeoutMs);
  config.disable_auto_redirect = true;
  config.max_redirection_count = 0;
  config.max_authorization_retries = -1;
  config.event_handler = httpEvent;
  config.user_data = &context;
  config.buffer_size = 2048;
  config.buffer_size_tx = 2048;
  config.skip_cert_common_name_check = false;
  config.crt_bundle_attach = endpoint.tls ? esp_crt_bundle_attach : nullptr;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return Status(ErrorCode::Transport, 0,
                  "MyAI HTTP client allocation failed");
  }

  esp_err_t setup = ESP_OK;
  for (const auto& header : request.headers) {
    setup = esp_http_client_set_header(client, header.first.c_str(),
                                       header.second.c_str());
    if (setup != ESP_OK) break;
  }
  if (setup == ESP_OK && !request.body.empty()) {
    setup = esp_http_client_set_post_field(
        client, request.body.data(), static_cast<int>(request.body.size()));
  }
  const esp_err_t performed =
      setup == ESP_OK ? esp_http_client_perform(client) : setup;
  response.status = esp_http_client_get_status_code(client);
  const bool complete = request.method == "HEAD" ||
                        esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);

  if (context.peerRejected) {
    response = HttpResponse();
    return Status(ErrorCode::Security, 0,
                  "MyAI connected peer was not public");
  }
  if (context.responseTooLarge) {
    response = HttpResponse();
    return Status(ErrorCode::TooLarge, 0,
                  "MyAI HTTP response exceeded its byte limit");
  }
  // ESP-IDF returns ESP_FAIL when authorization retry is disabled and a
  // server replies 401, even though the response status is authoritative.
  // Preserve that status so the portable client can revoke the stale device
  // credential and start a fresh six-digit pairing flow. The body may not
  // have been drained on this SDK path, so never expose or parse a partial one.
  if (response.status == 401) {
    if (!complete) response.body.clear();
    return Status::success();
  }
  if (performed != ESP_OK || !complete || response.status <= 0) {
    response = HttpResponse();
    return Status(ErrorCode::Transport, 0,
                  "MyAI HTTPS request failed");
  }
  return Status::success();
}

uint64_t EspClock::monotonicMs() const {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

std::string EspClock::utcIso8601() const {
  const std::time_t current = std::time(nullptr);
  if (current < 1577836800) return std::string();
  std::tm utc{};
  if (!gmtime_r(&current, &utc)) return std::string();
  char output[21] = {};
  if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc) != 20) {
    return std::string();
  }
  return output;
}

EspGatewayProbeSet::EspGatewayProbeSet(IClock& clock) : clock_(clock) {}

Status EspGatewayProbeSet::probeConcurrent(
    const std::vector<GatewayCandidate>& candidates,
    const std::map<std::string, std::string>& headers,
    uint32_t totalDeadlineMs, std::vector<GatewayProbe>& results) {
  results.clear();
  Status valid = GatewayProbeContract::validateCandidates(candidates);
  if (!valid.ok()) return valid;
  EspNetworkOperationLease network_lease(totalDeadlineMs);
  if (!network_lease.acquired()) {
    return Status(ErrorCode::Transport, 0,
                  "MyAI gateway probe gate timed out");
  }
  valid = validateHeaders(headers);
  if (!valid.ok()) return valid;
  const auto authorization = headers.find("Authorization");
  // The caller supplies exactly the short-lived probe credential.  The
  // adapter adds the candidate-specific X-Gateway-ID below so a durable
  // Center credential (or an injected identity/header) cannot cross into the
  // Gateway probe plane.
  if (headers.size() != 1U || authorization == headers.end() ||
      authorization->second.size() <= sizeof("Bearer ") - 1U ||
      authorization->second.compare(0, sizeof("Bearer ") - 1U,
                                    "Bearer ") != 0 ||
      headers.find("X-Device-ID") != headers.end() ||
      headers.find("X-Device-MAC") != headers.end()) {
    return Status(ErrorCode::Security, 0,
                  "invalid MyAI gateway probe credential boundary");
  }
  if (totalDeadlineMs == 0 ||
      totalDeadlineMs > GatewayProbeContract::kTotalDeadlineMs) {
    return Status(ErrorCode::InvalidArgument, 0,
                  "invalid MyAI gateway probe deadline");
  }
  const std::string checkedAt = clock_.utcIso8601();
  if (checkedAt.empty() || checkedAt.size() > 32) {
    return Status(ErrorCode::InvalidState, 0,
                  "UTC clock is not ready for MyAI gateway probing");
  }
  for (const GatewayCandidate& candidate : candidates) {
    HttpsEndpoint endpoint;
    if (candidate.id.size() > kMaximumHeaderValueBytes ||
        !lineSafe(candidate.id) ||
        !EndpointPolicy::parsePublicUrl(candidate.pingUrl, true, endpoint).ok()) {
      return Status(ErrorCode::Security, 0,
                    "invalid public MyAI gateway probe endpoint");
    }
  }

  const uint64_t startedMs = clock_.monotonicMs();
  results.reserve(candidates.size());
  bool peerRejected = false;
  bool unexpectedBody = false;

  // ESP-IDF's async HTTP client keeps one live TLS/lwIP state machine per
  // candidate.  Keeping several of them alive on the S3 slow-service core was
  // observed to corrupt lwIP timeout ownership under the concurrent Inkloop,
  // Voice and AIGC workload. Probe one candidate at a time instead. Each
  // candidate receives a fair share of the one bounded global deadline, so a
  // failed endpoint cannot starve the remaining Center candidates.
  for (size_t index = 0; index < candidates.size(); ++index) {
    GatewayProbe result;
    result.gatewayId = candidates[index].id;
    result.checkedAt = checkedAt;
    result.error = "transport";

    const uint64_t beforeProbeMs = clock_.monotonicMs();
    const uint32_t elapsed = boundedLatency(startedMs, beforeProbeMs);
    if (elapsed >= totalDeadlineMs) {
      result.error = "timeout";
      result.latencyMs = totalDeadlineMs;
      results.push_back(result);
      continue;
    }
    const uint32_t remainingMs = totalDeadlineMs - elapsed;
    const size_t remainingCandidates = candidates.size() - index;
    const uint32_t perCandidateDeadlineMs = std::max<uint32_t>(
        1U, remainingMs / static_cast<uint32_t>(remainingCandidates));

    HttpsEndpoint endpoint;
    const Status parsed = EndpointPolicy::parsePublicUrl(
        candidates[index].pingUrl, true, endpoint);
    if (!parsed.ok()) {
      result.error = "security";
      results.push_back(result);
      continue;
    }

    HttpEventContext event{};
    event.maximumResponseBytes = 0;
    esp_http_client_config_t config{};
    config.url = candidates[index].pingUrl.c_str();
    config.method = HTTP_METHOD_HEAD;
    config.timeout_ms = static_cast<int>(perCandidateDeadlineMs);
    config.disable_auto_redirect = true;
    config.max_redirection_count = 0;
    config.max_authorization_retries = -1;
    config.event_handler = httpEvent;
    config.user_data = &event;
    config.is_async = false;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
    config.skip_cert_common_name_check = false;
    config.crt_bundle_attach = endpoint.tls ? esp_crt_bundle_attach : nullptr;
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      results.push_back(result);
      continue;
    }
    bool setupOk = true;
    for (const auto& header : headers) {
      if (esp_http_client_set_header(client, header.first.c_str(),
                                     header.second.c_str()) != ESP_OK) {
        setupOk = false;
        break;
      }
    }
    if (setupOk &&
        esp_http_client_set_header(client, "X-Gateway-ID",
                                   candidates[index].id.c_str()) != ESP_OK) {
      setupOk = false;
    }
    const uint64_t probeStartedMs = clock_.monotonicMs();
    const esp_err_t performed =
        setupOk ? esp_http_client_perform(client) : ESP_FAIL;
    const uint64_t probeFinishedMs = clock_.monotonicMs();
    result.latencyMs = std::min<uint32_t>(
        boundedLatency(probeStartedMs, probeFinishedMs),
        perCandidateDeadlineMs);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (!setupOk) {
      result.error = "setup";
    } else if (boundedLatency(probeStartedMs, probeFinishedMs) >=
               perCandidateDeadlineMs) {
      result.error = "timeout";
    } else if (event.peerRejected) {
      result.error = "peer_rejected";
    } else if (event.responseTooLarge) {
      result.error = "unexpected_body";
    } else if (performed != ESP_OK) {
      result.error = "transport";
    } else if (status < 200 || status >= 400) {
      result.error = "http_status";
    } else {
      result.ok = true;
      result.error.clear();
    }
    peerRejected = peerRejected || event.peerRejected;
    unexpectedBody = unexpectedBody || event.responseTooLarge;
    results.push_back(result);
  }
  if (peerRejected) {
    results.clear();
    return Status(ErrorCode::Security, 0,
                  "MyAI gateway probe connected to a non-public peer");
  }
  if (unexpectedBody) {
    results.clear();
    return Status(ErrorCode::Protocol, 0,
                  "MyAI gateway HEAD probe returned a body");
  }
  return Status::success();
}

}  // namespace myai
}  // namespace inkloop
