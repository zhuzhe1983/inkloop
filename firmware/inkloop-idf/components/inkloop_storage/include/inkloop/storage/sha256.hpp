#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {
namespace storage {

class Sha256 {
 public:
  Sha256();
  bool update(const uint8_t* bytes, size_t length);
  bool finish(std::array<uint8_t, 32>& digest);
  bool finishHex(std::string& hex);

 private:
  void transform(const uint8_t block[64]);

  std::array<uint32_t, 8> state_{};
  std::array<uint8_t, 64> buffer_{};
  uint64_t total_bytes_ = 0;
  size_t buffer_bytes_ = 0;
  bool finished_ = false;
};

}  // namespace storage
}  // namespace inkloop
