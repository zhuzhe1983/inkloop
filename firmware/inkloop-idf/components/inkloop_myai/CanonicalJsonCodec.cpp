#include "CanonicalJsonCodec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace inkloop {
namespace myai {
namespace {

std::string escapeJson(const std::string& value) {
  std::string result;
  result.reserve(value.size() + 8);
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch == '"' || ch == '\\') {
      result.push_back('\\');
      result.push_back(static_cast<char>(ch));
    } else if (ch == '\n') {
      result += "\\n";
    } else if (ch == '\r') {
      result += "\\r";
    } else if (ch == '\t') {
      result += "\\t";
    } else if (ch < 0x20) {
      result += "\\u00";
      result.push_back(hex[(ch >> 4) & 0x0f]);
      result.push_back(hex[ch & 0x0f]);
    } else {
      result.push_back(static_cast<char>(ch));
    }
  }
  return result;
}

std::string quote(const std::string& value) { return "\"" + escapeJson(value) + "\""; }

size_t skipSpace(const std::string& json, size_t position) {
  while (position < json.size() &&
         (json[position] == ' ' || json[position] == '\n' ||
          json[position] == '\r' || json[position] == '\t')) {
    ++position;
  }
  return position;
}

bool keyValuePosition(const std::string& json, const std::string& key,
                      size_t& position, size_t begin = 0) {
  const std::string needle = "\"" + key + "\"";
  size_t found = begin;
  while ((found = json.find(needle, found)) != std::string::npos) {
    const size_t separator = skipSpace(json, found + needle.size());
    if (separator < json.size() && json[separator] == ':') {
      position = skipSpace(json, separator + 1);
      return position < json.size();
    }
    // A quoted string value may equal a later field name (the canonical
    // pairing response has status="bound" before its bound boolean). It is
    // not a key unless the closing quote is followed by optional space and a
    // colon, so continue searching instead of stealing the next field's colon.
    found += needle.size();
  }
  return false;
}

bool readStringAt(const std::string& json, size_t position, std::string& output) {
  if (position >= json.size() || json[position] != '"') return false;
  output.clear();
  for (size_t index = position + 1; index < json.size(); ++index) {
    char ch = json[index];
    if (ch == '"') return true;
    if (ch != '\\') {
      output.push_back(ch);
      continue;
    }
    if (++index >= json.size()) return false;
    ch = json[index];
    if (ch == 'n') output.push_back('\n');
    else if (ch == 'r') output.push_back('\r');
    else if (ch == 't') output.push_back('\t');
    else if (ch == '"' || ch == '\\' || ch == '/') output.push_back(ch);
    else if (ch == 'u') {
      // Preserve non-ASCII unicode escapes for diagnostics instead of risking
      // a lossy home-grown UTF-8 conversion.
      if (index + 4 >= json.size()) return false;
      output += "\\u";
      output.append(json, index + 1, 4);
      index += 4;
    } else {
      return false;
    }
  }
  return false;
}

bool stringField(const std::string& json, const std::string& key,
                 std::string& output, size_t begin = 0) {
  size_t position = 0;
  return keyValuePosition(json, key, position, begin) &&
         readStringAt(json, position, output);
}

bool boolField(const std::string& json, const std::string& key, bool& output,
               size_t begin = 0) {
  size_t position = 0;
  if (!keyValuePosition(json, key, position, begin)) return false;
  if (json.compare(position, 4, "true") == 0) {
    output = true;
    return true;
  }
  if (json.compare(position, 5, "false") == 0) {
    output = false;
    return true;
  }
  return false;
}

bool intField(const std::string& json, const std::string& key, int& output,
              size_t begin = 0) {
  size_t position = 0;
  if (!keyValuePosition(json, key, position, begin)) return false;
  char* end = NULL;
  const long parsed = std::strtol(json.c_str() + position, &end, 10);
  if (end == json.c_str() + position) return false;
  output = static_cast<int>(parsed);
  return true;
}

