#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace inkloop {
namespace storage {

class PaperColorPngValidator {
 public:
  explicit PaperColorPngValidator(size_t maximum_bytes = 1500000U) {
    reset(maximum_bytes);
  }

  void reset(size_t maximum_bytes);
  bool append(const uint8_t* bytes, size_t length);
  bool finish(size_t exact_bytes) const;

  bool landscape() const { return landscape_; }
  size_t totalBytes() const { return total_bytes_; }
  const char* failureName() const;

 private:
  enum class Phase : uint8_t {
    Signature,
    ChunkHeader,
    ChunkData,
    ChunkCrc,
    Complete,
    Failed,
  };

  static uint32_t updateCrc(uint32_t crc, uint8_t value);
  bool isType(const char* expected) const;
  bool validChunkType() const;
  bool beginChunk();
  bool finishChunk();
  bool validateIhdr();
  bool fail(const char* reason);

  size_t maximum_bytes_ = 0;
  size_t total_bytes_ = 0;
  size_t signature_position_ = 0;
  size_t chunk_header_position_ = 0;
  uint32_t chunk_data_position_ = 0;
  uint8_t chunk_crc_position_ = 0;
  uint32_t chunk_length_ = 0;
  uint32_t running_crc_ = 0xffffffffUL;
  uint32_t expected_crc_ = 0;
  Phase phase_ = Phase::Signature;
  bool saw_ihdr_ = false;
  bool saw_plte_ = false;
  bool saw_idat_ = false;
  bool idat_sequence_closed_ = false;
  bool complete_ = false;
  bool failed_ = false;
  const char* failure_ = "none";
  bool landscape_ = false;
  uint8_t color_type_ = 255;
  uint8_t chunk_header_[8]{};
  uint8_t ihdr_data_[13]{};
};

}  // namespace storage
}  // namespace inkloop
