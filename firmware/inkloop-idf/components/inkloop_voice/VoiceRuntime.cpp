#include "VoiceRuntime.h"

#include <sstream>

namespace inkloop {
namespace voice {
namespace {

bool cancelled(const std::string& normalized) {
  return normalized == "取消" || normalized == "取消操作" ||
         normalized == "cancel" || normalized == "cancel operation";
}

std::string imageListDetail(const std::vector<ImageEntry>& images) {
  std::ostringstream detail;
  detail << images.size();
  for (size_t index = 0; index < images.size(); ++index)
    detail << (index == 0 ? ":" : ",") << images[index].id;
  return detail.str();
}

}  // namespace

VoiceRuntime::VoiceRuntime(
    const VoiceRuntimeConfig& config, IClock& clock,
    IConversationTransport& transport, IVoiceLed& led,
    IDisplayActivity& display, ILocalDeviceActions& actions,
    AudioPromptController& prompts, IVoiceRuntimeEvents& events)
    : config_(config), clock_(clock), transport_(transport), led_(led),
      display_(display), actions_(actions), prompts_(prompts), events_(events),
      state_(RuntimeState::Disabled), stateDeadlineMs_(0), turnCounter_(0),
      activeTurnGeneration_(0), sessionGeneration_(0), enabled_(false),
      sessionReady_(false), captureActive_(false), turnActive_(false),
      remoteSpeaking_(false), cleanupPending_(false), cleanupRetryAtMs_(0) {}

void VoiceRuntime::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!enabled) {
    pending_.clear();
    sessionReady_ = false;
    ++sessionGeneration_;
    const Status cleanup = cleanupAudio();
    if (!cleanup.success) {
      retainCleanupFailure(cleanup);
      return;
    }
    clearCleanupBarrier();
    finishTurn();
    setState(RuntimeState::Disabled);
  } else if (cleanupPending_) {
    setState(RuntimeState::Error);
  } else if (sessionReady_) {
    setState(RuntimeState::Ready);
  }
}

void VoiceRuntime::onSessionReady() {
  ++sessionGeneration_;
  pending_.clear();
  const Status cleanup = cleanupAudio();
  if (!enabled_) {
    sessionReady_ = false;
    if (!cleanup.success) {
      retainCleanupFailure(cleanup);
      return;
    }
    clearCleanupBarrier();
    finishTurn();
    setState(RuntimeState::Disabled);
    return;
  }
  sessionReady_ = true;
  if (!cleanup.success) {
    retainCleanupFailure(cleanup);
    return;
  }
  clearCleanupBarrier();
  finishTurn();
  setState(RuntimeState::Ready);
}

void VoiceRuntime::onSessionLost(const Status& status) {
  sessionReady_ = false;
  ++sessionGeneration_;
  pending_.clear();
  enterError(status, false);
}

Status VoiceRuntime::onTopButtonTap() {
  if (!enabled_)
    return Status::error("voice_disabled", "voice runtime is disabled");
  if (state_ == RuntimeState::AwaitingPhysicalConfirmation)
    return onTrustedPhysicalConfirmation();
  if (state_ == RuntimeState::Listening) return stopListening();
  if (state_ == RuntimeState::Thinking || state_ == RuntimeState::Speaking)
    return cancelCurrentTurn();
  if (state_ == RuntimeState::Error) return resetError();
  if (state_ == RuntimeState::Ready ||
      state_ == RuntimeState::AwaitingSpokenConfirmation) {
    if (display_.refreshBusy()) {
      Status cue = prompts_.notifyRefreshBusy(display_.refreshGeneration());
      if (!cue.success) {
        enterError(cue, false);
        return cue;
      }
      return Status::error("display_busy", "display refresh is busy");
    }
    return startListening();
  }
  return Status::error("voice_busy", "voice runtime is busy");
}

Status VoiceRuntime::startListening() {
  if (!sessionReady_)
    return Status::error("session_not_ready", "voice session is not ready");

  // The packaged speaker must be synchronously silent before capture begins.
  Status status = prompts_.cancel();
  if (!status.success) {
    enterError(status, false);
    return status;
  }

  ++turnCounter_;
  if (turnCounter_ == 0) ++turnCounter_;
  activeTurnGeneration_ = turnCounter_;
  std::ostringstream streamId;
  streamId << "papercolor-" << activeTurnGeneration_;

  // Treat even a failed start as potentially partially started, so cleanup
  // always invokes the adapter's idempotent cancel/release operation.
  turnActive_ = true;
  captureActive_ = true;
  status = transport_.startListening(streamId.str());
  if (!status.success) {
    enterError(status, true);
    return status;
  }
  setState(RuntimeState::Listening, config_.listeningTimeoutMs);
  return Status::ok();
}

