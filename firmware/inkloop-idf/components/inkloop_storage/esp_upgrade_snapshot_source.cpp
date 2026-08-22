#include "inkloop/storage/esp_upgrade_snapshot_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "nvs.h"

namespace inkloop {
namespace storage {
namespace {

constexpr std::array<std::uint8_t, 8> kNvsStreamMagic{{
    'I', 'N', 'K', 'N', 'V', 'S', '1', 0}};
constexpr std::size_t kMaximumNvsKeys = 640U;
constexpr std::size_t kFileChunkBytes = 2048U;

struct NvsEntry {
  std::array<char, NVS_KEY_NAME_MAX_SIZE> key{};
  nvs_type_t type = NVS_TYPE_ANY;
};

class ScopedNvsHandle final {
 public:
  explicit ScopedNvsHandle(nvs_handle_t handle) : handle_(handle) {}
  ~ScopedNvsHandle() {
    if (handle_) nvs_close(handle_);
  }
  nvs_handle_t get() const { return handle_; }

 private:
  nvs_handle_t handle_ = 0;
};

class ScopedFile final {
 public:
  explicit ScopedFile(std::FILE* file) : file_(file) {}
  ~ScopedFile() {
    if (file_) std::fclose(file_);
  }
  std::FILE* get() const { return file_; }

