#include "InkloopClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "AppConfig.h"
#include "Diagnostics.h"
#include "InkloopPairingPrimitives.h"

extern "C" char inkloop_api_url_slot[192];

namespace inkloop {

namespace {

// Inkloop's public endpoint currently chains through Let's Encrypt / ISRG
// Root X1, while older deployments used Google Trust Services. Keep both long-lived
// trust anchors so a routine certificate renewal cannot strand an already
// provisioned device with HTTPClient error -1.
constexpr char kRootCa[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

}  // namespace

void DownloadedFrame::release() {
  free(bytes);
  bytes = nullptr;
  length = 0;
  landscape = false;
}

String InkloopClient::hexBytes(const uint8_t* bytes, size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 0x0f];
  }
  return result;
}

bool InkloopClient::beginIdentity() {
  const uint64_t mac = ESP.getEfuseMac();
  char id[24];
  snprintf(id, sizeof(id), "M5PC-%012llX", static_cast<unsigned long long>(mac));
  hardwareId_ = id;
  deviceId_ = "";
  deviceSecret_ = "";
  appliedRevision_ = 0;

  preferences_.end();
  if (!preferences_.begin("inkloop", false)) {
    Diagnostics::event("ERROR", "IDENTITY_NVS_OPEN_FAILED");
    return false;
  }
  deviceId_ = preferences_.getString("device-id", "");
  deviceSecret_ = preferences_.getString("secret", "");
  appliedRevision_ = preferences_.getUInt("revision", 0);
  if (deviceSecret_.length() != 64) {
    uint8_t secret[32];
    esp_fill_random(secret, sizeof(secret));
    const String nextSecret = hexBytes(secret, sizeof(secret));
    preferences_.remove("device-id");
    const bool deviceIdCleared = preferences_.getString("device-id", "").isEmpty();
    const bool revisionSaved = preferences_.putUInt("revision", 0) == sizeof(uint32_t);
    const bool secretSaved = deviceIdCleared && revisionSaved &&
      preferences_.putString("secret", nextSecret) == nextSecret.length();
    deviceId_ = "";
    appliedRevision_ = 0;
    if (!secretSaved || !revisionSaved) {
      deviceSecret_ = "";
      preferences_.end();
      Diagnostics::event("ERROR", "IDENTITY_NVS_WRITE_FAILED");
      return false;
    }
    deviceSecret_ = nextSecret;
  }
  Diagnostics::event("HARDWARE_ID", hardwareId_);
  return true;
}

String InkloopClient::apiUrl() const {
  return strncmp(inkloop_api_url_slot, kUnpatchedApiPrefix, strlen(kUnpatchedApiPrefix)) == 0
    ? String(kDefaultApiUrl)
    : String(inkloop_api_url_slot);
}

bool InkloopClient::allowedPrivateHttp(const String& url) {
  if (url.startsWith("http://192.168.") || url.startsWith("http://10.")) return true;
  if (!url.startsWith("http://172.")) return false;
  const int secondStart = 11;
  const int secondEnd = url.indexOf('.', secondStart);
  if (secondEnd < 0) return false;
  const int secondOctet = url.substring(secondStart, secondEnd).toInt();
  return secondOctet >= 16 && secondOctet <= 31;
}

template <typename Client>
int InkloopClient::postJsonWithClient(
  Client& client,
  const String& url,
  const String& body,
  String& response,
  bool authenticate
) {
  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");
  if (authenticate && deviceId_.length() && deviceSecret_.length()) {
    http.addHeader("Authorization", "InkloopDevice " + deviceId_ + ":" + deviceSecret_);
  }
  const int status = http.POST(body);
  if (status > 0) response = http.getString();
  http.end();
  return status;
}

int InkloopClient::postJson(const String& body, String& response, bool authenticate) {
  const String url = apiUrl();
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    client.setCACert(kRootCa);
    client.setTimeout(20);
    return postJsonWithClient(client, url, body, response, authenticate);
  }
  if (allowedPrivateHttp(url)) {
    WiFiClient client;
    return postJsonWithClient(client, url, body, response, authenticate);
  }
  return -1;
}

