#include "PaperColorMyAiAdapters.h"

#include "Diagnostics.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <ctime>
#include <limits>

#include "Diagnostics.h"
#include "MyAiCredentialPersistencePrimitives.h"
#include "ResponsiveWorkExecutor.h"

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

bool base64Encode(const uint8_t* bytes, size_t length, std::string& output) {
  if (!bytes || !length) return false;
  const size_t capacity = 4U * ((length + 2U) / 3U) + 1U;
  std::vector<uint8_t> encoded(capacity);
  size_t written = 0;
  if (mbedtls_base64_encode(
          encoded.data(), encoded.size(), &written, bytes, length) != 0 ||
      written == 0 || written >= encoded.size()) {
    return false;
  }
  output.assign(reinterpret_cast<const char*>(encoded.data()), written);
  return true;
}

std::string lowerAscii(const std::string& value) {
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(), ::tolower);
  return output;
}

std::string trimAscii(const std::string& value) {
  size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t')) ++first;
  size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t')) --last;
  return value.substr(first, last - first);
}

bool websocketHeaderValue(
    const std::string& response, const char* requested,
    std::string& value) {
  const std::string wanted = lowerAscii(requested);
  size_t lineAt = response.find("\r\n") + 2U;
  while (lineAt >= 2U && lineAt < response.size()) {
    const size_t lineEnd = response.find("\r\n", lineAt);
    if (lineEnd == std::string::npos || lineEnd == lineAt) break;
    const size_t colon = response.find(':', lineAt);
    if (colon != std::string::npos && colon < lineEnd &&
        lowerAscii(response.substr(lineAt, colon - lineAt)) == wanted) {
      value = trimAscii(response.substr(colon + 1U, lineEnd - colon - 1U));
      return true;
    }
    lineAt = lineEnd + 2U;
  }
  return false;
}

