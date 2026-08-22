#include "inkloop/storage/papercolor_png.hpp"

namespace inkloop {
namespace storage {

void PaperColorPngValidator::reset(size_t maximum_bytes) {
  maximum_bytes_ = maximum_bytes;
  total_bytes_ = 0;
  signature_position_ = 0;
  chunk_header_position_ = 0;
  chunk_data_position_ = 0;
  chunk_crc_position_ = 0;
  chunk_length_ = 0;
  running_crc_ = 0xffffffffUL;
  expected_crc_ = 0;
  phase_ = Phase::Signature;
  saw_ihdr_ = false;
  saw_plte_ = false;
  saw_idat_ = false;
  idat_sequence_closed_ = false;
  complete_ = false;
  failed_ = false;
  failure_ = "none";
  landscape_ = false;
  color_type_ = 255;
  std::memset(chunk_header_, 0, sizeof(chunk_header_));
  std::memset(ihdr_data_, 0, sizeof(ihdr_data_));
}

uint32_t PaperColorPngValidator::updateCrc(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t bit = 0; bit < 8U; ++bit) {
    crc = (crc & 1U) ? (crc >> 1U) ^ 0xedb88320UL : crc >> 1U;
  }
  return crc;
}

bool PaperColorPngValidator::isType(const char* expected) const {
  return expected && std::memcmp(chunk_header_ + 4, expected, 4) == 0;
}

bool PaperColorPngValidator::validChunkType() const {
  for (size_t index = 4; index < 8; ++index) {
    const uint8_t ch = chunk_header_[index];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
      return false;
  }
  return chunk_header_[6] >= 'A' && chunk_header_[6] <= 'Z';
}

bool PaperColorPngValidator::validateIhdr() {
  const uint32_t width = (static_cast<uint32_t>(ihdr_data_[0]) << 24U) |
                         (static_cast<uint32_t>(ihdr_data_[1]) << 16U) |
                         (static_cast<uint32_t>(ihdr_data_[2]) << 8U) |
                         ihdr_data_[3];
  const uint32_t height = (static_cast<uint32_t>(ihdr_data_[4]) << 24U) |
                          (static_cast<uint32_t>(ihdr_data_[5]) << 16U) |
                          (static_cast<uint32_t>(ihdr_data_[6]) << 8U) |
                          ihdr_data_[7];
  if (!((width == 400U && height == 600U) ||
        (width == 600U && height == 400U))) return false;
  const uint8_t bit_depth = ihdr_data_[8];
  color_type_ = ihdr_data_[9];
  bool valid_format = false;
  if (color_type_ == 0U) {
    valid_format = bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
                   bit_depth == 8U || bit_depth == 16U;
  } else if (color_type_ == 2U || color_type_ == 4U || color_type_ == 6U) {
    valid_format = bit_depth == 8U || bit_depth == 16U;
  } else if (color_type_ == 3U) {
    valid_format = bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
                   bit_depth == 8U;
  }
  if (!valid_format || ihdr_data_[10] != 0U || ihdr_data_[11] != 0U ||
      ihdr_data_[12] > 1U) return false;
  landscape_ = width == 600U;
  return true;
}

bool PaperColorPngValidator::beginChunk() {
  chunk_length_ = (static_cast<uint32_t>(chunk_header_[0]) << 24U) |
                  (static_cast<uint32_t>(chunk_header_[1]) << 16U) |
                  (static_cast<uint32_t>(chunk_header_[2]) << 8U) |
                  chunk_header_[3];
  if (!validChunkType() || total_bytes_ > maximum_bytes_ ||
      maximum_bytes_ - total_bytes_ < 4U ||
      chunk_length_ > maximum_bytes_ - total_bytes_ - 4U) {
    return fail("chunk_header_or_length");
  }
  const bool ihdr = isType("IHDR");
  const bool plte = isType("PLTE");
  const bool idat = isType("IDAT");
  const bool iend = isType("IEND");
  const bool ancillary = (chunk_header_[4] & 0x20U) != 0;
  if (!saw_ihdr_ && !ihdr) return fail("ihdr_not_first");
  if (ihdr && (saw_ihdr_ || chunk_length_ != 13U))
    return fail("ihdr_duplicate_or_length");
  if (!ihdr && !plte && !idat && !iend && !ancillary)
    return fail("unknown_critical_chunk");
  if (plte && (saw_plte_ || saw_idat_ || chunk_length_ == 0U ||
               chunk_length_ > 768U || chunk_length_ % 3U != 0U ||
               color_type_ == 0U || color_type_ == 4U)) {
    return fail("plte_order_or_format");
  }
  if (idat && (!saw_ihdr_ || idat_sequence_closed_ || chunk_length_ == 0U ||
               (color_type_ == 3U && !saw_plte_))) {
    return fail("idat_order_or_length");
  }
  if (iend && (chunk_length_ != 0U || !saw_idat_))
    return fail("iend_before_image");
  if (saw_idat_ && !idat && !iend) idat_sequence_closed_ = true;
  running_crc_ = 0xffffffffUL;
  for (size_t index = 4; index < 8; ++index)
    running_crc_ = updateCrc(running_crc_, chunk_header_[index]);
  chunk_data_position_ = 0;
  chunk_crc_position_ = 0;
  expected_crc_ = 0;
  phase_ = chunk_length_ == 0U ? Phase::ChunkCrc : Phase::ChunkData;
  return true;
}

bool PaperColorPngValidator::finishChunk() {
  if (expected_crc_ != (running_crc_ ^ 0xffffffffUL))
    return fail("chunk_crc");
  if (isType("IHDR")) {
    if (!validateIhdr()) return fail("ihdr_format_or_dimensions");
    saw_ihdr_ = true;
  } else if (isType("PLTE")) {
    saw_plte_ = true;
  } else if (isType("IDAT")) {
    saw_idat_ = true;
  } else if (isType("IEND")) {
    complete_ = true;
    phase_ = Phase::Complete;
    return true;
  }
  phase_ = Phase::ChunkHeader;
  chunk_header_position_ = 0;
  return true;
}

bool PaperColorPngValidator::fail(const char* reason) {
  failed_ = true;
  failure_ = reason ? reason : "unknown";
  complete_ = false;
  phase_ = Phase::Failed;
  return false;
}

bool PaperColorPngValidator::append(const uint8_t* bytes, size_t length) {
  if (!bytes || length == 0 || failed_ || complete_ ||
      total_bytes_ > maximum_bytes_ || length > maximum_bytes_ - total_bytes_) {
    return fail(complete_ ? "trailing_bytes" : "append_bounds");
  }
  static constexpr uint8_t kSignature[] = {
      0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  for (size_t index = 0; index < length; ++index) {
    if (complete_) return fail("trailing_bytes");
    const uint8_t value = bytes[index];
    ++total_bytes_;
    if (phase_ == Phase::Signature) {
      if (value != kSignature[signature_position_++]) return fail("signature");
      if (signature_position_ == sizeof(kSignature)) {
        phase_ = Phase::ChunkHeader;
        chunk_header_position_ = 0;
      }
    } else if (phase_ == Phase::ChunkHeader) {
      chunk_header_[chunk_header_position_++] = value;
      if (chunk_header_position_ == sizeof(chunk_header_) && !beginChunk())
        return false;
    } else if (phase_ == Phase::ChunkData) {
      running_crc_ = updateCrc(running_crc_, value);
      if (isType("IHDR") && chunk_data_position_ < sizeof(ihdr_data_))
        ihdr_data_[chunk_data_position_] = value;
      ++chunk_data_position_;
      if (chunk_data_position_ == chunk_length_) {
        phase_ = Phase::ChunkCrc;
        chunk_crc_position_ = 0;
        expected_crc_ = 0;
      }
    } else if (phase_ == Phase::ChunkCrc) {
      expected_crc_ = (expected_crc_ << 8U) | value;
      ++chunk_crc_position_;
      if (chunk_crc_position_ == 4U && !finishChunk()) return false;
    } else {
      return fail("invalid_phase");
    }
  }
  return !failed_;
}

bool PaperColorPngValidator::finish(size_t exact_bytes) const {
  return !failed_ && complete_ && saw_ihdr_ && saw_idat_ &&
         phase_ == Phase::Complete && total_bytes_ == exact_bytes;
}

const char* PaperColorPngValidator::failureName() const {
  return failed_ ? failure_ : (complete_ ? "none" : "incomplete");
}

}  // namespace storage
}  // namespace inkloop
