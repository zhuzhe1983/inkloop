#include "inkloop/settings/settings_extension_journal.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace inkloop {
namespace settings {
namespace {

constexpr std::uint8_t kMagic[] = {'I', 'N', 'K', 'X'};
constexpr std::uint8_t kLedRolesSwappedFlag = 1U;
constexpr std::size_t kChecksumAt = 16U;

void append32(std::uint32_t value, std::vector<std::uint8_t>& output) {
  for (std::uint8_t shift = 0U; shift < 32U; shift += 8U)
    output.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t read32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
      (static_cast<std::uint32_t>(bytes[1]) << 8U) |
      (static_cast<std::uint32_t>(bytes[2]) << 16U) |
      (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t length) {
  std::uint32_t value = 0xFFFFFFFFU;
  for (std::size_t at = 0U; at < length; ++at) {
    value ^= bytes[at];
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - static_cast<std::uint32_t>(value & 1U);
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return value ^ 0xFFFFFFFFU;
}

bool valid(const SettingsExtensionValues& values) {
  return values.aigc_steps >= kMinimumAigcSteps &&
      values.aigc_steps <= kMaximumAigcSteps;
}

SettingsStatus corrupt(const char* detail) {
  return {SettingsError::Corrupt, detail};
}

SettingsStatus storage(const char* detail) {
  return {SettingsError::Storage, detail};
}

DeviceSettings mainValues(const DeviceSettings& values) {
  DeviceSettings output = values;
  output.led_roles_swapped = false;
  output.aigc_steps = kDefaultAigcSteps;
  return output;
}

bool sameMainValues(const DeviceSettings& left,
                    const DeviceSettings& right) {
  return mainValues(left) == mainValues(right);
}

}  // namespace

bool operator==(const SettingsExtensionValues& left,
                const SettingsExtensionValues& right) {
  return left.led_roles_swapped == right.led_roles_swapped &&
      left.aigc_steps == right.aigc_steps;
}

void SettingsExtensionJournalState::clear() {
  for (std::vector<std::uint8_t>& bytes : slot) {
    std::fill(bytes.begin(), bytes.end(), 0U);
    bytes.clear();
  }
  *this = SettingsExtensionJournalState{};
}

SettingsStatus encodeSettingsExtensionRecord(
    const SettingsExtensionSnapshot& snapshot,
    std::vector<std::uint8_t>& output) {
  output.clear();
  if (snapshot.sequence == 0U || !valid(snapshot.values)) {
    return {SettingsError::InvalidArgument,
            "settings extension snapshot invalid"};
  }
  output.reserve(kSettingsExtensionRecordBytes);
  output.insert(output.end(), std::begin(kMagic), std::end(kMagic));
  output.push_back(kSettingsExtensionRecordSchema);
  output.push_back(snapshot.values.led_roles_swapped
                       ? kLedRolesSwappedFlag : 0U);
  output.push_back(snapshot.values.aigc_steps);
  output.push_back(0U);
  append32(snapshot.sequence, output);
  append32(snapshot.settings_generation, output);
  append32(crc32(output.data(), output.size()), output);
  return output.size() == kSettingsExtensionRecordBytes
      ? SettingsStatus::success()
      : storage("settings extension encoding failed");
}

SettingsStatus decodeSettingsExtensionRecord(
    const std::vector<std::uint8_t>& input,
    SettingsExtensionSnapshot& output) {
  output = SettingsExtensionSnapshot{};
  if (input.size() != kSettingsExtensionRecordBytes ||
      !std::equal(std::begin(kMagic), std::end(kMagic), input.begin()) ||
      input[4] != kSettingsExtensionRecordSchema ||
      (input[5] & ~kLedRolesSwappedFlag) != 0U || input[7] != 0U ||
      crc32(input.data(), kChecksumAt) != read32(input.data() + kChecksumAt)) {
    return corrupt("settings extension record invalid");
  }
  output.sequence = read32(input.data() + 8U);
  output.settings_generation = read32(input.data() + 12U);
  output.values.led_roles_swapped =
      (input[5] & kLedRolesSwappedFlag) != 0U;
  output.values.aigc_steps = input[6];
  if (output.sequence == 0U || !valid(output.values)) {
    output = SettingsExtensionSnapshot{};
    return corrupt("settings extension values invalid");
  }
  return SettingsStatus::success();
}

SettingsExtensionStoreCore::SettingsExtensionStoreCore(
    ISettingsExtensionJournalStore& journal) : journal_(journal) {}

SettingsStatus SettingsExtensionStoreCore::inspectCommitted(
    SettingsExtensionJournalState& state,
    SettingsExtensionSnapshot& committed,
    bool& missing) {
  state.clear();
  committed = SettingsExtensionSnapshot{};
  missing = false;
  SettingsStatus status = journal_.inspect(state);
  if (!status.ok() || !state.namespace_available) {
    state.clear();
    return storage("settings extension inspection failed");
  }
  if (!state.head_present) {
    missing = true;
    return SettingsStatus::success();
  }
  if (state.head_sequence == 0U) {
    state.clear();
    return corrupt("settings extension head invalid");
  }
  const std::uint8_t selected =
      static_cast<std::uint8_t>(state.head_sequence & 1U);
  if (!state.slot_present[selected] ||
      !decodeSettingsExtensionRecord(state.slot[selected], committed).ok() ||
      committed.sequence != state.head_sequence) {
    state.clear();
    committed = SettingsExtensionSnapshot{};
    return corrupt("settings extension committed slot invalid");
  }
  return SettingsStatus::success();
}

SettingsStatus SettingsExtensionStoreCore::load(
    std::uint32_t selected_main_generation,
    const SettingsExtensionValues& legacy_fallback,
    SettingsExtensionSnapshot& snapshot) {
  snapshot = SettingsExtensionSnapshot{};
  if (!valid(legacy_fallback)) {
    return {SettingsError::InvalidArgument,
            "settings extension fallback invalid"};
  }
  SettingsExtensionJournalState state;
  SettingsExtensionSnapshot committed;
  bool missing = false;
  SettingsStatus status = inspectCommitted(state, committed, missing);
  if (!status.ok()) return status;
  state.clear();
  if (missing) {
    snapshot.settings_generation = selected_main_generation;
    snapshot.values = legacy_fallback;
    return SettingsStatus::success();
  }
  if (committed.settings_generation > selected_main_generation) {
    return corrupt("settings extension newer than main settings");
  }
  snapshot = std::move(committed);
  return SettingsStatus::success();
}

SettingsStatus SettingsExtensionStoreCore::prepare(
    std::uint32_t target_main_generation,
    const SettingsExtensionValues& values,
    SettingsExtensionSnapshot& prepared_snapshot) {
  prepared_snapshot = SettingsExtensionSnapshot{};
  if (!valid(values)) {
    return {SettingsError::InvalidArgument,
            "settings extension target invalid"};
  }
  SettingsExtensionJournalState state;
  SettingsExtensionSnapshot current;
  bool missing = false;
  SettingsStatus status = inspectCommitted(state, current, missing);
  if (!status.ok()) return status;
  const std::uint32_t current_sequence = missing ? 0U : current.sequence;
  state.clear();
  if (current_sequence == std::numeric_limits<std::uint32_t>::max())
    return {SettingsError::Exhausted,
            "settings extension sequence exhausted"};

  SettingsExtensionSnapshot next;
  next.sequence = current_sequence + 1U;
  next.settings_generation = target_main_generation;
  next.values = values;
  std::vector<std::uint8_t> encoded;
  status = encodeSettingsExtensionRecord(next, encoded);
  if (!status.ok()) return status;
  const std::uint8_t slot = static_cast<std::uint8_t>(next.sequence & 1U);
  status = journal_.writeSlot(slot, encoded);
  std::fill(encoded.begin(), encoded.end(), 0U);
  encoded.clear();
  if (!status.ok()) return storage("settings extension slot write failed");

  SettingsExtensionJournalState verify;
  status = journal_.inspect(verify);
  SettingsExtensionSnapshot decoded;
  const bool verified = status.ok() && verify.namespace_available &&
      verify.slot_present[slot] &&
      decodeSettingsExtensionRecord(verify.slot[slot], decoded).ok() &&
      decoded.sequence == next.sequence &&
      decoded.settings_generation == next.settings_generation &&
      decoded.values == next.values;
  verify.clear();
  if (!verified)
    return storage("settings extension slot verification failed");
  prepared_snapshot = std::move(next);
  return SettingsStatus::success();
}

SettingsStatus SettingsExtensionStoreCore::publish(
    const SettingsExtensionSnapshot& prepared_snapshot,
    SettingsExtensionSnapshot& committed_snapshot) {
  committed_snapshot = SettingsExtensionSnapshot{};
  if (prepared_snapshot.sequence == 0U ||
      !valid(prepared_snapshot.values)) {
    return {SettingsError::InvalidArgument,
            "prepared settings extension invalid"};
  }
  SettingsExtensionJournalState before;
  SettingsExtensionSnapshot current;
  bool missing = false;
  SettingsStatus status = inspectCommitted(before, current, missing);
  if (!status.ok()) return status;
  const std::uint32_t current_sequence = missing ? 0U : current.sequence;
  if (current_sequence == std::numeric_limits<std::uint32_t>::max() ||
      current_sequence + 1U != prepared_snapshot.sequence) {
    before.clear();
    return {SettingsError::Conflict,
            "settings extension sequence conflict"};
  }
  const std::uint8_t slot =
      static_cast<std::uint8_t>(prepared_snapshot.sequence & 1U);
  SettingsExtensionSnapshot staged;
  const bool staged_valid = before.slot_present[slot] &&
      decodeSettingsExtensionRecord(before.slot[slot], staged).ok() &&
      staged.sequence == prepared_snapshot.sequence &&
      staged.settings_generation == prepared_snapshot.settings_generation &&
      staged.values == prepared_snapshot.values;
  before.clear();
  if (!staged_valid)
    return storage("prepared settings extension slot invalid");

  const SettingsStatus write = journal_.writeHead(prepared_snapshot.sequence);
  SettingsExtensionJournalState after;
  SettingsExtensionSnapshot authoritative;
  bool after_missing = false;
  status = inspectCommitted(after, authoritative, after_missing);
  after.clear();
  if (!after_missing && status.ok() &&
      authoritative.sequence == prepared_snapshot.sequence &&
      authoritative.settings_generation ==
          prepared_snapshot.settings_generation &&
      authoritative.values == prepared_snapshot.values) {
    committed_snapshot = std::move(authoritative);
    return SettingsStatus::success();
  }
  return write.ok() ? storage("settings extension head verification failed")
                    : storage("settings extension head write failed");
}

SettingsExtensionValues settingsExtensionValues(
    const DeviceSettings& settings) {
  SettingsExtensionValues output;
  output.led_roles_swapped = settings.led_roles_swapped;
  output.aigc_steps = settings.aigc_steps;
  return output;
}

void applySettingsExtension(const SettingsExtensionSnapshot& extension,
                            DeviceSettings& settings) {
  settings.led_roles_swapped = extension.values.led_roles_swapped;
  settings.aigc_steps = extension.values.aigc_steps;
}

SettingsStatus loadRollbackCompatibleSettings(
    SettingsStoreCore& main_store,
    SettingsExtensionStoreCore& extension_store,
    SettingsSnapshot& snapshot,
    SettingsExtensionSnapshot* selected_extension) {
  snapshot = SettingsSnapshot{};
  if (selected_extension) *selected_extension = SettingsExtensionSnapshot{};
  SettingsStatus status = main_store.load(snapshot);
  if (!status.ok()) return status;
  SettingsExtensionValues fallback;
  fallback.led_roles_swapped = snapshot.values.led_roles_swapped;
  fallback.aigc_steps = kDefaultAigcSteps;
  SettingsExtensionSnapshot extension;
  status = extension_store.load(snapshot.generation, fallback, extension);
  if (!status.ok()) {
    snapshot = SettingsSnapshot{};
    return status;
  }
  applySettingsExtension(extension, snapshot.values);
  if (!validDeviceSettings(snapshot.values)) {
    snapshot = SettingsSnapshot{};
    return corrupt("composite settings values invalid");
  }
  if (selected_extension) *selected_extension = extension;
  return SettingsStatus::success();
}

SettingsStatus saveRollbackCompatibleSettings(
    SettingsStoreCore& main_store,
    SettingsExtensionStoreCore& extension_store,
    const DeviceSettings& values,
    std::uint32_t expected_generation,
    SettingsSnapshot& authoritative_snapshot,
    bool force_main_write,
    bool force_extension_write) {
  authoritative_snapshot = SettingsSnapshot{};
  if (!validDeviceSettings(values))
    return {SettingsError::InvalidArgument, "device settings invalid"};
  SettingsSnapshot current;
  SettingsStatus status =
      loadRollbackCompatibleSettings(main_store, extension_store, current);
  if (!status.ok()) return status;
  authoritative_snapshot = current;
  if (current.generation != expected_generation)
    return {SettingsError::Conflict, "settings generation conflict"};

  const bool main_changed = force_main_write ||
      current.decoded_record_schema > kSettingsRecordSchema ||
      !sameMainValues(current.values, values);
  const bool extension_changed =
      settingsExtensionValues(current.values) != settingsExtensionValues(values) ||
      (main_changed && current.decoded_record_schema >= 3U) ||
      force_main_write || force_extension_write;
  if (!main_changed && !extension_changed)
    return SettingsStatus::success();
  if (main_changed &&
      current.generation == std::numeric_limits<std::uint32_t>::max()) {
    return {SettingsError::Exhausted, "settings generation exhausted"};
  }
  const std::uint32_t target_main_generation = main_changed
      ? current.generation + 1U : current.generation;

  SettingsExtensionSnapshot prepared_extension;
  if (extension_changed) {
    status = extension_store.prepare(
        target_main_generation, settingsExtensionValues(values),
        prepared_extension);
    if (!status.ok()) return status;
  }

  if (main_changed) {
    SettingsSnapshot main_prepared;
    status = main_store.prepare(
        mainValues(values), expected_generation, main_prepared);
    if (!status.ok()) return status;
    SettingsSnapshot main_committed;
    status = main_store.commitPrepared(main_prepared, main_committed);
    if (!status.ok()) {
      SettingsSnapshot reloaded;
      if (loadRollbackCompatibleSettings(
              main_store, extension_store, reloaded).ok()) {
        authoritative_snapshot = std::move(reloaded);
      } else {
        authoritative_snapshot = SettingsSnapshot{};
      }
      return status;
    }
  }

  if (extension_changed) {
    SettingsExtensionSnapshot committed_extension;
    status = extension_store.publish(
        prepared_extension, committed_extension);
    if (!status.ok()) {
      SettingsSnapshot reloaded;
      if (loadRollbackCompatibleSettings(
              main_store, extension_store, reloaded).ok()) {
        authoritative_snapshot = std::move(reloaded);
      } else {
        authoritative_snapshot = SettingsSnapshot{};
      }
      return status;
    }
  }

  SettingsSnapshot reloaded;
  status = loadRollbackCompatibleSettings(
      main_store, extension_store, reloaded);
  if (!status.ok() || reloaded.generation != target_main_generation ||
      reloaded.values != values) {
    authoritative_snapshot = status.ok() ? std::move(reloaded)
                                         : SettingsSnapshot{};
    return storage("composite settings commit verification failed");
  }
  authoritative_snapshot = std::move(reloaded);
  return SettingsStatus::success();
}

}  // namespace settings
}  // namespace inkloop
