#include "PaperColorVoiceAdapters.h"

#include <algorithm>
#include <cstring>

namespace inkloop {
namespace {

#define INKLOOP_WAV_SYMBOL(name)                                      \
  extern const uint8_t _binary_lib_InkloopVoice_assets_speech_##name##_wav_start[] \
      asm("_binary_lib_InkloopVoice_assets_speech_" #name "_wav_start"); \
  extern const uint8_t _binary_lib_InkloopVoice_assets_speech_##name##_wav_end[] \
      asm("_binary_lib_InkloopVoice_assets_speech_" #name "_wav_end")

INKLOOP_WAV_SYMBOL(ordinal_first);
INKLOOP_WAV_SYMBOL(ordinal_second);
INKLOOP_WAV_SYMBOL(ordinal_third);
INKLOOP_WAV_SYMBOL(ordinal_number);
INKLOOP_WAV_SYMBOL(display_please_wait);
INKLOOP_WAV_SYMBOL(confirmation_repeat_exactly);
INKLOOP_WAV_SYMBOL(confirmation_press_top_button);
INKLOOP_WAV_SYMBOL(confirmation_cancelled);
INKLOOP_WAV_SYMBOL(confirmation_expired);
INKLOOP_WAV_SYMBOL(storage_free_space);
INKLOOP_WAV_SYMBOL(storage_formatted);
INKLOOP_WAV_SYMBOL(images_empty);
INKLOOP_WAV_SYMBOL(images_list_ready);
INKLOOP_WAV_SYMBOL(images_deleted);
INKLOOP_WAV_SYMBOL(images_cleared);
INKLOOP_WAV_SYMBOL(settings_saved);
INKLOOP_WAV_SYMBOL(settings_reset);
INKLOOP_WAV_SYMBOL(voice_error);
INKLOOP_WAV_SYMBOL(voice_listening);

struct PromptAsset {
  const char* id;
  const uint8_t* start;
  const uint8_t* end;
};

#define INKLOOP_PROMPT(id, name) \
  {id, _binary_lib_InkloopVoice_assets_speech_##name##_wav_start, \
       _binary_lib_InkloopVoice_assets_speech_##name##_wav_end}

const PromptAsset kPrompts[] = {
    INKLOOP_PROMPT("ordinal.first", ordinal_first),
    INKLOOP_PROMPT("ordinal.second", ordinal_second),
    INKLOOP_PROMPT("ordinal.third", ordinal_third),
    INKLOOP_PROMPT("ordinal.number", ordinal_number),
    INKLOOP_PROMPT("display.please_wait", display_please_wait),
    INKLOOP_PROMPT("confirmation.repeat_exactly", confirmation_repeat_exactly),
    INKLOOP_PROMPT("confirmation.press_top_button", confirmation_press_top_button),
    INKLOOP_PROMPT("confirmation.cancelled", confirmation_cancelled),
    INKLOOP_PROMPT("confirmation.expired", confirmation_expired),
    INKLOOP_PROMPT("storage.free_space", storage_free_space),
    INKLOOP_PROMPT("storage.formatted", storage_formatted),
    INKLOOP_PROMPT("images.empty", images_empty),
    INKLOOP_PROMPT("images.list_ready", images_list_ready),
    INKLOOP_PROMPT("images.deleted", images_deleted),
    INKLOOP_PROMPT("images.cleared", images_cleared),
    INKLOOP_PROMPT("settings.saved", settings_saved),
    INKLOOP_PROMPT("settings.reset", settings_reset),
    INKLOOP_PROMPT("voice.error", voice_error),
    INKLOOP_PROMPT("voice.listening", voice_listening),
};

const PromptAsset* findPrompt(const std::string& id) {
  for (const PromptAsset& prompt : kPrompts) {
    if (id == prompt.id) return &prompt;
  }
  return nullptr;
}

}  // namespace

bool EmbeddedPromptPlayer::busy() const { return M5.Speaker.isPlaying(); }

voice::Status EmbeddedPromptPlayer::play(
    const std::string& fileId, uint32_t) {
  const PromptAsset* prompt = findPrompt(fileId);
  if (!prompt || prompt->end <= prompt->start) {
    return voice::Status::error("prompt_missing", fileId);
  }
  // isEnabled() only means that the PaperColor microphone pins are configured;
  // it stays true even while the microphone is idle.  Blocking on it made every
  // local prompt (including the volume preview) fail on real hardware.
  if (M5.Mic.isRecording() || M5.Mic.isRunning()) {
    return voice::Status::error("microphone_active", "prompt blocked while microphone is active");
  }
  M5.Speaker.setVolume(static_cast<uint8_t>(volumePercent_ * 255U / 100U));
  const bool accepted = M5.Speaker.playWav(
      prompt->start, static_cast<size_t>(prompt->end - prompt->start), 1, -1, true);
  return accepted ? voice::Status::ok()
                  : voice::Status::error("prompt_start_failed", fileId);
}

voice::Status EmbeddedPromptPlayer::stop() {
  M5.Speaker.stop();
  const uint32_t started = millis();
  while (M5.Speaker.isPlaying() && millis() - started < 800U) delay(5);
  return M5.Speaker.isPlaying()
      ? voice::Status::error("prompt_stop_failed", "speaker did not quiesce")
      : voice::Status::ok();
}

void PaperColorVoiceLed::setLeftVoiceState(voice::VoiceLedState state) {
  LedState mapped = LedState::Off;
  switch (state) {
    case voice::VoiceLedState::Listening: mapped = LedState::Listening; break;
    case voice::VoiceLedState::Thinking: mapped = LedState::Thinking; break;
    case voice::VoiceLedState::Speaking: mapped = LedState::Speaking; break;
    case voice::VoiceLedState::Error: mapped = LedState::Error; break;
    default: break;
  }
  leds_.setRoleState(LedRole::Voice, mapped, 40);
}

uint32_t PaperColorDisplayActivity::beginRefresh() {
  ++generation_;
  if (generation_ == 0) ++generation_;
  busy_ = true;
  return generation_;
}

void PaperColorDisplayActivity::endRefresh(uint32_t generation) {
  if (generation == generation_) busy_ = false;
}

voice::Status PaperColorConversationTransport::convert(
    const myai::Status& status, const char* operation) {
  if (status.ok()) return voice::Status::ok();
  return voice::Status::error(
      std::string("myai_") + operation,
      status.detail.empty() ? std::to_string(static_cast<int>(status.code))
                            : status.detail);
}

voice::Status PaperColorConversationTransport::startListening(
    const std::string& streamId) {
  if (audio_.active()) return voice::Status::error("speaker_active", "speaker is not quiescent");
  myai::Status status = client_.beginVoiceTurn(streamId);
  if (!status.ok()) return convert(status, "voice_start");
  auto config = M5.Mic.config();
  config.sample_rate = myai::kVoiceSampleRateHz;
  config.stereo = false;
  M5.Mic.config(config);
  if (!M5.Mic.begin()) {
    client_.disconnectVoice("microphone_start_failed");
    return voice::Status::error("microphone_start_failed", "M5 microphone unavailable");
  }
  capturing_ = true;
  return voice::Status::ok();
}

void PaperColorConversationTransport::pollCapture() {
  if (!capturing_) return;
  if (!M5.Mic.record(samples_, sizeof(samples_) / sizeof(samples_[0]),
                     myai::kVoiceSampleRateHz, false)) return;
  const uint32_t started = millis();
  while (M5.Mic.isRecording() && millis() - started < 40U) delay(1);
  if (!M5.Mic.isRecording()) {
    const myai::Status status = client_.sendPcm16(
        reinterpret_cast<const uint8_t*>(samples_), sizeof(samples_));
    if (!status.ok()) capturing_ = false;
  }
}

voice::Status PaperColorConversationTransport::stopListeningAndSend() {
  if (capturing_) {
    M5.Mic.end();
    capturing_ = false;
  }
  return convert(client_.endVoiceTurn(), "voice_stop");
}

voice::Status PaperColorConversationTransport::requestResponse(
    const std::string& transcript) {
  return convert(client_.requestResponse(transcript), "response");
}

voice::Status PaperColorConversationTransport::cancelTts() {
  audio_.abort();
  return voice::Status::ok();
}

voice::Status PaperColorConversationTransport::cancelTurn() {
  if (capturing_) {
    M5.Mic.end();
    capturing_ = false;
  }
  audio_.abort();
  const myai::Status status = client_.disconnectVoice("turn_cancelled");
  return status.ok() || status.code == myai::ErrorCode::InvalidState
      ? voice::Status::ok() : convert(status, "cancel");
}

}  // namespace inkloop
