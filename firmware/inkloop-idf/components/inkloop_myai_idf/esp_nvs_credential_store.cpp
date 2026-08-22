#include "inkloop/myai/esp_nvs_credential_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include "nvs.h"
#include "psa/crypto.h"

namespace inkloop {
namespace myai {
namespace {

constexpr char kNamespace[] = "ink-myai-v1";
constexpr char kMarkerKey[] = "initialized";
constexpr char kHeadKey[] = "head";
constexpr char kSlot0Key[] = "slot0";
constexpr char kSlot1Key[] = "slot1";

Status storageError(const char* detail) {
  return Status(ErrorCode::Storage, 0, detail);
}

std::string sha256Hex(const std::string& input) {
  uint8_t digest[32]{};
  size_t digest_length = 0;
  if (psa_crypto_init() != PSA_SUCCESS ||
      psa_hash_compute(
          PSA_ALG_SHA_256,
          reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest,
          sizeof(digest), &digest_length) != PSA_SUCCESS ||
      digest_length != sizeof(digest)) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(64, '0');
  for (size_t index = 0; index < sizeof(digest); ++index) {
    output[index * 2] = kHex[digest[index] >> 4U];
    output[index * 2 + 1] = kHex[digest[index] & 0x0FU];
  }
  return output;
}

void appendJsonString(const std::string& input, std::string& output) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.push_back('"');
  for (unsigned char ch : input) {
    if (ch == '"') output.append("\\\"");
    else if (ch == '\\') output.append("\\\\");
    else if (ch < 0x20U) {
      output.append("\\u00");
      output.push_back(kHex[ch >> 4U]);
      output.push_back(kHex[ch & 0x0FU]);
    } else output.push_back(static_cast<char>(ch));
  }
  output.push_back('"');
}

std::string canonicalRecord(const CredentialSnapshot& snapshot) {
  std::string output;
  output.reserve(1024 + snapshot.pending.pairingToken.size() +
                 snapshot.deviceToken.size());
  output.append("{\"schema\":1,\"generation\":");
  output.append(std::to_string(snapshot.generation));
  output.append(",\"fingerprint\":");
  appendJsonString(snapshot.installationFingerprint, output);
  output.append(",\"device_id\":");
  appendJsonString(snapshot.deviceId, output);
  output.append(",\"pending\":{\"device_id\":");
  appendJsonString(snapshot.pending.deviceId, output);
  output.append(",\"token\":");
  appendJsonString(snapshot.pending.pairingToken, output);
  output.append(",\"binding_url\":");
  appendJsonString(snapshot.pending.bindingUrl, output);
  output.append(",\"expires_at\":");
  appendJsonString(snapshot.pending.expiresAt, output);
  output.append("},\"device_token\":");
  appendJsonString(snapshot.deviceToken, output);
  output.append(",\"active\":");
  output.append(snapshot.active ? "true" : "false");
  output.push_back('}');
  return output;
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

void appendCodepoint(uint32_t codepoint, std::string& output) {
  if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

class RecordCursor {
 public:
  explicit RecordCursor(const std::string& input) : input_(input) {}

  bool literal(const char* expected) {
    const size_t length = std::strlen(expected);
    if (length > input_.size() - at_ ||
        input_.compare(at_, length, expected) != 0) return false;
    at_ += length;
    return true;
  }

  bool unsigned32(uint32_t& output) {
    if (at_ >= input_.size() || input_[at_] < '0' || input_[at_] > '9')
      return false;
    if (input_[at_] == '0' && at_ + 1 < input_.size() &&
        input_[at_ + 1] >= '0' && input_[at_ + 1] <= '9') return false;
    uint64_t value = 0;
    while (at_ < input_.size() && input_[at_] >= '0' &&
           input_[at_] <= '9') {
      value = value * 10U + static_cast<uint8_t>(input_[at_] - '0');
      if (value > std::numeric_limits<uint32_t>::max()) return false;
      ++at_;
    }
    output = static_cast<uint32_t>(value);
    return true;
  }

  bool boolean(bool& output) {
    if (literal("true")) { output = true; return true; }
    if (literal("false")) { output = false; return true; }
    return false;
  }

  bool string(std::string& output, size_t maximum) {
    if (!literal("\"")) return false;
    output.clear();
    while (at_ < input_.size()) {
      const uint8_t ch = static_cast<uint8_t>(input_[at_++]);
      if (ch == '"') return output.size() <= maximum;
      if (ch < 0x20U) return false;
      if (ch != '\\') output.push_back(static_cast<char>(ch));
      else {
        if (at_ >= input_.size()) return false;
        const char escaped = input_[at_++];
        switch (escaped) {
          case '"': output.push_back('"'); break;
          case '\\': output.push_back('\\'); break;
          case '/': output.push_back('/'); break;
          case 'b': output.push_back('\b'); break;
          case 'f': output.push_back('\f'); break;
          case 'n': output.push_back('\n'); break;
          case 'r': output.push_back('\r'); break;
          case 't': output.push_back('\t'); break;
          case 'u': {
            uint32_t first = 0;
            if (!hexQuad(first)) return false;
            uint32_t codepoint = first;
            if (first >= 0xD800U && first <= 0xDBFFU) {
              if (!literal("\\u")) return false;
              uint32_t second = 0;
              if (!hexQuad(second) || second < 0xDC00U || second > 0xDFFFU)
                return false;
              codepoint = 0x10000U + ((first - 0xD800U) << 10U) +
                          (second - 0xDC00U);
            } else if (first >= 0xDC00U && first <= 0xDFFFU) return false;
            appendCodepoint(codepoint, output);
            break;
          }
          default: return false;
        }
      }
      if (output.size() > maximum) return false;
    }
    return false;
  }

  bool atEnd() const { return at_ == input_.size(); }

 private:
  bool hexQuad(uint32_t& output) {
    if (at_ + 4U > input_.size()) return false;
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
      const int digit = hexValue(input_[at_++]);
      if (digit < 0) return false;
      value = (value << 4U) | static_cast<uint32_t>(digit);
    }
    output = value;
    return true;
  }

  const std::string& input_;
  size_t at_ = 0;
};

bool validChecksum(const std::string& value) {
  if (value.size() != 64) return false;
  for (char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  return true;
}

Status readString(nvs_handle_t handle, const char* key, bool& present,
                  std::string& output) {
  present = false;
  output.clear();
  size_t length = 0;
  esp_err_t result = nvs_get_str(handle, key, nullptr, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return Status::success();
  if (result != ESP_OK || length == 0 ||
      length > kMaximumCredentialRecordBytes + 1U)
    return storageError("credential slot length invalid");
  std::vector<char> buffer(length, '\0');
  result = nvs_get_str(handle, key, buffer.data(), &length);
  if (result != ESP_OK || length == 0 || buffer[length - 1] != '\0')
    return storageError("credential slot read failed");
  output.assign(buffer.data(), length - 1U);
  std::fill(buffer.begin(), buffer.end(), '\0');
  present = true;
  return Status::success();
}

}  // namespace

Status JsonSha256CredentialCodec::encode(const CredentialSnapshot& snapshot,
                                         std::string& output) const {
  output.clear();
  if (!credentialSnapshotCoherent(snapshot))
    return Status(ErrorCode::InvalidArgument, 0, "credential snapshot invalid");
  const std::string canonical = canonicalRecord(snapshot);
  const std::string checksum = sha256Hex(canonical);
  if (!validChecksum(checksum) || canonical.empty())
    return storageError("credential checksum failed");
  output.assign(canonical.data(), canonical.size() - 1U);
  output.append(",\"checksum\":\"");
  output.append(checksum);
  output.append("\"}");
  return output.size() <= kMaximumCredentialRecordBytes
             ? Status::success()
             : Status(ErrorCode::TooLarge, 0, "credential record too large");
}

Status JsonSha256CredentialCodec::decode(const std::string& input,
                                         CredentialSnapshot& snapshot) const {
  snapshot.redact();
  snapshot = CredentialSnapshot();
  if (input.empty() || input.size() > kMaximumCredentialRecordBytes)
    return storageError("credential record size invalid");
  RecordCursor cursor(input);
  std::string checksum;
  CredentialSnapshot decoded;
  const bool valid = cursor.literal("{\"schema\":1,\"generation\":") &&
      cursor.unsigned32(decoded.generation) &&
      cursor.literal(",\"fingerprint\":") &&
      cursor.string(decoded.installationFingerprint, 256) &&
      cursor.literal(",\"device_id\":") &&
      cursor.string(decoded.deviceId, 6) &&
      cursor.literal(",\"pending\":{\"device_id\":") &&
      cursor.string(decoded.pending.deviceId, 6) &&
      cursor.literal(",\"token\":") &&
      cursor.string(decoded.pending.pairingToken, 1024) &&
      cursor.literal(",\"binding_url\":") &&
      cursor.string(decoded.pending.bindingUrl, 1024) &&
      cursor.literal(",\"expires_at\":") &&
      cursor.string(decoded.pending.expiresAt, 128) &&
      cursor.literal("},\"device_token\":") &&
      cursor.string(decoded.deviceToken, 2048) &&
      cursor.literal(",\"active\":") && cursor.boolean(decoded.active) &&
      cursor.literal(",\"checksum\":") && cursor.string(checksum, 64) &&
      cursor.literal("}") && cursor.atEnd();
  if (!valid || !validChecksum(checksum) ||
      sha256Hex(canonicalRecord(decoded)) != checksum ||
      !credentialSnapshotCoherent(decoded)) {
    decoded.redact();
    return storageError("credential record verification failed");
  }
  snapshot = decoded;
  return Status::success();
}

Status EspNvsCredentialJournalStore::inspect(CredentialJournalState& state) {
  state.redact();
  state = CredentialJournalState();
  nvs_handle_t handle = 0;
  // Inspection is part of the first-upgrade audit and must not create an
  // empty namespace. A missing namespace is a coherent fresh state; write
  // operations below remain the only paths allowed to create it.
  const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) {
    state.namespaceAvailable = true;
    return Status::success();
  }
  if (opened != ESP_OK) return storageError("credential NVS open failed");
  state.namespaceAvailable = true;
  uint8_t marker = 0;
  esp_err_t result = nvs_get_u8(handle, kMarkerKey, &marker);
  state.markerPresent = result == ESP_OK;
  state.markerValid = state.markerPresent && marker == kCredentialInitializedMarker;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return storageError("credential marker read failed");
  }
  uint32_t head = 0;
  result = nvs_get_u32(handle, kHeadKey, &head);
  state.headPresent = result == ESP_OK;
  state.head = state.headPresent ? head : 0;
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return storageError("credential head read failed");
  }
  Status status = readString(handle, kSlot0Key, state.slotPresent[0], state.slot[0]);
  if (status.ok())
    status = readString(handle, kSlot1Key, state.slotPresent[1], state.slot[1]);
  nvs_close(handle);
  if (!status.ok()) state.redact();
  return status;
}

Status EspNvsCredentialJournalStore::writeSlotAndCommit(
    uint8_t slot, const std::string& encoded) {
  if (slot > 1 || encoded.empty() ||
      encoded.size() > kMaximumCredentialRecordBytes)
    return Status(ErrorCode::InvalidArgument, 0, "credential slot invalid");
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storageError("credential NVS open failed");
  const char* key = slot ? kSlot1Key : kSlot0Key;
  esp_err_t result = nvs_set_str(handle, key, encoded.c_str());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? Status::success()
                          : storageError("credential slot write failed");
}

Status EspNvsCredentialJournalStore::writeHeadAndMarkerAndCommit(
    uint32_t generation) {
  if (generation == 0)
    return Status(ErrorCode::InvalidArgument, 0, "credential head invalid");
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return storageError("credential NVS open failed");
  esp_err_t result = nvs_set_u32(handle, kHeadKey, generation);
  if (result == ESP_OK)
    result = nvs_set_u8(handle, kMarkerKey, kCredentialInitializedMarker);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK ? Status::success()
                          : storageError("credential head write failed");
}

}  // namespace myai
}  // namespace inkloop
