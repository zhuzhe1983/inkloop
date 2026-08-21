#pragma once

#include "VoiceAdapters.h"

namespace inkloop {
namespace voice {

class AudioPromptController {
 public:
  AudioPromptController(IClock& clock, IDisplayActivity& display,
                        IAudioPromptPlayer& player);

  // Selection only creates a pending cue. Playback is authorized exclusively
  // by an exact displayCommitSuccess(frame/revision/index) callback.
  Status queueOrdinal(const std::string& frameId, uint64_t revision,
                      uint32_t oneBasedOrdinal);
  Status displayCommitSuccess(const std::string& frameId, uint64_t revision,
                              uint32_t oneBasedOrdinal);
  void displayCommitFailure(const std::string& frameId, uint64_t revision);

  // Busy feedback is bound to one refresh generation and is stopped/discarded
  // as soon as that exact generation ends or is superseded.
  Status notifyRefreshBusy(uint32_t refreshGeneration);
  Status displayRefreshEnded(uint32_t refreshGeneration);

  Status playStatus(const std::string& fileId);
  Status tick();
  // Success proves packaged playback is synchronously stopped.
  Status cancel();

  static const char* ordinalFileId(uint32_t oneBasedOrdinal);

 private:
  struct OrdinalCue {
    std::string frameId;
    uint64_t revision;
    uint32_t ordinal;
    bool committed;

    OrdinalCue() : revision(0), ordinal(0), committed(false) {}
    bool active() const { return !frameId.empty(); }
    void clear() {
      frameId.clear();
      revision = 0;
      ordinal = 0;
      committed = false;
    }
  };

  bool busyGenerationMatches(uint32_t generation) const;
  Status tryPlayOrdinal();
  Status stopBusyCueIfEnded();

  IClock& clock_;
  IDisplayActivity& display_;
  IAudioPromptPlayer& player_;
  OrdinalCue ordinal_;
  uint32_t pendingBusyGeneration_;
  uint32_t playingBusyGeneration_;
  std::string pendingStatus_;
  uint32_t lastBusyCueGeneration_;
  uint64_t lastBusyCueAtMs_;
};

}  // namespace voice
}  // namespace inkloop
