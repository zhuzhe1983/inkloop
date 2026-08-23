#pragma once

#include "MyAiAdapters.h"

namespace inkloop {
namespace myai {

// Small dependency-free codec for the documented MyAI public client contract.
// It intentionally decodes only fields consumed by the device and ignores
// unknown fields for forward compatibility.
class CanonicalJsonCodec final : public IWireCodec {
 public:
  explicit CanonicalJsonCodec(
      const std::string& hardwareSku = std::string(kHardwareSku));
  std::string pairingStartBody(const std::string& code,
                               const std::string& fingerprint,
                               const std::string& label) const override;
  Status parsePairingStart(const std::string& body,
                           PairingStartResponse& output) const override;
  std::string pairingStatusBody(const std::string& deviceId,
                                const std::string& pairingToken) const override;
  Status parsePairingStatus(const std::string& body,
                            PairingStatusResponse& output) const override;
  std::string parseErrorCode(const std::string& body) const override;
  std::string parseErrorDiagnostic(const std::string& body) const override;
  std::string deviceCheckBody(const std::string& deviceId,
                              const std::string& fingerprint) const override;
  Status parseDeviceCheck(const std::string& body, bool& authorized,
                          bool& active) const override;
  Status parseModelPreference(const std::string& body,
                              std::string& providerProfileId) const override;
  std::string sessionRequestBody(Capability capability,
                                 const std::string& deviceId,
                                 const std::string& fingerprint,
                                 const std::string& clientRegion,
                                 const std::string& clientVersion) const override;
  Status parseSessionRequest(const std::string& body,
                             SessionRequestResponse& output) const override;
  std::string sessionSelectBody(const std::string& sessionId,
                                const std::string& gatewayId,
                                const std::vector<GatewayProbe>& probes) const override;
  Status parseSessionSelect(const std::string& body,
                            SessionSelectResponse& output) const override;
  std::string gatewayStartBody(const GatewayLease& lease) const override;
  Status parseGatewayStart(const std::string& body,
                           std::string& providerProfileId) const override;
  std::string heartbeatBody(const GatewayLease& lease,
                            uint32_t activeSeconds) const override;
  std::string disconnectBody(const GatewayLease& lease,
                             const std::string& reason) const override;
  std::string sessionUpdateMessage(const GatewayLease& lease,
                                   const std::string& deviceId,
                                   const std::string& systemPrompt) const override;
  std::string audioStartMessage(const std::string& streamId) const override;
  std::string audioStopMessage(const std::string& streamId,
                               uint32_t lastSeq) const override;
  std::string responseCreateMessage(const std::string& text) const override;
  Status parseVoiceEvent(const std::string& message,
                         VoiceEvent& event) const override;
  std::string comboVoiceBody(const std::string& deviceId,
                             const std::string& fingerprint,
                             const std::string& sessionId,
                             const std::string& audioBase64) const override;
  Status parseComboVoice(const std::string& body, std::string& transcript,
                         std::string& reply,
                         std::string& audioBase64) const override;
  std::string aigcGenerateBody(const std::string& deviceId,
                               const std::string& fingerprint,
                               const ImageRequest& request) const override;
  Status parseAigcGenerate(const std::string& body,
                           AigcGenerateResponse& output) const override;
  std::string aigcStatusBody(const std::string& deviceId,
                             const std::string& fingerprint,
                             const std::string& promptId) const override;
  Status parseAigcStatus(const std::string& body,
                         AigcStatusResponse& output) const override;
  std::string aigcOutputBody(const std::string& deviceId,
                             const std::string& fingerprint,
                             const std::string& promptId,
                             const AigcOutputRef& output) const override;

 private:
  std::string hardwareSku_;
};

}  // namespace myai
}  // namespace inkloop
