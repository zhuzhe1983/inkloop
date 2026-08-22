#include "inkloop/ota_https_acquisition.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace inkloop {
namespace {

constexpr char kHttpsPrefix[] = "https://";

bool textEqual(OtaTextView left, const char* right) {
  if (!right) return false;
  const std::size_t length = std::strlen(right);
  return left.data && left.length == length &&
      std::memcmp(left.data, right, length) == 0;
}

bool copyText(OtaTextView input, char* output, std::size_t capacity,
              std::size_t& length) {
  length = 0U;
  if (!input.data || input.length == 0U || input.length > capacity)
    return false;
  std::memcpy(output, input.data, input.length);
  length = input.length;
  return true;
}

bool hostCharacter(char value) {
  return (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9') || value == '-' || value == '.';
}

bool validHost(OtaTextView host) {
  if (!host.data || host.length == 0U ||
      host.length > kMaximumOtaHostBytes)
    return false;
  bool have_alpha = false;
  bool have_dot = false;
  std::size_t label_length = 0U;
  for (std::size_t at = 0U; at < host.length; ++at) {
    const char value = host.data[at];
    if (!hostCharacter(value)) return false;
    if (value == '.') {
      if (label_length == 0U || label_length > 63U ||
          host.data[at - 1U] == '-')
        return false;
      label_length = 0U;
      have_dot = true;
      continue;
    }
    if (label_length == 0U && value == '-') return false;
    ++label_length;
    have_alpha = have_alpha || (value >= 'a' && value <= 'z');
  }
  return have_alpha && have_dot && label_length != 0U &&
      label_length <= 63U && host.data[host.length - 1U] != '-';
}

bool pathCharacter(char value) {
  return (value >= 'a' && value <= 'z') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '/' || value == '-' ||
      value == '_' || value == '.' || value == '~';
}

struct SemVersion {
  std::array<std::uint32_t, 3U> core{};
  OtaTextView prerelease{};
};

bool decimalComponent(OtaTextView input, std::size_t& at,
                      std::uint32_t& output) {
  const std::size_t start = at;
  std::uint64_t value = 0U;
  while (at < input.length && input.data[at] >= '0' &&
         input.data[at] <= '9') {
    if (value > (std::numeric_limits<std::uint32_t>::max() -
                 static_cast<unsigned>(input.data[at] - '0')) / 10U)
      return false;
    value = value * 10U + static_cast<unsigned>(input.data[at] - '0');
    ++at;
  }
  if (at == start || (at - start > 1U && input.data[start] == '0'))
    return false;
  output = static_cast<std::uint32_t>(value);
  return true;
}

bool validIdentifierSequence(OtaTextView sequence, bool prerelease) {
  if (!sequence.data || sequence.length == 0U) return false;
  std::size_t identifier_start = 0U;
  bool identifier_numeric = true;
  for (std::size_t at = 0U; at <= sequence.length; ++at) {
    if (at == sequence.length || sequence.data[at] == '.') {
      if (at == identifier_start ||
          (prerelease && identifier_numeric &&
           at - identifier_start > 1U &&
           sequence.data[identifier_start] == '0'))
        return false;
      identifier_start = at + 1U;
      identifier_numeric = true;
      continue;
    }
    const char value = sequence.data[at];
    const bool numeric = value >= '0' && value <= '9';
    const bool alpha = (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z');
    if (!numeric && !alpha && value != '-') return false;
    identifier_numeric = identifier_numeric && numeric;
  }
  return true;
}

bool parseSemVersion(OtaTextView input, SemVersion& output) {
  output = SemVersion{};
  if (!input.data || input.length == 0U ||
      input.length > kMaximumOtaFirmwareVersionBytes)
    return false;
  std::size_t at = 0U;
  for (std::size_t part = 0U; part < output.core.size(); ++part) {
    if (!decimalComponent(input, at, output.core[part])) return false;
    if (part + 1U < output.core.size()) {
      if (at >= input.length || input.data[at++] != '.') return false;
    }
  }
  if (at < input.length && input.data[at] == '-') {
    const std::size_t start = ++at;
    while (at < input.length && input.data[at] != '+') ++at;
    output.prerelease = {input.data + start, at - start};
    if (!validIdentifierSequence(output.prerelease, true)) return false;
  }
  if (at < input.length && input.data[at] == '+') {
    const std::size_t start = ++at;
    const OtaTextView build{input.data + start, input.length - start};
    if (!validIdentifierSequence(build, false)) return false;
    at = input.length;
  }
  return at == input.length;
}

struct Identifier {
  OtaTextView bytes{};
  bool numeric = false;
};

bool nextIdentifier(OtaTextView sequence, std::size_t& at,
                    Identifier& output) {
  if (at >= sequence.length) return false;
  const std::size_t start = at;
  bool numeric = true;
  while (at < sequence.length && sequence.data[at] != '.') {
    numeric = numeric && sequence.data[at] >= '0' &&
        sequence.data[at] <= '9';
    ++at;
  }
  output.bytes = {sequence.data + start, at - start};
  output.numeric = numeric;
  if (at < sequence.length) ++at;
  return true;
}

int compareBytes(OtaTextView left, OtaTextView right) {
  const std::size_t common = std::min(left.length, right.length);
  const int compared = std::memcmp(left.data, right.data, common);
  if (compared < 0) return -1;
  if (compared > 0) return 1;
  if (left.length < right.length) return -1;
  if (left.length > right.length) return 1;
  return 0;
}

int compareSemVersion(const SemVersion& left, const SemVersion& right) {
  for (std::size_t at = 0U; at < left.core.size(); ++at) {
    if (left.core[at] < right.core[at]) return -1;
    if (left.core[at] > right.core[at]) return 1;
  }
  if (left.prerelease.length == 0U && right.prerelease.length == 0U)
    return 0;
  if (left.prerelease.length == 0U) return 1;
  if (right.prerelease.length == 0U) return -1;
  std::size_t left_at = 0U;
  std::size_t right_at = 0U;
  while (left_at < left.prerelease.length &&
         right_at < right.prerelease.length) {
    Identifier left_id;
    Identifier right_id;
    nextIdentifier(left.prerelease, left_at, left_id);
    nextIdentifier(right.prerelease, right_at, right_id);
    if (left_id.numeric && !right_id.numeric) return -1;
    if (!left_id.numeric && right_id.numeric) return 1;
    int compared = 0;
    if (left_id.numeric) {
      if (left_id.bytes.length < right_id.bytes.length) return -1;
      if (left_id.bytes.length > right_id.bytes.length) return 1;
      compared = compareBytes(left_id.bytes, right_id.bytes);
    } else {
      compared = compareBytes(left_id.bytes, right_id.bytes);
    }
    if (compared != 0) return compared;
  }
  if (left_at < left.prerelease.length) return 1;
  if (right_at < right.prerelease.length) return -1;
  return 0;
}

struct JsonString {
  const std::uint8_t* data = nullptr;
  std::size_t length = 0U;
};

class JsonCursor final {
 public:
  explicit JsonCursor(OtaBytesView input) : input_(input) {}

  bool take(char expected) {
    whitespace();
    if (at_ >= input_.length || input_.data[at_] !=
        static_cast<std::uint8_t>(expected))
      return false;
    ++at_;
    return true;
  }

  bool string(JsonString& output) {
    whitespace();
    if (at_ >= input_.length || input_.data[at_++] != '"') return false;
    const std::size_t start = at_;
    while (at_ < input_.length) {
      const std::uint8_t value = input_.data[at_++];
      if (value == '"') {
        output = {input_.data + start, at_ - start - 1U};
        return true;
      }
      if (value < 0x20U || value > 0x7EU || value == '\\') return false;
    }
    return false;
  }

  bool unsignedInteger(std::uint64_t& output) {
    whitespace();
    const std::size_t start = at_;
    std::uint64_t value = 0U;
    while (at_ < input_.length && input_.data[at_] >= '0' &&
           input_.data[at_] <= '9') {
      const std::uint8_t digit =
          static_cast<std::uint8_t>(input_.data[at_] - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
        return false;
      value = value * 10U + digit;
      ++at_;
    }
    if (at_ == start || (at_ - start > 1U && input_.data[start] == '0'))
      return false;
    output = value;
    return true;
  }

  bool at(char expected) {
    whitespace();
    return at_ < input_.length && input_.data[at_] ==
        static_cast<std::uint8_t>(expected);
  }

  bool done() {
    whitespace();
    return at_ == input_.length;
  }

 private:
  void whitespace() {
    while (at_ < input_.length &&
           (input_.data[at_] == ' ' || input_.data[at_] == '\n' ||
            input_.data[at_] == '\r' || input_.data[at_] == '\t'))
      ++at_;
  }

  OtaBytesView input_{};
  std::size_t at_ = 0U;
};

OtaTextView text(const JsonString& value) {
  return {reinterpret_cast<const char*>(value.data), value.length};
}

bool keyEqual(const JsonString& key, const char* expected) {
  return textEqual(text(key), expected);
}

bool lowerHexNibble(std::uint8_t value, std::uint8_t& output) {
  if (value >= '0' && value <= '9') {
    output = static_cast<std::uint8_t>(value - '0');
    return true;
  }
  if (value >= 'a' && value <= 'f') {
    output = static_cast<std::uint8_t>(value - 'a' + 10U);
    return true;
  }
  return false;
}

bool decodeLowerHex(const JsonString& input, std::uint8_t* output,
                    std::size_t output_length) {
  if (!output || input.length != output_length * 2U) return false;
  for (std::size_t at = 0U; at < output_length; ++at) {
    std::uint8_t high = 0U;
    std::uint8_t low = 0U;
    if (!lowerHexNibble(input.data[at * 2U], high) ||
        !lowerHexNibble(input.data[at * 2U + 1U], low))
      return false;
    output[at] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

OtaManifestParseCode mapManifestCode(OtaManifestCode code) {
  switch (code) {
    case OtaManifestCode::Ok: return OtaManifestParseCode::Ok;
    case OtaManifestCode::BoardMismatch:
    case OtaManifestCode::InvalidBoardSku:
      return OtaManifestParseCode::InvalidBoard;
    case OtaManifestCode::InvalidFirmwareVersion:
      return OtaManifestParseCode::InvalidVersion;
    case OtaManifestCode::InvalidImageSize:
      return OtaManifestParseCode::InvalidImageSize;
    case OtaManifestCode::InvalidDigest:
      return OtaManifestParseCode::InvalidDigest;
    case OtaManifestCode::UnknownSignaturePolicy:
      return OtaManifestParseCode::InvalidSignaturePolicy;
    case OtaManifestCode::InvalidSignature:
      return OtaManifestParseCode::InvalidSignature;
    case OtaManifestCode::InvalidArgument:
    case OtaManifestCode::UnsupportedSchema:
    case OtaManifestCode::CanonicalOverflow:
      return OtaManifestParseCode::ManifestRejected;
  }
  return OtaManifestParseCode::ManifestRejected;
}

class ManifestSink final : public IOtaHttpsBodySink {
 public:
  bool append(const std::uint8_t* bytes, std::size_t length) override {
    if (!bytes || length == 0U || length > bytes_.size() - length_)
      return false;
    std::memcpy(bytes_.data() + length_, bytes, length);
    length_ += length;
    return true;
  }

  OtaBytesView view() const { return {bytes_.data(), length_}; }

 private:
  std::array<std::uint8_t, kMaximumOtaManifestDocumentBytes> bytes_{};
  std::size_t length_ = 0U;
};

class StagingSink final : public IOtaHttpsBodySink {
 public:
  explicit StagingSink(EspOtaStagingAdapter& staging) : staging_(staging) {}

  bool append(const std::uint8_t* bytes, std::size_t length) override {
    observation_ = staging_.write(bytes, length);
    return observation_.code == EspOtaStagingCode::Ok;
  }

  const EspOtaStagingObservation& observation() const {
    return observation_;
  }

 private:
  EspOtaStagingAdapter& staging_;
  EspOtaStagingObservation observation_{};
};

OtaHttpsAcquisitionObservation acquisitionFailure(
    OtaHttpsAcquisitionCode code) {
  OtaHttpsAcquisitionObservation output;
  output.code = code;
  return output;
}

}  // namespace

OtaHttpsUrlCode parseOtaHttpsUrl(OtaTextView url,
                                 ParsedOtaHttpsUrl& output) {
  output = ParsedOtaHttpsUrl{};
  if (!url.data || url.length == 0U)
    return OtaHttpsUrlCode::InvalidArgument;
  if (url.length > kMaximumOtaUrlBytes) return OtaHttpsUrlCode::TooLong;
  constexpr std::size_t kPrefixLength = sizeof(kHttpsPrefix) - 1U;
  if (url.length <= kPrefixLength ||
      std::memcmp(url.data, kHttpsPrefix, kPrefixLength) != 0)
    return OtaHttpsUrlCode::NonHttps;

  const std::size_t authority_start = kPrefixLength;
  std::size_t authority_end = authority_start;
  while (authority_end < url.length && url.data[authority_end] != '/' &&
         url.data[authority_end] != '?' && url.data[authority_end] != '#')
    ++authority_end;
  if (authority_end == authority_start || authority_end == url.length)
    return OtaHttpsUrlCode::InvalidPath;
  for (std::size_t at = authority_start; at < authority_end; ++at) {
    if (url.data[at] == '@') return OtaHttpsUrlCode::CredentialsRejected;
  }

  std::size_t host_end = authority_end;
  std::size_t colon = authority_end;
  for (std::size_t at = authority_start; at < authority_end; ++at) {
    if (url.data[at] == ':') {
      if (colon != authority_end) return OtaHttpsUrlCode::InvalidHost;
      colon = at;
    }
  }
  if (colon != authority_end) host_end = colon;
  const OtaTextView host{url.data + authority_start,
                         host_end - authority_start};
  if (!validHost(host)) return OtaHttpsUrlCode::InvalidHost;
  std::memcpy(output.host.data(), host.data, host.length);
  output.host_length = host.length;

  if (colon != authority_end) {
    if (colon + 1U == authority_end) return OtaHttpsUrlCode::InvalidPort;
    std::uint32_t port = 0U;
    for (std::size_t at = colon + 1U; at < authority_end; ++at) {
      if (url.data[at] < '0' || url.data[at] > '9')
        return OtaHttpsUrlCode::InvalidPort;
      port = port * 10U + static_cast<unsigned>(url.data[at] - '0');
      if (port > 65535U) return OtaHttpsUrlCode::InvalidPort;
    }
    if (port == 0U) return OtaHttpsUrlCode::InvalidPort;
    output.port = static_cast<std::uint16_t>(port);
  }

  if (url.data[authority_end] != '/') return OtaHttpsUrlCode::InvalidPath;
  for (std::size_t at = authority_end; at < url.length; ++at) {
    if (!pathCharacter(url.data[at])) return OtaHttpsUrlCode::InvalidPath;
  }
  return OtaHttpsUrlCode::Ok;
}

bool sameOtaHttpsOrigin(const ParsedOtaHttpsUrl& left,
                        const ParsedOtaHttpsUrl& right) {
  return left.host_length != 0U &&
      left.host_length == right.host_length && left.port == right.port &&
      std::memcmp(left.host.data(), right.host.data(), left.host_length) == 0;
}

ReviewedOtaManifest AcquiredOtaManifest::reviewed() const {
  ReviewedOtaManifest output;
  output.schema_version = schema_version;
  output.board_sku = {board_sku.data(), board_sku_length};
  output.firmware_version = {firmware_version.data(),
                             firmware_version_length};
  output.image_size = image_size;
  output.image_sha256 = image_sha256;
  output.signature_policy = {signature_policy.data(),
                             signature_policy_length};
  output.detached_signature = {detached_signature.data(),
                               detached_signature.size()};
  return output;
}

OtaTextView AcquiredOtaManifest::imageUrl() const {
  return {image_url.data(), image_url_length};
}

OtaManifestParseCode parseOtaManifestDocument(
    OtaBytesView document, OtaTextView device_board_sku,
    OtaTextView current_firmware_version, AcquiredOtaManifest& output) {
  output = AcquiredOtaManifest{};
  if (!document.data || document.length == 0U || !device_board_sku.data ||
      device_board_sku.length == 0U || !current_firmware_version.data ||
      current_firmware_version.length == 0U)
    return OtaManifestParseCode::InvalidArgument;
  if (document.length > kMaximumOtaManifestDocumentBytes)
    return OtaManifestParseCode::DocumentTooLarge;

  AcquiredOtaManifest candidate;
  JsonCursor cursor(document);
  if (!cursor.take('{')) return OtaManifestParseCode::MalformedJson;
  std::uint16_t fields = 0U;
  constexpr std::uint16_t kAllFields = 0xFFU;
  while (!cursor.at('}')) {
    JsonString key;
    if (!cursor.string(key) || !cursor.take(':'))
      return OtaManifestParseCode::MalformedJson;
    std::uint16_t field = 0U;
    if (keyEqual(key, "schema_version")) field = 1U << 0U;
    else if (keyEqual(key, "board_sku")) field = 1U << 1U;
    else if (keyEqual(key, "firmware_version")) field = 1U << 2U;
    else if (keyEqual(key, "image_url")) field = 1U << 3U;
    else if (keyEqual(key, "image_size")) field = 1U << 4U;
    else if (keyEqual(key, "image_sha256")) field = 1U << 5U;
    else if (keyEqual(key, "signature_policy")) field = 1U << 6U;
    else if (keyEqual(key, "detached_signature")) field = 1U << 7U;
    else return OtaManifestParseCode::UnknownField;
    if ((fields & field) != 0U) return OtaManifestParseCode::DuplicateField;
    fields = static_cast<std::uint16_t>(fields | field);

    if (field == (1U << 0U)) {
      std::uint64_t value = 0U;
      if (!cursor.unsignedInteger(value) ||
          value > std::numeric_limits<std::uint16_t>::max())
        return OtaManifestParseCode::MalformedJson;
      candidate.schema_version = static_cast<std::uint16_t>(value);
    } else if (field == (1U << 4U)) {
      if (!cursor.unsignedInteger(candidate.image_size))
        return OtaManifestParseCode::MalformedJson;
    } else {
      JsonString value;
      if (!cursor.string(value)) return OtaManifestParseCode::MalformedJson;
      const OtaTextView value_text = text(value);
      if (field == (1U << 1U)) {
        if (!copyText(value_text, candidate.board_sku.data(),
                      candidate.board_sku.size(),
                      candidate.board_sku_length))
          return OtaManifestParseCode::InvalidBoard;
      } else if (field == (1U << 2U)) {
        if (!copyText(value_text, candidate.firmware_version.data(),
                      candidate.firmware_version.size(),
                      candidate.firmware_version_length))
          return OtaManifestParseCode::InvalidVersion;
      } else if (field == (1U << 3U)) {
        if (!copyText(value_text, candidate.image_url.data(),
                      candidate.image_url.size(), candidate.image_url_length))
          return OtaManifestParseCode::InvalidImageUrl;
      } else if (field == (1U << 5U)) {
        if (!decodeLowerHex(value, candidate.image_sha256.data(),
                            candidate.image_sha256.size()))
          return OtaManifestParseCode::InvalidDigest;
      } else if (field == (1U << 6U)) {
        if (!copyText(value_text, candidate.signature_policy.data(),
                      candidate.signature_policy.size(),
                      candidate.signature_policy_length))
          return OtaManifestParseCode::InvalidSignaturePolicy;
      } else if (field == (1U << 7U)) {
        if (!decodeLowerHex(value, candidate.detached_signature.data(),
                            candidate.detached_signature.size()))
          return OtaManifestParseCode::InvalidSignature;
      }
    }
    if (cursor.at(',')) {
      if (!cursor.take(',')) return OtaManifestParseCode::MalformedJson;
      if (cursor.at('}')) return OtaManifestParseCode::MalformedJson;
    } else if (!cursor.at('}')) {
      return OtaManifestParseCode::MalformedJson;
    }
  }
  if (!cursor.take('}') || !cursor.done())
    return OtaManifestParseCode::MalformedJson;
  if (fields != kAllFields) return OtaManifestParseCode::MissingField;
  if (candidate.schema_version != kOtaManifestSchemaVersion)
    return OtaManifestParseCode::InvalidSchema;

  SemVersion current;
  SemVersion target;
  if (!parseSemVersion(current_firmware_version, current) ||
      !parseSemVersion({candidate.firmware_version.data(),
                        candidate.firmware_version_length}, target))
    return OtaManifestParseCode::InvalidVersion;
  if (compareSemVersion(target, current) <= 0)
    return OtaManifestParseCode::TargetNotNewer;

  ParsedOtaHttpsUrl image_url;
  if (parseOtaHttpsUrl(candidate.imageUrl(), image_url) !=
      OtaHttpsUrlCode::Ok)
    return OtaManifestParseCode::InvalidImageUrl;

  PreparedOtaManifest prepared;
  const OtaManifestCode manifest_code = prepareOtaManifest(
      candidate.reviewed(), device_board_sku, prepared);
  const OtaManifestParseCode mapped = mapManifestCode(manifest_code);
  if (mapped != OtaManifestParseCode::Ok) return mapped;
  output = candidate;
  return OtaManifestParseCode::Ok;
}

OtaHttpsAcquisitionObservation OtaHttpsAcquisition::run(
    const OtaHttpsAcquisitionConfig& config) {
  if (attempted_)
    return acquisitionFailure(OtaHttpsAcquisitionCode::InvalidState);
  attempted_ = true;
  if (config.total_deadline_ms == 0U ||
      config.total_deadline_ms > kMaximumOtaAcquisitionDeadlineMs ||
      !config.device_board_sku.data || config.device_board_sku.length == 0U ||
      !config.current_firmware_version.data ||
      config.current_firmware_version.length == 0U) {
    return acquisitionFailure(OtaHttpsAcquisitionCode::InvalidConfiguration);
  }
  ParsedOtaHttpsUrl manifest_origin;
  if (parseOtaHttpsUrl(config.manifest_url, manifest_origin) !=
      OtaHttpsUrlCode::Ok) {
    return acquisitionFailure(OtaHttpsAcquisitionCode::InvalidConfiguration);
  }
  SemVersion current;
  if (!parseSemVersion(config.current_firmware_version, current))
    return acquisitionFailure(OtaHttpsAcquisitionCode::InvalidConfiguration);

  const std::uint64_t started = clock_.nowMs();
  if (started > std::numeric_limits<std::uint64_t>::max() -
                    config.total_deadline_ms)
    return acquisitionFailure(OtaHttpsAcquisitionCode::InvalidConfiguration);
  const std::uint64_t deadline = started + config.total_deadline_ms;
  if (clock_.nowMs() >= deadline)
    return acquisitionFailure(OtaHttpsAcquisitionCode::DeadlineExceeded);

  OtaHttpsAcquisitionObservation result;
  result.deadline_ms = deadline;
  ManifestSink manifest_sink;
  const OtaHttpsFetchRequest manifest_request{
      config.manifest_url, deadline, 0U,
      kMaximumOtaManifestDocumentBytes,
      kMaximumOtaHttpsTransportChunkBytes};
  result.fetch = transport_.get(manifest_request, manifest_sink);
  if (result.fetch.code != OtaHttpsFetchCode::Ok) {
    result.code = result.fetch.code == OtaHttpsFetchCode::DeadlineExceeded
        ? OtaHttpsAcquisitionCode::DeadlineExceeded
        : OtaHttpsAcquisitionCode::ManifestFetchFailed;
    return result;
  }
  if (clock_.nowMs() >= deadline) {
    result.code = OtaHttpsAcquisitionCode::DeadlineExceeded;
    return result;
  }

  AcquiredOtaManifest manifest;
  result.manifest_code = parseOtaManifestDocument(
      manifest_sink.view(), config.device_board_sku,
      config.current_firmware_version, manifest);
  if (result.manifest_code != OtaManifestParseCode::Ok) {
    result.code = OtaHttpsAcquisitionCode::ManifestRejected;
    return result;
  }
  ParsedOtaHttpsUrl image_origin;
  if (parseOtaHttpsUrl(manifest.imageUrl(), image_origin) !=
          OtaHttpsUrlCode::Ok ||
      !sameOtaHttpsOrigin(manifest_origin, image_origin)) {
    result.code = OtaHttpsAcquisitionCode::ImageOriginMismatch;
    return result;
  }

  result.staging = staging_.begin(manifest.reviewed(),
                                  config.device_board_sku);
  if (result.staging.code != EspOtaStagingCode::Ok) {
    result.code = OtaHttpsAcquisitionCode::StagingBeginFailed;
    return result;
  }
  if (clock_.nowMs() >= deadline) {
    result.staging = staging_.abort();
    result.code = OtaHttpsAcquisitionCode::DeadlineExceeded;
    return result;
  }

  StagingSink image_sink(staging_);
  const OtaHttpsFetchRequest image_request{
      manifest.imageUrl(), deadline, manifest.image_size,
      manifest.image_size, kMaximumOtaHttpsTransportChunkBytes};
  result.fetch = transport_.get(image_request, image_sink);
  if (result.fetch.code != OtaHttpsFetchCode::Ok) {
    if (result.fetch.code == OtaHttpsFetchCode::SinkRejected)
      result.staging = image_sink.observation();
    else
      result.staging = staging_.abort();
    result.code = result.fetch.code == OtaHttpsFetchCode::DeadlineExceeded
        ? OtaHttpsAcquisitionCode::DeadlineExceeded
        : OtaHttpsAcquisitionCode::ImageFetchFailed;
    return result;
  }
  if (clock_.nowMs() >= deadline) {
    result.staging = staging_.abort();
    result.code = OtaHttpsAcquisitionCode::DeadlineExceeded;
    return result;
  }
  result.staging = staging_.finish();
  if (result.staging.code != EspOtaStagingCode::Ok) {
    result.code = OtaHttpsAcquisitionCode::StagingFinishFailed;
    return result;
  }
  result.code = OtaHttpsAcquisitionCode::Ok;
  return result;
}

const char* otaHttpsUrlCodeName(OtaHttpsUrlCode code) {
  switch (code) {
    case OtaHttpsUrlCode::Ok: return "OK";
    case OtaHttpsUrlCode::InvalidArgument: return "INVALID_ARGUMENT";
    case OtaHttpsUrlCode::TooLong: return "TOO_LONG";
    case OtaHttpsUrlCode::NonHttps: return "NON_HTTPS";
    case OtaHttpsUrlCode::CredentialsRejected: return "CREDENTIALS_REJECTED";
    case OtaHttpsUrlCode::InvalidHost: return "INVALID_HOST";
    case OtaHttpsUrlCode::InvalidPort: return "INVALID_PORT";
    case OtaHttpsUrlCode::InvalidPath: return "INVALID_PATH";
  }
  return "UNKNOWN";
}

const char* otaManifestParseCodeName(OtaManifestParseCode code) {
  switch (code) {
    case OtaManifestParseCode::Ok: return "OK";
    case OtaManifestParseCode::InvalidArgument: return "INVALID_ARGUMENT";
    case OtaManifestParseCode::DocumentTooLarge: return "DOCUMENT_TOO_LARGE";
    case OtaManifestParseCode::MalformedJson: return "MALFORMED_JSON";
    case OtaManifestParseCode::DuplicateField: return "DUPLICATE_FIELD";
    case OtaManifestParseCode::UnknownField: return "UNKNOWN_FIELD";
    case OtaManifestParseCode::MissingField: return "MISSING_FIELD";
    case OtaManifestParseCode::InvalidSchema: return "INVALID_SCHEMA";
    case OtaManifestParseCode::InvalidBoard: return "INVALID_BOARD";
    case OtaManifestParseCode::InvalidVersion: return "INVALID_VERSION";
    case OtaManifestParseCode::TargetNotNewer: return "TARGET_NOT_NEWER";
    case OtaManifestParseCode::InvalidImageUrl: return "INVALID_IMAGE_URL";
    case OtaManifestParseCode::InvalidImageSize: return "INVALID_IMAGE_SIZE";
    case OtaManifestParseCode::InvalidDigest: return "INVALID_DIGEST";
    case OtaManifestParseCode::InvalidSignaturePolicy:
      return "INVALID_SIGNATURE_POLICY";
    case OtaManifestParseCode::InvalidSignature: return "INVALID_SIGNATURE";
    case OtaManifestParseCode::ManifestRejected: return "MANIFEST_REJECTED";
  }
  return "UNKNOWN";
}

const char* otaHttpsFetchCodeName(OtaHttpsFetchCode code) {
  switch (code) {
    case OtaHttpsFetchCode::Ok: return "OK";
    case OtaHttpsFetchCode::InvalidRequest: return "INVALID_REQUEST";
    case OtaHttpsFetchCode::DeadlineExceeded: return "DEADLINE_EXCEEDED";
    case OtaHttpsFetchCode::UrlRejected: return "URL_REJECTED";
    case OtaHttpsFetchCode::ClientUnavailable: return "CLIENT_UNAVAILABLE";
    case OtaHttpsFetchCode::ConnectionFailed: return "CONNECTION_FAILED";
    case OtaHttpsFetchCode::PeerRejected: return "PEER_REJECTED";
    case OtaHttpsFetchCode::RedirectRejected: return "REDIRECT_REJECTED";
    case OtaHttpsFetchCode::HttpStatusRejected: return "HTTP_STATUS_REJECTED";
    case OtaHttpsFetchCode::ContentLengthRequired:
      return "CONTENT_LENGTH_REQUIRED";
    case OtaHttpsFetchCode::ContentLengthMismatch:
      return "CONTENT_LENGTH_MISMATCH";
    case OtaHttpsFetchCode::ResponseTooLarge: return "RESPONSE_TOO_LARGE";
    case OtaHttpsFetchCode::ReadFailed: return "READ_FAILED";
    case OtaHttpsFetchCode::Truncated: return "TRUNCATED";
    case OtaHttpsFetchCode::SinkRejected: return "SINK_REJECTED";
  }
  return "UNKNOWN";
}

const char* otaHttpsAcquisitionCodeName(OtaHttpsAcquisitionCode code) {
  switch (code) {
    case OtaHttpsAcquisitionCode::Ok: return "OK";
    case OtaHttpsAcquisitionCode::InvalidState: return "INVALID_STATE";
    case OtaHttpsAcquisitionCode::InvalidConfiguration:
      return "INVALID_CONFIGURATION";
    case OtaHttpsAcquisitionCode::DeadlineExceeded:
      return "DEADLINE_EXCEEDED";
    case OtaHttpsAcquisitionCode::ManifestFetchFailed:
      return "MANIFEST_FETCH_FAILED";
    case OtaHttpsAcquisitionCode::ManifestRejected:
      return "MANIFEST_REJECTED";
    case OtaHttpsAcquisitionCode::ImageOriginMismatch:
      return "IMAGE_ORIGIN_MISMATCH";
    case OtaHttpsAcquisitionCode::StagingBeginFailed:
      return "STAGING_BEGIN_FAILED";
    case OtaHttpsAcquisitionCode::ImageFetchFailed:
      return "IMAGE_FETCH_FAILED";
    case OtaHttpsAcquisitionCode::StagingFinishFailed:
      return "STAGING_FINISH_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
