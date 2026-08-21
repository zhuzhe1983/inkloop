#include "PaperColorMyAiAdapters.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <ctime>
#include <limits>

#include "Diagnostics.h"
#include "MyAiCredentialPersistencePrimitives.h"

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");

namespace inkloop {
namespace {

constexpr char kMyAiCredentialNamespace[] = "ink-myai-v1";
constexpr char kMyAiCredentialInitialized[] = "initialized";
constexpr uint8_t kMyAiCredentialInitializedValue = 0xA7;

using myai::ErrorCode;
using myai::Status;

Status error(ErrorCode code, const char* detail, int httpStatus = 0) {
  return Status(code, httpStatus, detail);
}

std::string sha256Hex(const std::string& input) {
  uint8_t digest[32];
  if (mbedtls_sha256_ret(
          reinterpret_cast<const unsigned char*>(input.data()), input.size(),
          digest, 0) != 0) {
    return std::string();
  }
  static const char digits[] = "0123456789abcdef";
  std::string output;
  output.resize(64);
  for (size_t index = 0; index < sizeof(digest); ++index) {
    output[index * 2] = digits[digest[index] >> 4];
    output[index * 2 + 1] = digits[digest[index] & 0x0f];
  }
  return output;
}

bool safeOpaque(const std::string& value, size_t maximum, bool emptyAllowed) {
  if ((!emptyAllowed && value.empty()) || value.size() > maximum) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const uint8_t ch = static_cast<uint8_t>(value[index]);
    if (ch < 0x20 || ch == 0x7f) return false;
  }
  return true;
}

class BoundedWriteStream final : public Stream {
 public:
  explicit BoundedWriteStream(size_t maximum) : maximum_(maximum) {
    output_.reserve(std::min<size_t>(maximum, 8192));
  }