Status VoiceRuntime::stopListening() {
  if (!captureActive_)
    return Status::error("capture_not_active", "microphone capture is not active");
  Status status = transport_.stopListeningAndSend();
  if (!status.success) {
    enterError(status, true);
    return status;
  }
  captureActive_ = false;
  setState(RuntimeState::Thinking, config_.thinkingTimeoutMs);
  return Status::ok();
}

Status VoiceRuntime::cleanupAudio() {
  Status first = Status::ok();
  if (remoteSpeaking_) {
    Status status = transport_.cancelTts();
    if (status.success)
      remoteSpeaking_ = false;
    else if (first.success)
      first = status;
  }
  if (turnActive_ || captureActive_) {
    Status status = transport_.cancelTurn();
    if (status.success) {
      turnActive_ = false;
      captureActive_ = false;
    } else if (first.success) {
      first = status;
    }
  }
  Status promptStatus = prompts_.cancel();
  if (!promptStatus.success && first.success) first = promptStatus;
  return first;
}

void VoiceRuntime::retainCleanupFailure(const Status& status) {
  cleanupPending_ = true;
  const uint64_t delay = config_.cleanupRetryMs == 0 ? 1 : config_.cleanupRetryMs;
  cleanupRetryAtMs_ = clock_.monotonicMs() + delay;
  setState(RuntimeState::Error);
  events_.onError(status);
}

void VoiceRuntime::clearCleanupBarrier() {
  cleanupPending_ = false;
  cleanupRetryAtMs_ = 0;
}

void VoiceRuntime::retryPendingCleanup() {
  if (!cleanupPending_) return;
  const Status cleanup = cleanupAudio();
  if (!cleanup.success) {
    retainCleanupFailure(cleanup);
    return;
  }
  clearCleanupBarrier();
  finishTurn();
  if (!enabled_)
    setState(RuntimeState::Disabled);
  else
    setState(RuntimeState::Error);
}

Status VoiceRuntime::cancelCurrentTurn() {
  pending_.clear();
  const bool sessionCloses = transport_.cancelTurnClosesSession();
  if (sessionCloses) {
    // Invalidate before cancelTts/cancelTurn: a synchronous audio-ended or
    // socket callback during cleanup must also be unable to publish Ready.
    sessionReady_ = false;
    ++sessionGeneration_;
  }
  Status status = cleanupAudio();
  if (!status.success) {
    enterError(status, true);
    return status;
  }
  finishTurn();
  setState(sessionReady_ ? RuntimeState::Ready : RuntimeState::Error);
  return Status::ok();
}

Status VoiceRuntime::resetError() {
  pending_.clear();
  Status status = cleanupAudio();
  if (!status.success) {
    retainCleanupFailure(status);
    return status;
  }
  clearCleanupBarrier();
  finishTurn();
  if (!sessionReady_) {
    setState(RuntimeState::Error);
    return Status::error("session_not_ready",
                         "session must reconnect before voice can resume");
  }
  setState(RuntimeState::Ready);
  return Status::ok();
}

void VoiceRuntime::tick() {
  if (cleanupPending_) {
    if (clock_.monotonicMs() >= cleanupRetryAtMs_) retryPendingCleanup();
    return;
  }
  Status promptStatus = prompts_.tick();
  if (!promptStatus.success) {
    enterError(promptStatus, false);
    return;
  }
  clearExpiredConfirmation();
  const uint64_t now = clock_.monotonicMs();
  if (stateDeadlineMs_ == 0 || now < stateDeadlineMs_) return;
  if (state_ == RuntimeState::Listening) {
    stopListening();
  } else if (state_ == RuntimeState::Thinking) {
    enterError(Status::error("response_timeout", "voice response timed out"),
               true);
  } else if (state_ == RuntimeState::Speaking) {
    enterError(Status::error("tts_timeout", "TTS playback timed out"), true);
  }
}

bool VoiceRuntime::acceptsTurnEvent(uint32_t turnGeneration) const {
  return enabled_ && sessionReady_ && turnActive_ && turnGeneration != 0 &&
         turnGeneration == activeTurnGeneration_;
}