bool websocketAcceptForKey(const std::string& key, std::string& accept) {
  static const char kWebSocketGuid[] =
      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::string source = key + kWebSocketGuid;
  uint8_t digest[20];
  if (mbedtls_sha1_ret(
          reinterpret_cast<const uint8_t*>(source.data()), source.size(),
          digest) != 0) {
    return false;
  }
  return base64Encode(digest, sizeof(digest), accept);
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

// HTTPClient::getStreamPtr() exposes the underlying TCP stream. When the
// gateway uses Transfer-Encoding: chunked, reading that pointer directly also
// reads chunk-size lines; hexadecimal size characters are valid Base64 and
// silently corrupt the PNG. Feed a bounded incremental decoder through
// HTTPClient::writeToStream() instead, because that API removes HTTP framing
// before delivering JSON bytes.
class AigcJsonBase64Decoder final : public Stream {
 public:
  AigcJsonBase64Decoder(
      size_t maximumEncoded, size_t maximumDecoded,
      myai::IImageSink& sink, myai::AigcOutputMetadata& metadata)
      : maximumEncoded_(maximumEncoded),
        maximumDecoded_(maximumDecoded),
        sink_(sink),
        metadata_(metadata) {
    prefix_.reserve(2048);
  }

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* bytes, size_t length) override {
    if (!bytes || !length || !result_.ok()) return 0;
    size_t consumed = 0;
    while (consumed < length) {
      if (!consume(static_cast<char>(bytes[consumed]))) {
        setWriteError();
        break;
      }
      ++consumed;
    }
    return consumed;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  const Status& result() const { return result_; }
  bool begun() const { return begun_; }
  bool complete() const { return complete_; }
  size_t decodedBytes() const { return decoded_; }

 private:
  bool consume(char ch) {
    if (complete_) return true;
    if (++envelopeBytes_ > maximumEncoded_ + 8192U) {
      result_ = error(ErrorCode::TooLarge, "AIGC output envelope exceeds cap");
      return false;
    }
    if (!decoding_) {
      if (prefix_.size() >= 8192U) {
        result_ = error(ErrorCode::Protocol, "AIGC output metadata too large");
        return false;
      }
      prefix_.push_back(ch);
      static const std::string marker = "\"content_base64\"";
      const size_t markerAt = prefix_.find(marker);
      if (markerAt == std::string::npos) return true;
      const size_t colon = prefix_.find(':', markerAt + marker.size());
      const size_t quote = colon == std::string::npos
          ? std::string::npos : prefix_.find('"', colon + 1);
      if (quote == std::string::npos) return true;
      if (!extractJsonString(
              prefix_.substr(0, markerAt), "content_type",
              metadata_.contentType) ||
          (metadata_.contentType != "image/png" &&
           metadata_.contentType != "image/x-png")) {
        result_ = error(
            ErrorCode::Protocol,
            "AIGC output content type missing or unsupported");
        return false;
      }
      result_ = sink_.begin(metadata_);
      if (!result_.ok()) return false;
      begun_ = true;
      decoding_ = true;
      prefix_.clear();
      return true;
    }

    if (ch == '"') {
      if (encoded_ == 0 || quartetLength_ == 1) {
        result_ = error(
            ErrorCode::Protocol, "truncated AIGC base64 payload");
        return false;
      }
      if (quartetLength_ == 2 || quartetLength_ == 3) {
        while (quartetLength_ < 4) quartet_[quartetLength_++] = '=';
        result_ = decodeQuartet(
            quartet_, sink_, maximumDecoded_, decoded_, padded_);
        if (!result_.ok()) return false;
      }
      complete_ = true;
      return true;
    }
    if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') return true;
    if (base64Value(ch) == -1 || encoded_ >= maximumEncoded_) {
      result_ = encoded_ >= maximumEncoded_
          ? error(ErrorCode::TooLarge, "AIGC encoded image exceeds cap")
          : error(ErrorCode::Protocol, "invalid AIGC base64 character");
      return false;
    }
    quartet_[quartetLength_++] = ch;
    ++encoded_;
    if (quartetLength_ == 4) {
      result_ = decodeQuartet(
          quartet_, sink_, maximumDecoded_, decoded_, padded_);
      quartetLength_ = 0;
      if (!result_.ok()) return false;
    }
    return true;
  }

  size_t maximumEncoded_;
  size_t maximumDecoded_;
  myai::IImageSink& sink_;
  myai::AigcOutputMetadata& metadata_;
  Status result_ = Status::success();
  std::string prefix_;
  bool decoding_ = false;
  bool begun_ = false;
  bool complete_ = false;
  bool padded_ = false;
  size_t encoded_ = 0;
  size_t decoded_ = 0;
  size_t envelopeBytes_ = 0;
  size_t quartetLength_ = 0;
  char quartet_[4]{};
};

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

  struct DnsWork {
    std::string host;
    Status status;
  } work{host, Status::success()};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::MyAiNetwork,
      [](void* raw) {
        DnsWork* work = static_cast<DnsWork*>(raw);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int resolved = getaddrinfo(
            work->host.c_str(), nullptr, &hints, &addresses);
        if (resolved != 0 || !addresses) {
          work->status = error(
              ErrorCode::Transport,
              "public endpoint DNS resolution failed");
          return;
        }
        bool found = false;
        bool valid = true;
        for (addrinfo* next = addresses; next; next = next->ai_next) {
          found = true;
          if (!Esp32PublicEndpointSecurity::publicAddress(next->ai_addr))
            valid = false;
        }
        freeaddrinfo(addresses);
        if (!found || !valid) {
          work->status = error(
              ErrorCode::Security,
              "non-public endpoint address rejected");
        }
      },
      &work);
  return dispatched
      ? work.status
      : error(ErrorCode::Transport, "responsive I/O worker busy");
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
  struct HttpWork {
    const myai::HttpRequest* request;
    myai::HttpResponse* response;
    Status status;
  } work{&request, &response, Status::success()};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::MyAiNetwork,
      [](void* raw) {
        HttpWork* work = static_cast<HttpWork*>(raw);
        WiFiClientSecure client;
        HTTPClient http;
        if (!configureSecureHttp(*work->request, client, http)) {
          work->status = error(
              ErrorCode::Security, "secure HTTP policy unavailable");
          return;
        }
        const int status = sendHttpRequest(*work->request, http);
        if (status <= 0) {
          http.end();
          work->status = error(
              ErrorCode::Transport, "MyAI HTTPS request failed");
          return;
        }
        work->response->status = status;
        if (work->request->maxResponseBytes != 0 &&
            work->request->method != "HEAD") {
          work->status = Esp32HttpsTransport::readBounded(
              http, work->request->maxResponseBytes,
              work->response->body);
        }
        http.end();
      },
      &work);
  return dispatched
      ? work.status
      : error(ErrorCode::Transport, "responsive I/O worker busy");
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