bool rangeForValue(const std::string& json, const std::string& key, char opener,
                   char closer, size_t& begin, size_t& end, size_t from = 0) {
  size_t position = 0;
  if (!keyValuePosition(json, key, position, from) || json[position] != opener) return false;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (size_t index = position; index < json.size(); ++index) {
    const char ch = json[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == opener) {
      ++depth;
    } else if (ch == closer) {
      if (--depth == 0) {
        begin = position;
        end = index + 1;
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> objectItems(const std::string& arrayJson) {
  std::vector<std::string> output;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  size_t begin = std::string::npos;
  for (size_t index = 0; index < arrayJson.size(); ++index) {
    const char ch = arrayJson[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '{') {
      if (depth++ == 0) begin = index;
    } else if (ch == '}' && depth > 0 && --depth == 0 && begin != std::string::npos) {
      output.push_back(arrayJson.substr(begin, index - begin + 1));
      begin = std::string::npos;
    }
  }
  return output;
}

std::string number(double value) {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return std::string(buffer);
}

Status requireFields(bool condition, const char* detail) {
  return condition ? Status::success() : Status(ErrorCode::Protocol, 0, detail);
}

bool validContractErrorCode(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == ' ')) {
      return false;
    }
  }
  return true;
}

void appendIdentity(std::ostringstream& body, const std::string& deviceId,
                    const std::string& fingerprint) {
  body << "\"device_id\":" << quote(deviceId)
       << ",\"mac_address\":" << quote(fingerprint)
       << ",\"app_id\":\"" << kAppId << "\"";
}

}  // namespace

CanonicalJsonCodec::CanonicalJsonCodec(const std::string& hardwareSku)
    : hardwareSku_(hardwareSku.empty() ? kHardwareSku : hardwareSku) {}

std::string CanonicalJsonCodec::pairingStartBody(
    const std::string& code, const std::string& fingerprint,
    const std::string& label) const {
  std::ostringstream body;
  body << "{";
  appendIdentity(body, code, fingerprint);
  body << ",\"label\":" << quote(label)
       << ",\"hardware_sku\":" << quote(hardwareSku_) << "}";
  return body.str();
}

Status CanonicalJsonCodec::parsePairingStart(
    const std::string& body, PairingStartResponse& output) const {
  const bool valid = stringField(body, "device_id", output.deviceId) &&
                     stringField(body, "app_id", output.appId) &&
                     stringField(body, "status", output.status) &&
                     stringField(body, "pairing_token", output.pairingToken) &&
                     stringField(body, "binding_url", output.bindingUrl) &&
                     stringField(body, "expires_at", output.expiresAt) &&
                     isSixDigitCode(output.deviceId) &&
                     !output.appId.empty() && !output.status.empty() &&
                     !output.pairingToken.empty() && !output.bindingUrl.empty() &&
                     !output.expiresAt.empty();
  return requireFields(valid, "invalid pairing/start response");
}

std::string CanonicalJsonCodec::pairingStatusBody(
    const std::string& deviceId, const std::string& pairingToken) const {
  return "{\"device_id\":" + quote(deviceId) + ",\"pairing_token\":" +
         quote(pairingToken) + "}";
}

Status CanonicalJsonCodec::parsePairingStatus(
    const std::string& body, PairingStatusResponse& output) const {
  const bool valid = stringField(body, "device_id", output.deviceId) &&
                     stringField(body, "app_id", output.appId) &&
                     stringField(body, "status", output.status) &&
                     stringField(body, "expires_at", output.expiresAt);
  boolField(body, "bound", output.bound);
  boolField(body, "recovery_required", output.recoveryRequired);
  boolField(body, "payment_required", output.paymentRequired);
  stringField(body, "device_token", output.deviceToken);
  size_t deviceBegin = 0, deviceEnd = 0;
  if (rangeForValue(body, "device", '{', '}', deviceBegin, deviceEnd)) {
    const std::string device = body.substr(deviceBegin, deviceEnd - deviceBegin);
    boolField(device, "active", output.active);
  }
  return requireFields(valid, "invalid pairing/status response");
}