void VoiceRuntime::onAsrPartial(const std::string&,
                                uint32_t turnGeneration) {
  if (!acceptsTurnEvent(turnGeneration)) return;
  // A partial transcript never changes audio mode or confirmation state.
}

TranscriptDecision VoiceRuntime::onAsrFinal(const std::string& transcript,
                                            uint32_t turnGeneration) {
  // A late/duplicate ASR final after spoken confirmation advanced to the
  // physical stage is not a trusted button gesture. Invalidate the authority
  // rather than allowing an ambiguous voice event to coexist with it.
  if (enabled_ && sessionReady_ && pending_.stage == ConfirmationStage::Physical &&
      turnGeneration != 0 && turnGeneration == activeTurnGeneration_) {
    const std::string name = commandName(pending_.command.kind);
    pending_.clear();
    finishTurn();
    const Status promptStatus = prompts_.cancel();
    if (!promptStatus.success) {
      enterError(promptStatus, false);
      return TranscriptDecision(true, false, name);
    }
    events_.onCommandResult(name, "physical_confirmation_invalidated_by_asr");
    setState(RuntimeState::Ready);
    return TranscriptDecision(true, false, name);
  }
  if (!acceptsTurnEvent(turnGeneration))
    return TranscriptDecision(false, false, "stale_turn");
  if (state_ == RuntimeState::Listening) {
    Status stopped = stopListening();
    if (!stopped.success) return TranscriptDecision(false, false);
  }
  if (state_ != RuntimeState::Thinking)
    return TranscriptDecision(false, false, "invalid_state");

  if (pending_.active()) return handlePendingConfirmation(transcript);
  const ParsedCommand command = parser_.parse(transcript);
  if (!command.matched()) {
    Status status = transport_.requestResponse(transcript);
    if (!status.success) {
      enterError(status, true);
      return TranscriptDecision(false, false);
    }
    setState(RuntimeState::Thinking, config_.thinkingTimeoutMs);
    return TranscriptDecision(false, false);
  }
  if (LocalCommandParser::needsSpokenConfirmation(command.kind))
    return beginConfirmation(command);

  finishTurn();
  const Status status = dispatch(command);
  if (!status.success)
    enterError(status, false);
  else
    setState(RuntimeState::Ready);
  return TranscriptDecision(true, false, commandName(command.kind));
}

Status VoiceRuntime::validatePendingBinding() const {
  if (!pending_.active() || pending_.sessionGeneration != sessionGeneration_)
    return Status::error("confirmation_context_changed",
                         "confirmation session is no longer current");
  AlbumRevision current;
  Status status = actions_.currentAlbumRevision(current);
  if (!status.success) return status;
  if (current.albumId != pending_.targetAlbumId ||
      current.revision != pending_.targetRevision)
    return Status::error("confirmation_context_changed",
                         "target album or revision changed");
  return Status::ok();
}

TranscriptDecision VoiceRuntime::handlePendingConfirmation(
    const std::string& transcript) {
  const uint64_t now = clock_.monotonicMs();
  const std::string name = commandName(pending_.command.kind);
  if (now >= pending_.expiresAtMs) {
    pending_.clear();
    finishTurn();
    setState(RuntimeState::Ready);
    events_.onCommandResult(name, "confirmation_expired");
    return TranscriptDecision(true, false, name);
  }
  Status binding = validatePendingBinding();
  if (!binding.success) {
    pending_.clear();
    finishTurn();
    setState(RuntimeState::Ready);
    events_.onCommandResult(name, "confirmation_context_changed");
    return TranscriptDecision(true, false, name);
  }

  const std::string normalized = LocalCommandParser::normalize(transcript);
  if (cancelled(normalized)) {
    pending_.clear();
    finishTurn();
    setState(RuntimeState::Ready);
    events_.onCommandResult(name, "cancelled");
    Status promptStatus = prompts_.playStatus("confirmation.cancelled");
    if (!promptStatus.success) enterError(promptStatus, false);
    return TranscriptDecision(true, false, name);
  }
  if (pending_.stage == ConfirmationStage::Physical) {
    pending_.clear();
    finishTurn();
    events_.onCommandResult(name, "physical_confirmation_invalidated_by_asr");
    setState(RuntimeState::Ready);
    return TranscriptDecision(true, false, name);
  }
  if (normalized != LocalCommandParser::normalize(pending_.exactPhrase)) {
    pending_.clear();
    finishTurn();
    events_.onCommandResult(name, "spoken_confirmation_mismatch");
    Status promptStatus = prompts_.playStatus("confirmation.cancelled");
    if (!promptStatus.success) {
      enterError(promptStatus, false);
      return TranscriptDecision(true, false, name);
    }
    setState(RuntimeState::Ready);
    return TranscriptDecision(true, false, name);
  }

  const PendingConfirmation consumed = pending_;
  pending_.clear();
  finishTurn();
  if (LocalCommandParser::needsPhysicalConfirmation(consumed.command.kind)) {
    pending_ = consumed;
    pending_.stage = ConfirmationStage::Physical;
    pending_.exactPhrase.clear();
    pending_.expiresAtMs = now + config_.confirmationTimeoutMs;
    events_.onConfirmationRequired("press_top_button", true,
                                   pending_.expiresAtMs);
    Status promptStatus = prompts_.playStatus("confirmation.press_top_button");
    if (!promptStatus.success) {
      enterError(promptStatus, false);
      return TranscriptDecision(true, false, name);
    }
    setState(RuntimeState::AwaitingPhysicalConfirmation);
    return TranscriptDecision(true, true, name);
  }

  const Status status = dispatch(consumed.command);
  if (!status.success)
    enterError(status, false);
  else
    setState(RuntimeState::Ready);
  return TranscriptDecision(true, false, name);
}