bool Esp32MyAiWebSocket::writeAll(const uint8_t* bytes, size_t length) {
  if (!bytes || length == 0) return false;
  const uint32_t startedAt = millis();
  size_t written = 0;
  while (written < length) {
    const size_t next = client_.write(bytes + written, length - written);
    if (next > 0) {
      written += next;
      continue;
    }
    if (millis() - startedAt >= 5000U) return false;
    delay(1);
  }
  return true;
}

bool Esp32MyAiWebSocket::sendMaskedFrame(
    uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!connected_ || (length && !payload) || length > 65535U) {
    return false;
  }

  uint8_t header[8] = {};
  size_t headerLength = 0;
  header[headerLength++] = static_cast<uint8_t>(0x80U | (opcode & 0x0fU));
  if (length < 126U) {
    header[headerLength++] = static_cast<uint8_t>(0x80U | length);
  } else {
    header[headerLength++] = 0x80U | 126U;
    header[headerLength++] = static_cast<uint8_t>((length >> 8U) & 0xffU);
    header[headerLength++] = static_cast<uint8_t>(length & 0xffU);
  }
  const uint32_t randomMask = esp_random();
  const uint8_t mask[4] = {
      static_cast<uint8_t>(randomMask >> 24U),
      static_cast<uint8_t>(randomMask >> 16U),
      static_cast<uint8_t>(randomMask >> 8U),
      static_cast<uint8_t>(randomMask)};
  for (size_t index = 0; index < sizeof(mask); ++index)
    header[headerLength++] = mask[index];
  if (!writeAll(header, headerLength)) return false;

  uint8_t chunk[512];
  size_t offset = 0;
  while (offset < length) {
    const size_t chunkLength =
        std::min<size_t>(sizeof(chunk), length - offset);
    for (size_t index = 0; index < chunkLength; ++index)
      chunk[index] = payload[offset + index] ^ mask[(offset + index) & 3U];
    if (!writeAll(chunk, chunkLength)) return false;
    offset += chunkLength;
  }
  return true;
}

Esp32MyAiWebSocket::Esp32MyAiWebSocket() { receiveBuffer_.reserve(16384); }