RegistrationResult InkloopClient::registerDevice() {
  return registerDeviceImpl(nullptr);
}

RegistrationResult InkloopClient::registerDevice(
  const String& requestedPairingCode
) {
  return registerDeviceImpl(&requestedPairingCode);
}

RegistrationResult InkloopClient::registerDeviceImpl(
  const String* requestedPairingCode
) {
  RegistrationResult result;
  if (requestedPairingCode && !exactSixDigitPairingCode(
        requestedPairingCode->c_str(), requestedPairingCode->length())) {
    result.httpStatus = 422;
    Diagnostics::event("ERROR", "INKLOOP_PAIRING_CODE_INVALID");
    return result;
  }
  JsonDocument request;
  request["action"] = "register";
  request["hardwareId"] = hardwareId_;
  request["secret"] = deviceSecret_;
  request["skuId"] = kSkuId;
  request["firmwareVersion"] = kProtocolFirmwareVersion;
  if (requestedPairingCode) request["pairingCode"] = *requestedPairingCode;
  String body;
  serializeJson(request, body);
  String response;
  result.httpStatus = postJson(body, response, false);
  Diagnostics::event("REGISTER_HTTP", String(result.httpStatus));
  if (result.httpStatus < 200 || result.httpStatus >= 300) {
    Diagnostics::event("ERROR", "DEVICE_REGISTER_FAILED");
    return result;
  }

  JsonDocument payload;
  if (deserializeJson(payload, response)) {
    Diagnostics::event("ERROR", "DEVICE_REGISTER_RESPONSE_INVALID");
    return result;
  }
  const String nextDeviceId = payload["deviceId"] | "";
  if (!nextDeviceId.length()) return result;
  const bool nextPaired = payload["paired"] | false;
  const String nextPairingCode = nextPaired
    ? "" : String(payload["pairingCode"] | "------");
  result.pairingExpiresAt = nextPaired
    ? "" : String(payload["pairingExpiresAt"] | "");
  if (requestedPairingCode && !nextPaired &&
      nextPairingCode != *requestedPairingCode) {
    Diagnostics::event("ERROR", "INKLOOP_PAIRING_CODE_MISMATCH");
    return result;
  }
  deviceId_ = nextDeviceId;
  if (preferences_.putString("device-id", deviceId_) != deviceId_.length()) {
    deviceId_ = "";
    Diagnostics::event("ERROR", "DEVICE_ID_NVS_WRITE_FAILED");
    return result;
  }
  paired_ = nextPaired;
  pairingCode_ = nextPairingCode;
  result.ok = true;
  result.paired = paired_;
  result.requestedPairingCodeAccepted = requestedPairingCode != nullptr &&
      (nextPaired || nextPairingCode == *requestedPairingCode);
  result.pairingCode = pairingCode_;
  return result;
}

SyncResult InkloopClient::syncTasks() {
  SyncResult result;
  if (!tasks_.ready()) {
    Diagnostics::event("TASK_SYNC_BLOCKED", "TASK_STORE_UNAVAILABLE");
    return result;
  }
  if (!deviceId_.length() || WiFi.status() != WL_CONNECTED) return result;
  JsonDocument request;
  request["action"] = "sync";
  request["appliedRevision"] = appliedRevision_;
  request["firmwareVersion"] = kProtocolFirmwareVersion;
  String body;
  serializeJson(request, body);
  String response;
  const int status = postJson(body, response, true);
  if (status < 200 || status >= 300) return result;
  JsonDocument payload;
  if (deserializeJson(payload, response)) return result;
  const bool nowPaired = payload["paired"] | false;
  if (!nowPaired) {
    paired_ = false;
    result.requiresRegistration = true;
    return result;
  }
  result.becamePaired = !paired_;
  paired_ = true;
  pairingCode_ = "";
  const bool changed = payload["changed"] | false;
  const uint32_t revision = payload["revision"] | appliedRevision_;
  if (changed) {
    JsonArrayConst tasks = payload["tasks"].as<JsonArrayConst>();
    if (!tasks_.replace(tasks)) return result;
    appliedRevision_ = revision;
    preferences_.putUInt("revision", appliedRevision_);
  }
  result.ok = true;
  return result;
}