std::string CanonicalJsonCodec::parseErrorCode(const std::string& body) const {
  // Flutter/public Center authorize a top-level error string, a top-level
  // code, or error.code. Keep this diagnostic parser deliberately small so a
  // hostile error body cannot become an unbounded firmware log/status value.
  if (body.empty() || body.size() > 4096) return std::string();
  std::string error;
  if (stringField(body, "error", error) && validContractErrorCode(error))
    return error;
  size_t nestedBegin = 0, nestedEnd = 0;
  if (rangeForValue(
          body, "error", '{', '}', nestedBegin, nestedEnd)) {
    const std::string nested = body.substr(
        nestedBegin, nestedEnd - nestedBegin);
    if (stringField(nested, "code", error) && validContractErrorCode(error))
      return error;
  }
  if (stringField(body, "code", error) && validContractErrorCode(error))
    return error;
  return std::string();
}

std::string CanonicalJsonCodec::deviceCheckBody(
    const std::string& deviceId, const std::string& fingerprint) const {
  return "{\"device_id\":" + quote(deviceId) + ",\"mac_address\":" +
         quote(fingerprint) + "}";
}

Status CanonicalJsonCodec::parseDeviceCheck(const std::string& body,
                                             bool& authorized,
                                             bool& active) const {
  if (!boolField(body, "authorized", authorized)) {
    return Status(ErrorCode::Protocol, 0, "invalid devices/check response");
  }
  size_t begin = 0, end = 0;
  if (rangeForValue(body, "device", '{', '}', begin, end)) {
    boolField(body.substr(begin, end - begin), "active", active);
  }
  return Status::success();
}

Status CanonicalJsonCodec::parseModelPreference(
    const std::string& body, std::string& providerProfileId) const {
  if (!stringField(body, "provider_profile_id", providerProfileId) ||
      providerProfileId.empty()) {
    return Status(ErrorCode::Protocol, 0,
                  "model preferences omitted provider_profile_id");
  }
  return Status::success();
}

std::string CanonicalJsonCodec::sessionRequestBody(
    Capability capability, const std::string& deviceId,
    const std::string& fingerprint, const std::string& clientRegion,
    const std::string& clientVersion) const {
  const bool voice = capability == Capability::Voice;
  std::ostringstream body;
  body << "{";
  appendIdentity(body, deviceId, fingerprint);
  body << ",\"client_id\":\"inkloop-papercolor-" << (voice ? "voice" : "image")
       << "\",\"client_version\":" << quote(clientVersion);
  if (!clientRegion.empty()) body << ",\"client_region\":" << quote(clientRegion);
  body << ",\"required_scenarios\":[\"" << (voice ? "voice" : "image") << "\"]"
       << ",\"required_kinds\":"
       << (voice ? "[\"asr\",\"llm\",\"tts\",\"vad\"]" : "[\"aigc\"]")
       << "}";
  return body.str();
}

Status CanonicalJsonCodec::parseSessionRequest(
    const std::string& body, SessionRequestResponse& output) const {
  size_t sessionBegin = 0, sessionEnd = 0;
  if (!rangeForValue(body, "session", '{', '}', sessionBegin, sessionEnd) ||
      !stringField(body.substr(sessionBegin, sessionEnd - sessionBegin), "id",
                   output.sessionId) || output.sessionId.empty()) {
    return Status(ErrorCode::Protocol, 0, "missing client session id");
  }
  stringField(body, "probe_token", output.probeToken);
  size_t arrayBegin = 0, arrayEnd = 0;
  if (!rangeForValue(body, "gateways", '[', ']', arrayBegin, arrayEnd)) {
    return Status(ErrorCode::Protocol, 0, "missing gateway candidates");
  }
  const std::vector<std::string> items =
      objectItems(body.substr(arrayBegin, arrayEnd - arrayBegin));
  for (size_t index = 0; index < items.size(); ++index) {
    GatewayCandidate gateway;
    if (!stringField(items[index], "id", gateway.id) ||
        !stringField(items[index], "base_url", gateway.baseUrl) ||
        !stringField(items[index], "ping_url", gateway.pingUrl) ||
        gateway.id.empty() || gateway.baseUrl.empty() || gateway.pingUrl.empty()) {
      continue;
    }
    stringField(items[index], "region", gateway.region);
    stringField(items[index], "status", gateway.status);
    output.gateways.push_back(gateway);
  }
  return output.gateways.empty()
             ? Status(ErrorCode::NoGateway, 0, "no valid gateway candidates")
             : Status::success();
}