  size_t write(uint8_t value) override {
    if (overflow_ || output_.size() >= maximum_) {
      overflow_ = true;
      return 0;
    }
    output_.push_back(static_cast<char>(value));
    return 1;
  }
  size_t write(const uint8_t* bytes, size_t length) override {
    if (!bytes || overflow_ || length > maximum_ - output_.size()) {
      overflow_ = true;
      return 0;
    }
    output_.append(reinterpret_cast<const char*>(bytes), length);
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool overflow() const { return overflow_; }
  const std::string& output() const { return output_; }

 private:
  size_t maximum_;
  bool overflow_ = false;
  std::string output_;
};

bool configureSecureHttp(const myai::HttpRequest& request,
                         WiFiClientSecure& client, HTTPClient& http) {
  if (!request.tlsPeerVerificationRequired ||
      !request.rejectPrivateResolvedAddresses || request.redirectsAllowed ||
      request.url.compare(0, 8, "https://") != 0) {
    return false;
  }
  client.setCACertBundle(rootca_crt_bundle_start);
  client.setHandshakeTimeout(std::max<uint32_t>(1, request.timeoutMs / 1000));
  client.setTimeout(std::max<uint32_t>(1, request.timeoutMs / 1000));
  if (!http.begin(client, request.url.c_str())) return false;
  http.setConnectTimeout(request.timeoutMs);
  http.setTimeout(request.timeoutMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  for (std::map<std::string, std::string>::const_iterator it =
           request.headers.begin();
       it != request.headers.end(); ++it) {
    http.addHeader(it->first.c_str(), it->second.c_str(), false, true);
  }
  return true;
}

int sendHttpRequest(const myai::HttpRequest& request, HTTPClient& http) {
  if (request.method == "GET") return http.GET();
  if (request.method == "HEAD") return http.sendRequest("HEAD");
  if (request.method == "POST") {
    return http.POST(reinterpret_cast<uint8_t*>(
                         const_cast<char*>(request.body.data())),
                     request.body.size());
  }
  return http.sendRequest(
      request.method.c_str(),
      reinterpret_cast<uint8_t*>(const_cast<char*>(request.body.data())),
      request.body.size());
}

bool extractJsonString(const std::string& prefix, const char* key,
                       std::string& value) {
  const std::string quoted = std::string("\"") + key + "\"";
  const size_t keyAt = prefix.find(quoted);
  if (keyAt == std::string::npos) return false;
  const size_t colon = prefix.find(':', keyAt + quoted.size());
  if (colon == std::string::npos) return false;
  const size_t quote = prefix.find('"', colon + 1);
  if (quote == std::string::npos) return false;
  value.clear();
  bool escaped = false;
  for (size_t index = quote + 1; index < prefix.size(); ++index) {
    const char ch = prefix[index];
    if (escaped) {
      if (ch == '"' || ch == '\\' || ch == '/') value.push_back(ch);
      else return false;
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return !value.empty();
    } else {
      value.push_back(ch);
    }
  }
  return false;
}

int base64Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  if (ch == '=') return -2;
  return -1;
}

Status decodeQuartet(const char quartet[4], myai::IImageSink& sink,
                     size_t maximum, size_t& decoded, bool& padded) {
  const int a = base64Value(quartet[0]);
  const int b = base64Value(quartet[1]);
  const int c = base64Value(quartet[2]);
  const int d = base64Value(quartet[3]);
  if (a < 0 || b < 0 || c == -1 || d == -1 || padded ||
      (c == -2 && d != -2)) {
    return error(ErrorCode::Protocol, "invalid AIGC base64 payload");
  }
  uint8_t output[3];
  size_t count = 1;
  output[0] = static_cast<uint8_t>((a << 2) | (b >> 4));
  if (c >= 0) {
    output[1] = static_cast<uint8_t>((b << 4) | (c >> 2));
    count = 2;
    if (d >= 0) {
      output[2] = static_cast<uint8_t>((c << 6) | d);
      count = 3;
    }
  }
  padded = c == -2 || d == -2;
  if (decoded > maximum || count > maximum - decoded)
    return error(ErrorCode::TooLarge, "AIGC decoded image exceeds cap");
  Status status = sink.write(output, count);
  if (!status.ok()) return status;
  decoded += count;
  return Status::success();
}

}  // namespace

uint64_t PaperColorClock::monotonicMs() const {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

std::string PaperColorClock::utcIso8601() const {
  const time_t now = time(nullptr);
  if (now < 1700000000) return "1970-01-01T00:00:00Z";
  tm utc{};
  gmtime_r(&now, &utc);
  char output[32];
  if (!strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc))
    return "1970-01-01T00:00:00Z";
  return output;
}

bool Esp32PublicEndpointSecurity::parsePublicTlsUrl(
    const std::string& url, std::string& host, uint16_t& port,
    std::string* path) {
  const bool https = url.compare(0, 8, "https://") == 0;
  const bool wss = url.compare(0, 6, "wss://") == 0;
  if (!https && !wss) return false;
  const size_t authorityAt = https ? 8 : 6;
  const size_t authorityEnd = url.find_first_of("/?#", authorityAt);
  const std::string authority = url.substr(
      authorityAt, authorityEnd == std::string::npos
                       ? std::string::npos
                       : authorityEnd - authorityAt);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority[0] == '[') {
    return false;
  }
  const size_t colon = authority.rfind(':');
  host = colon == std::string::npos ? authority : authority.substr(0, colon);
  port = 443;
  if (colon != std::string::npos) {
    const std::string encoded = authority.substr(colon + 1);
    if (encoded.empty() || encoded.size() > 5) return false;
    unsigned long parsed = 0;
    for (size_t index = 0; index < encoded.size(); ++index) {
      if (encoded[index] < '0' || encoded[index] > '9') return false;
      parsed = parsed * 10 + static_cast<unsigned long>(encoded[index] - '0');
    }
    if (parsed == 0 || parsed > 65535) return false;
    port = static_cast<uint16_t>(parsed);
  }
  if (host.empty() || host.size() > 253 || host == "localhost" ||
      host.find("..") != std::string::npos || host[host.size() - 1] == '.') {
    return false;
  }
  std::string lower = host;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if ((lower.size() >= 6 && lower.compare(lower.size() - 6, 6, ".local") == 0) ||
      (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lan") == 0) ||
      (lower.size() >= 9 &&
       lower.compare(lower.size() - 9, 9, ".internal") == 0)) {
    return false;
  }
  for (size_t index = 0; index < host.size(); ++index) {
    const char ch = host[index];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')) {
      return false;
    }
  }
  if (path) {
    *path = authorityEnd == std::string::npos ? "/" : url.substr(authorityEnd);
    if (path->empty() || (*path)[0] != '/') return false;
  }
  return true;
}

