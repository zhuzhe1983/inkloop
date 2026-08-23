#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inkloop/esp_i2s_audio.hpp"

namespace inkloop {

enum class LocalPrompt : uint8_t {
  PleaseWait,
  AlbumEmpty,
  DeviceRestored,
  ConfirmationRequired,
  ConfirmationExpired,
  StorageQueried,
  StorageFormatted,
  ImageDeleted,
  AlbumCleared,
  SettingsSaved,
  Error,
};

struct LocalPromptDiagnostics {
  uint32_t requests = 0;
  uint32_t interruptions = 0;
  uint32_t invalid_assets = 0;
  uint32_t playback_failures = 0;
  size_t pcm_bytes = 0;
};

// Voice-task-only player for bounded embedded PCM16 WAV prompts. It writes one
// short chunk per tick, so a newer button event can replace an obsolete spoken
// ordinal instead of waiting for a full sentence to finish.
class LocalPromptPlayer final {
 public:
  bool requestOrdinal(size_t one_based_ordinal, bool refresh_start,
                      EspI2sAudioDevice& device);
  bool request(LocalPrompt prompt, EspI2sAudioDevice& device);
  bool requestVolumePreview(EspI2sAudioDevice& device);
  esp_err_t service(EspI2sAudioDevice& device);
  void cancel(EspI2sAudioDevice& device);
  bool busy() const { return active_; }
  LocalPromptDiagnostics diagnostics() const { return diagnostics_; }

 private:
  struct Clip {
    const uint8_t* pcm = nullptr;
    size_t bytes = 0;
  };

  bool replace(const uint8_t* const* starts, const uint8_t* const* ends,
               size_t count, EspI2sAudioDevice& device);
  static bool parseWav(const uint8_t* start, const uint8_t* end, Clip& clip);

  static constexpr size_t kMaximumClips = 6U;
  std::array<Clip, kMaximumClips> clips_{};
  size_t clip_count_ = 0;
  size_t clip_index_ = 0;
  size_t clip_offset_ = 0;
  size_t tone_sample_offset_ = 0;
  bool tone_active_ = false;
  bool playback_started_ = false;
  bool startup_feed_complete_ = false;
  bool active_ = false;
  LocalPromptDiagnostics diagnostics_{};
};

}  // namespace inkloop