std::string CanonicalJsonCodec::sessionSelectBody(
    const std::string& sessionId, const std::string& gatewayId,
    const std::vector<GatewayProbe>& probes) const {
  std::ostringstream body;
  body << "{\"session_id\":" << quote(sessionId)
       << ",\"gateway_id\":" << quote(gatewayId) << ",\"probe_results\":[";
  for (size_t index = 0; index < probes.size(); ++index) {
    if (index) body << ',';
    body << "{\"gateway_id\":" << quote(probes[index].gatewayId)
         << ",\"ok\":" << (probes[index].ok ? "true" : "false")
         << ",\"latency_ms\":" << probes[index].latencyMs
         << ",\"checked_at\":" << quote(probes[index].checkedAt);
    if (!probes[index].error.empty())
      body << ",\"error\":" << quote(probes[index].error);
    body << "}";
  }
  body << "]}";
  return body.str();
}

Status CanonicalJsonCodec::parseSessionSelect(
    const std::string& body, SessionSelectResponse& output) const {
  if (!stringField(body, "gateway_token", output.gatewayToken) ||
      output.gatewayToken.empty()) {
    return Status(ErrorCode::Protocol, 0, "missing gateway token");
  }
  size_t begin = 0, end = 0;
  if (!rangeForValue(body, "gateway", '{', '}', begin, end)) {
    return Status(ErrorCode::Protocol, 0, "missing selected gateway");
  }
  const std::string gateway = body.substr(begin, end - begin);
  if (!stringField(gateway, "id", output.gateway.id) ||
      !stringField(gateway, "base_url", output.gateway.baseUrl) ||
      output.gateway.id.empty() || output.gateway.baseUrl.empty()) {
    return Status(ErrorCode::Protocol, 0, "invalid selected gateway");
  }
  stringField(gateway, "ping_url", output.gateway.pingUrl);
  stringField(gateway, "region", output.gateway.region);
  stringField(gateway, "status", output.gateway.status);
  return Status::success();
}

std::string CanonicalJsonCodec::gatewayStartBody(const GatewayLease& lease) const {
  return "{\"session_id\":" + quote(lease.sessionId) + ",\"gateway_id\":" +
         quote(lease.gatewayId) + "}";
}

Status CanonicalJsonCodec::parseGatewayStart(
    const std::string& body, std::string& providerProfileId) const {
  providerProfileId.clear();
  if (body.empty()) return Status::success();
  std::string parsed;
  if (!stringField(body, "provider_profile_id", parsed)) {
    return Status::success();
  }
  if (parsed.empty() || parsed.size() > 512U) {
    return Status(ErrorCode::Protocol, 0,
                  "invalid gateway provider profile id");
  }
  providerProfileId = parsed;
  return Status::success();
}

std::string CanonicalJsonCodec::heartbeatBody(const GatewayLease& lease,
                                               uint32_t activeSeconds) const {
  std::ostringstream body;
  body << "{\"session_id\":" << quote(lease.sessionId)
       << ",\"gateway_id\":" << quote(lease.gatewayId)
       << ",\"usage\":{\"active_seconds\":" << activeSeconds
       << ",\"request_count\":" << lease.requestCount << "}}";
  return body.str();
}

std::string CanonicalJsonCodec::disconnectBody(const GatewayLease& lease,
                                                const std::string& reason) const {
  return "{\"session_id\":" + quote(lease.sessionId) + ",\"gateway_id\":" +
         quote(lease.gatewayId) + ",\"reason\":" + quote(reason) + "}";
}

