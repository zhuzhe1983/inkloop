#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

using OtaSha256Digest = std::array<std::uint8_t, 32>;

class OtaSha256 final {
 public:
  OtaSha256();

  bool update(const std::uint8_t* bytes, std::size_t length);
  bool finish(OtaSha256Digest& digest);

 private:
  void transform(const std::uint8_t block[64]);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0U;
  std::size_t buffer_bytes_ = 0U;
  bool finished_ = false;
};

}  // namespace inkloop
