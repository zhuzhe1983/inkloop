#pragma once

#include "VoiceTypes.h"

namespace inkloop {
namespace voice {

class IClock {
 public:
  virtual ~IClock() {}
  virtual uint64_t monotonicMs() const = 0;
};

// Adapter over MyAiClient's half-duplex methods. Implementations own microphone
// capture and forward PCM frames through MyAiClient while listening.
class IConversationTransport {
 public:
  virtual ~IConversationTransport() {}
  virtual Status startListening(const std::string& streamId) = 0;
  virtual Status stopListeningAndSend() = 0;
  virtual Status requestResponse(const std::string& transcript) = 0;
  virtual Status cancelTts() = 0;
  // True when cancelTurn() closes the underlying session rather than only the
  // active response. VoiceRuntime uses this before cleanup so it can never
  // publish Ready over a socket/lease that the adapter is about to close.
  virtual bool cancelTurnClosesSession() const { return false; }
  virtual Status cancelTurn() = 0;
};

class IVoiceLed {
 public:
  virtual ~IVoiceLed() {}
  virtual void setLeftVoiceState(VoiceLedState state) = 0;
};

class IDisplayActivity {
 public:
  virtual ~IDisplayActivity() {}
  virtual bool refreshBusy() const = 0;
  // Non-zero and stable for one refresh. A new refresh must use a new value.
  virtual uint32_t refreshGeneration() const = 0;
};

// fileId resolves to packaged PCM/WAV later. `argument` is metadata for a
// generic ordinal-number asset; it is not synthesized dynamic TTS.
class IAudioPromptPlayer {
 public:
  virtual ~IAudioPromptPlayer() {}
  virtual bool busy() const = 0;
  virtual Status play(const std::string& fileId, uint32_t argument = 0) = 0;
  // Success means the packaged speaker is synchronously quiescent.
  virtual Status stop() = 0;
};

class ILocalDeviceActions {
 public:
  virtual ~ILocalDeviceActions() {}
  virtual Status queryFreeSpace(StorageSpace& output) = 0;
  virtual Status listImages(std::vector<ImageEntry>& output) = 0;
  // Returns the active immutable album ID and its monotonic content revision.
  virtual Status currentAlbumRevision(AlbumRevision& output) = 0;
  // Accept a stable image ID or an ordinal token such as `@2`; return the
  // selected stable ID and one-based ordinal plus the exact display commit
  // ticket (album/frame/revision) that the display owner will later complete.
  virtual Status selectImage(const std::string& target,
                             ImageSelection& output) = 0;
  virtual Status deleteImageById(const std::string& exactId) = 0;
  virtual Status clearAllUserImages() = 0;
  virtual Status setVolumePercent(uint8_t value) = 0;
  virtual Status formatStorage(const std::string& exactStorageId) = 0;
  virtual Status setAssistantPrompt(const std::string& prompt) = 0;
  virtual Status setImageSetting(const std::string& key,
                                 const std::string& value) = 0;
  virtual Status resetTarget(const std::string& exactTargetId) = 0;
};

class IVoiceRuntimeEvents {
 public:
  virtual ~IVoiceRuntimeEvents() {}
  virtual void onRuntimeState(RuntimeState state) = 0;
  virtual void onCommandResult(const std::string& commandName,
                               const std::string& detail) = 0;
  virtual void onConfirmationRequired(const std::string& exactPhrase,
                                      bool physicalAlsoRequired,
                                      uint64_t expiresAtMs) = 0;
  virtual void onError(const Status& status) = 0;
};

}  // namespace voice
}  // namespace inkloop