bool Esp32MyAiWebSocket::performHandshake(
    const std::string& host, uint16_t port, const std::string& path,
    const std::map<std::string, std::string>& headers) {
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  std::string key;
  std::string expectedAccept;
  if (!base64Encode(nonce, sizeof(nonce), key) ||
      !websocketAcceptForKey(key, expectedAccept)) return false;

  client_.stop();
  client_.setCACertBundle(rootca_crt_bundle_start);
  client_.setHandshakeTimeout(15);
  // WiFiClientSecure also feeds Stream::_timeout into the TLS socket select.
  // Keeping this at one second made the nominal 15-second handshake budget a
  // lie and caused healthy public gateways to fail whenever their TLS setup
  // took slightly longer than one second.
  client_.setTimeout(15);
  Diagnostics::event("MYAI_WS_HANDSHAKE", "TLS_CONNECTING");
  if (!client_.connect(host.c_str(), port)) {
    Diagnostics::event("MYAI_WS_HANDSHAKE", "TLS_FAILED");
    return false;
  }
  Diagnostics::event("MYAI_WS_HANDSHAKE", "TLS_READY");

  std::string request = "GET " + (path.empty() ? std::string("/") : path) +
      " HTTP/1.1\r\nHost: " + host;
  if (port != 443U) request += ":" + std::to_string(port);
  request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + key + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  for (std::map<std::string, std::string>::const_iterator it = headers.begin();
       it != headers.end(); ++it) {
    request += it->first + ": " + it->second + "\r\n";
  }
  request += "\r\n";
  if (!writeAll(reinterpret_cast<const uint8_t*>(request.data()),
                request.size())) {
    Diagnostics::event("MYAI_WS_HANDSHAKE", "REQUEST_FAILED");
    return false;
  }

  std::string response;
  response.reserve(2048);
  const uint32_t deadline = millis() + 15000U;
  while (response.find("\r\n\r\n") == std::string::npos &&
         static_cast<int32_t>(millis() - deadline) < 0) {
    while (client_.available() > 0) {
      const int next = client_.read();
      if (next < 0) break;
      if (response.size() >= 8192U) {
        Diagnostics::event("MYAI_WS_HANDSHAKE", "RESPONSE_TOO_LARGE");
        return false;
      }
      response.push_back(static_cast<char>(next));
    }
    if (response.find("\r\n\r\n") == std::string::npos) delay(1);
  }

  const size_t headerEnd = response.find("\r\n\r\n");
  std::string upgrade;
  std::string connection;
  std::string accept;
  const bool valid = headerEnd != std::string::npos &&
      (response.compare(0, 12, "HTTP/1.1 101") == 0 ||
       response.compare(0, 10, "HTTP/1 101") == 0) &&
      websocketHeaderValue(response, "upgrade", upgrade) &&
      lowerAscii(upgrade) == "websocket" &&
      websocketHeaderValue(response, "connection", connection) &&
      lowerAscii(connection).find("upgrade") != std::string::npos &&
      websocketHeaderValue(response, "sec-websocket-accept", accept) &&
      accept == expectedAccept;
  if (!valid) {
    Diagnostics::event("MYAI_WS_HANDSHAKE", "UPGRADE_REJECTED");
    return false;
  }
  Diagnostics::event("MYAI_WS_HANDSHAKE", "UPGRADE_READY");
  return true;
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
  for (std::map<std::string, std::string>::const_iterator it = headers.begin();
       it != headers.end(); ++it) {
    if (it->first.empty() || it->first.find(':') != std::string::npos ||
        it->first.find('\r') != std::string::npos ||
        it->first.find('\n') != std::string::npos ||
        it->second.find('\r') != std::string::npos ||
        it->second.find('\n') != std::string::npos) {
      return error(ErrorCode::InvalidArgument, "invalid WebSocket header");
    }
  }
  listener_ = &listener;
  receiveBuffer_.clear();
  struct HandshakeWork {
    Esp32MyAiWebSocket* socket;
    const std::string* host;
    uint16_t port;
    const std::string* path;
    const std::map<std::string, std::string>* headers;
    bool connected;
  } work{this, &host, port, &path, &headers, false};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::WebSocketHandshake,
      [](void* raw) {
        HandshakeWork* work = static_cast<HandshakeWork*>(raw);
        work->connected = work->socket->performHandshake(
            *work->host, work->port, *work->path, *work->headers);
      },
      &work);
  if (!dispatched || !work.connected) {
    client_.stop();
    listener_ = nullptr;
    return error(ErrorCode::Transport, "MyAI WebSocket handshake failed");
  }
  connected_ = true;
  openPending_ = true;
  return Status::success();
}

Status Esp32MyAiWebSocket::sendText(const std::string& message) {
  if (!connected_ || message.empty())
    return error(ErrorCode::InvalidState, "MyAI WebSocket is not open");
  Diagnostics::event(
      "MYAI_WS_SEND",
      String(static_cast<unsigned long>(message.size())) + ":" +
          (connected_ ? "SOCKET_OPEN" : "SOCKET_CLOSED"));
  const bool sent = sendMaskedFrame(
      0x1U, reinterpret_cast<const uint8_t*>(message.data()), message.size());
  Diagnostics::event("MYAI_WS_SEND", sent ? "OK" : "FAILED");
  return sent ? Status::success()
              : error(ErrorCode::Transport, "MyAI WebSocket text send failed");
}

