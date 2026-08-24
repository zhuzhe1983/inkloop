#include "inkloop/settings/settings_journal.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace inkloop {
namespace settings {
namespace {

constexpr std::uint8_t kMagic[] = {'I', 'N', 'K', 'S'};
constexpr std::size_t kSchema1FixedHeaderBytes = 24U;
constexpr std::size_t kFixedHeaderBytes = 26U;
constexpr std::size_t kChecksumBytes = 4U;
constexpr std::uint16_t kVoiceAssistanceFlag = 1U;
constexpr std::uint16_t kLedRolesSwappedFlag = 1U << 1U;

void append16(std::uint16_t value, std::vector<std::uint8_t>& output) {
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append32(std::uint32_t value, std::vector<std::uint8_t>& output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool read16(const std::vector<std::uint8_t>& input, std::size_t& at,
            std::uint16_t& output) {
  if (at + 2U > input.size()) return false;
  output = static_cast<std::uint16_t>(input[at]) |
      (static_cast<std::uint16_t>(input[at + 1U]) << 8U);
  at += 2U;
  return true;
}

bool read32(const std::vector<std::uint8_t>& input, std::size_t& at,
            std::uint32_t& output) {
  if (at + 4U > input.size()) return false;
  output = static_cast<std::uint32_t>(input[at]) |
      (static_cast<std::uint32_t>(input[at + 1U]) << 8U) |
      (static_cast<std::uint32_t>(input[at + 2U]) << 16U) |
      (static_cast<std::uint32_t>(input[at + 3U]) << 24U);
  at += 4U;
  return true;
}

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

SettingsStatus corrupt(const char* detail) {
  return {SettingsError::Corrupt, detail};
}

SettingsStatus storage(const char* detail) {
  return {SettingsError::Storage, detail};
}

bool sameSnapshot(const SettingsSnapshot& left,
                  const SettingsSnapshot& right) {
  return left.generation == right.generation &&
      left.decoded_record_schema == right.decoded_record_schema &&
      left.values == right.values;
}

}  // namespace

void SettingsJournalState::clear() {
  for (std::vector<std::uint8_t>& bytes : slot) {
    std::fill(bytes.begin(), bytes.end(), 0U);
    bytes.clear();
  }
  *this = SettingsJournalState{};
}

SettingsStatus encodeSettingsRecord(const SettingsSnapshot& snapshot,
                                    std::vector<std::uint8_t>& output) {
  output.clear();
  if (snapshot.generation == 0U || !validDeviceSettings(snapshot.values))
    return {SettingsError::InvalidArgument, "invalid settings snapshot"};
  const DeviceSettings& value = snapshot.values;
  const std::size_t payload_bytes = value.assistant_prompt.size() +
      value.aigc_prompt_template.size() + value.negative_prompt.size() +
      value.default_render_strategy.size() +
      value.local_management_password_override.size();
  if (kFixedHeaderBytes + payload_bytes + kChecksumBytes >
      kMaximumSettingsRecordBytes)
    return {SettingsError::TooLarge, "settings record too large"};

  output.reserve(kFixedHeaderBytes + payload_bytes + kChecksumBytes);
  output.insert(output.end(), std::begin(kMagic), std::end(kMagic));
  append16(kSettingsRecordSchema, output);
  append16((value.voice_assistance_enabled ? kVoiceAssistanceFlag : 0U) |
               (value.led_roles_swapped ? kLedRolesSwappedFlag : 0U),
           output);
  append32(snapshot.generation, output);
  output.push_back(value.volume_percent);
  output.push_back(value.led_maximum_brightness_percent);
  output.push_back(
      static_cast<std::uint8_t>(value.asset_storage_preference));
  output.push_back(0U);
  append16(static_cast<std::uint16_t>(value.assistant_prompt.size()), output);
  append16(static_cast<std::uint16_t>(value.aigc_prompt_template.size()),
           output);
  append16(static_cast<std::uint16_t>(value.negative_prompt.size()), output);
  append16(static_cast<std::uint16_t>(value.default_render_strategy.size()),
           output);
  append16(static_cast<std::uint16_t>(
               value.local_management_password_override.size()), output);
  const std::string* fields[] = {
      &value.assistant_prompt, &value.aigc_prompt_template,
      &value.negative_prompt, &value.default_render_strategy,
      &value.local_management_password_override};
  for (const std::string* field : fields)
    output.insert(output.end(), field->begin(), field->end());
  append32(crc32(output.data(), output.size()), output);
  return SettingsStatus::success();
}

SettingsStatus decodeSettingsRecord(const std::vector<std::uint8_t>& input,
                                    SettingsSnapshot& output) {
  output = SettingsSnapshot{};
  if (input.size() < kSchema1FixedHeaderBytes + kChecksumBytes ||
      input.size() > kMaximumSettingsRecordBytes)
    return corrupt("settings record size invalid");
  if (!std::equal(std::begin(kMagic), std::end(kMagic), input.begin()))
    return corrupt("settings record magic invalid");
  std::size_t at = sizeof(kMagic);
  std::uint16_t schema = 0U;
  std::uint16_t flags = 0U;
  std::uint32_t generation = 0U;
  if (!read16(input, at, schema) || !read16(input, at, flags) ||
      !read32(input, at, generation) ||
      (schema != 1U && schema != 2U && schema != kSettingsRecordSchema) ||
      (flags & ~(kVoiceAssistanceFlag |
                 (schema >= 3U ? kLedRolesSwappedFlag : 0U))) != 0U ||
      generation == 0U)
    return corrupt("settings record header invalid");
  if (at + 4U > input.size()) return corrupt("settings record truncated");
  DeviceSettings decoded;
  decoded.volume_percent = input[at++];
  decoded.led_maximum_brightness_percent = input[at++];
  decoded.asset_storage_preference =
      static_cast<AssetStoragePreference>(input[at++]);
  if (input[at++] != 0U) return corrupt("settings record reserved byte invalid");
  std::uint16_t lengths[5]{};
  const std::size_t field_count = schema == 1U ? 4U : 5U;
  for (std::size_t index = 0; index < field_count; ++index) {
    if (!read16(input, at, lengths[index]))
      return corrupt("settings record truncated");
  }
  const std::size_t expected_header =
      schema == 1U ? kSchema1FixedHeaderBytes : kFixedHeaderBytes;
  if (at != expected_header) return corrupt("settings header layout invalid");
  const std::size_t payload_bytes = static_cast<std::size_t>(lengths[0]) +
      lengths[1] + lengths[2] + lengths[3] + lengths[4];
  if (payload_bytes > input.size() ||
      at + payload_bytes + kChecksumBytes != input.size())
    return corrupt("settings record lengths invalid");
  std::size_t checksum_at = input.size() - kChecksumBytes;
  std::size_t checksum_cursor = checksum_at;
  std::uint32_t expected_checksum = 0U;
  if (!read32(input, checksum_cursor, expected_checksum) ||
      checksum_cursor != input.size() ||
      crc32(input.data(), checksum_at) != expected_checksum)
    return corrupt("settings record checksum invalid");

  std::string* fields[] = {
      &decoded.assistant_prompt, &decoded.aigc_prompt_template,
      &decoded.negative_prompt, &decoded.default_render_strategy,
      &decoded.local_management_password_override};
  for (std::size_t index = 0; index < field_count; ++index) {
    fields[index]->assign(
        reinterpret_cast<const char*>(input.data() + at), lengths[index]);
    at += lengths[index];
  }
  decoded.voice_assistance_enabled =
      (flags & kVoiceAssistanceFlag) != 0U;
  decoded.led_roles_swapped =
      schema >= 3U && (flags & kLedRolesSwappedFlag) != 0U;
  if (at != checksum_at || !validDeviceSettings(decoded))
    return corrupt("settings values invalid");
  output.generation = generation;
  output.decoded_record_schema = schema;
  output.values = std::move(decoded);
  return SettingsStatus::success();
}

SettingsStoreCore::SettingsStoreCore(ISettingsJournalStore& journal,
                                     const DeviceSettings& fresh_defaults)
    : journal_(journal), fresh_defaults_(fresh_defaults) {}

SettingsStatus SettingsStoreCore::load(SettingsSnapshot& snapshot) {
  snapshot = SettingsSnapshot{};
  if (!validDeviceSettings(fresh_defaults_))
    return {SettingsError::InvalidState, "fresh settings defaults invalid"};
  SettingsJournalState state;
  SettingsStatus status = journal_.inspect(state);
  if (!status.ok()) {
    state.clear();
    return storage("settings journal inspection failed");
  }
  if (!state.namespace_available) {
    state.clear();
    return storage("settings namespace unavailable");
  }
  const bool any_slot = state.slot_present[0] || state.slot_present[1];
  const bool metadata_absent = !state.marker_present && !state.head_present;
  if (metadata_absent) {
    // No committed head means slots are either absent or an interrupted first
    // save. They are never exposed as settings and may be overwritten later.
    (void)any_slot;
    snapshot.values = fresh_defaults_;
    state.clear();
    return SettingsStatus::success();
  }
  // The adapter writes head before the advisory marker. If power disappears
  // between those two writes, a checksum-valid, generation-matching selected
  // slot is already a complete atomic commit and is safe to load. A present
  // but invalid marker still signals real corruption and fails closed.
  if ((state.marker_present && !state.marker_valid) || !state.head_present ||
      state.head_generation == 0U) {
    state.clear();
    return corrupt("settings journal metadata invalid");
  }
  const std::uint8_t selected =
      static_cast<std::uint8_t>(state.head_generation & 1U);
  if (!state.slot_present[selected]) {
    state.clear();
    return corrupt("settings committed slot missing");
  }
  const std::uint32_t committed_generation = state.head_generation;
  SettingsSnapshot decoded;
  status = decodeSettingsRecord(state.slot[selected], decoded);
  state.clear();
  if (!status.ok() || decoded.generation != committed_generation) {
    decoded = SettingsSnapshot{};
    return corrupt("settings committed slot invalid");
  }
  snapshot = std::move(decoded);
  return SettingsStatus::success();
}

SettingsStatus SettingsStoreCore::save(
    const DeviceSettings& values, std::uint32_t expected_generation,
    SettingsSnapshot& committed_snapshot) {
  committed_snapshot = SettingsSnapshot{};
  if (!validDeviceSettings(values))
    return {SettingsError::InvalidArgument, "device settings invalid"};
  SettingsSnapshot current;
  SettingsStatus status = load(current);
  if (!status.ok()) return status;
  if (current.generation != expected_generation)
    return {SettingsError::Conflict, "settings generation conflict"};
  if (current.generation == std::numeric_limits<std::uint32_t>::max())
    return {SettingsError::Exhausted, "settings generation exhausted"};

  SettingsSnapshot next;
  next.generation = current.generation + 1U;
  next.decoded_record_schema = kSettingsRecordSchema;
  next.values = values;
  std::vector<std::uint8_t> encoded;
  status = encodeSettingsRecord(next, encoded);
  if (!status.ok()) return status;
  const std::uint8_t slot = static_cast<std::uint8_t>(next.generation & 1U);
  status = journal_.writeSlotAndCommit(slot, encoded);
  std::fill(encoded.begin(), encoded.end(), 0U);
  encoded.clear();
  if (!status.ok()) return storage("settings slot commit failed");

  SettingsJournalState verify;
  status = journal_.inspect(verify);
  SettingsSnapshot decoded;
  const bool slot_verified = status.ok() && verify.namespace_available &&
      verify.slot_present[slot] &&
      decodeSettingsRecord(verify.slot[slot], decoded).ok() &&
      sameSnapshot(decoded, next);
  verify.clear();
  if (!slot_verified) return storage("settings slot verification failed");

  status = journal_.writeHeadAndMarkerAndCommit(next.generation);
  SettingsSnapshot authoritative;
  const SettingsStatus load_after_commit = load(authoritative);
  if (!load_after_commit.ok() || !sameSnapshot(authoritative, next))
    return status.ok() ? storage("settings commit verification failed")
                       : storage("settings head commit failed");
  committed_snapshot = std::move(authoritative);
  return SettingsStatus::success();
}

}  // namespace settings
}  // namespace inkloop