bool Esp32PublicEndpointSecurity::publicAddress(const sockaddr* address) {
  if (!address) return false;
  if (address->sa_family == AF_INET) {
    const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    const uint32_t value = ntohl(ipv4->sin_addr.s_addr);
    const uint8_t a = static_cast<uint8_t>(value >> 24);
    const uint8_t b = static_cast<uint8_t>(value >> 16);
    if (a == 0 || a == 10 || a == 127 || a >= 224 ||
        (a == 100 && b >= 64 && b <= 127) ||
        (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) ||
        (a == 192 && (b == 0 || b == 168)) ||
        (a == 198 && (b == 18 || b == 19)) ||
        (a == 198 && b == 51) || (a == 203 && b == 0)) {
      return false;
    }
    return true;
  }
  if (address->sa_family == AF_INET6) {
    const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
    const uint8_t* value = ipv6->sin6_addr.s6_addr;
    bool allZero = true;
    for (size_t index = 0; index < 16; ++index) allZero &= value[index] == 0;
    if (allZero || value[0] == 0xff || (value[0] & 0xfe) == 0xfc ||
        (value[0] == 0xfe && (value[1] & 0xc0) == 0x80)) {
      return false;
    }
    // Only globally routable 2000::/3 addresses are accepted.
    if ((value[0] & 0xe0) != 0x20) return false;
    if (value[0] == 0x20 && value[1] == 0x01 && value[2] == 0x0d &&
        value[3] == 0xb8) {
      return false;
    }
    return true;
  }
  return false;
}

Status Esp32PublicEndpointSecurity::validatePublicTlsEndpoint(
    const std::string& httpsUrl) {
  std::string host;
  uint16_t port = 0;
  if (!parsePublicTlsUrl(httpsUrl, host, port))
    return error(ErrorCode::Security, "invalid public TLS endpoint");
  if (time(nullptr) < 1700000000)
    return error(ErrorCode::Security, "TLS clock is not synchronized");

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  const int resolved = getaddrinfo(host.c_str(), nullptr, &hints, &addresses);
  if (resolved != 0 || !addresses)
    return error(ErrorCode::Transport, "public endpoint DNS resolution failed");
  bool found = false;
  bool valid = true;
  for (addrinfo* next = addresses; next; next = next->ai_next) {
    found = true;
    if (!publicAddress(next->ai_addr)) valid = false;
  }
  freeaddrinfo(addresses);
  if (!found || !valid)
    return error(ErrorCode::Security, "non-public endpoint address rejected");
  return Status::success();
}

Status Esp32HttpsTransport::readBounded(HTTPClient& http, size_t maximum,
                                        std::string& output) {
  output.clear();
  const int length = http.getSize();
  if (length > 0 && static_cast<size_t>(length) > maximum)
    return error(ErrorCode::TooLarge, "MyAI HTTP response exceeds cap");
  BoundedWriteStream sink(maximum);
  const int written = http.writeToStream(&sink);
  if (sink.overflow())
    return error(ErrorCode::TooLarge, "MyAI HTTP response exceeds cap");
  if (written < 0)
    return error(ErrorCode::Transport, "MyAI HTTP response read failed");
  output = sink.output();
  return Status::success();
}

Status Esp32HttpsTransport::perform(const myai::HttpRequest& request,
                                    myai::HttpResponse& response) {
  response = myai::HttpResponse();
  WiFiClientSecure client;
  HTTPClient http;
  if (!configureSecureHttp(request, client, http))
    return error(ErrorCode::Security, "secure HTTP policy unavailable");
  const int status = sendHttpRequest(request, http);
  if (status <= 0) {
    http.end();
    return error(ErrorCode::Transport, "MyAI HTTPS request failed");
  }
  response.status = status;
  Status read = Status::success();
  if (request.maxResponseBytes != 0 && request.method != "HEAD")
    read = readBounded(http, request.maxResponseBytes, response.body);
  http.end();
  return read;
}

bool NvsMyAiCredentialStore::encode(
    const myai::CredentialSnapshot& snapshot, std::string& output) {
  JsonDocument document;
  document["schema"] = 1;
  document["generation"] = snapshot.generation;
  document["fingerprint"] = snapshot.installationFingerprint;
  document["device_id"] = snapshot.deviceId;
  JsonObject pending = document["pending"].to<JsonObject>();
  pending["device_id"] = snapshot.pending.deviceId;
  pending["token"] = snapshot.pending.pairingToken;
  pending["binding_url"] = snapshot.pending.bindingUrl;
  pending["expires_at"] = snapshot.pending.expiresAt;
  document["device_token"] = snapshot.deviceToken;
  document["active"] = snapshot.active;
  std::string canonical;
  serializeJson(document, canonical);
  const std::string checksum = sha256Hex(canonical);
  if (checksum.size() != 64) return false;
  document["checksum"] = checksum;
  output.clear();
  serializeJson(document, output);
  return !output.empty() && output.size() <= 8192;
}