Status Esp32MyAiWebSocket::sendBinary(const uint8_t* bytes, size_t length) {
  if (!connected_ || !bytes || length == 0)
    return error(ErrorCode::InvalidState, "MyAI WebSocket binary send rejected");
  return sendMaskedFrame(0x2U, bytes, length)
             ? Status::success()
             : error(ErrorCode::Transport, "MyAI WebSocket audio send failed");
}

void Esp32MyAiWebSocket::close(uint16_t code, const std::string& reason) {
  if (connected_) {
    const size_t reasonLength = std::min<size_t>(reason.size(), 123U);
    uint8_t payload[125];
    payload[0] = static_cast<uint8_t>(code >> 8U);
    payload[1] = static_cast<uint8_t>(code & 0xffU);
    if (reasonLength) memcpy(payload + 2U, reason.data(), reasonLength);
    sendMaskedFrame(0x8U, payload, reasonLength + 2U);
  }
  client_.stop();
  connected_ = false;
  openPending_ = false;
  listener_ = nullptr;
  receiveBuffer_.clear();
}

void Esp32MyAiWebSocket::loop() {
  if (!listener_) return;
  if (openPending_) {
    openPending_ = false;
    myai::IWebSocketListener* listener = listener_;
    Diagnostics::event("MYAI_WS", "OPEN");
    listener->onWebSocketOpen();
    if (!listener_ || !connected_) return;
  }

  // Consume at most one complete frame per cooperative iteration. Draining
  // the entire TCP burst used to enqueue TTS much faster than the speaker can
  // play it. Leaving bytes in lwIP applies natural TCP backpressure while the
  // main loop continues to service buttons and audio every few milliseconds.
  if (listener_ && parseOneFrame()) return;
  uint8_t chunk[2048];
  if (client_.available() > 0) {
    const int count = client_.read(chunk, sizeof(chunk));
    if (count > 0) {
      if (receiveBuffer_.size() + static_cast<size_t>(count) >
          kMaximumMyAiWebSocketTextFrameBytes + 14U) {
        rejectIngress(listener_, 1009, "receive_buffer_too_large");
        return;
      }
      receiveBuffer_.insert(receiveBuffer_.end(), chunk, chunk + count);
      if (listener_) (void)parseOneFrame();
    }
  }
  if (listener_ && connected_ && client_.available() == 0 &&
      !client_.connected()) {
    notifyClosed(1006, "transport_closed");
  }
}

