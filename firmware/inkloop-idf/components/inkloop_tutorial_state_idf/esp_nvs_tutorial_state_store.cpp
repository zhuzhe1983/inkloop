#include "inkloop/onboarding/esp_nvs_tutorial_state_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nvs.h"

namespace inkloop::onboarding {
namespace {

constexpr char kNamespace[] = "ink-tutorial";
constexpr char kRecordKey[] = "state";
constexpr std::array<std::uint8_t, 4> kMagic{{'I', 'N', 'K', 'T'}};
constexpr std::uint8_t kSchema = 1U;
constexpr std::size_t kRecordBytes = 12U;

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t value = 0xFFFFFFFFU;
  for (std::size_t at = 0; at < length; ++at) {
    value ^= bytes[at];
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(value & 1U);
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return value ^ 0xFFFFFFFFU;
}

void write32(std::uint32_t value, std::uint8_t* output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

std::uint32_t read32(const std::uint8_t* input) {
  return static_cast<std::uint32_t>(input[0]) |
      (static_cast<std::uint32_t>(input[1]) << 8U) |
      (static_cast<std::uint32_t>(input[2]) << 16U) |
      (static_cast<std::uint32_t>(input[3]) << 24U);
}

TutorialStateResult decode(const std::array<std::uint8_t, kRecordBytes>& input,
                           TutorialStep& output) {
  if (!std::equal(kMagic.begin(), kMagic.end(), input.begin()) ||
      input[4] != kSchema || input[6] != 0U || input[7] != 0U ||
      crc32(input.data(), 8U) != read32(input.data() + 8U)) {
    return TutorialStateResult::Corrupt;
  }
  const TutorialStep step = static_cast<TutorialStep>(input[5]);
  if (!validTutorialStep(step)) return TutorialStateResult::Corrupt;
  output = step;
  return TutorialStateResult::Ok;
}

}  // namespace

TutorialStateResult EspNvsTutorialStateStore::load(TutorialStep& output) {
  output = TutorialStep::PressToTalk;
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return TutorialStateResult::Absent;
  if (opened != ESP_OK) return TutorialStateResult::Storage;
  std::array<std::uint8_t, kRecordBytes> record{};
  std::size_t length = record.size();
  const esp_err_t read = nvs_get_blob(
      handle, kRecordKey, record.data(), &length);
  nvs_close(handle);
  if (read == ESP_ERR_NVS_NOT_FOUND) return TutorialStateResult::Absent;
  if (read != ESP_OK) return TutorialStateResult::Storage;
  if (length != record.size()) return TutorialStateResult::Corrupt;
  return decode(record, output);
}

TutorialStateResult EspNvsTutorialStateStore::save(TutorialStep value) {
  if (!validTutorialStep(value)) return TutorialStateResult::InvalidArgument;
  std::array<std::uint8_t, kRecordBytes> record{};
  std::copy(kMagic.begin(), kMagic.end(), record.begin());
  record[4] = kSchema;
  record[5] = static_cast<std::uint8_t>(value);
  write32(crc32(record.data(), 8U), record.data() + 8U);
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return TutorialStateResult::Storage;
  esp_err_t result = nvs_set_blob(
      handle, kRecordKey, record.data(), record.size());
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return TutorialStateResult::Storage;
  TutorialStep verified = TutorialStep::PressToTalk;
  return load(verified) == TutorialStateResult::Ok && verified == value
      ? TutorialStateResult::Ok
      : TutorialStateResult::Storage;
}

}  // namespace inkloop::onboarding
