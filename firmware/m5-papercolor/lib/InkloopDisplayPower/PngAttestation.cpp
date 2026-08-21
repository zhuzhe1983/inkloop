#include "PngAttestation.h"

#include <string.h>

#include "ImageProcessing.h"

namespace inkloop {
namespace displaypower {

namespace {

uint32_t rotateRight(uint32_t value, uint8_t shift) {
  return (value >> shift) | (value << (32U - shift));
}

uint32_t readBigEndian32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24U) |
      (static_cast<uint32_t>(bytes[1]) << 16U) |
      (static_cast<uint32_t>(bytes[2]) << 8U) |
      static_cast<uint32_t>(bytes[3]);
}

void writeBigEndian32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value >> 24U);
  bytes[1] = static_cast<uint8_t>(value >> 16U);
  bytes[2] = static_cast<uint8_t>(value >> 8U);
  bytes[3] = static_cast<uint8_t>(value);
}

class Sha256State {
 public:
  Sha256State() : totalBytes_(0), buffered_(0) {
    const uint32_t initial[] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    memcpy(state_, initial, sizeof(state_));
  }

  void update(const uint8_t* bytes, size_t length) {
    if (!bytes || length == 0) return;
    totalBytes_ += length;
    while (length > 0) {
      const size_t available = sizeof(buffer_) - buffered_;
      const size_t take = length < available ? length : available;
      memcpy(buffer_ + buffered_, bytes, take);
      buffered_ += take;
      bytes += take;
      length -= take;
      if (buffered_ == sizeof(buffer_)) {
        transform(buffer_);
        buffered_ = 0;
      }
    }
  }

  Sha256Digest finish() {
    const uint64_t bitLength = totalBytes_ * 8ULL;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56U) {
      while (buffered_ < sizeof(buffer_)) buffer_[buffered_++] = 0;
      transform(buffer_);
      buffered_ = 0;
    }
    while (buffered_ < 56U) buffer_[buffered_++] = 0;
    for (uint8_t index = 0; index < 8; ++index) {
      buffer_[63U - index] = static_cast<uint8_t>(bitLength >> (index * 8U));
    }
    transform(buffer_);
    Sha256Digest digest;
    for (uint8_t index = 0; index < 8; ++index) {
      writeBigEndian32(digest.bytes + index * 4U, state_[index]);
    }
    return digest;
  }

 private:
  void transform(const uint8_t* block) {
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    uint32_t words[64];
    for (uint8_t index = 0; index < 16; ++index) {
      words[index] = readBigEndian32(block + index * 4U);
    }
    for (uint8_t index = 16; index < 64; ++index) {
      const uint32_t previous15 = words[index - 15U];
      const uint32_t previous2 = words[index - 2U];
      const uint32_t small0 = rotateRight(previous15, 7) ^
          rotateRight(previous15, 18) ^ (previous15 >> 3U);
      const uint32_t small1 = rotateRight(previous2, 17) ^
          rotateRight(previous2, 19) ^ (previous2 >> 10U);
      words[index] = words[index - 16U] + small0 + words[index - 7U] + small1;
    }
    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (uint8_t index = 0; index < 64; ++index) {
      const uint32_t big1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const uint32_t choose = (e & f) ^ ((~e) & g);
      const uint32_t temporary1 = h + big1 + choose + constants[index] + words[index];
      const uint32_t big0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temporary2 = big0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  uint32_t state_[8];
  uint64_t totalBytes_;
  uint8_t buffer_[64];
  size_t buffered_;
};

uint32_t crc32Chunk(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return crc ^ 0xffffffffU;
}

bool validChunkType(const uint8_t* type) {
  for (uint8_t index = 0; index < 4; ++index) {
    if (!((type[index] >= 'A' && type[index] <= 'Z') ||
          (type[index] >= 'a' && type[index] <= 'z'))) {
      return false;
    }
  }
  // PNG reserves the third type-code bit; it must be uppercase in conforming
  // files so future critical semantics cannot be silently ignored.
  return (type[2] & 0x20U) == 0U;
}

bool chunkTypeEquals(const uint8_t* type, const char* expected) {
  return memcmp(type, expected, 4) == 0;
}

bool validBitDepthForColorType(uint8_t depth, uint8_t colorType) {
  if (colorType == 0) return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  if (colorType == 2) return depth == 8 || depth == 16;
  if (colorType == 3) return depth == 1 || depth == 2 || depth == 4 || depth == 8;
  if (colorType == 4 || colorType == 6) return depth == 8 || depth == 16;
  return false;
}

}  // namespace

Sha256Digest::Sha256Digest() : bytes() {}

bool Sha256Digest::operator==(const Sha256Digest& other) const {
  uint8_t difference = 0;
  for (size_t index = 0; index < sizeof(bytes); ++index) {
    difference |= bytes[index] ^ other.bytes[index];
  }
  return difference == 0;
}

Sha256Digest sha256Bytes(const uint8_t* bytes, size_t length) {
  Sha256State state;
  state.update(bytes, length);
  return state.finish();
}