bool Esp32MyAiWebSocket::parseOneFrame() {
  if (receiveBuffer_.size() < 2U) return false;
  const uint8_t first = receiveBuffer_[0];
  const uint8_t second = receiveBuffer_[1];
  const bool final = (first & 0x80U) != 0;
  const uint8_t opcode = first & 0x0fU;
  if ((first & 0x70U) != 0 || (second & 0x80U) != 0) {
    rejectIngress(listener_, 1002, "invalid_server_frame");
    return false;
  }
  uint64_t length = second & 0x7fU;
  size_t headerLength = 2U;
  if (length == 126U) {
    if (receiveBuffer_.size() < 4U) return false;
    length = (static_cast<uint64_t>(receiveBuffer_[2]) << 8U) |
        receiveBuffer_[3];
    headerLength = 4U;
  } else if (length == 127U) {
    if (receiveBuffer_.size() < 10U) return false;
    length = 0;
    for (size_t index = 2; index < 10U; ++index)
      length = (length << 8U) | receiveBuffer_[index];
    headerLength = 10U;
  }
  const bool control = (opcode & 0x08U) != 0;
  if ((!final && (opcode == 0x1U || opcode == 0x2U || control)) ||
      opcode == 0x0U || length > kMaximumMyAiWebSocketTextFrameBytes ||
      (control && length > 125U)) {
    rejectIngress(listener_, control ? 1002 : 1009,
                  !final ? "fragmented_frame_rejected" :
                  "frame_too_large_or_invalid");
    return false;
  }
  if (length > static_cast<uint64_t>(SIZE_MAX - headerLength) ||
      receiveBuffer_.size() < headerLength + static_cast<size_t>(length)) {
    return false;
  }
  const uint8_t* payload = receiveBuffer_.data() + headerLength;
  const size_t payloadLength = static_cast<size_t>(length);
  myai::IWebSocketListener* listener = listener_;
  if (opcode == 0x1U) {
    if (!acceptMyAiIngressFrame(
            MyAiIngressFrameKind::Text, payload, payloadLength)) {
      rejectIngress(listener, 1009, "text_frame_too_large");
      return false;
    }
    listener->onWebSocketText(std::string(
        reinterpret_cast<const char*>(payload), payloadLength));
  } else if (opcode == 0x2U) {
    if (!acceptMyAiIngressFrame(
            MyAiIngressFrameKind::Audio, payload, payloadLength)) {
      rejectIngress(listener, 1009, "audio_frame_too_large");
      return false;
    }
    listener->onWebSocketBinary(payload, payloadLength);
  } else if (opcode == 0x8U) {
    uint16_t code = 1000;
    if (payloadLength == 1U) {
      rejectIngress(listener, 1002, "invalid_close_frame");
      return false;
    }
    if (payloadLength >= 2U)
      code = static_cast<uint16_t>((payload[0] << 8U) | payload[1]);
    notifyClosed(code, "server_closed");
    return false;
  } else if (opcode == 0x9U) {
    if (!sendMaskedFrame(0xAU, payload, payloadLength)) {
      notifyClosed(1006, "pong_send_failed");
      return false;
    }
  } else if (opcode != 0xAU) {
    rejectIngress(listener, 1003, "unsupported_frame_type");
    return false;
  }
  if (!listener_) return false;
  receiveBuffer_.erase(
      receiveBuffer_.begin(),
      receiveBuffer_.begin() + headerLength + payloadLength);
  return true;
}

void Esp32MyAiWebSocket::notifyClosed(
    uint16_t code, const char* safeReason) {
  myai::IWebSocketListener* listener = listener_;
  connected_ = false;
  openPending_ = false;
  listener_ = nullptr;
  receiveBuffer_.clear();
  client_.stop();
  Diagnostics::event("MYAI_WS", "CLOSED");
  if (listener) listener->onWebSocketClosed(code, safeReason);
}

void Esp32MyAiWebSocket::rejectIngress(
    myai::IWebSocketListener* listener, uint16_t code,
    const char* safeReason) {
  connected_ = false;
  openPending_ = false;
  listener_ = nullptr;
  receiveBuffer_.clear();
  client_.stop();
  if (listener) listener->onWebSocketClosed(code, safeReason);
}

void PaperColorStreamingAudio::setVolume(uint8_t percent) {
  volumePercent_ = std::min<uint8_t>(percent, 100);
  M5.Speaker.setVolume(static_cast<uint8_t>(
      (static_cast<uint16_t>(volumePercent_) * 255U) / 100U));
}