std::string CanonicalJsonCodec::sessionUpdateMessage(
    const GatewayLease& lease, const std::string& deviceId,
    const std::string& systemPrompt) const {
  std::ostringstream body;
  body << "{\"type\":\"session.update\",\"payload\":{"
       << "\"client_id\":\"inkloop-papercolor-voice\""
       << ",\"device_id\":" << quote(deviceId)
       << ",\"app_id\":\"" << kAppId << "\""
       << ",\"session_id\":" << quote(lease.sessionId);
  if (!lease.providerProfileId.empty())
    body << ",\"provider_profile_id\":" << quote(lease.providerProfileId);
  body << ",\"response_language\":\"auto\",\"asr_language\":\"zh\""
       << ",\"auto_response\":false"
       << ",\"voice_assistant\":{\"contract\":\"myai_voice_mcp_tools_v1\""
       << ",\"ui_context\":{\"current_scene\":\"papercolor\",\"client\":\"inkloop\"}"
       << ",\"assistant_intent\":{\"scope\":\"conversation\",\"action\":\"chat_or_tool\""
       << ",\"requires_confirmation\":false,\"ui_surface\":\"papercolor\"}"
       << ",\"metadata\":{\"action_transport\":\"voice_ws.action.execute\"";
  if (!systemPrompt.empty()) body << ",\"shared_system_prompt\":" << quote(systemPrompt);
  body << ",\"mcp_tools\":[{\"name\":\"myai.aigc.generate\",\"kind\":\"aigc.generate\""
       << ",\"description\":\"Generate one image through the selected MyAI AIGC gateway.\""
       << ",\"input_schema\":{\"type\":\"object\",\"required\":[\"prompt\"]"
       << ",\"properties\":{\"prompt\":{\"type\":\"string\"}"
       << ",\"original_request\":{\"type\":\"string\"}}}}]}}"
       << ",\"format\":{\"codec\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1}}}";
  return body.str();
}

std::string CanonicalJsonCodec::audioStartMessage(const std::string& streamId) const {
  return "{\"type\":\"audio.start\",\"payload\":{\"stream_id\":" +
         quote(streamId) +
         ",\"codec\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1}}";
}

std::string CanonicalJsonCodec::audioStopMessage(const std::string& streamId,
                                                  uint32_t lastSeq) const {
  std::ostringstream body;
  body << "{\"type\":\"audio.stop\",\"payload\":{\"stream_id\":"
       << quote(streamId) << ",\"last_seq\":" << lastSeq << "}}";
  return body.str();
}

std::string CanonicalJsonCodec::responseCreateMessage(const std::string& text) const {
  return "{\"type\":\"response.create\",\"payload\":{\"text\":" +
         quote(text) + "}}";
}

Status CanonicalJsonCodec::parseVoiceEvent(const std::string& message,
                                            VoiceEvent& event) const {
  if (!stringField(message, "type", event.type))
    return Status(ErrorCode::Protocol, 0, "voice event missing type");
  size_t begin = 0, end = 0;
  const std::string payload =
      rangeForValue(message, "payload", '{', '}', begin, end)
          ? message.substr(begin, end - begin)
          : std::string("{}");
  event.rawPayload = payload;
  stringField(payload, "text", event.text);
  stringField(payload, "code", event.code);
  stringField(payload, "message", event.message);
  stringField(payload, "stream_id", event.streamId);
  stringField(payload, "action_id", event.actionId);
  stringField(payload, "kind", event.kind);
  stringField(payload, "prompt", event.prompt);
  int numberValue = 0;
  if (intField(payload, "sample_rate_hz", numberValue))
    event.sampleRateHz = static_cast<uint32_t>(std::max(0, numberValue));
  if (intField(payload, "channels", numberValue))
    event.channels = static_cast<uint8_t>(std::max(0, numberValue));
  if (intField(payload, "last_seq", numberValue)) event.lastSeq = numberValue;
  return Status::success();
}

std::string CanonicalJsonCodec::comboVoiceBody(
    const std::string& deviceId, const std::string& fingerprint,
    const std::string& sessionId, const std::string& audioBase64) const {
  std::ostringstream body;
  body << "{";
  appendIdentity(body, deviceId, fingerprint);
  body << ",\"session_id\":" << quote(sessionId)
       << ",\"format\":{\"codec\":\"pcm_s16le\",\"sample_rate_hz\":16000,\"channels\":1}"
       << ",\"audio_base64\":" << quote(audioBase64) << "}";
  return body.str();
}

