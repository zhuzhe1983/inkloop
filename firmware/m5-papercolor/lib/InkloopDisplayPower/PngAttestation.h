#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {
namespace displaypower {

struct Sha256Digest {
  uint8_t bytes[32];

  Sha256Digest();
  bool operator==(const Sha256Digest& other) const;
  bool operator!=(const Sha256Digest& other) const { return !(*this == other); }
};

Sha256Digest sha256Bytes(const uint8_t* bytes, size_t length);

enum class PngValidationError : uint8_t {
  None,
  MissingBytes,
  EncodedSizeOutOfRange,
  InvalidSignature,
  TruncatedChunk,
  InvalidChunkType,
  InvalidChunkCrc,
  MissingOrInvalidHeader,
  UnsupportedHeader,
  WrongDimensions,
  InvalidChunkOrder,
  MissingImageData,
  MissingEnd,
  TrailingData,
};

struct PngValidationResult;

class ValidatedPng {
 public:
  ValidatedPng();

  bool valid() const { return valid_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  size_t encodedLength() const { return encodedLength_; }
  const Sha256Digest& digest() const { return digest_; }
  bool matchesExactBytes(const uint8_t* bytes, size_t length) const;

 private:
  friend struct PngValidationResult;
  friend PngValidationResult validatePaperColorPng(
      const uint8_t* bytes,
      size_t length,
      size_t maximumEncodedBytes);

  bool valid_;
  uint32_t width_;
  uint32_t height_;
  size_t encodedLength_;
  Sha256Digest digest_;
};

struct PngValidationResult {
  PngValidationError error;
  ValidatedPng png;

  PngValidationResult() : error(PngValidationError::MissingBytes), png() {}
};

PngValidationResult validatePaperColorPng(
    const uint8_t* bytes,
    size_t length,
    size_t maximumEncodedBytes = 16U * 1024U * 1024U);

}  // namespace displaypower
}  // namespace inkloop