template <typename Client>
bool InkloopClient::downloadFrameWithClient(Client& client, const String& frameUrl, DownloadedFrame& frame) {
  HTTPClient http;
  if (!http.begin(client, frameUrl)) {
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", "BEGIN_FAILED");
    return false;
  }
  http.setTimeout(30000);
  http.addHeader("Authorization", "InkloopDevice " + deviceId_ + ":" + deviceSecret_);
  const int status = http.GET();
  const int size = http.getSize();
  if (status != HTTP_CODE_OK || size <= 24 || static_cast<size_t>(size) > kMaxFrameBytes) {
    String detail("HTTP_");
    detail += String(status);
    detail += ":SIZE_";
    detail += String(size);
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", detail);
    http.end();
    return false;
  }
  frame.bytes = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame.bytes) frame.bytes = static_cast<uint8_t*>(malloc(size));
  if (!frame.bytes) {
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", "ALLOC_FAILED");
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  const size_t received = stream->readBytes(frame.bytes, size);
  http.end();
  if (received != static_cast<size_t>(size)) {
    String detail("READ_");
    detail += String(received);
    detail += "_OF_";
    detail += String(size);
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", detail);
    frame.release();
    return false;
  }
  const uint32_t width = (static_cast<uint32_t>(frame.bytes[16]) << 24) |
    (static_cast<uint32_t>(frame.bytes[17]) << 16) |
    (static_cast<uint32_t>(frame.bytes[18]) << 8) | frame.bytes[19];
  const uint32_t height = (static_cast<uint32_t>(frame.bytes[20]) << 24) |
    (static_cast<uint32_t>(frame.bytes[21]) << 16) |
    (static_cast<uint32_t>(frame.bytes[22]) << 8) | frame.bytes[23];
  frame.landscape = width == 600 && height == 400;
  if ((!frame.landscape && (width != 400 || height != 600))) {
    String detail("DIMENSIONS_");
    detail += String(width);
    detail += "x";
    detail += String(height);
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", detail);
    frame.release();
    return false;
  }
  frame.length = received;
  return true;
}

bool InkloopClient::downloadFrame(const String& frameUrl, DownloadedFrame& frame) {
  frame.release();
  String safeUrl = frameUrl;
  // The self-hosted Inkloop instance is behind a TLS reverse proxy. Some
  // proxy stacks expose the internal HTTP scheme to the route handler, which
  // can make an otherwise valid frame URL arrive as public cleartext HTTP.
  // Upgrade only Inkloop's exact production host; never relax the general
  // public-HTTP rejection policy.
  if (safeUrl.startsWith("http://inkloop.mess.host/")) {
    safeUrl = "https://" + safeUrl.substring(strlen("http://"));
    Diagnostics::event("FRAME_DOWNLOAD_DIAG", "UPGRADED_CANONICAL_HTTPS");
  }
  if (safeUrl.startsWith("https://")) {
    WiFiClientSecure client;
    client.setCACert(kRootCa);
    client.setTimeout(30);
    return downloadFrameWithClient(client, safeUrl, frame);
  }
  if (allowedPrivateHttp(safeUrl)) {
    WiFiClient client;
    return downloadFrameWithClient(client, safeUrl, frame);
  }
  String rejected = safeUrl;
  const int query = rejected.indexOf('?');
  if (query >= 0) rejected = rejected.substring(0, query);
  if (rejected.length() > 120) rejected = rejected.substring(0, 120);
  Diagnostics::event("FRAME_DOWNLOAD_DIAG", "URL_REJECTED:" + rejected);
  return false;
}

}  // namespace inkloop
