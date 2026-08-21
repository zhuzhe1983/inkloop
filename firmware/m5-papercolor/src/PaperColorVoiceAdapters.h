#pragma once

#include <M5Unified.h>

#include <functional>
#include <string>

#include "LedStatusController.h"
#include "MyAiClient.h"
#include "PaperColorMyAiAdapters.h"
#include "VoiceAdapters.h"

namespace inkloop {

class EmbeddedPromptPlayer final : public voice::IAudioPromptPlayer {
 public:
  bool busy() const override;
  voice::Status play(const std::string& fileId, uint32_t argument = 0) override;
  voice::Status stop() override;
  void setVolume(uint8_t percent) { volumePercent_ = percent > 100 ? 100 : percent; }

 private:
  uint8_t volumePercent_ = 60;
};

class PaperColorVoiceLed final : public voice::IVoiceLed {
 public:
  explicit PaperColorVoiceLed(LedStatusController& leds) : leds_(leds) {}
  void setLeftVoiceState(voice::VoiceLedState state) override;

 private:
  LedStatusController& leds_;
};

class PaperColorDisplayActivity final : public voice::IDisplayActivity {
 public:
  bool refreshBusy() const override { return busy_; }
  uint32_t refreshGeneration() const override { return generation_; }
  uint32_t beginRefresh();
  void endRefresh(uint32_t generation);

 private:
  volatile bool busy_ = false;
  volatile uint32_t generation_ = 1;
};

class PaperColorConversationTransport final
    : public voice::IConversationTransport {
 public:
  PaperColorConversationTransport(
      myai::MyAiClient& client, PaperColorStreamingAudio& audio)
      : client_(client), audio_(audio) {}

  voice::Status startListening(const std::string& streamId) override;
  voice::Status stopListeningAndSend() override;
  voice::Status requestResponse(const std::string& transcript) override;
  voice::Status cancelTts() override;
  bool cancelTurnClosesSession() const override { return true; }
  voice::Status cancelTurn() override;
  void pollCapture();

 private:
  static voice::Status convert(const myai::Status& status, const char* operation);
  myai::MyAiClient& client_;
  PaperColorStreamingAudio& audio_;
  bool capturing_ = false;
  int16_t samples_[320] = {};
};

}  // namespace inkloop