ValidatedPng::ValidatedPng()
    : valid_(false), width_(0), height_(0), encodedLength_(0), digest_() {}

bool ValidatedPng::matchesExactBytes(const uint8_t* bytes, size_t length) const {
  return valid_ && bytes && length == encodedLength_ && sha256Bytes(bytes, length) == digest_;
}

PngValidationResult validatePaperColorPng(
    const uint8_t* bytes,
    size_t length,
    size_t maximumEncodedBytes) {
  PngValidationResult result;
  if (!bytes || length == 0) return result;
  if (maximumEncodedBytes == 0 || length > maximumEncodedBytes) {
    result.error = PngValidationError::EncodedSizeOutOfRange;
    return result;
  }
  static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (length < sizeof(signature) || memcmp(bytes, signature, sizeof(signature)) != 0) {
    result.error = PngValidationError::InvalidSignature;
    return result;
  }

  size_t offset = sizeof(signature);
  bool sawHeader = false;
  bool sawPalette = false;
  bool sawImageData = false;
  bool endedImageData = false;
  bool sawEnd = false;
  uint8_t colorType = 255;
  uint8_t bitDepth = 0;
  while (offset < length) {
    if (length - offset < 12U) {
      result.error = PngValidationError::TruncatedChunk;
      return result;
    }
    const uint32_t chunkLength = readBigEndian32(bytes + offset);
    const uint8_t* type = bytes + offset + 4U;
    if (!validChunkType(type)) {
      result.error = PngValidationError::InvalidChunkType;
      return result;
    }
    if (static_cast<size_t>(chunkLength) > length - offset - 12U) {
      result.error = PngValidationError::TruncatedChunk;
      return result;
    }
    const uint8_t* data = type + 4U;
    const uint32_t expectedCrc = readBigEndian32(data + chunkLength);
    if (crc32Chunk(type, static_cast<size_t>(chunkLength) + 4U) != expectedCrc) {
      result.error = PngValidationError::InvalidChunkCrc;
      return result;
    }

    if (!sawHeader) {
      if (!chunkTypeEquals(type, "IHDR") || chunkLength != 13U) {
        result.error = PngValidationError::MissingOrInvalidHeader;
        return result;
      }
      const uint32_t width = readBigEndian32(data);
      const uint32_t height = readBigEndian32(data + 4U);
      bitDepth = data[8];
      colorType = data[9];
      if (!validBitDepthForColorType(bitDepth, colorType) || data[10] != 0 ||
          data[11] != 0 || data[12] > 1) {
        result.error = PngValidationError::UnsupportedHeader;
        return result;
      }
      const bool portrait = width == kPaperColorWidth &&
          height == kPaperColorHeight;
      const bool bottomDown = width == kPaperColorHeight &&
          height == kPaperColorWidth;
      if (!portrait && !bottomDown) {
        result.error = PngValidationError::WrongDimensions;
        return result;
      }
      result.png.width_ = width;
      result.png.height_ = height;
      sawHeader = true;
    } else if (chunkTypeEquals(type, "IHDR")) {
      result.error = PngValidationError::InvalidChunkOrder;
      return result;
    } else if (chunkTypeEquals(type, "PLTE")) {
      if (sawPalette || sawImageData || chunkLength == 0 || chunkLength % 3U != 0 ||
          chunkLength > 768U || colorType == 0 || colorType == 4 ||
          (colorType == 3 && chunkLength / 3U > (1UL << bitDepth))) {
        result.error = PngValidationError::InvalidChunkOrder;
        return result;
      }
      sawPalette = true;
    } else if (chunkTypeEquals(type, "IDAT")) {
      if (endedImageData || (colorType == 3 && !sawPalette)) {
        result.error = PngValidationError::InvalidChunkOrder;
        return result;
      }
      sawImageData = true;
    } else if (chunkTypeEquals(type, "IEND")) {
      if (!sawImageData || chunkLength != 0 || sawEnd) {
        result.error = PngValidationError::InvalidChunkOrder;
        return result;
      }
      sawEnd = true;
    } else {
      if (sawImageData) endedImageData = true;
      // Unknown critical chunks are not safe to hand to a decoder.
      if ((type[0] & 0x20U) == 0U) {
        result.error = PngValidationError::InvalidChunkType;
        return result;
      }
    }
    offset += static_cast<size_t>(chunkLength) + 12U;
    if (sawEnd) break;
  }
  if (!sawHeader) {
    result.error = PngValidationError::MissingOrInvalidHeader;
    return result;
  }
  if (!sawImageData) {
    result.error = PngValidationError::MissingImageData;
    return result;
  }
  if (!sawEnd) {
    result.error = PngValidationError::MissingEnd;
    return result;
  }
  if (offset != length) {
    result.error = PngValidationError::TrailingData;
    return result;
  }
  result.png.valid_ = true;
  result.png.encodedLength_ = length;
  result.png.digest_ = sha256Bytes(bytes, length);
  result.error = PngValidationError::None;
  return result;
}

}  // namespace displaypower
}  // namespace inkloop
