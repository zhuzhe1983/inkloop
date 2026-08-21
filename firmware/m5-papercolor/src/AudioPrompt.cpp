#include "AudioPrompt.h"

#include "Diagnostics.h"

namespace inkloop {

void AudioPrompt::request(AudioPromptKind kind, size_t value) {
  // A single replaceable slot is deliberately nonblocking. Actual localized
  // spoken assets can later consume this seam without entering display code.
  if (!enabled_) return;
  pending_ = kind;
  value_ = value;
}

void AudioPrompt::requestDisplayBusy() {
  const uint32_t now = millis();
  if (lastBusyPromptAt_ && now - lastBusyPromptAt_ < 2000) return;
  lastBusyPromptAt_ = now;
  request(AudioPromptKind::DisplayBusy);
}

void AudioPrompt::requestPageBoundary() {
  request(AudioPromptKind::PageBoundary);
}

void AudioPrompt::requestPageOrdinal(size_t oneBasedPage) {
  request(AudioPromptKind::PageOrdinal, oneBasedPage);
}

void AudioPrompt::poll() {
  const AudioPromptKind prompt = pending_;
  const size_t value = value_;
  pending_ = AudioPromptKind::None;
  value_ = 0;
  if (prompt == AudioPromptKind::DisplayBusy) {
    Diagnostics::event("AUDIO_PROMPT", "DISPLAY_BUSY");
  } else if (prompt == AudioPromptKind::PageBoundary) {
    Diagnostics::event("AUDIO_PROMPT", "PAGE_BOUNDARY");
  } else if (prompt == AudioPromptKind::PageOrdinal) {
    Diagnostics::event("AUDIO_PROMPT", String("PAGE_") + String(value));
  }
}

}  // namespace inkloop
