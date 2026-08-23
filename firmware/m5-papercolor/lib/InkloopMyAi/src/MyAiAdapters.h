#pragma once

#include "MyAiTypes.h"

#include <map>
#include <string>
#include <vector>

namespace inkloop {
namespace myai {

struct HttpRequest {
  std::string method;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  uint32_t timeoutMs;
  size_t maxResponseBytes;
  bool tlsPeerVerificationRequired;
  bool rejectPrivateResolvedAddresses;
  bool redirectsAllowed;

  HttpRequest()
      : timeoutMs(30000), maxResponseBytes(64U * 1024U),
        tlsPeerVerificationRequired(true), rejectPrivateResolvedAddresses(true),
        redirectsAllowed(false) {}
};

struct HttpResponse {
  int status;
  std::string body;
  std::map<std::string, std::string> headers;

  HttpResponse() : status(0) {}
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() {}
  // The three security fields in HttpRequest are mandatory policy, not hints.
  // Implementations fail if TLS peer/hostname validation is unavailable, never
  // follow redirects, and reject every resolved private/loopback/link-local IP.
  virtual Status perform(const HttpRequest& request, HttpResponse& response) = 0;
};

// Injected DNS/TLS preflight. Implementations resolve every A/AAAA answer and
// reject the endpoint if any answer is private, loopback, link-local, unspecified,
// multicast, or otherwise non-public. TLS chain and hostname validation must be
// available for the same hostname. This is called before HTTP, WSS, and streaming
// output transports so DNS rebinding and disabled certificate checks fail closed.
class IEndpointSecurity {
 public:
  virtual ~IEndpointSecurity() {}
  virtual Status validatePublicTlsEndpoint(const std::string& httpsUrl) = 0;
};

class IClock {
 public:
  virtual ~IClock() {}
  virtual uint64_t monotonicMs() const = 0;
  virtual std::string utcIso8601() const = 0;
};

// Implementations must commit each method atomically. Secret values must never be
// logged, returned by diagnostics, or mixed into the existing Inkloop namespace.
class ICredentialStore {
 public:
  virtual ~ICredentialStore() {}
  virtual Status load(CredentialSnapshot& snapshot) = 0;
  virtual Status initializeFingerprintAtomically(
      const std::string& installationFingerprint) = 0;
  virtual Status savePendingAtomically(const PendingPairing& pending) = 0;
  virtual Status promoteBoundAtomically(const std::string& expectedPairingToken,
                                        const std::string& deviceId,
                                        const std::string& deviceToken,
                                        bool active) = 0;
  virtual Status clearPendingAtomically() = 0;
  virtual Status clearRuntimeCredentialAtomically() = 0;
};

class IWireCodec {
 public:
  virtual ~IWireCodec() {}

  virtual std::string pairingStartBody(const std::string& code,
                                       const std::string& fingerprint,
                                       const std::string& label) const = 0;
  virtual Status parsePairingStart(const std::string& body,
                                   PairingStartResponse& output) const = 0;
  virtual std::string pairingStatusBody(const std::string& deviceId,
                                        const std::string& pairingToken) const = 0;
  virtual Status parsePairingStatus(const std::string& body,
                                    PairingStatusResponse& output) const = 0;
  virtual std::string parseErrorCode(const std::string& body) const = 0;
  virtual std::string deviceCheckBody(const std::string& deviceId,
                                      const std::string& fingerprint) const = 0;
  virtual Status parseDeviceCheck(const std::string& body, bool& authorized,
                                  bool& active) const = 0;
  virtual Status parseModelPreference(const std::string& body,
                                      std::string& providerProfileId) const = 0;
  virtual std::string sessionRequestBody(
      Capability capability, const std::string& deviceId,
      const std::string& fingerprint, const std::string& clientRegion,
      const std::string& clientVersion) const = 0;
  virtual Status parseSessionRequest(const std::string& body,
                                     SessionRequestResponse& output) const = 0;
  virtual std::string sessionSelectBody(
      const std::string& sessionId, const std::string& gatewayId,
      const std::vector<GatewayProbe>& probes) const = 0;
  virtual Status parseSessionSelect(const std::string& body,
                                    SessionSelectResponse& output) const = 0;
  virtual std::string gatewayStartBody(const GatewayLease& lease) const = 0;
  virtual std::string heartbeatBody(const GatewayLease& lease,
                                    uint32_t activeSeconds) const = 0;
  virtual std::string disconnectBody(const GatewayLease& lease,
                                     const std::string& reason) const = 0;