TranscriptDecision VoiceRuntime::beginConfirmation(
    const ParsedCommand& command) {
  AlbumRevision binding;
  Status status = actions_.currentAlbumRevision(binding);
  if (!status.success || !LocalCommandParser::isValidStableId(binding.albumId) ||
      binding.revision == 0) {
    finishTurn();
    if (status.success)
      status = Status::error("invalid_album_revision",
                             "pending action requires a stable album revision");
    enterError(status, false);
    return TranscriptDecision(true, false, commandName(command.kind));
  }

  pending_.clear();
  pending_.stage = ConfirmationStage::Spoken;
  pending_.command = command;
  pending_.exactPhrase = LocalCommandParser::confirmationPhrase(command);
  pending_.targetAlbumId = binding.albumId;
  pending_.targetRevision = binding.revision;
  pending_.sessionGeneration = sessionGeneration_;
  pending_.expiresAtMs = clock_.monotonicMs() + config_.confirmationTimeoutMs;
  finishTurn();
  events_.onConfirmationRequired(
      pending_.exactPhrase,
      LocalCommandParser::needsPhysicalConfirmation(command.kind),
      pending_.expiresAtMs);
  Status promptStatus = prompts_.playStatus("confirmation.repeat_exactly");
  if (!promptStatus.success) {
    enterError(promptStatus, false);
    return TranscriptDecision(true, false, commandName(command.kind));
  }
  setState(RuntimeState::AwaitingSpokenConfirmation);
  return TranscriptDecision(true, true, commandName(command.kind));
}

Status VoiceRuntime::onTrustedPhysicalConfirmation() {
  if (pending_.stage != ConfirmationStage::Physical)
    return Status::error("no_physical_confirmation",
                         "no physical confirmation is pending");
  const std::string name = commandName(pending_.command.kind);
  if (clock_.monotonicMs() >= pending_.expiresAtMs) {
    pending_.clear();
    setState(RuntimeState::Ready);
    events_.onCommandResult(name, "confirmation_expired");
    return Status::error("confirmation_expired", "physical confirmation expired");
  }
  Status binding = validatePendingBinding();
  if (!binding.success) {
    pending_.clear();
    setState(RuntimeState::Ready);
    events_.onCommandResult(name, "confirmation_context_changed");
    return binding;
  }

  const ParsedCommand confirmed = pending_.command;
  pending_.clear();
  const Status status = dispatch(confirmed);
  if (!status.success)
    enterError(status, false);
  else
    setState(RuntimeState::Ready);
  return status;
}

