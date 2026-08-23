#pragma once

#include "inkloop/myai/MyAiTypes.h"

namespace inkloop {

// Product-level fail-closed gate for moving MyAiClient ownership from the
// Network lane to the exclusive AIGC/Portal lane. `tts.stop` is segment-local;
// the client's response-in-flight bit stays set until authoritative
// `response.done`, cancellation, or socket failure.
inline bool voiceBlocksAigcHandoff(myai::VoiceState observed_state,
                                   bool response_in_flight,
                                   bool begin_pending) noexcept {
  return response_in_flight || begin_pending ||
         observed_state == myai::VoiceState::Connecting ||
         observed_state == myai::VoiceState::Listening ||
         observed_state == myai::VoiceState::Thinking ||
         observed_state == myai::VoiceState::Speaking;
}

}  // namespace inkloop
