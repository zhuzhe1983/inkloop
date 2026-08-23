#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace inkloop {
namespace myai {

static const char kAppId[] = "inkloop";
static const char kCenterBaseUrl[] = "https://myai.mess.host";
static const char kHardwareSku[] = "m5-papercolor-c151";
static const uint32_t kVoiceSampleRateHz = 16000;
static const uint8_t kVoiceChannels = 1;

enum class ErrorCode : uint8_t {
  None,
  InvalidArgument,
  InvalidState,
  Storage,
  Security,
  Transport,
  Protocol,
  Unauthorized,
  PaymentRequired,
  RecoveryRequired,
  PairingExpired,
  Conflict,
  AppNotRegistered,
  NoGateway,
  TooLarge,
  Cancelled,
};

struct Status {
  ErrorCode code;
  int httpStatus;
  uint32_t retryAfterMs;
  std::string detail;

  Status(ErrorCode value = ErrorCode::None, int http = 0,
         const std::string& message = std::string(), uint32_t retry = 0)
      : code(value), httpStatus(http), retryAfterMs(retry), detail(message) {}

  bool ok() const { return code == ErrorCode::None; }
  static Status success() { return Status(); }
};

enum class ActivationState : uint8_t {
  Unconfigured,
  Pairing,
  Bound,
  PaymentRequired,
  RecoveryRequired,
  Offline,
  Error,
};

enum class VoiceState : uint8_t {
  Idle,
  Connecting,
  Ready,
  Listening,
  Thinking,
  Speaking,
  Error,
};

enum class AigcState : uint8_t {
  Idle,
  Generating,
  Polling,
  Downloading,
  Complete,
  Error,
};

enum class Capability : uint8_t { Voice, Image };

struct PendingPairing {
  std::string deviceId;
  std::string pairingToken;
  std::string bindingUrl;
  std::string expiresAt;

  bool valid() const {
    if (deviceId.size() != 6 || pairingToken.empty() || bindingUrl.empty() ||
        expiresAt.empty()) {
      return false;
    }
    for (size_t index = 0; index < deviceId.size(); ++index) {
      if (deviceId[index] < '0' || deviceId[index] > '9') return false;
    }
    return true;
  }
  bool empty() const {
    return deviceId.empty() && pairingToken.empty() && bindingUrl.empty() &&
        expiresAt.empty();
  }
  void clearSensitive() {
    pairingToken.assign(pairingToken.size(), '\0');
    pairingToken.clear();
  }
};

struct CredentialSnapshot {
  uint32_t generation;
  std::string installationFingerprint;
  std::string deviceId;
  PendingPairing pending;
  std::string deviceToken;
  bool active;

  CredentialSnapshot() : generation(0), active(false) {}
  bool hasDeviceToken() const { return !deviceToken.empty(); }
  void redact() {
    pending.clearSensitive();
    deviceToken.assign(deviceToken.size(), '\0');
    deviceToken.clear();
  }
};

struct PairingStartResponse {
  std::string deviceId;
  std::string appId;
  std::string status;
  std::string pairingToken;
  std::string bindingUrl;
  std::string expiresAt;
};

struct PairingStatusResponse {
  std::string deviceId;
  std::string appId;
  std::string status;
  std::string deviceToken;
  std::string expiresAt;
  bool bound;
  bool active;
  bool recoveryRequired;
  bool paymentRequired;

  PairingStatusResponse()
      : bound(false), active(false), recoveryRequired(false), paymentRequired(false) {}
};

struct PairingView {
  std::string onboardingCode;
  std::string bindingUrl;
  std::string expiresAt;
};

struct GatewayCandidate {
  std::string id;
  std::string baseUrl;
  std::string pingUrl;
  std::string region;
  std::string status;
};

struct GatewayProbe {
  std::string gatewayId;
  bool ok;
  uint32_t latencyMs;
  std::string checkedAt;
  std::string error;

  GatewayProbe() : ok(false), latencyMs(0) {}
};

struct SessionRequestResponse {
  std::string sessionId;
  std::string probeToken;
  std::vector<GatewayCandidate> gateways;
};

struct SessionSelectResponse {
  std::string gatewayToken;
  GatewayCandidate gateway;
};

struct GatewayLease {
  Capability capability;
  std::string sessionId;
  std::string gatewayId;
  std::string gatewayBaseUrl;
  std::string providerProfileId;
  std::string gatewayToken;
  uint64_t startedAtMs;
  uint32_t requestCount;

  GatewayLease()
      : capability(Capability::Voice), startedAtMs(0), requestCount(0) {}
  bool valid() const {
    return !sessionId.empty() && !gatewayId.empty() && !gatewayBaseUrl.empty() &&
           !gatewayToken.empty();
  }
  void clearSensitive() {
    gatewayToken.assign(gatewayToken.size(), '\0');
    gatewayToken.clear();
  }
};

struct VoiceEvent {
  std::string type;
  std::string text;
  std::string code;
  std::string message;
  std::string streamId;
  std::string actionId;
  std::string kind;
  std::string prompt;
  std::string rawPayload;
  uint32_t sampleRateHz;
  uint8_t channels;
  int lastSeq;

  VoiceEvent() : sampleRateHz(0), channels(0), lastSeq(-1) {}
};

struct LocalTranscriptDecision {
  bool handled;
  bool responseAlreadyRequested;
  std::string commandName;

  LocalTranscriptDecision(bool value = false,
                          const std::string& name = std::string(),
                          bool delegatedResponse = false)
      : handled(value), responseAlreadyRequested(delegatedResponse),
        commandName(name) {}
};

struct ImageRequest {
  std::string prompt;
  std::string negativePrompt;
  std::string model;
  std::string size;
  int steps;
  double guidanceScale;
  double cfgScale;
  size_t maxEncodedBytes;
  size_t maxDecodedBytes;

  ImageRequest()
      : model("t2i"), size("512x512"), steps(0), guidanceScale(0), cfgScale(0),
        maxEncodedBytes(4U * 1024U * 1024U),
        maxDecodedBytes(3U * 1024U * 1024U) {}
};

struct AigcGenerateResponse {
  std::string promptId;
  std::string provider;
  std::string model;
  std::string status;
  std::string message;
};

struct AigcOutputRef {
  std::string nodeId;
  std::string filename;
  std::string subfolder;
  std::string type;
};

struct AigcStatusResponse {
  std::string promptId;
  std::string status;
  std::string message;
  std::vector<AigcOutputRef> outputs;
};

struct AigcOutputMetadata {
  std::string promptId;
  std::string filename;
  std::string contentType;
  size_t decodedBytes;

  AigcOutputMetadata() : decodedBytes(0) {}
};

inline bool isSixDigitCode(const std::string& value) {
  if (value.size() != 6) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  return true;
}

}  // namespace myai
}  // namespace inkloop