Status CanonicalJsonCodec::parseComboVoice(const std::string& body,
                                            std::string& transcript,
                                            std::string& reply,
                                            std::string& audioBase64) const {
  const bool valid = stringField(body, "transcript", transcript) &&
                     stringField(body, "reply", reply) &&
                     stringField(body, "audio_base64", audioBase64);
  return requireFields(valid, "invalid combo voice response");
}

std::string CanonicalJsonCodec::aigcGenerateBody(
    const std::string& deviceId, const std::string& fingerprint,
    const ImageRequest& request) const {
  std::ostringstream body;
  body << "{";
  appendIdentity(body, deviceId, fingerprint);
  body << ",\"prompt\":" << quote(request.prompt);
  if (!request.negativePrompt.empty())
    body << ",\"negative_prompt\":" << quote(request.negativePrompt);
  if (!request.model.empty()) body << ",\"model\":" << quote(request.model);
  if (!request.size.empty()) body << ",\"size\":" << quote(request.size);
  if (request.steps > 0) body << ",\"steps\":" << request.steps;
  if (request.guidanceScale > 0)
    body << ",\"guidance_scale\":" << number(request.guidanceScale);
  if (request.cfgScale > 0) body << ",\"cfg_scale\":" << number(request.cfgScale);
  body << "}";
  return body.str();
}

Status CanonicalJsonCodec::parseAigcGenerate(
    const std::string& body, AigcGenerateResponse& output) const {
  if (!stringField(body, "prompt_id", output.promptId))
    return Status(ErrorCode::Protocol, 0, "missing AIGC prompt_id");
  stringField(body, "provider", output.provider);
  stringField(body, "model", output.model);
  stringField(body, "status", output.status);
  stringField(body, "message", output.message);
  return Status::success();
}

std::string CanonicalJsonCodec::aigcStatusBody(
    const std::string& deviceId, const std::string& fingerprint,
    const std::string& promptId) const {
  std::ostringstream body;
  body << "{";
  appendIdentity(body, deviceId, fingerprint);
  body << ",\"prompt_id\":" << quote(promptId) << "}";
  return body.str();
}

Status CanonicalJsonCodec::parseAigcStatus(
    const std::string& body, AigcStatusResponse& output) const {
  if (!stringField(body, "prompt_id", output.promptId) ||
      !stringField(body, "status", output.status))
    return Status(ErrorCode::Protocol, 0, "invalid AIGC status response");
  stringField(body, "message", output.message);
  size_t begin = 0, end = 0;
  if (!rangeForValue(body, "outputs", '[', ']', begin, end)) return Status::success();
  const std::vector<std::string> items =
      objectItems(body.substr(begin, end - begin));
  for (size_t index = 0; index < items.size(); ++index) {
    AigcOutputRef ref;
    stringField(items[index], "node_id", ref.nodeId);
    stringField(items[index], "filename", ref.filename);
    stringField(items[index], "subfolder", ref.subfolder);
    stringField(items[index], "type", ref.type);
    if (!ref.filename.empty()) output.outputs.push_back(ref);
  }
  return Status::success();
}

std::string CanonicalJsonCodec::aigcOutputBody(
    const std::string& deviceId, const std::string& fingerprint,
    const std::string& promptId, const AigcOutputRef& output) const {
  std::ostringstream body;
  body << "{";
  appendIdentity(body, deviceId, fingerprint);
  body << ",\"prompt_id\":" << quote(promptId)
       << ",\"node_id\":" << quote(output.nodeId)
       << ",\"filename\":" << quote(output.filename)
       << ",\"subfolder\":" << quote(output.subfolder)
       << ",\"type\":" << quote(output.type.empty() ? "output" : output.type)
       << "}";
  return body.str();
}

}  // namespace myai
}  // namespace inkloop