Status VoiceRuntime::dispatch(ParsedCommand command) {
  Status status;
  std::string detail = "ok";
  switch (command.kind) {
    case CommandKind::QueryFreeSpace: {
      StorageSpace space;
      status = actions_.queryFreeSpace(space);
      if (status.success) {
        std::ostringstream text;
        text << space.storageId << ':' << space.freeBytes << '/' << space.totalBytes;
        detail = text.str();
        status = prompts_.playStatus("storage.free_space");
      }
      break;
    }
    case CommandKind::ListImages: {
      std::vector<ImageEntry> images;
      status = actions_.listImages(images);
      if (status.success) {
        detail = imageListDetail(images);
        status = prompts_.playStatus(images.empty() ? "images.empty" :
                                                    "images.list_ready");
      }
      break;
    }
    case CommandKind::SelectImage: {
      if (display_.refreshBusy()) {
        status = prompts_.notifyRefreshBusy(display_.refreshGeneration());
        if (status.success)
          status = Status::error("display_busy", "display refresh is busy");
        break;
      }
      ImageSelection selected;
      status = actions_.selectImage(command.targetId, selected);
      if (status.success) {
        if (!LocalCommandParser::isValidStableId(selected.id) ||
            !LocalCommandParser::isValidStableId(selected.albumId)) {
          status = Status::error("invalid_selection", "selection IDs are invalid");
        } else {
          status = prompts_.queueOrdinal(selected.frameId, selected.revision,
                                         selected.ordinal);
          detail = selected.id;
        }
      }
      break;
    }
    case CommandKind::DeleteImage:
      if (display_.refreshBusy()) {
        status = prompts_.notifyRefreshBusy(display_.refreshGeneration());
        if (status.success)
          status = Status::error("display_busy", "display refresh is busy");
      } else {
        status = actions_.deleteImageById(command.targetId);
        if (status.success) status = prompts_.playStatus("images.deleted");
      }
      detail = command.targetId;
      break;
    case CommandKind::ClearAllImages:
      if (display_.refreshBusy()) {
        status = prompts_.notifyRefreshBusy(display_.refreshGeneration());
        if (status.success)
          status = Status::error("display_busy", "display refresh is busy");
      } else {
        status = actions_.clearAllUserImages();
        if (status.success) status = prompts_.playStatus("images.cleared");
      }
      break;
    case CommandKind::SetVolume:
      status = actions_.setVolumePercent(static_cast<uint8_t>(command.number));
      detail = std::to_string(command.number);
      if (status.success) status = prompts_.playStatus("settings.saved");
      break;
    case CommandKind::FormatStorage:
      if (display_.refreshBusy()) {
        status = prompts_.notifyRefreshBusy(display_.refreshGeneration());
        if (status.success)
          status = Status::error("display_busy", "display refresh is busy");
      } else {
        status = actions_.formatStorage(command.targetId);
        if (status.success) status = prompts_.playStatus("storage.formatted");
      }
      detail = command.targetId;
      break;
    case CommandKind::SetAssistantPrompt:
      status = actions_.setAssistantPrompt(command.value);
      detail = "saved";
      if (status.success) status = prompts_.playStatus("settings.saved");
      break;
    case CommandKind::SetImageSetting:
      status = actions_.setImageSetting(command.key, command.value);
      detail = command.key + '=' + command.value;
      if (status.success) status = prompts_.playStatus("settings.saved");
      break;
    case CommandKind::ResetTarget:
      status = actions_.resetTarget(command.targetId);
      detail = command.targetId;
      if (status.success) status = prompts_.playStatus("settings.reset");
      break;
    default:
      return Status::error("unknown_command", "local command is not implemented");
  }
  if (status.success) events_.onCommandResult(commandName(command.kind), detail);
  return status;
}

Status VoiceRuntime::onTtsStart(uint32_t turnGeneration) {
  if (!acceptsTurnEvent(turnGeneration))
    return Status::error("stale_turn", "TTS start is not for the active turn");
  if (state_ != RuntimeState::Thinking && state_ != RuntimeState::Listening)
    return Status::error("invalid_state", "TTS cannot start in this state");
  if (captureActive_) {
    Status stopped = transport_.stopListeningAndSend();
    if (!stopped.success) {
      enterError(stopped, true);
      return stopped;
    }
    captureActive_ = false;
  }
  Status promptStatus = prompts_.cancel();
  if (!promptStatus.success) {
    enterError(promptStatus, false);
    return promptStatus;
  }
  remoteSpeaking_ = true;
  setState(RuntimeState::Speaking, config_.speakingTimeoutMs);
  return Status::ok();
}

void VoiceRuntime::onTtsStop(uint32_t turnGeneration) {
  if (!acceptsTurnEvent(turnGeneration) || !remoteSpeaking_) return;
  remoteSpeaking_ = false;
  finishTurn();
  setState(sessionReady_ ? RuntimeState::Ready : RuntimeState::Error);
}

