#pragma once

#include "MyAiAdapters.h"

namespace inkloop {
namespace myai {

struct ClientConfig {
  std::string installationFingerprint;
  // Canonical public-contract MAC (AA:BB:CC:DD:EE:FF). It is deliberately
  // separate from the durable installation fingerprint so firmware upgrades
  // never have to rewrite or discard an existing device credential.
  std::string macAddress;
  std::string deviceLabel;
  std::string clientRegion;
  std::string systemPrompt;

  ClientConfig() : deviceLabel("Inkloop PaperColor") {}
};

class MyAiClient final : public IWebSocketListener {
 public:
  MyAiClient(const ClientConfig& config, IHttpTransport& http,
             IGatewayProbeSet& gatewayProbes,
             IWebSocketTransport& webSocket, IAigcOutputTransport& output,
             IEndpointSecurity& endpointSecurity, ICredentialStore& credentials,
             IWireCodec& codec, IClock& clock, IAudioSink& audio,
             ILocalTranscriptInterceptor& localCommands, IMyAiEvents& events);
  ~MyAiClient();

  Status initialize();
  Status health();

  // MyAI pairing is started first. A successful response's device_id becomes
  // the authoritative onboarding code that Inkloop must reuse. A rejected,
  // missing, mismatched, or non-six-digit response blocks Inkloop binding and
  // requires a new MyAI candidate; there is no ordinary two-code fallback.
  Status startPairing(const std::string& candidateSixDigits,
                      PairingView& onboarding);
  // Rehydrates only the public onboarding view for an already-durable pending
  // pairing. The opaque pairing token remains private to this client.
  Status pendingPairing(PairingView& onboarding) const;
  Status pollPairing(bool& bound);
  Status checkAuthorization(bool& authorized);
  Status cancelPairing();
  // Explicit owner-authorized recovery path. It keeps the installation
  // fingerprint but clears the unusable device/pairing credential so a fresh
  // public pairing flow can start. Callers must provide their own UI
  // confirmation before invoking it.
  Status resetCredentialForRebind();

  Status connectVoice();
  Status beginVoiceTurn(const std::string& streamId);
  Status sendPcm16(const uint8_t* bytes, size_t length);
  Status endVoiceTurn();
  Status requestResponse(const std::string& transcript);
  Status heartbeatVoice();
  Status disconnectVoice(const std::string& reason = "client_disconnect");
  // Some compatible gateways terminate the final TTS segment without a
  // response.done event.  The application may call this only after the audio
  // sink reports tts.stop, the speaker is idle, and a grace window elapsed.
  Status completeVoiceResponseAfterTtsStop();

  // Bounded degraded path for devices that cannot sustain realtime WebSocket.
  Status comboVoice(const std::string& pcm16Base64, size_t maxResponseBase64Bytes,
                    std::string& transcript, std::string& reply,
                    std::string& responseAudioBase64);

  Status startImage(const ImageRequest& request,
                    AigcGenerateResponse& generated);
  Status pollImage(const std::string& promptId, AigcStatusResponse& status);
  Status downloadImage(const std::string& promptId,
                       const AigcOutputRef& output, const ImageRequest& limits,
                       IImageSink& sink, AigcOutputMetadata& metadata);
  Status disconnectImage(const std::string& reason = "client_disconnect");

  // Applied to the next voice session update. Updating while a voice lease is
  // active is rejected so a conversation never changes policy mid-turn.
  Status setSystemPrompt(const std::string& prompt);

  ActivationState activationState() const { return activationState_; }
  // Public six-digit device identity used by a third-party product to mirror
  // the same onboarding code into its own binding flow. The opaque device and
  // pairing tokens remain private. Call only from the MyAI network owner.
  const std::string& authoritativeDeviceId() const {
    return credentials_.deviceId;
  }
  VoiceState voiceState() const { return voiceState_; }
  AigcState aigcState() const { return aigcState_; }
  uint32_t suggestedVoiceReconnectDelayMs() const;

  static uint32_t reconnectDelayMs(uint8_t attempt);
  static bool isPublicGatewayUrl(const std::string& url);

  void onWebSocketOpen() override;
  void onWebSocketText(const std::string& message) override;
  void onWebSocketBinary(const uint8_t* bytes, size_t length) override;
  void onWebSocketClosed(int code, const std::string& reason) override;

 private:
  enum class RequestKind : uint8_t {
    Health,
    PairingStart,
    PairingStatus,
    DeviceCheck,
    CenterAuthenticated,
    GatewaySessionStart,
    GatewayBusiness,
  };

  Status loadCredentials();
  Status requireCredentialHealth() const;
  Status storageFailure(const std::string& operation, const Status& cause);
  Status clearPendingFailClosed();
  Status clearRuntimeCredentialFailClosed();
  Status verifyRecoveryReadable();
  Status openGatewaySession(Capability capability, GatewayLease& lease);
  Status disconnectLease(GatewayLease& lease, const std::string& reason);
  Status perform(const HttpRequest& request, HttpResponse& response,
                 RequestKind kind);
  HttpRequest centerRequest(const std::string& method, const std::string& path,
                            const std::string& body, bool authenticated) const;
  HttpRequest gatewayRequest(const GatewayLease& lease, const std::string& path,
                             const std::string& body) const;
  std::map<std::string, std::string> deviceHeaders(bool json) const;
  std::map<std::string, std::string> gatewayHeaders(
      const GatewayLease& lease, bool json) const;
  const std::string& wireMacAddress() const;
  Status classifyHttp(const HttpResponse& response, RequestKind kind);
  void setActivation(ActivationState state, const Status& status);
  void setVoice(VoiceState state);
  void setAigc(AigcState state, const std::string& detail = std::string());
  std::string voiceWebSocketUrl(const GatewayLease& lease) const;

  ClientConfig config_;
  IHttpTransport& http_;
  IGatewayProbeSet& gatewayProbes_;
  IWebSocketTransport& webSocket_;
  IAigcOutputTransport& output_;
  IEndpointSecurity& endpointSecurity_;
  ICredentialStore& credentialStore_;
  IWireCodec& codec_;
  IClock& clock_;
  IAudioSink& audio_;
  ILocalTranscriptInterceptor& localCommands_;
  IMyAiEvents& events_;
  CredentialSnapshot credentials_;
  GatewayLease voiceLease_;
  GatewayLease imageLease_;
  ActivationState activationState_;
  VoiceState voiceState_;
  AigcState aigcState_;
  std::string voiceStreamId_;
  uint32_t voiceLastSeq_;
  uint8_t voiceReconnectAttempts_;
  bool voiceSocketOpen_;
  bool voiceClosing_;
  bool credentialHealthy_;
  bool initialized_;
};

}  // namespace myai
}  // namespace inkloop