 private:
  std::FILE* file_ = nullptr;
};

UpgradeRecordStreamCode mapProbe(RecordProbe probe) {
  switch (probe) {
    case RecordProbe::Missing: return UpgradeRecordStreamCode::Missing;
    case RecordProbe::Valid: return UpgradeRecordStreamCode::Valid;
    case RecordProbe::Recoverable:
      return UpgradeRecordStreamCode::Recoverable;
    case RecordProbe::Ambiguous: return UpgradeRecordStreamCode::Ambiguous;
    case RecordProbe::Invalid: return UpgradeRecordStreamCode::Invalid;
    case RecordProbe::Unvalidated:
      return UpgradeRecordStreamCode::Unvalidated;
    case RecordProbe::IoError: return UpgradeRecordStreamCode::IoError;
  }
  return UpgradeRecordStreamCode::IoError;
}

bool streamable(RecordProbe probe) {
  return probe == RecordProbe::Valid || probe == RecordProbe::Recoverable;
}

bool emit(IUpgradeByteSink& sink, const std::uint8_t* bytes,
          std::size_t length, std::uint64_t maximum,
          std::uint64_t& emitted) {
  if ((!bytes && length != 0U) ||
      length > std::numeric_limits<std::uint64_t>::max() - emitted ||
      emitted + static_cast<std::uint64_t>(length) > maximum ||
      !sink.write(bytes, length)) {
    return false;
  }
  emitted += static_cast<std::uint64_t>(length);
  return true;
}

bool emitU8(IUpgradeByteSink& sink, std::uint8_t value,
            std::uint64_t maximum, std::uint64_t& emitted) {
  return emit(sink, &value, 1U, maximum, emitted);
}

bool emitU16(IUpgradeByteSink& sink, std::uint16_t value,
             std::uint64_t maximum, std::uint64_t& emitted) {
  const std::array<std::uint8_t, 2> bytes{{
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8U)}};
  return emit(sink, bytes.data(), bytes.size(), maximum, emitted);
}

bool emitU32(IUpgradeByteSink& sink, std::uint32_t value,
             std::uint64_t maximum, std::uint64_t& emitted) {
  const std::array<std::uint8_t, 4> bytes{{
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 24U)}};
  return emit(sink, bytes.data(), bytes.size(), maximum, emitted);
}

template <typename Unsigned>
void appendLittle(Unsigned value, std::vector<std::uint8_t>& output) {
  output.resize(sizeof(Unsigned));
  for (std::size_t at = 0U; at < sizeof(Unsigned); ++at)
    output[at] = static_cast<std::uint8_t>(value >> (at * 8U));
}

bool readNvsValue(nvs_handle_t handle, const NvsEntry& entry,
                  std::uint64_t maximum,
                  std::vector<std::uint8_t>& output) {
  output.clear();
  const char* key = entry.key.data();
  esp_err_t status = ESP_FAIL;
  switch (entry.type) {
    case NVS_TYPE_U8: {
      std::uint8_t value = 0;
      status = nvs_get_u8(handle, key, &value);
      if (status == ESP_OK) appendLittle(value, output);
      break;
    }
    case NVS_TYPE_I8: {
      std::int8_t value = 0;
      status = nvs_get_i8(handle, key, &value);
      if (status == ESP_OK)
        appendLittle(static_cast<std::uint8_t>(value), output);
      break;
    }
    case NVS_TYPE_U16: {
      std::uint16_t value = 0;
      status = nvs_get_u16(handle, key, &value);
      if (status == ESP_OK) appendLittle(value, output);
      break;
    }
    case NVS_TYPE_I16: {
      std::int16_t value = 0;
      status = nvs_get_i16(handle, key, &value);
      if (status == ESP_OK)
        appendLittle(static_cast<std::uint16_t>(value), output);
      break;
    }
    case NVS_TYPE_U32: {
      std::uint32_t value = 0;
      status = nvs_get_u32(handle, key, &value);
      if (status == ESP_OK) appendLittle(value, output);
      break;
    }
    case NVS_TYPE_I32: {
      std::int32_t value = 0;
      status = nvs_get_i32(handle, key, &value);
      if (status == ESP_OK)
        appendLittle(static_cast<std::uint32_t>(value), output);
      break;
    }
    case NVS_TYPE_U64: {
      std::uint64_t value = 0;
      status = nvs_get_u64(handle, key, &value);
      if (status == ESP_OK) appendLittle(value, output);
      break;
    }
    case NVS_TYPE_I64: {
      std::int64_t value = 0;
      status = nvs_get_i64(handle, key, &value);
      if (status == ESP_OK)
        appendLittle(static_cast<std::uint64_t>(value), output);
      break;
    }
    case NVS_TYPE_STR: {
      std::size_t length = 0U;
      status = nvs_get_str(handle, key, nullptr, &length);
      if (status != ESP_OK || length == 0U || length - 1U > maximum)
        return false;
      std::vector<char> value(length, '\0');
      status = nvs_get_str(handle, key, value.data(), &length);
      if (status == ESP_OK && length > 0U && value[length - 1U] == '\0')
        output.assign(value.begin(), value.begin() + length - 1U);
      else
        status = ESP_FAIL;
      std::fill(value.begin(), value.end(), '\0');
      break;
    }
    case NVS_TYPE_BLOB: {
      std::size_t length = 0U;
      status = nvs_get_blob(handle, key, nullptr, &length);
      if (status != ESP_OK || length > maximum) return false;
      if (length == 0U) return true;
      output.resize(length);
      status = nvs_get_blob(handle, key, output.data(), &length);
      if (status == ESP_OK && length != output.size()) status = ESP_FAIL;
      break;
    }
    case NVS_TYPE_ANY:
      return false;
  }
  return status == ESP_OK && output.size() <= maximum;
}

}  // namespace

EspUpgradeSnapshotSource::EspUpgradeSnapshotSource(
    std::string internal_root,
    const IUpgradeSnapshotMetadataProvider& metadata_provider)
    : internal_root_(std::move(internal_root)),
      metadata_provider_(metadata_provider), paths_(internal_root_) {
  nvs_classifications_.fill(RecordProbe::IoError);
  file_classifications_.fill(RecordProbe::IoError);
}

bool EspUpgradeSnapshotSource::inspectMetadata(
    UpgradeSnapshotMetadata& output) const {
  pass_ready_ = false;
  output = UpgradeSnapshotMetadata{};
  if (!paths_.pathsValid() ||
      !metadata_provider_.inspectUpgradeSnapshotMetadata(output) ||
      !output.internal_mounted) {
    return false;
  }
  nvs_classifications_ = nvs_.inspect();
  file_classifications_ = paths_.inspectFiles();
  pass_ready_ = true;
  return true;
}