bool NvsMyAiCredentialStore::decode(
    const std::string& input, myai::CredentialSnapshot& snapshot) {
  if (input.empty() || input.size() > 8192) return false;
  JsonDocument document;
  if (deserializeJson(document, input) ||
      document["schema"].as<int>() != 1 ||
      !document["generation"].is<uint32_t>() ||
      !document["fingerprint"].is<const char*>() ||
      !document["device_id"].is<const char*>() ||
      !document["pending"].is<JsonObject>() ||
      !document["device_token"].is<const char*>() ||
      !document["active"].is<bool>() ||
      !document["checksum"].is<const char*>()) {
    return false;
  }
  const std::string expected = document["checksum"].as<const char*>();
  document.remove("checksum");
  std::string canonical;
  serializeJson(document, canonical);
  if (expected.size() != 64 || sha256Hex(canonical) != expected) return false;

  myai::CredentialSnapshot decoded;
  decoded.generation = document["generation"].as<uint32_t>();
  decoded.installationFingerprint =
      document["fingerprint"].as<const char*>();
  decoded.deviceId = document["device_id"].as<const char*>();
  JsonObjectConst pending = document["pending"].as<JsonObjectConst>();
  if (!pending["device_id"].is<const char*>() ||
      !pending["token"].is<const char*>() ||
      !pending["binding_url"].is<const char*>() ||
      !pending["expires_at"].is<const char*>()) {
    return false;
  }
  decoded.pending.deviceId = pending["device_id"].as<const char*>();
  decoded.pending.pairingToken = pending["token"].as<const char*>();
  decoded.pending.bindingUrl = pending["binding_url"].as<const char*>();
  decoded.pending.expiresAt = pending["expires_at"].as<const char*>();
  decoded.deviceToken = document["device_token"].as<const char*>();
  decoded.active = document["active"].as<bool>();
  if (decoded.generation == 0 ||
      !safeOpaque(decoded.installationFingerprint, 256, true) ||
      !safeOpaque(decoded.deviceId, 6, true) ||
      !safeOpaque(decoded.pending.pairingToken, 1024, true) ||
      !safeOpaque(decoded.pending.bindingUrl, 1024, true) ||
      !safeOpaque(decoded.pending.expiresAt, 128, true) ||
      !safeOpaque(decoded.deviceToken, 2048, true)) {
    decoded.redact();
    return false;
  }
  snapshot.redact();
  snapshot = decoded;
  return true;
}

Status NvsMyAiCredentialStore::load(myai::CredentialSnapshot& snapshot) {
  snapshot.redact();
  snapshot = myai::CredentialSnapshot();
  Preferences preferences;
  // A read-only open returns NOT_FOUND on a genuine first boot. A read/write
  // open is a no-key availability probe and must not erase or initialize any
  // credential. Partial/committed namespaces are classified below.
  if (!preferences.begin(kMyAiCredentialNamespace, false))
    return error(ErrorCode::Storage, "MyAI NVS open failed");
  MyAiCredentialStorageProbe probe;
  probe.namespaceAvailable = true;
  probe.markerPresent = preferences.isKey(kMyAiCredentialInitialized);
  probe.markerValid = probe.markerPresent &&
      preferences.getUChar(kMyAiCredentialInitialized, 0) ==
          kMyAiCredentialInitializedValue;
  probe.headPresent = preferences.isKey("head");
  probe.slot0Present = preferences.isKey("slot0");
  probe.slot1Present = preferences.isKey("slot1");
  const uint32_t head = preferences.getUInt("head", 0);
  probe.headValid = probe.headPresent && head != 0;
  const char* key = (head & 1U) ? "slot1" : "slot0";
  const String encoded = preferences.getString(key, "");
  myai::CredentialSnapshot decoded;
  probe.committedSlotValid = probe.headValid &&
      decode(std::string(encoded.c_str(), encoded.length()), decoded) &&
      decoded.generation == head;
  preferences.end();
  const MyAiCredentialLoadResult result =
      classifyMyAiCredentialStorage(probe);
  if (result == MyAiCredentialLoadResult::Absent) {
    decoded.redact();
    return Status::success();
  }
  if (result == MyAiCredentialLoadResult::Unavailable) {
    decoded.redact();
    return error(ErrorCode::Storage, "MyAI NVS unavailable");
  }
  if (result == MyAiCredentialLoadResult::Corrupt) {
    decoded.redact();
    return error(ErrorCode::Storage, "MyAI NVS committed slot invalid");
  }
  snapshot = decoded;
  return Status::success();
}

