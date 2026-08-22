#include "AudioPromptController.h"

namespace inkloop {
namespace voice {

AudioPromptController::AudioPromptController(IClock& clock,
                                             IDisplayActivity& display,
                                             IAudioPromptPlayer& player)
    : clock_(clock), display_(display), player_(player),
      pendingBusyGeneration_(0), playingBusyGeneration_(0),
      lastBusyCueGeneration_(0), lastBusyCueAtMs_(0) {}

Status AudioPromptController::queueOrdinal(const std::string& frameId,
                                           uint64_t revision,
                                           uint32_t oneBasedOrdinal) {
  if (frameId.empty() || revision == 0 || oneBasedOrdinal == 0)
    return Status::error("invalid_display_ticket",
                         "ordinal requires frame, revision, and index");
  ordinal_.frameId = frameId;
  ordinal_.revision = revision;
  ordinal_.ordinal = oneBasedOrdinal;
  ordinal_.committed = false;
  return Status::ok();
}

Status AudioPromptController::displayCommitSuccess(
    const std::string& frameId, uint64_t revision,
    uint32_t oneBasedOrdinal) {
  if (!ordinal_.active() || ordinal_.frameId != frameId ||
      ordinal_.revision != revision || ordinal_.ordinal != oneBasedOrdinal)
    return Status::error("display_commit_mismatch",
                         "display commit does not match pending ordinal");
  ordinal_.committed = true;
  return tryPlayOrdinal();
}

void AudioPromptController::displayCommitFailure(const std::string& frameId,
                                                 uint64_t revision) {
  if (ordinal_.active() && ordinal_.frameId == frameId &&
      ordinal_.revision == revision)
    ordinal_.clear();
}

bool AudioPromptController::busyGenerationMatches(uint32_t generation) const {
  return generation != 0 && display_.refreshBusy() &&
         display_.refreshGeneration() == generation;
}

Status AudioPromptController::notifyRefreshBusy(uint32_t refreshGeneration) {
  if (!busyGenerationMatches(refreshGeneration))
    return Status::error("stale_display_generation",
                         "display busy generation is no longer active");
  const uint64_t now = clock_.monotonicMs();
  if (lastBusyCueGeneration_ == refreshGeneration &&
      lastBusyCueAtMs_ != 0 && now - lastBusyCueAtMs_ < 1500U)
    return Status::ok();
  if (player_.busy()) {
    pendingBusyGeneration_ = refreshGeneration;
    return Status::ok();
  }
  Status status = player_.play("display.please_wait");
  if (status.success) {
    playingBusyGeneration_ = refreshGeneration;
    lastBusyCueGeneration_ = refreshGeneration;
    lastBusyCueAtMs_ = now;
  }
  return status;
}

Status AudioPromptController::displayRefreshEnded(uint32_t refreshGeneration) {
  if (pendingBusyGeneration_ == refreshGeneration) pendingBusyGeneration_ = 0;
  if (playingBusyGeneration_ != refreshGeneration) return Status::ok();
  playingBusyGeneration_ = 0;
  Status status = player_.stop();
  if (!status.success) return status;
  if (player_.busy())
    return Status::error("prompt_stop_incomplete",
                         "busy wait cue remained active after stop");
  return Status::ok();
}

Status AudioPromptController::playStatus(const std::string& fileId) {
  if (fileId.empty())
    return Status::error("missing_prompt", "missing prompt file ID");
  if (player_.busy()) {
    pendingStatus_ = fileId;
    return Status::ok();
  }
  return player_.play(fileId);
}

Status AudioPromptController::stopBusyCueIfEnded() {
  if (playingBusyGeneration_ == 0) return Status::ok();
  if (!player_.busy()) {
    playingBusyGeneration_ = 0;
    return Status::ok();
  }
  if (busyGenerationMatches(playingBusyGeneration_)) return Status::ok();
  const uint32_t ended = playingBusyGeneration_;
  playingBusyGeneration_ = 0;
  Status status = player_.stop();
  if (!status.success) return status;
  if (player_.busy()) {
    playingBusyGeneration_ = ended;
    return Status::error("prompt_stop_incomplete",
                         "stale busy cue remained active after stop");
  }
  return Status::ok();
}

Status AudioPromptController::tick() {
  Status status = stopBusyCueIfEnded();
  if (!status.success) return status;

  if (pendingBusyGeneration_ != 0 &&
      !busyGenerationMatches(pendingBusyGeneration_))
    pendingBusyGeneration_ = 0;
  if (player_.busy()) return Status::ok();

  if (pendingBusyGeneration_ != 0) {
    const uint32_t generation = pendingBusyGeneration_;
    pendingBusyGeneration_ = 0;
    status = player_.play("display.please_wait");
    if (status.success) {
      playingBusyGeneration_ = generation;
      lastBusyCueGeneration_ = generation;
      lastBusyCueAtMs_ = clock_.monotonicMs();
    }
    return status;
  }
  if (!pendingStatus_.empty()) {
    const std::string fileId = pendingStatus_;
    pendingStatus_.clear();
    return player_.play(fileId);
  }
  return tryPlayOrdinal();
}

Status AudioPromptController::cancel() {
  ordinal_.clear();
  pendingBusyGeneration_ = 0;
  pendingStatus_.clear();
  playingBusyGeneration_ = 0;
  Status status = player_.stop();
  if (!status.success) return status;
  if (player_.busy())
    return Status::error("prompt_stop_incomplete",
                         "packaged playback remained active after stop");
  return Status::ok();
}

const char* AudioPromptController::ordinalFileId(uint32_t oneBasedOrdinal) {
  if (oneBasedOrdinal == 1) return "ordinal.first";
  if (oneBasedOrdinal == 2) return "ordinal.second";
  if (oneBasedOrdinal == 3) return "ordinal.third";
  return "ordinal.number";
}

Status AudioPromptController::tryPlayOrdinal() {
  if (!ordinal_.active() || !ordinal_.committed || player_.busy())
    return Status::ok();
  const uint32_t ordinal = ordinal_.ordinal;
  ordinal_.clear();
  return player_.play(ordinalFileId(ordinal), ordinal > 3 ? ordinal : 0);
}

}  // namespace voice
}  // namespace inkloop
