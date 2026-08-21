#pragma once

#include "AudioPromptController.h"
#include "LocalCommandParser.h"

namespace inkloop {
namespace voice {

struct VoiceRuntimeConfig {
  uint32_t listeningTimeoutMs;
  uint32_t thinkingTimeoutMs;
  uint32_t speakingTimeoutMs;
  uint32_t confirmationTimeoutMs;
  uint32_t cleanupRetryMs;

  VoiceRuntimeConfig()
      : listeningTimeoutMs(8000), thinkingTimeoutMs(45000),
        speakingTimeoutMs(60000), confirmationTimeoutMs(20000),
        cleanupRetryMs(100) {}
};

class VoiceRuntime {
 public:
  VoiceRuntime(const VoiceRuntimeConfig& config, IClock& clock,
               IConversationTransport& transport, IVoiceLed& led,
               IDisplayActivity& display, ILocalDeviceActions& actions,
               AudioPromptController& prompts, IVoiceRuntimeEvents& events);

  void setEnabled(bool enabled);
  void onSessionReady();
  void onSessionLost(const Status& status);
  Status onTopButtonTap();
  void tick();

  // All streamed callbacks are bound to activeTurnGeneration(). Late events
  // from cancelled/reconnected turns are ignored without side effects.
  void onAsrPartial(const std::string& transcript, uint32_t turnGeneration);
  TranscriptDecision onAsrFinal(const std::string& transcript,
                                uint32_t turnGeneration);
  Status onTtsStart(uint32_t turnGeneration);
  void onTtsStop(uint32_t turnGeneration);
  void onResponseDone(uint32_t turnGeneration);
  void onTransportError(const Status& status);
  Status onTrustedPhysicalConfirmation();

  Status onDisplayCommitSuccess(const std::string& frameId, uint64_t revision,
                                uint32_t oneBasedOrdinal);
  void onDisplayCommitFailure(const std::string& frameId, uint64_t revision);
  Status onDisplayRefreshEnded(uint32_t refreshGeneration);
  void onAlbumRevisionChanged(const std::string& albumId, uint64_t revision);

  RuntimeState state() const { return state_; }
  const PendingConfirmation& pendingConfirmation() const { return pending_; }
  uint32_t activeTurnGeneration() const { return activeTurnGeneration_; }
  uint32_t sessionGeneration() const { return sessionGeneration_; }
  bool captureActive() const { return captureActive_; }
  bool turnActive() const { return turnActive_; }
  bool cleanupPending() const { return cleanupPending_; }

 private:
  Status startListening();
  Status stopListening();
  Status cancelCurrentTurn();
  Status resetError();
  Status cleanupAudio();
  void retainCleanupFailure(const Status& status);
  void clearCleanupBarrier();
  void retryPendingCleanup();
  bool acceptsTurnEvent(uint32_t turnGeneration) const;
  void finishTurn();
  TranscriptDecision handlePendingConfirmation(const std::string& transcript);
  TranscriptDecision beginConfirmation(const ParsedCommand& command);
  Status validatePendingBinding() const;
  Status dispatch(ParsedCommand command);
  void clearExpiredConfirmation();
  void setState(RuntimeState state, uint32_t timeoutMs = 0);
  VoiceLedState ledForState(RuntimeState state) const;
  void enterError(const Status& status, bool sessionInvalid);

  VoiceRuntimeConfig config_;
  IClock& clock_;
  IConversationTransport& transport_;
  IVoiceLed& led_;
  IDisplayActivity& display_;
  ILocalDeviceActions& actions_;
  AudioPromptController& prompts_;
  IVoiceRuntimeEvents& events_;
  LocalCommandParser parser_;
  PendingConfirmation pending_;
  RuntimeState state_;
  uint64_t stateDeadlineMs_;
  uint32_t turnCounter_;
  uint32_t activeTurnGeneration_;
  uint32_t sessionGeneration_;
  bool enabled_;
  bool sessionReady_;
  bool captureActive_;
  bool turnActive_;
  bool remoteSpeaking_;
  bool cleanupPending_;
  uint64_t cleanupRetryAtMs_;
};

}  // namespace voice
}  // namespace inkloop