Status NvsMyAiCredentialStore::store(
    const myai::CredentialSnapshot& snapshot) {
  myai::CredentialSnapshot current;
  Status status = load(current);
  if (!status.ok()) return status;
  if (snapshot.generation == 0 || snapshot.generation != current.generation + 1)
    return error(ErrorCode::Storage, "MyAI credential generation conflict");
  std::string encoded;
  if (!encode(snapshot, encoded))
    return error(ErrorCode::Storage, "MyAI credential serialization failed");

  Preferences preferences;
  if (!preferences.begin(kMyAiCredentialNamespace, false))
    return error(ErrorCode::Storage, "MyAI NVS write open failed");
  const char* key = (snapshot.generation & 1U) ? "slot1" : "slot0";
  const size_t stored = preferences.putString(key, encoded.c_str());
  const String verify = preferences.getString(key, "");
  myai::CredentialSnapshot decoded;
  const bool valid = stored == encoded.size() &&
      verify.length() == encoded.size() &&
      decode(std::string(verify.c_str(), verify.length()), decoded) &&
      decoded.generation == snapshot.generation;
  decoded.redact();
  if (!valid || preferences.putUInt("head", snapshot.generation) !=
                    sizeof(uint32_t) ||
      preferences.getUInt("head", 0) != snapshot.generation ||
      preferences.putUChar(
          kMyAiCredentialInitialized,
          kMyAiCredentialInitializedValue) != sizeof(uint8_t) ||
      preferences.getUChar(kMyAiCredentialInitialized, 0) !=
          kMyAiCredentialInitializedValue) {
    preferences.end();
    return error(ErrorCode::Storage, "MyAI credential atomic commit failed");
  }
  preferences.end();
  return Status::success();
}

Status NvsMyAiCredentialStore::initializeFingerprintAtomically(
    const std::string& installationFingerprint) {
  if (!safeOpaque(installationFingerprint, 256, false))
    return error(ErrorCode::InvalidArgument, "invalid installation fingerprint");
  myai::CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (!snapshot.installationFingerprint.empty())
    return snapshot.installationFingerprint == installationFingerprint
               ? Status::success()
               : error(ErrorCode::Conflict, "installation fingerprint conflict");
  snapshot.installationFingerprint = installationFingerprint;
  ++snapshot.generation;
  return store(snapshot);
}

Status NvsMyAiCredentialStore::savePendingAtomically(
    const myai::PendingPairing& pending) {
  if (!pending.valid())
    return error(ErrorCode::InvalidArgument, "invalid pending MyAI pairing");
  myai::CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (snapshot.installationFingerprint.empty() || snapshot.hasDeviceToken())
    return error(ErrorCode::Conflict, "MyAI credential state rejects pairing");
  snapshot.deviceId = pending.deviceId;
  snapshot.pending = pending;
  snapshot.active = false;
  ++snapshot.generation;
  return store(snapshot);
}

Status NvsMyAiCredentialStore::promoteBoundAtomically(
    const std::string& expectedPairingToken, const std::string& deviceId,
    const std::string& deviceToken, bool active) {
  if (!myai::isSixDigitCode(deviceId) || deviceToken.empty())
    return error(ErrorCode::InvalidArgument, "invalid bound MyAI credential");
  myai::CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  if (!snapshot.pending.valid() ||
      snapshot.pending.pairingToken != expectedPairingToken ||
      snapshot.pending.deviceId != deviceId) {
    return error(ErrorCode::Conflict, "pending MyAI pairing changed");
  }
  snapshot.deviceId = deviceId;
  snapshot.deviceToken = deviceToken;
  snapshot.active = active;
  snapshot.pending.clearSensitive();
  snapshot.pending = myai::PendingPairing();
  ++snapshot.generation;
  return store(snapshot);
}

Status NvsMyAiCredentialStore::clearPendingAtomically() {
  myai::CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  snapshot.pending.clearSensitive();
  snapshot.pending = myai::PendingPairing();
  if (snapshot.deviceToken.empty()) snapshot.deviceId.clear();
  ++snapshot.generation;
  return store(snapshot);
}

Status NvsMyAiCredentialStore::clearRuntimeCredentialAtomically() {
  myai::CredentialSnapshot snapshot;
  Status status = load(snapshot);
  if (!status.ok()) return status;
  snapshot.pending.clearSensitive();
  snapshot.pending = myai::PendingPairing();
  snapshot.deviceToken.assign(snapshot.deviceToken.size(), '\0');
  snapshot.deviceToken.clear();
  snapshot.deviceId.clear();
  snapshot.active = false;
  ++snapshot.generation;
  return store(snapshot);
}

Esp32MyAiWebSocket::Esp32MyAiWebSocket() {
  socket_.setReconnectInterval(0);
  socket_.enableHeartbeat(15000, 4000, 2);
  socket_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    onEvent(type, payload, length);
  });
}