void VoiceRuntime::onResponseDone(uint32_t turnGeneration) {
  if (!acceptsTurnEvent(turnGeneration) || state_ == RuntimeState::Speaking) return;
  if (state_ != RuntimeState::Thinking || captureActive_) {
    enterError(Status::error("response_done_out_of_order",
                             "response completed before capture was quiescent"),
               true);
    return;
  }
  finishTurn();
  if (!pending_.active())
    setState(sessionReady_ ? RuntimeState::Ready : RuntimeState::Error);
}

void VoiceRuntime::onTransportError(const Status& status) {
  pending_.clear();
  enterError(status, true);
}

Status VoiceRuntime::onDisplayCommitSuccess(const std::string& frameId,
                                            uint64_t revision,
                                            uint32_t oneBasedOrdinal) {
  Status status = prompts_.displayCommitSuccess(frameId, revision,
                                                oneBasedOrdinal);
  if (!status.success && status.code != "display_commit_mismatch")
    enterError(status, false);
  return status;
}

void VoiceRuntime::onDisplayCommitFailure(const std::string& frameId,
                                          uint64_t revision) {
  prompts_.displayCommitFailure(frameId, revision);
}

Status VoiceRuntime::onDisplayRefreshEnded(uint32_t refreshGeneration) {
  Status status = prompts_.displayRefreshEnded(refreshGeneration);
  if (!status.success) enterError(status, false);
  return status;
}

void VoiceRuntime::onAlbumRevisionChanged(const std::string& albumId,
                                          uint64_t revision) {
  if (!pending_.active()) return;
  if (pending_.targetAlbumId == albumId && pending_.targetRevision == revision)
    return;
  const std::string name = commandName(pending_.command.kind);
  pending_.clear();
  events_.onCommandResult(name, "confirmation_context_changed");
  if (!turnActive_) setState(sessionReady_ ? RuntimeState::Ready : RuntimeState::Error);
}

void VoiceRuntime::clearExpiredConfirmation() {
  if (!pending_.active() || clock_.monotonicMs() < pending_.expiresAtMs) return;
  const std::string name = commandName(pending_.command.kind);
  pending_.clear();
  events_.onCommandResult(name, "confirmation_expired");
  // Never start a packaged expiry cue over an in-flight microphone turn.
  if (!turnActive_) {
    Status promptStatus = prompts_.playStatus("confirmation.expired");
    if (!promptStatus.success) {
      enterError(promptStatus, false);
      return;
    }
    setState(sessionReady_ ? RuntimeState::Ready : RuntimeState::Error);
  }
}

void VoiceRuntime::finishTurn() {
  captureActive_ = false;
  turnActive_ = false;
  remoteSpeaking_ = false;
}

void VoiceRuntime::setState(RuntimeState state, uint32_t timeoutMs) {
  state_ = state;
  stateDeadlineMs_ = timeoutMs == 0 ? 0 : clock_.monotonicMs() + timeoutMs;
  led_.setLeftVoiceState(ledForState(state));
  events_.onRuntimeState(state);
}

VoiceLedState VoiceRuntime::ledForState(RuntimeState state) const {
  if (state == RuntimeState::Listening) return VoiceLedState::Listening;
  if (state == RuntimeState::Thinking) return VoiceLedState::Thinking;
  if (state == RuntimeState::Speaking) return VoiceLedState::Speaking;
  if (state == RuntimeState::Error) return VoiceLedState::Error;
  return VoiceLedState::Off;
}

void VoiceRuntime::enterError(const Status& status, bool sessionInvalid) {
  pending_.clear();
  if (sessionInvalid) {
    sessionReady_ = false;
    ++sessionGeneration_;
  }
  const Status cleanup = cleanupAudio();
  setState(RuntimeState::Error);
  events_.onError(status);
  if (!cleanup.success) {
    cleanupPending_ = true;
    const uint64_t delay = config_.cleanupRetryMs == 0 ? 1 : config_.cleanupRetryMs;
    cleanupRetryAtMs_ = clock_.monotonicMs() + delay;
    if (cleanup.code != status.code || cleanup.detail != status.detail)
      events_.onError(cleanup);
  } else {
    clearCleanupBarrier();
    finishTurn();
  }
}

}  // namespace voice
}  // namespace inkloop
