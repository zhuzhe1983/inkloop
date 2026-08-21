#pragma once

namespace inkloop {

inline bool useLegacyDirectDisplay(bool myAiEnabled, bool albumEnabled) {
  return !myAiEnabled || !albumEnabled;
}

inline bool shouldRecoverClosedVoiceAfterCancel(
    bool wasThinkingOrSpeaking, bool cancelSucceeded,
    bool transportSessionClosed) {
  return wasThinkingOrSpeaking && cancelSucceeded && transportSessionClosed;
}

struct DirectDisplayResult {
  bool displayed;
  bool acknowledged;

  DirectDisplayResult(bool shown = false, bool acked = false)
    : displayed(shown), acknowledged(acked) {}
};

template <typename DisplayOperation, typename AcknowledgeOperation>
DirectDisplayResult runAlbumDisabledDirectPath(
  DisplayOperation display,
  AcknowledgeOperation acknowledge
) {
  if (!display()) return DirectDisplayResult(false, false);
  return DirectDisplayResult(true, acknowledge());
}

}  // namespace inkloop