Status PaperColorStreamingAudio::begin(uint32_t sampleRateHz,
                                       uint8_t channels) {
  // A gateway may split one logical answer into adjacent tts.start/tts.stop
  // segments. If the preceding segment is still draining, keep the same
  // queue and speaker channel instead of introducing a stop/start gap.
  if (ending_ && authorized_ && sampleRateHz_ == sampleRateHz &&
      channels_ == channels) {
    ending_ = false;
    authorized_ = false;
    pending_ = true;
    return Status::success();
  }
  if (pending_ || authorized_ || ending_ || queuedBytes_ != 0 ||
      !speakerIdle() || sampleRateHz < 8000 || sampleRateHz > 48000 ||
      (channels != 1 && channels != 2)) {
    return error(ErrorCode::InvalidState, "streaming TTS format/state rejected");
  }
  if (!ensureQueue())
    return error(ErrorCode::Transport, "streaming TTS queue unavailable");
  sampleRateHz_ = sampleRateHz;
  channels_ = channels;
  queueRead_ = 0;
  queueWrite_ = 0;
  queuedBytes_ = 0;
  playbackCursor_ = 0;
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

bool PaperColorStreamingAudio::ensureQueue() {
  if (queue_) return true;
  queue_ = static_cast<uint8_t*>(heap_caps_malloc(
      kPreferredQueueBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  queueCapacity_ = queue_ ? kPreferredQueueBytes : 0;
  if (!queue_) {
    queue_ = static_cast<uint8_t*>(
        heap_caps_malloc(kFallbackQueueBytes, MALLOC_CAP_8BIT));
    queueCapacity_ = queue_ ? kFallbackQueueBytes : 0;
  }
  return queue_ != nullptr;
}

bool PaperColorStreamingAudio::speakerIdle() const {
  return M5.Speaker.isPlaying(kSpeakerChannel) == 0;
}

bool PaperColorStreamingAudio::appendQueued(
    const uint8_t* bytes, size_t length) {
  if (!queue_ || length > queueCapacity_ - queuedBytes_) return false;
  const size_t first = std::min(length, queueCapacity_ - queueWrite_);
  memcpy(queue_ + queueWrite_, bytes, first);
  if (length > first) memcpy(queue_, bytes + first, length - first);
  queueWrite_ = (queueWrite_ + length) % queueCapacity_;
  queuedBytes_ += length;
  return true;
}

bool PaperColorStreamingAudio::pumpPlayback() {
  if (!authorized_) return true;
  const size_t sampleBytes = sizeof(int16_t) * channels_;
  while (queuedBytes_ >= sampleBytes &&
         M5.Speaker.isPlaying(kSpeakerChannel) < 2) {
    size_t bytes = std::min(queuedBytes_, kPlaybackChunkBytes);
    bytes -= bytes % sampleBytes;
    if (!bytes) break;

    std::vector<int16_t>& buffer = playback_[playbackCursor_];
    buffer.resize(bytes / sizeof(int16_t));
    uint8_t* destination = reinterpret_cast<uint8_t*>(buffer.data());
    const size_t first = std::min(bytes, queueCapacity_ - queueRead_);
    memcpy(destination, queue_ + queueRead_, first);
    if (bytes > first) memcpy(destination + first, queue_, bytes - first);

    if (!M5.Speaker.playRaw(
            buffer.data(), buffer.size(), sampleRateHz_, channels_ == 2,
            1, kSpeakerChannel, false)) {
      return false;
    }
    queueRead_ = (queueRead_ + bytes) % queueCapacity_;
    queuedBytes_ -= bytes;
    playbackCursor_ = (playbackCursor_ + 1) % kPlaybackBufferCount;
  }
  return true;
}

Status PaperColorStreamingAudio::write(const uint8_t* bytes, size_t length) {
  if (!authorized_ || ending_ || !bytes || length == 0 ||
      length > kMaximumMyAiWebSocketAudioFrameBytes ||
      length % (sizeof(int16_t) * channels_) != 0)
    return error(ErrorCode::InvalidState, "streaming TTS write rejected");
  if (!appendQueued(bytes, length))
    return error(ErrorCode::Transport, "streaming TTS queue full");
  if (!pumpPlayback()) {
    return error(ErrorCode::Transport, "streaming TTS playback failed");
  }
  return Status::success();
}

Status PaperColorStreamingAudio::end() {
  if (!pending_ && !authorized_ && !ending_) return Status::success();
  pending_ = false;
  if (!authorized_) {
    finishPlayback();
    return Status::success();
  }
  ending_ = true;
  if (!pumpPlayback()) {
    abort();
    return error(ErrorCode::Transport, "streaming TTS playback failed");
  }
  if (queuedBytes_ == 0 && speakerIdle()) finishPlayback();
  return Status::success();
}

void PaperColorStreamingAudio::poll() {
  if (!authorized_) return;
  if (!pumpPlayback()) {
    Diagnostics::event("ERROR", "STREAMING_TTS_PLAYBACK_FAILED");
    abort();
    return;
  }
  if (ending_ && queuedBytes_ == 0 && speakerIdle()) finishPlayback();
}

void PaperColorStreamingAudio::finishPlayback() {
  pending_ = false;
  authorized_ = false;
  ending_ = false;
  queueRead_ = 0;
  queueWrite_ = 0;
  queuedBytes_ = 0;
  if (endedCallback_) endedCallback_();
}

void PaperColorStreamingAudio::abort() {
  M5.Speaker.stop(kSpeakerChannel);
  pending_ = false;
  authorized_ = false;
  ending_ = false;
  queueRead_ = 0;
  queueWrite_ = 0;
  queuedBytes_ = 0;
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

  struct ImageWork {
    const myai::HttpRequest* request;
    size_t maxEncodedBytes;
    size_t maxDecodedBytes;
    myai::IImageSink* sink;
    myai::AigcOutputMetadata* metadata;
    Status status;
  } work{
      &request, maxEncodedBytes, maxDecodedBytes, &sink, &metadata,
      Status::success()};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::MyAiImageStream,
      [](void* raw) {
        ImageWork* work = static_cast<ImageWork*>(raw);
        WiFiClientSecure client;
        HTTPClient http;
        if (!configureSecureHttp(*work->request, client, http)) {
          work->status = error(
              ErrorCode::Security,
              "secure AIGC output policy unavailable");
          return;
        }
        const int httpStatus = sendHttpRequest(*work->request, http);
        if (httpStatus < 200 || httpStatus >= 300) {
          http.end();
          work->status = error(
              ErrorCode::Protocol, "AIGC output HTTP rejected", httpStatus);
          return;
        }
        AigcJsonBase64Decoder decoder(
            work->maxEncodedBytes, work->maxDecodedBytes,
            *work->sink, *work->metadata);
        const int transferred = http.writeToStream(&decoder);
        http.end();
        work->status = decoder.result();
        if (work->status.ok() && transferred < 0) {
          work->status = error(
              ErrorCode::Transport, "AIGC output read failed");
        }
        if (!work->status.ok() || !decoder.complete() || !decoder.begun()) {
          work->sink->abort();
          if (work->status.ok()) {
            work->status = error(
                ErrorCode::Protocol, "AIGC output payload incomplete");
          }
          return;
        }
        work->metadata->decodedBytes = decoder.decodedBytes();
      },
      &work);
  if (!dispatched) {
    sink.abort();
    return error(ErrorCode::Transport, "responsive I/O worker busy");
  }
  if (!work.status.ok()) return work.status;
  // Album/SD mutation remains on the sole product-state owner. The worker
  // only decoded into bounded PSRAM, so Portal storage snapshots cannot race
  // an SD catalog transaction.
  work.status = sink.commit(metadata);
  if (!work.status.ok()) sink.abort();
  return work.status;
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
  Diagnostics::event(
      "AIGC_IMAGE_META",
      String(width) + "x" + String(height) + ":" +
          String(static_cast<unsigned long>(length_)));

  DownloadedFrame frame;
  frame.bytes = bytes_;
  frame.length = length_;
  frame.landscape = landscape;
  AlbumAsset asset;
  const String origin = String("myai:") + metadata.promptId.c_str();
  struct CacheWork {
    AlbumStore* album;
    const DownloadedFrame* frame;
    const String* origin;
    AlbumAsset* asset;
    bool committed;
  } work{&album_, &frame, &origin, &asset, false};
  const bool dispatched = responsiveWorkExecutor().execute(
      ResponsiveWorkKind::StorageHardware,
      [](void* raw) {
        CacheWork* item = static_cast<CacheWork*>(raw);
        item->committed = item->album->cacheFrame(
            *item->frame, "", *item->origin, "official-quality",
            MetadataBudget(0, 0), *item->asset);
      },
      &work);
  if (!dispatched || !work.committed)
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