Status Esp32MyAiWebSocket::connect(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    myai::IWebSocketListener& listener) {
  if (connected_ || listener_)
    return error(ErrorCode::InvalidState, "MyAI WebSocket already active");
  std::string host;
  std::string path;
  uint16_t port = 0;
  if (!Esp32PublicEndpointSecurity::parsePublicTlsUrl(url, host, port, &path) ||
      url.compare(0, 6, "wss://") != 0) {
    return error(ErrorCode::Security, "MyAI WebSocket requires public WSS");
  }
  extraHeaders_.clear();
  for (std::map<std::string, std::string>::const_iterator it = headers.begin();
       it != headers.end(); ++it) {
    if (it->first.empty() || it->second.find('\r') != std::string::npos ||
        it->second.find('\n') != std::string::npos) {
      return error(ErrorCode::InvalidArgument, "invalid WebSocket header");
    }
    extraHeaders_ += it->first + ": " + it->second + "\r\n";
  }
  listener_ = &listener;
  socket_.setExtraHeaders(extraHeaders_.c_str());
  socket_.beginSslWithBundle(host.c_str(), port, path.c_str(),
                             rootca_crt_bundle_start, nullptr);
  return Status::success();
}

Status Esp32MyAiWebSocket::sendText(const std::string& message) {
  if (!connected_ || message.empty())
    return error(ErrorCode::InvalidState, "MyAI WebSocket is not open");
  return socket_.sendTXT(
             reinterpret_cast<const uint8_t*>(message.data()), message.size())
             ? Status::success()
             : error(ErrorCode::Transport, "MyAI WebSocket text send failed");
}

Status Esp32MyAiWebSocket::sendBinary(const uint8_t* bytes, size_t length) {
  if (!connected_ || !bytes || length == 0)
    return error(ErrorCode::InvalidState, "MyAI WebSocket binary send rejected");
  return socket_.sendBIN(bytes, length)
             ? Status::success()
             : error(ErrorCode::Transport, "MyAI WebSocket audio send failed");
}

void Esp32MyAiWebSocket::close(uint16_t, const std::string&) {
  socket_.disconnect();
  connected_ = false;
  listener_ = nullptr;
  extraHeaders_.clear();
}

void Esp32MyAiWebSocket::loop() { socket_.loop(); }

void Esp32MyAiWebSocket::onEvent(WStype_t type, uint8_t* payload,
                                 size_t length) {
  myai::IWebSocketListener* listener = listener_;
  if (!listener) return;
  if (type == WStype_CONNECTED) {
    connected_ = true;
    listener->onWebSocketOpen();
  } else if (type == WStype_TEXT) {
    if (!acceptMyAiIngressFrame(
            MyAiIngressFrameKind::Text, payload, length)) {
      rejectIngress(listener, 1009, "text_frame_too_large");
      return;
    }
    listener->onWebSocketText(std::string(
        reinterpret_cast<const char*>(payload), length));
  } else if (type == WStype_BIN) {
    if (!acceptMyAiIngressFrame(
            MyAiIngressFrameKind::Audio, payload, length)) {
      rejectIngress(listener, 1009, "audio_frame_too_large");
      return;
    }
    listener->onWebSocketBinary(payload, length);
  } else if (type == WStype_FRAGMENT_TEXT_START ||
             type == WStype_FRAGMENT_BIN_START ||
             type == WStype_FRAGMENT || type == WStype_FRAGMENT_FIN) {
    // The MyAI gateway contract uses complete JSON or PCM frames. Reject
    // fragmentation so an attacker cannot bypass per-frame limits by making
    // the application accumulate an unbounded message.
    rejectIngress(listener, 1003, "fragmented_frame_rejected");
  } else if (type == WStype_DISCONNECTED) {
    connected_ = false;
    listener_ = nullptr;
    listener->onWebSocketClosed(1006, "transport_closed");
  } else if (type == WStype_ERROR) {
    connected_ = false;
    listener_ = nullptr;
    listener->onWebSocketClosed(1011, "transport_error");
  }
}

void Esp32MyAiWebSocket::rejectIngress(
    myai::IWebSocketListener* listener, uint16_t code,
    const char* safeReason) {
  connected_ = false;
  listener_ = nullptr;
  extraHeaders_.clear();
  socket_.disconnect();
  if (listener) listener->onWebSocketClosed(code, safeReason);
}

void PaperColorStreamingAudio::setVolume(uint8_t percent) {
  volumePercent_ = std::min<uint8_t>(percent, 100);
  M5.Speaker.setVolume(static_cast<uint8_t>(
      (static_cast<uint16_t>(volumePercent_) * 255U) / 100U));
}

Status PaperColorStreamingAudio::begin(uint32_t sampleRateHz,
                                       uint8_t channels) {
  if (pending_ || authorized_ || sampleRateHz < 8000 || sampleRateHz > 48000 ||
      (channels != 1 && channels != 2)) {
    return error(ErrorCode::InvalidState, "streaming TTS format/state rejected");
  }
  sampleRateHz_ = sampleRateHz;
  channels_ = channels;
  pending_ = true;
  return Status::success();
}

