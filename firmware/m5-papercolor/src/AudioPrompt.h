#pragma once

#include <Arduino.h>

namespace inkloop {

enum class AudioPromptKind : uint8_t { None, DisplayBusy, PageBoundary, PageOrdinal };

class AudioPrompt {
 public:
  void requestDisplayBusy();
  void requestPageBoundary();
  void requestPageOrdinal(size_t oneBasedPage);
  void poll();

 private:
  void request(AudioPromptKind kind, size_t value = 0);

  AudioPromptKind pending_ = AudioPromptKind::None;
  size_t value_ = 0;
  uint32_t lastBusyPromptAt_ = 0;
};

}  // namespace inkloop
