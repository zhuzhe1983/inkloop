#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace inkloop {

struct PaperColorPngHeader {
  bool valid;
  uint16_t width;
  uint16_t height;
  bool landscape;

  PaperColorPngHeader()
      : valid(false), width(0), height(0), landscape(false) {}
};

inline PaperColorPngHeader parsePaperColorPngHeader(
    const uint8_t* bytes, size_t length) {
  PaperColorPngHeader result;
  static const uint8_t signature[] = {
      0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  static const uint8_t ihdrPrefix[] = {
      0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R'};
  if (!bytes || length < 33 ||
      memcmp(bytes, signature, sizeof(signature)) != 0) return result;
  if (memcmp(bytes + 8, ihdrPrefix, sizeof(ihdrPrefix)) != 0) return result;
  const uint32_t width = (static_cast<uint32_t>(bytes[16]) << 24) |
      (static_cast<uint32_t>(bytes[17]) << 16) |
      (static_cast<uint32_t>(bytes[18]) << 8) | bytes[19];
  const uint32_t height = (static_cast<uint32_t>(bytes[20]) << 24) |
      (static_cast<uint32_t>(bytes[21]) << 16) |
      (static_cast<uint32_t>(bytes[22]) << 8) | bytes[23];
  if (!((width == 400 && height == 600) ||
        (width == 600 && height == 400))) return result;
  const uint8_t bitDepth = bytes[24];
  const uint8_t colorType = bytes[25];
  bool validFormat = false;
  switch (colorType) {
    case 0:
      validFormat = bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
          bitDepth == 8 || bitDepth == 16;
      break;
    case 2:
    case 4:
    case 6:
      validFormat = bitDepth == 8 || bitDepth == 16;
      break;
    case 3:
      validFormat = bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
          bitDepth == 8;
      break;
    default:
      return result;
  }
  if (!validFormat || bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1)
    return result;
  result.valid = true;
  result.width = static_cast<uint16_t>(width);
  result.height = static_cast<uint16_t>(height);
  result.landscape = width == 600;
  return result;
}

inline bool validPaperColorPngTrailer(
    const uint8_t* bytes, size_t length) {
  static const uint8_t trailer[] = {
      0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D',
      0xae, 0x42, 0x60, 0x82};
  return bytes && length >= sizeof(trailer) &&
      memcmp(bytes + length - sizeof(trailer), trailer, sizeof(trailer)) == 0;
}

class PaperColorPngStreamValidator {
 public:
  explicit PaperColorPngStreamValidator(size_t maximumBytes = 1500000U) {
    reset(maximumBytes);
  }

  void reset(size_t maximumBytes) {
    maximumBytes_ = maximumBytes;
    totalBytes_ = 0;
    signaturePosition_ = 0;
    chunkHeaderPosition_ = 0;
    chunkDataPosition_ = 0;
    chunkCrcPosition_ = 0;
    chunkLength_ = 0;
    runningCrc_ = 0xffffffffUL;
    expectedCrc_ = 0;
    phase_ = Phase::Signature;
    sawIhdr_ = false;
    sawPlte_ = false;
    sawIdat_ = false;
    idatSequenceClosed_ = false;
    complete_ = false;
    failed_ = false;
    failure_ = "none";
    landscape_ = false;
    colorType_ = 255;
    memset(chunkHeader_, 0, sizeof(chunkHeader_));
    memset(ihdrData_, 0, sizeof(ihdrData_));
  }

  bool append(const uint8_t* bytes, size_t length) {
    if (!bytes || !length || failed_ || complete_ ||
        totalBytes_ > maximumBytes_ || length > maximumBytes_ - totalBytes_) {
      failed_ = true;
      failure_ = complete_ ? "trailing_bytes" : "append_bounds";
      return false;
    }
    static const uint8_t signature[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    for (size_t index = 0; index < length; ++index) {
      if (complete_) return fail("trailing_bytes");
      const uint8_t value = bytes[index];
      ++totalBytes_;
      if (phase_ == Phase::Signature) {
        if (value != signature[signaturePosition_++])
          return fail("signature");
        if (signaturePosition_ == sizeof(signature)) {
          phase_ = Phase::ChunkHeader;
          chunkHeaderPosition_ = 0;
        }
        continue;
      }
      if (phase_ == Phase::ChunkHeader) {
        chunkHeader_[chunkHeaderPosition_++] = value;
        if (chunkHeaderPosition_ == sizeof(chunkHeader_) &&
            !beginChunk()) return false;
        continue;
      }
      if (phase_ == Phase::ChunkData) {
        runningCrc_ = updateCrc(runningCrc_, value);
        if (isType("IHDR") && chunkDataPosition_ < sizeof(ihdrData_))
          ihdrData_[chunkDataPosition_] = value;
        ++chunkDataPosition_;
        if (chunkDataPosition_ == chunkLength_) {
          phase_ = Phase::ChunkCrc;
          chunkCrcPosition_ = 0;
          expectedCrc_ = 0;
        }
        continue;
      }
      if (phase_ == Phase::ChunkCrc) {
        expectedCrc_ = (expectedCrc_ << 8) | value;
        ++chunkCrcPosition_;
        if (chunkCrcPosition_ == 4 && !finishChunk()) return false;
        continue;
      }
      return fail("invalid_phase");
    }
    return !failed_;
  }

  bool finish(size_t exactBytes) const {
    return !failed_ && complete_ && sawIhdr_ && sawIdat_ &&
        phase_ == Phase::Complete && totalBytes_ == exactBytes;
  }

  bool landscape() const { return landscape_; }
  size_t totalBytes() const { return totalBytes_; }
  const char* failureName() const {
    return failed_ ? failure_ : (complete_ ? "none" : "incomplete");
  }

 private:
  enum class Phase : uint8_t {
    Signature,
    ChunkHeader,
    ChunkData,
    ChunkCrc,
    Complete,
    Failed,
  };

  static uint32_t updateCrc(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
    }
    return crc;
  }

  bool isType(const char* expected) const {
    return expected && memcmp(chunkHeader_ + 4, expected, 4) == 0;
  }

  bool validChunkType() const {
    for (size_t index = 4; index < 8; ++index) {
      const uint8_t ch = chunkHeader_[index];
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
        return false;
    }
    // PNG reserves the third type letter as uppercase.
    return chunkHeader_[6] >= 'A' && chunkHeader_[6] <= 'Z';
  }

  bool beginChunk() {
    chunkLength_ = (static_cast<uint32_t>(chunkHeader_[0]) << 24) |
        (static_cast<uint32_t>(chunkHeader_[1]) << 16) |
        (static_cast<uint32_t>(chunkHeader_[2]) << 8) | chunkHeader_[3];
    if (!validChunkType() || totalBytes_ > maximumBytes_ ||
        maximumBytes_ - totalBytes_ < 4U ||
        chunkLength_ > maximumBytes_ - totalBytes_ - 4U)
      return fail("chunk_header_or_length");
    const bool ihdr = isType("IHDR");
    const bool plte = isType("PLTE");
    const bool idat = isType("IDAT");
    const bool iend = isType("IEND");
    const bool ancillary = (chunkHeader_[4] & 0x20U) != 0;
    if (!sawIhdr_ && !ihdr) return fail("ihdr_not_first");
    if (ihdr && (sawIhdr_ || chunkLength_ != 13U))
      return fail("ihdr_duplicate_or_length");
    if (!ihdr && !plte && !idat && !iend && !ancillary)
      return fail("unknown_critical_chunk");
    if (plte && (sawPlte_ || sawIdat_ || chunkLength_ == 0U ||
                 chunkLength_ > 768U || chunkLength_ % 3U != 0U ||
                 colorType_ == 0U || colorType_ == 4U))
      return fail("plte_order_or_format");
    if (idat && (!sawIhdr_ || idatSequenceClosed_ || chunkLength_ == 0U ||
                 (colorType_ == 3U && !sawPlte_)))
      return fail("idat_order_or_length");
    if (iend && (chunkLength_ != 0U || !sawIdat_))
      return fail("iend_before_image");
    if (sawIdat_ && !idat && !iend) idatSequenceClosed_ = true;
    runningCrc_ = 0xffffffffUL;
    for (size_t index = 4; index < 8; ++index)
      runningCrc_ = updateCrc(runningCrc_, chunkHeader_[index]);
    chunkDataPosition_ = 0;
    chunkCrcPosition_ = 0;
    expectedCrc_ = 0;
    if (chunkLength_ == 0U) {
      phase_ = Phase::ChunkCrc;
    } else {
      phase_ = Phase::ChunkData;
    }
    return true;
  }

  bool finishChunk() {
    if (expectedCrc_ != (runningCrc_ ^ 0xffffffffUL))
      return fail("chunk_crc");
    if (isType("IHDR")) {
      uint8_t header[33] = {
          0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
          0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R'};
      memcpy(header + 16, ihdrData_, sizeof(ihdrData_));
      const PaperColorPngHeader parsed =
          parsePaperColorPngHeader(header, sizeof(header));
      if (!parsed.valid) return fail("ihdr_format_or_dimensions");
      sawIhdr_ = true;
      landscape_ = parsed.landscape;
      colorType_ = ihdrData_[9];
    } else if (isType("PLTE")) {
      sawPlte_ = true;
    } else if (isType("IDAT")) {
      sawIdat_ = true;
    } else if (isType("IEND")) {
      complete_ = true;
      phase_ = Phase::Complete;
      return true;
    }
    phase_ = Phase::ChunkHeader;
    chunkHeaderPosition_ = 0;
    return true;
  }

  bool fail(const char* reason) {
    failed_ = true;
    failure_ = reason ? reason : "unknown";
    complete_ = false;
    phase_ = Phase::Failed;
    return false;
  }

  size_t maximumBytes_ = 0;
  size_t totalBytes_ = 0;
  size_t signaturePosition_ = 0;
  size_t chunkHeaderPosition_ = 0;
  uint32_t chunkDataPosition_ = 0;
  uint8_t chunkCrcPosition_ = 0;
  uint32_t chunkLength_ = 0;
  uint32_t runningCrc_ = 0xffffffffUL;
  uint32_t expectedCrc_ = 0;
  Phase phase_ = Phase::Signature;
  bool sawIhdr_ = false;
  bool sawPlte_ = false;
  bool sawIdat_ = false;
  bool idatSequenceClosed_ = false;
  bool complete_ = false;
  bool failed_ = false;
  const char* failure_ = "none";
  bool landscape_ = false;
  uint8_t colorType_ = 255;
  uint8_t chunkHeader_[8]{};
  uint8_t ihdrData_[13]{};
};

inline bool validPaperColorPng(
    const uint8_t* bytes, size_t length, bool* landscape = nullptr) {
  PaperColorPngStreamValidator validator(length);
  if (!validator.append(bytes, length) || !validator.finish(length)) return false;
  if (landscape) *landscape = validator.landscape();
  return true;
}

inline bool albumPrepareMayMutate(bool uploadActive) {
  return !uploadActive;
}

inline bool boundedUploadAppend(
    size_t currentBytes,
    size_t incomingBytes,
    size_t declaredImageBytes,
    size_t absoluteMaximumBytes) {
  if (!incomingBytes || currentBytes > declaredImageBytes ||
      declaredImageBytes > absoluteMaximumBytes) return false;
  return incomingBytes <= declaredImageBytes - currentBytes &&
      incomingBytes <= absoluteMaximumBytes - currentBytes;
}

inline bool validUploadTitle(const std::string& title, size_t maximumBytes) {
  if (title.empty() || title.size() > maximumBytes) return false;
  for (size_t index = 0; index < title.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(title[index]);
    if (ch < 0x20 || ch > 0x7e || ch == '/' || ch == '\\' || ch == '"' ||
        ch == '<' || ch == '>') return false;
  }
  return title != "." && title != "..";
}

}  // namespace inkloop