Status PaperColorStreamingAudio::authorize() {
  if (!pending_ || authorized_)
    return error(ErrorCode::InvalidState, "streaming TTS is not pending");
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  if (!M5.Speaker.isEnabled() && !M5.Speaker.begin()) {
    pending_ = false;
    return error(ErrorCode::Transport, "PaperColor speaker start failed");
  }
  setVolume(volumePercent_);
  authorized_ = true;
  pending_ = false;
  return Status::success();
}

bool PaperColorStreamingAudio::waitForPlayback(uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (M5.Speaker.isPlaying()) {
    if (millis() - started >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

Status PaperColorStreamingAudio::write(const uint8_t* bytes, size_t length) {
  if (!authorized_ || !bytes || length == 0 ||
      length > kMaximumMyAiWebSocketAudioFrameBytes || (length & 1U) != 0)
    return error(ErrorCode::InvalidState, "streaming TTS write rejected");
  if (!waitForPlayback(4000))
    return error(ErrorCode::Transport, "streaming TTS speaker timeout");
  playback_.resize(length / sizeof(int16_t));
  memcpy(playback_.data(), bytes, length);
  if (!M5.Speaker.playRaw(playback_.data(), playback_.size(), sampleRateHz_,
                          channels_ == 2, 1, 0, false)) {
    return error(ErrorCode::Transport, "streaming TTS playback failed");
  }
  return Status::success();
}

Status PaperColorStreamingAudio::end() {
  if (!pending_ && !authorized_) return Status::success();
  if (authorized_ && !waitForPlayback(10000)) {
    abort();
    return error(ErrorCode::Transport, "streaming TTS completion timeout");
  }
  pending_ = false;
  authorized_ = false;
  playback_.clear();
  if (endedCallback_) endedCallback_();
  return Status::success();
}

void PaperColorStreamingAudio::abort() {
  M5.Speaker.stop();
  pending_ = false;
  authorized_ = false;
  playback_.clear();
}

Status Esp32AigcOutputTransport::postAndDecodeBase64(
    const myai::HttpRequest& request, size_t maxEncodedBytes,
    size_t maxDecodedBytes, myai::IImageSink& sink,
    myai::AigcOutputMetadata& metadata) {
  if (maxEncodedBytes == 0 || maxDecodedBytes == 0 ||
      maxEncodedBytes > 8U * 1024U * 1024U ||
      maxDecodedBytes > 6U * 1024U * 1024U) {
    return error(ErrorCode::InvalidArgument, "invalid AIGC output cap");
  }
  JsonDocument requestBody;
  if (!deserializeJson(requestBody, request.body)) {
    metadata.promptId = requestBody["prompt_id"] | "";
    metadata.filename = requestBody["filename"] | "";
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!configureSecureHttp(request, client, http))
    return error(ErrorCode::Security, "secure AIGC output policy unavailable");
  const int httpStatus = sendHttpRequest(request, http);
  if (httpStatus < 200 || httpStatus >= 300) {
    http.end();
    return error(ErrorCode::Protocol, "AIGC output HTTP rejected", httpStatus);
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    return error(ErrorCode::Transport, "AIGC output stream unavailable");
  }
  std::string prefix;
  prefix.reserve(2048);
  const std::string marker = "\"content_base64\"";
  bool decoding = false;
  bool begun = false;
  bool complete = false;
  bool padded = false;
  size_t encoded = 0;
  size_t decoded = 0;
  size_t quartetLength = 0;
  size_t total = 0;
  char quartet[4];
  const uint32_t started = millis();
  Status result = Status::success();

  while (!complete && result.ok() &&
         (http.connected() || stream->available())) {
    if (!stream->available()) {
      if (millis() - started > request.timeoutMs) {
        result = error(ErrorCode::Transport, "AIGC output read timeout");
        break;
      }
      delay(1);
      continue;
    }
    const int next = stream->read();
    if (next < 0) continue;
    ++total;
    if (total > maxEncodedBytes + 8192U) {
      result = error(ErrorCode::TooLarge, "AIGC output envelope exceeds cap");
      break;
    }
    const char ch = static_cast<char>(next);
    if (!decoding) {
      if (prefix.size() >= 8192U) {
        result = error(ErrorCode::Protocol, "AIGC output metadata too large");
        break;
      }
      prefix.push_back(ch);
      const size_t markerAt = prefix.find(marker);
      if (markerAt == std::string::npos) continue;
      const size_t colon = prefix.find(':', markerAt + marker.size());
      const size_t quote =
          colon == std::string::npos ? std::string::npos
                                     : prefix.find('"', colon + 1);
      if (quote == std::string::npos) continue;
      if (!extractJsonString(prefix.substr(0, markerAt), "content_type",
                             metadata.contentType) ||
          (metadata.contentType != "image/png" &&
           metadata.contentType != "image/x-png")) {
        result = error(ErrorCode::Protocol,
                       "AIGC output content type missing or unsupported");
        break;
      }
      result = sink.begin(metadata);
      if (!result.ok()) break;
      begun = true;
      decoding = true;
      prefix.clear();
      continue;
    }

    if (ch == '"') {
      if (quartetLength != 0 || encoded == 0) {
        result = error(ErrorCode::Protocol, "truncated AIGC base64 payload");
      } else {
        complete = true;
      }
      break;
    }
    if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
    if (base64Value(ch) == -1 || encoded >= maxEncodedBytes) {
      result = encoded >= maxEncodedBytes
                   ? error(ErrorCode::TooLarge, "AIGC encoded image exceeds cap")
                   : error(ErrorCode::Protocol, "invalid AIGC base64 character");
      break;
    }
    quartet[quartetLength++] = ch;
    ++encoded;
    if (quartetLength == 4) {
      result = decodeQuartet(quartet, sink, maxDecodedBytes, decoded, padded);
      quartetLength = 0;
    }
  }
  http.end();
  if (!result.ok() || !complete || !begun) {
    sink.abort();
    return result.ok()
               ? error(ErrorCode::Protocol, "AIGC output payload incomplete")
               : result;
  }
  metadata.decodedBytes = decoded;
  result = sink.commit(metadata);
  if (!result.ok()) sink.abort();
  return result;
}

Status AlbumImageSink::begin(const myai::AigcOutputMetadata&) {
  abort();
  if (maximumBytes_ < 24 || maximumBytes_ > 6U * 1024U * 1024U)
    return error(ErrorCode::InvalidArgument, "AIGC album cap invalid");
  bytes_ = static_cast<uint8_t*>(
      heap_caps_malloc(maximumBytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!bytes_) bytes_ = static_cast<uint8_t*>(malloc(maximumBytes_));
  if (!bytes_)
    return error(ErrorCode::Storage, "AIGC album staging allocation failed");
  return Status::success();
}

Status AlbumImageSink::write(const uint8_t* bytes, size_t length) {
  if (!bytes_ || !bytes || length == 0 || length > maximumBytes_ - length_)
    return error(length > maximumBytes_ - length_ ? ErrorCode::TooLarge
                                                   : ErrorCode::InvalidState,
                 "AIGC album staging write rejected");
  memcpy(bytes_ + length_, bytes, length);
  length_ += length;
  return Status::success();
}

Status AlbumImageSink::commit(myai::AigcOutputMetadata& metadata) {
  if (!bytes_ || length_ < 24 ||
      memcmp(bytes_, "\x89PNG\r\n\x1a\n", 8) != 0) {
    return error(ErrorCode::Protocol, "AIGC output is not a PNG");
  }
  const uint32_t width = (static_cast<uint32_t>(bytes_[16]) << 24) |
      (static_cast<uint32_t>(bytes_[17]) << 16) |
      (static_cast<uint32_t>(bytes_[18]) << 8) | bytes_[19];
  const uint32_t height = (static_cast<uint32_t>(bytes_[20]) << 24) |
      (static_cast<uint32_t>(bytes_[21]) << 16) |
      (static_cast<uint32_t>(bytes_[22]) << 8) | bytes_[23];
  const bool landscape = width == 600 && height == 400;
  if (!landscape && (width != 400 || height != 600))
    return error(ErrorCode::Protocol,
                 "AIGC output does not match PaperColor dimensions");

  DownloadedFrame frame;
  frame.bytes = bytes_;
  frame.length = length_;
  frame.landscape = landscape;
  AlbumAsset asset;
  const String origin = String("myai:") + metadata.promptId.c_str();
  if (!album_.cacheFrame(
          frame, "", origin, "official-quality", MetadataBudget(0, 0), asset))
    return error(ErrorCode::Storage, "AIGC album transaction failed");
  committed_ = asset;
  hasCommitted_ = true;
  metadata.decodedBytes = length_;
  free(bytes_);
  bytes_ = nullptr;
  length_ = 0;
  return Status::success();
}

void AlbumImageSink::abort() {
  if (bytes_) free(bytes_);
  bytes_ = nullptr;
  length_ = 0;
}

bool AlbumImageSink::takeCommittedAsset(AlbumAsset& asset) {
  if (!hasCommitted_) return false;
  asset = committed_;
  committed_ = AlbumAsset{};
  hasCommitted_ = false;
  return true;
}

}  // namespace inkloop
