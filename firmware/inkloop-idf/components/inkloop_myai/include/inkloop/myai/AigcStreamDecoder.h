#pragma once

#include <cstddef>
#include <string>

#include "MyAiAdapters.h"

namespace inkloop {
namespace myai {

// Incrementally consumes the bounded JSON envelope returned by MyAI's AIGC
// output endpoint and writes decoded bytes directly to an injected sink. The
// complete JSON/base64 payload is never materialized in RAM.
class AigcStreamDecoder {
 public:
  AigcStreamDecoder(size_t maximumEncodedBytes, size_t maximumDecodedBytes,
                    IImageSink& sink, AigcOutputMetadata& metadata);

  Status append(const uint8_t* bytes, size_t length);
  Status finish();

  bool begun() const { return begun_; }
  bool complete() const { return complete_; }
  size_t decodedBytes() const { return decoded_bytes_; }

 private:
  Status consume(char ch);
  Status decodeQuartet();
  Status fail(ErrorCode code, const char* detail);

  size_t maximum_encoded_bytes_;
  size_t maximum_decoded_bytes_;
  IImageSink& sink_;
  AigcOutputMetadata& metadata_;
  Status status_;
  std::string prefix_;
  bool decoding_ = false;
  bool begun_ = false;
  bool complete_ = false;
  bool padded_ = false;
  size_t encoded_bytes_ = 0;
  size_t decoded_bytes_ = 0;
  size_t envelope_bytes_ = 0;
  size_t quartet_length_ = 0;
  char quartet_[4]{};
};

}  // namespace myai
}  // namespace inkloop
