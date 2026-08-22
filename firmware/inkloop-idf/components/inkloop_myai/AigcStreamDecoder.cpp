#include "AigcStreamDecoder.h"

#include <algorithm>
#include <cctype>

namespace inkloop {
namespace myai {
namespace {

constexpr size_t kMaximumMetadataPrefixBytes = 8192U;

bool extractJsonString(const std::string& prefix, const char* key,
                       std::string& value) {
  const std::string quoted = std::string("\"") + key + "\"";
  const size_t key_at = prefix.find(quoted);
  if (key_at == std::string::npos) return false;
  size_t at = key_at + quoted.size();
  while (at < prefix.size() &&
         std::isspace(static_cast<unsigned char>(prefix[at]))) ++at;
  if (at >= prefix.size() || prefix[at++] != ':') return false;
  while (at < prefix.size() &&
         std::isspace(static_cast<unsigned char>(prefix[at]))) ++at;
  if (at >= prefix.size() || prefix[at++] != '"') return false;
  value.clear();
  bool escaped = false;
  for (; at < prefix.size(); ++at) {
    const char ch = prefix[at];
    if (escaped) {
      if (ch == '"' || ch == '\\' || ch == '/') value.push_back(ch);
      else return false;
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return !value.empty();
    } else if (static_cast<unsigned char>(ch) < 0x20U) {
      return false;
    } else {
      value.push_back(ch);
    }
    if (value.size() > 128U) return false;
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

}  // namespace

AigcStreamDecoder::AigcStreamDecoder(
    size_t maximumEncodedBytes, size_t maximumDecodedBytes,
    IImageSink& sink, AigcOutputMetadata& metadata)
    : maximum_encoded_bytes_(maximumEncodedBytes),
      maximum_decoded_bytes_(maximumDecodedBytes),
      sink_(sink),
      metadata_(metadata) {
  prefix_.reserve(std::min(maximumEncodedBytes, kMaximumMetadataPrefixBytes));
  if (maximumEncodedBytes == 0 || maximumDecodedBytes == 0 ||
      maximumEncodedBytes > 8U * 1024U * 1024U ||
      maximumDecodedBytes > 6U * 1024U * 1024U) {
    status_ = Status(ErrorCode::InvalidArgument, 0,
                     "invalid AIGC stream limits");
  }
}

Status AigcStreamDecoder::fail(ErrorCode code, const char* detail) {
  if (status_.ok()) status_ = Status(code, 0, detail);
  return status_;
}

Status AigcStreamDecoder::decodeQuartet() {
  const int a = base64Value(quartet_[0]);
  const int b = base64Value(quartet_[1]);
  const int c = base64Value(quartet_[2]);
  const int d = base64Value(quartet_[3]);
  if (a < 0 || b < 0 || c == -1 || d == -1 || padded_ ||
      (c == -2 && d != -2)) {
    return fail(ErrorCode::Protocol, "invalid AIGC base64 payload");
  }
  uint8_t output[3]{};
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
  padded_ = c == -2 || d == -2;
  if (decoded_bytes_ > maximum_decoded_bytes_ ||
      count > maximum_decoded_bytes_ - decoded_bytes_) {
    return fail(ErrorCode::TooLarge, "AIGC decoded image exceeds cap");
  }
  const Status written = sink_.write(output, count);
  if (!written.ok()) {
    status_ = written;
    return status_;
  }
  decoded_bytes_ += count;
  return Status::success();
}

Status AigcStreamDecoder::consume(char ch) {
  if (!status_.ok()) return status_;
  if (complete_) {
    // Only JSON whitespace and the closing object/array delimiters may follow
    // the base64 string. The transport still requires HTTP EOF separately.
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '}' ||
        ch == ']') return Status::success();
    return fail(ErrorCode::Protocol, "unexpected AIGC output trailing bytes");
  }
  if (++envelope_bytes_ > maximum_encoded_bytes_ +
                              kMaximumMetadataPrefixBytes) {
    return fail(ErrorCode::TooLarge, "AIGC output envelope exceeds cap");
  }
  if (!decoding_) {
    if (prefix_.size() >= kMaximumMetadataPrefixBytes) {
      return fail(ErrorCode::Protocol, "AIGC output metadata too large");
    }
    prefix_.push_back(ch);
    static const std::string marker = "\"content_base64\"";
    const size_t marker_at = prefix_.find(marker);
    if (marker_at == std::string::npos) return Status::success();
    size_t at = marker_at + marker.size();
    while (at < prefix_.size() &&
           std::isspace(static_cast<unsigned char>(prefix_[at]))) ++at;
    if (at >= prefix_.size()) return Status::success();
    if (prefix_[at++] != ':') {
      return fail(ErrorCode::Protocol, "invalid AIGC output field");
    }
    while (at < prefix_.size() &&
           std::isspace(static_cast<unsigned char>(prefix_[at]))) ++at;
    if (at >= prefix_.size()) return Status::success();
    if (prefix_[at] != '"') {
      return fail(ErrorCode::Protocol, "invalid AIGC output base64 field");
    }
    if (!extractJsonString(prefix_.substr(0, marker_at), "content_type",
                           metadata_.contentType) ||
        (metadata_.contentType != "image/png" &&
         metadata_.contentType != "image/x-png")) {
      return fail(ErrorCode::Protocol,
                  "AIGC output content type missing or unsupported");
    }
    status_ = sink_.begin(metadata_);
    if (!status_.ok()) return status_;
    begun_ = true;
    decoding_ = true;
    prefix_.clear();
    return Status::success();
  }

  if (ch == '"') {
    if (encoded_bytes_ == 0 || quartet_length_ == 1) {
      return fail(ErrorCode::Protocol, "truncated AIGC base64 payload");
    }
    if (quartet_length_ == 2 || quartet_length_ == 3) {
      while (quartet_length_ < 4) quartet_[quartet_length_++] = '=';
      const Status decoded = decodeQuartet();
      if (!decoded.ok()) return decoded;
    }
    complete_ = true;
    return Status::success();
  }
  if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t')
    return Status::success();
  if (base64Value(ch) == -1) {
    return fail(ErrorCode::Protocol, "invalid AIGC base64 character");
  }
  if (encoded_bytes_ >= maximum_encoded_bytes_) {
    return fail(ErrorCode::TooLarge, "AIGC encoded image exceeds cap");
  }
  quartet_[quartet_length_++] = ch;
  ++encoded_bytes_;
  if (quartet_length_ == 4) {
    const Status decoded = decodeQuartet();
    quartet_length_ = 0;
    if (!decoded.ok()) return decoded;
  }
  return Status::success();
}

Status AigcStreamDecoder::append(const uint8_t* bytes, size_t length) {
  if (!status_.ok()) return status_;
  if (!bytes || length == 0)
    return fail(ErrorCode::InvalidArgument, "invalid AIGC stream chunk");
  for (size_t index = 0; index < length; ++index) {
    const Status consumed = consume(static_cast<char>(bytes[index]));
    if (!consumed.ok()) return consumed;
  }
  return Status::success();
}

Status AigcStreamDecoder::finish() {
  if (!status_.ok()) return status_;
  if (!begun_ || !complete_ || decoded_bytes_ == 0) {
    return fail(ErrorCode::Protocol, "AIGC output payload incomplete");
  }
  metadata_.decodedBytes = decoded_bytes_;
  return Status::success();
}

}  // namespace myai
}  // namespace inkloop