  virtual std::string sessionUpdateMessage(
      const GatewayLease& lease, const std::string& deviceId,
      const std::string& systemPrompt) const = 0;
  virtual std::string audioStartMessage(const std::string& streamId) const = 0;
  virtual std::string audioStopMessage(const std::string& streamId,
                                       uint32_t lastSeq) const = 0;
  virtual std::string responseCreateMessage(const std::string& text) const = 0;
  virtual Status parseVoiceEvent(const std::string& message,
                                 VoiceEvent& event) const = 0;

  virtual std::string comboVoiceBody(const std::string& deviceId,
                                     const std::string& fingerprint,
                                     const std::string& sessionId,
                                     const std::string& audioBase64) const = 0;
  virtual Status parseComboVoice(const std::string& body, std::string& transcript,
                                 std::string& reply,
                                 std::string& audioBase64) const = 0;

  virtual std::string aigcGenerateBody(const std::string& deviceId,
                                       const std::string& fingerprint,
                                       const ImageRequest& request) const = 0;
  virtual Status parseAigcGenerate(const std::string& body,
                                   AigcGenerateResponse& output) const = 0;
  virtual std::string aigcStatusBody(const std::string& deviceId,
                                     const std::string& fingerprint,
                                     const std::string& promptId) const = 0;
  virtual Status parseAigcStatus(const std::string& body,
                                 AigcStatusResponse& output) const = 0;
  virtual std::string aigcOutputBody(const std::string& deviceId,
                                     const std::string& fingerprint,
                                     const std::string& promptId,
                                     const AigcOutputRef& output) const = 0;
};

class IWebSocketListener {
 public:
  virtual ~IWebSocketListener() {}
  virtual void onWebSocketOpen() = 0;
  virtual void onWebSocketText(const std::string& message) = 0;
  virtual void onWebSocketBinary(const uint8_t* bytes, size_t length) = 0;
  virtual void onWebSocketClosed(int code, const std::string& reason) = 0;
};

class IWebSocketTransport {
 public:
  virtual ~IWebSocketTransport() {}
  // Only WSS is allowed. Peer certificate and hostname validation are mandatory;
  // redirects/downgrades are forbidden and endpoint DNS is preflighted separately.
  virtual Status connect(const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         IWebSocketListener& listener) = 0;
  virtual Status sendText(const std::string& message) = 0;
  virtual Status sendBinary(const uint8_t* bytes, size_t length) = 0;
  virtual void close(uint16_t code, const std::string& reason) = 0;
};

class IAudioSink {
 public:
  virtual ~IAudioSink() {}
  virtual Status begin(uint32_t sampleRateHz, uint8_t channels) = 0;
  virtual Status write(const uint8_t* bytes, size_t length) = 0;
  virtual Status end() = 0;
  virtual void abort() = 0;
};

class IImageSink {
 public:
  virtual ~IImageSink() {}
  virtual Status begin(const AigcOutputMetadata& metadata) = 0;
  virtual Status write(const uint8_t* bytes, size_t length) = 0;
  virtual Status commit(AigcOutputMetadata& metadata) = 0;
  virtual void abort() = 0;
};

// Adapter owns incremental JSON tokenization and base64 decode. It must abort the
// sink before returning any failure and enforce both caps without buffering the
// complete JSON or image in RAM.
class IAigcOutputTransport {
 public:
  virtual ~IAigcOutputTransport() {}
  virtual Status postAndDecodeBase64(const HttpRequest& request,
                                     size_t maxEncodedBytes,
                                     size_t maxDecodedBytes, IImageSink& sink,
                                     AigcOutputMetadata& metadata) = 0;
};

class ILocalTranscriptInterceptor {
 public:
  virtual ~ILocalTranscriptInterceptor() {}
  virtual LocalTranscriptDecision inspect(const std::string& transcript) = 0;
};

class IMyAiEvents {
 public:
  virtual ~IMyAiEvents() {}
  virtual void onActivationState(ActivationState state, const Status& status) = 0;
  virtual void onPairingReady(const PairingView& pairing) = 0;
  virtual void onVoiceState(VoiceState state) = 0;
  virtual void onTranscript(const std::string& text, bool final) = 0;
  // Text events are separate from TTS audio so a local client can present a
  // bounded, privacy-preserving conversation view without decoding audio.
  // The default keeps older third-party adapters source compatible.
  virtual void onAssistantText(const std::string& text, bool final) {
    (void)text;
    (void)final;
  }
  virtual void onLocalCommand(const std::string& commandName,
                              const std::string& transcript) = 0;
  virtual void onVoiceAction(const VoiceEvent& action) = 0;
  virtual void onAigcState(AigcState state, const std::string& detail) = 0;
  virtual void onError(const Status& status) = 0;
};

}  // namespace myai
}  // namespace inkloop
