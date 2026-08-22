#include "inkloop/ota_sha256.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace inkloop {
namespace {

constexpr std::uint32_t kRound[64] = {
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

std::uint32_t rotateRight(std::uint32_t value, std::uint8_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

}  // namespace

OtaSha256::OtaSha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void OtaSha256::transform(const std::uint8_t block[64]) {
  std::uint32_t words[64]{};
  for (std::size_t index = 0U; index < 16U; ++index) {
    const std::size_t at = index * 4U;
    words[index] = (static_cast<std::uint32_t>(block[at]) << 24U) |
                   (static_cast<std::uint32_t>(block[at + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[at + 2U]) << 8U) |
                   block[at + 3U];
  }
  for (std::size_t index = 16U; index < 64U; ++index) {
    const std::uint32_t s0 = rotateRight(words[index - 15U], 7U) ^
        rotateRight(words[index - 15U], 18U) ^
        (words[index - 15U] >> 3U);
    const std::uint32_t s1 = rotateRight(words[index - 2U], 17U) ^
        rotateRight(words[index - 2U], 19U) ^
        (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0U; index < 64U; ++index) {
    const std::uint32_t sum1 = rotateRight(e, 6U) ^
        rotateRight(e, 11U) ^ rotateRight(e, 25U);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRound[index] + words[index];
    const std::uint32_t sum0 = rotateRight(a, 2U) ^
        rotateRight(a, 13U) ^ rotateRight(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
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

bool OtaSha256::update(const std::uint8_t* bytes, std::size_t length) {
  if (finished_ || (!bytes && length != 0U) ||
      length > std::numeric_limits<std::uint64_t>::max() - total_bytes_)
    return false;
  total_bytes_ += length;
  std::size_t at = 0U;
  if (buffer_bytes_ != 0U) {
    const std::size_t copied =
        std::min(length, buffer_.size() - buffer_bytes_);
    if (copied != 0U)
      std::memcpy(buffer_.data() + buffer_bytes_, bytes, copied);
    buffer_bytes_ += copied;
    at += copied;
    if (buffer_bytes_ == buffer_.size()) {
      transform(buffer_.data());
      buffer_bytes_ = 0U;
    }
  }
  while (length - at >= buffer_.size()) {
    transform(bytes + at);
    at += buffer_.size();
  }
  if (at < length) {
    buffer_bytes_ = length - at;
    std::memcpy(buffer_.data(), bytes + at, buffer_bytes_);
  }
  return true;
}

bool OtaSha256::finish(OtaSha256Digest& digest) {
  if (finished_ ||
      total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8U)
    return false;
  const std::uint64_t bit_length = total_bytes_ * 8U;
  buffer_[buffer_bytes_++] = 0x80U;
  if (buffer_bytes_ > 56U) {
    std::fill(buffer_.begin() + buffer_bytes_, buffer_.end(), 0U);
    transform(buffer_.data());
    buffer_bytes_ = 0U;
  }
  std::fill(buffer_.begin() + buffer_bytes_, buffer_.begin() + 56U, 0U);
  for (std::size_t index = 0U; index < 8U; ++index)
    buffer_[63U - index] =
        static_cast<std::uint8_t>(bit_length >> (index * 8U));
  transform(buffer_.data());
  for (std::size_t index = 0U; index < state_.size(); ++index) {
    digest[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
    digest[index * 4U + 1U] =
        static_cast<std::uint8_t>(state_[index] >> 16U);
    digest[index * 4U + 2U] =
        static_cast<std::uint8_t>(state_[index] >> 8U);
    digest[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
  }
  finished_ = true;
  std::fill(buffer_.begin(), buffer_.end(), 0U);
  return true;
}

}  // namespace inkloop