UpgradeRecordStreamCode EspUpgradeSnapshotSource::streamRecord(
    UpgradeRecordId record, std::uint64_t maximum_bytes,
    IUpgradeByteSink& sink) const {
  if (!pass_ready_ || !upgradeRecordIdValid(record) || maximum_bytes == 0U)
    return UpgradeRecordStreamCode::IoError;
  const std::uint64_t cap =
      std::min(maximum_bytes, upgradeRecordMaximumBytes(record));
  return record.domain == UpgradeRecordDomain::NvsNamespace
      ? streamNvsNamespace(record.index, cap, sink)
      : streamFile(record.index, cap, sink);
}

UpgradeRecordStreamCode EspUpgradeSnapshotSource::streamNvsNamespace(
    std::size_t index, std::uint64_t maximum_bytes,
    IUpgradeByteSink& sink) const {
  if (index >= nvs_classifications_.size())
    return UpgradeRecordStreamCode::IoError;
  const RecordProbe classified = nvs_classifications_[index];
  if (!streamable(classified)) return mapProbe(classified);
  const char* namespace_name = kProtectedNvsNamespaces[index];
  const std::size_t namespace_bytes = std::strlen(namespace_name);
  if (namespace_bytes == 0U || namespace_bytes >= NVS_NS_NAME_MAX_SIZE)
    return UpgradeRecordStreamCode::IoError;

  nvs_handle_t raw_handle = 0;
  const esp_err_t opened = nvs_open(namespace_name, NVS_READONLY, &raw_handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return UpgradeRecordStreamCode::Missing;
  if (opened != ESP_OK) return UpgradeRecordStreamCode::IoError;
  ScopedNvsHandle handle(raw_handle);

  std::vector<NvsEntry> entries;
  entries.reserve(32U);
  nvs_iterator_t iterator = nullptr;
  esp_err_t status = nvs_entry_find_in_handle(
      handle.get(), NVS_TYPE_ANY, &iterator);
  while (status == ESP_OK) {
    nvs_entry_info_t info{};
    if (nvs_entry_info(iterator, &info) != ESP_OK ||
        entries.size() >= kMaximumNvsKeys) {
      nvs_release_iterator(iterator);
      return entries.size() >= kMaximumNvsKeys
          ? UpgradeRecordStreamCode::TooLarge
          : UpgradeRecordStreamCode::IoError;
    }
    const std::size_t key_bytes = strnlen(info.key, NVS_KEY_NAME_MAX_SIZE);
    if (key_bytes == 0U || key_bytes >= NVS_KEY_NAME_MAX_SIZE ||
        info.type == NVS_TYPE_ANY) {
      nvs_release_iterator(iterator);
      return UpgradeRecordStreamCode::IoError;
    }
    NvsEntry entry;
    std::copy_n(info.key, key_bytes + 1U, entry.key.begin());
    entry.type = info.type;
    entries.push_back(entry);
    status = nvs_entry_next(&iterator);
  }
  nvs_release_iterator(iterator);
  if (status != ESP_ERR_NVS_NOT_FOUND) return UpgradeRecordStreamCode::IoError;
  if (entries.empty()) return UpgradeRecordStreamCode::Missing;
  std::sort(entries.begin(), entries.end(),
            [](const NvsEntry& left, const NvsEntry& right) {
              const int key = std::strcmp(left.key.data(), right.key.data());
              return key != 0 ? key < 0 : left.type < right.type;
            });
  for (std::size_t at = 1U; at < entries.size(); ++at) {
    if (std::strcmp(entries[at - 1U].key.data(),
                    entries[at].key.data()) == 0)
      return UpgradeRecordStreamCode::Ambiguous;
  }

  std::uint64_t emitted = 0U;
  if (!emit(sink, kNvsStreamMagic.data(), kNvsStreamMagic.size(),
            maximum_bytes, emitted) ||
      !emitU8(sink, static_cast<std::uint8_t>(namespace_bytes),
              maximum_bytes, emitted) ||
      !emit(sink, reinterpret_cast<const std::uint8_t*>(namespace_name),
            namespace_bytes, maximum_bytes, emitted) ||
      entries.size() > std::numeric_limits<std::uint16_t>::max() ||
      !emitU16(sink, static_cast<std::uint16_t>(entries.size()),
               maximum_bytes, emitted)) {
    return UpgradeRecordStreamCode::TooLarge;
  }
  std::vector<std::uint8_t> value;
  for (const NvsEntry& entry : entries) {
    const std::size_t key_bytes = std::strlen(entry.key.data());
    if (!readNvsValue(handle.get(), entry, maximum_bytes, value) ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        !emitU8(sink, static_cast<std::uint8_t>(key_bytes),
                maximum_bytes, emitted) ||
        !emit(sink,
              reinterpret_cast<const std::uint8_t*>(entry.key.data()),
              key_bytes, maximum_bytes, emitted) ||
        !emitU8(sink, static_cast<std::uint8_t>(entry.type),
                maximum_bytes, emitted) ||
        !emitU32(sink, static_cast<std::uint32_t>(value.size()),
                 maximum_bytes, emitted) ||
        !emit(sink, value.data(), value.size(), maximum_bytes, emitted)) {
      std::fill(value.begin(), value.end(), 0U);
      return emitted >= maximum_bytes ? UpgradeRecordStreamCode::TooLarge
                                      : UpgradeRecordStreamCode::IoError;
    }
    std::fill(value.begin(), value.end(), 0U);
  }
  return mapProbe(classified);
}

UpgradeRecordStreamCode EspUpgradeSnapshotSource::streamFile(
    std::size_t index, std::uint64_t maximum_bytes,
    IUpgradeByteSink& sink) const {
  if (index >= file_classifications_.size())
    return UpgradeRecordStreamCode::IoError;
  const RecordProbe classified = file_classifications_[index];
  if (!streamable(classified)) return mapProbe(classified);
  const std::string path = internal_root_ + kProtectedFilePaths[index];
  errno = 0;
  std::FILE* raw = std::fopen(path.c_str(), "rb");
  if (!raw) return errno == ENOENT ? UpgradeRecordStreamCode::Missing
                                  : UpgradeRecordStreamCode::IoError;
  ScopedFile file(raw);
  struct stat before {};
  if (fstat(fileno(file.get()), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0) {
    return UpgradeRecordStreamCode::IoError;
  }
  const std::uint64_t expected = static_cast<std::uint64_t>(before.st_size);
  if (expected > maximum_bytes) return UpgradeRecordStreamCode::TooLarge;
  std::array<std::uint8_t, kFileChunkBytes> chunk{};
  std::uint64_t emitted = 0U;
  while (emitted < expected) {
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk.size(), expected - emitted));
    const std::size_t read = std::fread(chunk.data(), 1U, wanted, file.get());
    if (read != wanted || !emit(sink, chunk.data(), read, maximum_bytes,
                                emitted)) {
      std::fill(chunk.begin(), chunk.end(), 0U);
      return UpgradeRecordStreamCode::IoError;
    }
  }
  const int trailing = std::fgetc(file.get());
  struct stat after {};
  const bool stable = trailing == EOF && !std::ferror(file.get()) &&
      fstat(fileno(file.get()), &after) == 0 &&
      after.st_size == before.st_size && after.st_ino == before.st_ino &&
      after.st_mtime == before.st_mtime;
  std::fill(chunk.begin(), chunk.end(), 0U);
  return stable ? mapProbe(classified) : UpgradeRecordStreamCode::IoError;
}

}  // namespace storage
}  // namespace inkloop
