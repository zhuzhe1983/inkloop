#include "inkloop/local_prompt_player.hpp"

#include <algorithm>
#include <cstring>

namespace inkloop {
namespace {

#define INKLOOP_EMBEDDED_WAV(name)                                      \
  extern const uint8_t _binary_##name##_wav_start[]                     \
      asm("_binary_" #name "_wav_start");                              \
  extern const uint8_t _binary_##name##_wav_end[]                       \
      asm("_binary_" #name "_wav_end")

INKLOOP_EMBEDDED_WAV(ordinal_prefix);
INKLOOP_EMBEDDED_WAV(ordinal_digit_zero);
INKLOOP_EMBEDDED_WAV(ordinal_digit_one);
INKLOOP_EMBEDDED_WAV(ordinal_digit_two);
INKLOOP_EMBEDDED_WAV(ordinal_digit_three);
INKLOOP_EMBEDDED_WAV(ordinal_digit_four);
INKLOOP_EMBEDDED_WAV(ordinal_digit_five);
INKLOOP_EMBEDDED_WAV(ordinal_digit_six);
INKLOOP_EMBEDDED_WAV(ordinal_digit_seven);
INKLOOP_EMBEDDED_WAV(ordinal_digit_eight);
INKLOOP_EMBEDDED_WAV(ordinal_digit_nine);
INKLOOP_EMBEDDED_WAV(ordinal_ten);
INKLOOP_EMBEDDED_WAV(ordinal_suffix);
INKLOOP_EMBEDDED_WAV(display_refresh_start);
INKLOOP_EMBEDDED_WAV(display_please_wait);
INKLOOP_EMBEDDED_WAV(images_empty);
INKLOOP_EMBEDDED_WAV(device_restored);
INKLOOP_EMBEDDED_WAV(confirmation_press_top_button);
INKLOOP_EMBEDDED_WAV(confirmation_expired);
INKLOOP_EMBEDDED_WAV(storage_free_space);
INKLOOP_EMBEDDED_WAV(storage_formatted);
INKLOOP_EMBEDDED_WAV(images_deleted);
INKLOOP_EMBEDDED_WAV(images_cleared);
INKLOOP_EMBEDDED_WAV(settings_saved);
INKLOOP_EMBEDDED_WAV(voice_error);

struct EmbeddedAsset {
  const uint8_t* start;
  const uint8_t* end;
};

#define INKLOOP_ASSET(name)                                             \
  EmbeddedAsset{_binary_##name##_wav_start, _binary_##name##_wav_end}

const EmbeddedAsset kPrefix = INKLOOP_ASSET(ordinal_prefix);
const EmbeddedAsset kTen = INKLOOP_ASSET(ordinal_ten);
const EmbeddedAsset kSuffix = INKLOOP_ASSET(ordinal_suffix);
const EmbeddedAsset kRefreshStart = INKLOOP_ASSET(display_refresh_start);
const EmbeddedAsset kPleaseWait = INKLOOP_ASSET(display_please_wait);
const EmbeddedAsset kAlbumEmpty = INKLOOP_ASSET(images_empty);
const EmbeddedAsset kDeviceRestored = INKLOOP_ASSET(device_restored);
const EmbeddedAsset kConfirmationRequired =
    INKLOOP_ASSET(confirmation_press_top_button);
const EmbeddedAsset kConfirmationExpired = INKLOOP_ASSET(confirmation_expired);
const EmbeddedAsset kStorageQueried = INKLOOP_ASSET(storage_free_space);
const EmbeddedAsset kStorageFormatted = INKLOOP_ASSET(storage_formatted);
const EmbeddedAsset kImageDeleted = INKLOOP_ASSET(images_deleted);
const EmbeddedAsset kAlbumCleared = INKLOOP_ASSET(images_cleared);
const EmbeddedAsset kSettingsSaved = INKLOOP_ASSET(settings_saved);
const EmbeddedAsset kError = INKLOOP_ASSET(voice_error);
const std::array<EmbeddedAsset, 10> kDigits{{
    INKLOOP_ASSET(ordinal_digit_zero),
    INKLOOP_ASSET(ordinal_digit_one),
    INKLOOP_ASSET(ordinal_digit_two),
    INKLOOP_ASSET(ordinal_digit_three),
    INKLOOP_ASSET(ordinal_digit_four),
    INKLOOP_ASSET(ordinal_digit_five),
    INKLOOP_ASSET(ordinal_digit_six),
    INKLOOP_ASSET(ordinal_digit_seven),
    INKLOOP_ASSET(ordinal_digit_eight),
    INKLOOP_ASSET(ordinal_digit_nine),
}};

// Prime 62.5 ms once, then submit at most 10 ms per Voice tick. The device
// starts after 60 ms of converted preload, while steady writes remain short
// enough for a newer button command to interrupt the prompt promptly.
constexpr size_t kPlaybackStartupChunkBytes = 2000U;
constexpr size_t kPlaybackSteadyChunkBytes = 320U;
constexpr size_t kPreviewToneSamples = 4800U;
constexpr size_t kPreviewToneStartupSamples = 1000U;
constexpr size_t kPreviewToneSteadySamples = 160U;
constexpr size_t kPreviewToneFadeSamples = 240U;
constexpr std::array<int16_t, 32> kSine32{{
    0, 1951, 3827, 5556, 7071, 8315, 9239, 9808,
    10000, 9808, 9239, 8315, 7071, 5556, 3827, 1951,
    0, -1951, -3827, -5556, -7071, -8315, -9239, -9808,
    -10000, -9808, -9239, -8315, -7071, -5556, -3827, -1951,
}};

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1]) << 8U;
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8U |
         static_cast<uint32_t>(bytes[2]) << 16U |
         static_cast<uint32_t>(bytes[3]) << 24U;
}

}  // namespace

bool LocalPromptPlayer::parseWav(const uint8_t* start, const uint8_t* end,
                                 Clip& clip) {
  clip = Clip{};
  if (!start || !end || end <= start ||
      static_cast<size_t>(end - start) < 44U ||
      std::memcmp(start, "RIFF", 4) != 0 ||
      std::memcmp(start + 8, "WAVE", 4) != 0) {
    return false;
  }
  const size_t length = static_cast<size_t>(end - start);
  bool format_valid = false;
  size_t offset = 12U;
  while (offset <= length && length - offset >= 8U) {
    const uint8_t* header = start + offset;
    const uint32_t chunk_bytes = readLe32(header + 4U);
    offset += 8U;
    if (chunk_bytes > length - offset) return false;
    if (std::memcmp(header, "fmt ", 4) == 0) {
      if (chunk_bytes < 16U || readLe16(start + offset) != 1U ||
          readLe16(start + offset + 2U) != 1U ||
          readLe32(start + offset + 4U) != 16000U ||
          readLe16(start + offset + 14U) != 16U) {
        return false;
      }
      format_valid = true;
    } else if (std::memcmp(header, "data", 4) == 0) {
      if (!format_valid || chunk_bytes == 0 || (chunk_bytes & 1U) != 0)
        return false;
      clip.pcm = start + offset;
      clip.bytes = chunk_bytes;
      return true;
    }
    const size_t padded = static_cast<size_t>(chunk_bytes) +
                          static_cast<size_t>(chunk_bytes & 1U);
    if (padded > length - offset) return false;
    offset += padded;
  }
  return false;
}

bool LocalPromptPlayer::replace(const uint8_t* const* starts,
                                const uint8_t* const* ends, size_t count,
                                EspI2sAudioDevice& device) {
  if (!starts || !ends || count == 0 || count > clips_.size()) return false;
  std::array<Clip, kMaximumClips> parsed{};
  for (size_t index = 0; index < count; ++index) {
    if (!parseWav(starts[index], ends[index], parsed[index])) {
      ++diagnostics_.invalid_assets;
      return false;
    }
  }
  if (active_ || device.mode() != EspI2sAudioDevice::Mode::Idle) {
    device.abort();
    ++diagnostics_.interruptions;
  }
  clips_ = parsed;
  clip_count_ = count;
  clip_index_ = 0;
  clip_offset_ = 0;
  tone_sample_offset_ = 0;
  tone_active_ = false;
  playback_started_ = false;
  startup_feed_complete_ = false;
  active_ = true;
  ++diagnostics_.requests;
  return true;
}

bool LocalPromptPlayer::requestOrdinal(size_t ordinal, bool refresh_start,
                                       EspI2sAudioDevice& device) {
  if (ordinal == 0 || ordinal > 99U) return false;
  std::array<const uint8_t*, kMaximumClips> starts{};
  std::array<const uint8_t*, kMaximumClips> ends{};
  size_t count = 0;
  auto append = [&](const EmbeddedAsset& asset) {
    if (count >= starts.size()) return false;
    starts[count] = asset.start;
    ends[count] = asset.end;
    ++count;
    return true;
  };
  if (refresh_start && !append(kRefreshStart)) return false;
  if (!append(kPrefix)) return false;
  if (ordinal < 10U) {
    if (!append(kDigits[ordinal])) return false;
  } else {
    const size_t tens = ordinal / 10U;
    const size_t ones = ordinal % 10U;
    if (tens > 1U && !append(kDigits[tens])) return false;
    if (!append(kTen)) return false;
    if (ones != 0U && !append(kDigits[ones])) return false;
  }
  if (!append(kSuffix)) return false;
  return replace(starts.data(), ends.data(), count, device);
}

bool LocalPromptPlayer::request(LocalPrompt prompt,
                                EspI2sAudioDevice& device) {
  EmbeddedAsset asset{};
  switch (prompt) {
    case LocalPrompt::PleaseWait:
      asset = kPleaseWait;
      break;
    case LocalPrompt::AlbumEmpty:
      asset = kAlbumEmpty;
      break;
    case LocalPrompt::DeviceRestored:
      asset = kDeviceRestored;
      break;
    case LocalPrompt::ConfirmationRequired:
      asset = kConfirmationRequired;
      break;
    case LocalPrompt::ConfirmationExpired:
      asset = kConfirmationExpired;
      break;
    case LocalPrompt::StorageQueried:
      asset = kStorageQueried;
      break;
    case LocalPrompt::StorageFormatted:
      asset = kStorageFormatted;
      break;
    case LocalPrompt::ImageDeleted:
      asset = kImageDeleted;
      break;
    case LocalPrompt::AlbumCleared:
      asset = kAlbumCleared;
      break;
    case LocalPrompt::SettingsSaved:
      asset = kSettingsSaved;
      break;
    case LocalPrompt::Error:
      asset = kError;
      break;
  }
  const uint8_t* starts[] = {asset.start};
  const uint8_t* ends[] = {asset.end};
  return replace(starts, ends, 1U, device);
}

bool LocalPromptPlayer::requestVolumePreview(EspI2sAudioDevice& device) {
  if (active_ || device.mode() != EspI2sAudioDevice::Mode::Idle) {
    device.abort();
    ++diagnostics_.interruptions;
  }
  clips_ = {};
  clip_count_ = 0;
  clip_index_ = 0;
  clip_offset_ = 0;
  tone_sample_offset_ = 0;
  tone_active_ = true;
  playback_started_ = false;
  startup_feed_complete_ = false;
  active_ = true;
  ++diagnostics_.requests;
  return true;
}

esp_err_t LocalPromptPlayer::service(EspI2sAudioDevice& device) {
  if (!active_) return ESP_OK;
  if (!playback_started_) {
    const esp_err_t started = device.beginPlayback(16000U, 1U);
    if (started != ESP_OK) {
      ++diagnostics_.playback_failures;
      active_ = false;
      return started;
    }
    playback_started_ = true;
  }
  if (tone_active_) {
    std::array<int16_t, kPreviewToneStartupSamples> pcm{};
    const size_t remaining = kPreviewToneSamples - tone_sample_offset_;
    const size_t limit = startup_feed_complete_
        ? kPreviewToneSteadySamples
        : kPreviewToneStartupSamples;
    const size_t count = std::min(remaining, limit);
    for (size_t index = 0; index < count; ++index) {
      const size_t absolute = tone_sample_offset_ + index;
      const size_t from_end = kPreviewToneSamples - absolute - 1U;
      const size_t fade = std::min(
          kPreviewToneFadeSamples, std::min(absolute, from_end));
      const size_t phase = absolute < kPreviewToneSamples / 2U
          ? absolute & 31U
          : (absolute * 2U) & 31U;
      pcm[index] = static_cast<int16_t>(
          static_cast<int32_t>(kSine32[phase]) * fade /
          kPreviewToneFadeSamples);
    }
    const size_t bytes = count * sizeof(int16_t);
    const esp_err_t written = device.writePlayback(
        reinterpret_cast<const uint8_t*>(pcm.data()), bytes, 20U);
    if (written != ESP_OK) {
      ++diagnostics_.playback_failures;
      device.abort();
      active_ = false;
      playback_started_ = false;
      tone_active_ = false;
      return written;
    }
    diagnostics_.pcm_bytes += bytes;
    tone_sample_offset_ += count;
    startup_feed_complete_ = true;
    if (tone_sample_offset_ == kPreviewToneSamples) tone_active_ = false;
    return ESP_OK;
  }
  if (clip_index_ >= clip_count_) {
    if (playback_started_ && device.finishPlaybackSource() != ESP_OK) {
      ++diagnostics_.playback_failures;
      device.abort();
      active_ = false;
      playback_started_ = false;
      return ESP_ERR_INVALID_STATE;
    }
    if (!device.playbackDrained()) return ESP_OK;
    const esp_err_t ended = device.endPlayback();
    if (ended != ESP_OK) ++diagnostics_.playback_failures;
    active_ = false;
    playback_started_ = false;
    return ended;
  }
  const Clip& clip = clips_[clip_index_];
  const size_t remaining = clip.bytes - clip_offset_;
  const size_t limit = startup_feed_complete_
      ? kPlaybackSteadyChunkBytes
      : kPlaybackStartupChunkBytes;
  const size_t count = std::min(remaining, limit);
  const esp_err_t written =
      device.writePlayback(clip.pcm + clip_offset_, count, 20U);
  if (written != ESP_OK) {
    ++diagnostics_.playback_failures;
    device.abort();
    active_ = false;
    playback_started_ = false;
    return written;
  }
  diagnostics_.pcm_bytes += count;
  clip_offset_ += count;
  startup_feed_complete_ = true;
  if (clip_offset_ == clip.bytes) {
    ++clip_index_;
    clip_offset_ = 0;
  }
  return ESP_OK;
}

void LocalPromptPlayer::cancel(EspI2sAudioDevice& device) {
  if (active_ || playback_started_) device.abort();
  active_ = false;
  playback_started_ = false;
  clip_count_ = 0;
  clip_index_ = 0;
  clip_offset_ = 0;
  tone_sample_offset_ = 0;
  tone_active_ = false;
  startup_feed_complete_ = false;
}

}  // namespace inkloop
