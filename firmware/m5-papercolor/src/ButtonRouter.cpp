#include "ButtonRouter.h"

#include <M5Unified.h>

namespace inkloop {

void ButtonRouter::begin(ButtonEventHandler handler, void* context) {
  handler_ = handler;
  context_ = context;
}

void ButtonRouter::poll() {
  if (!handler_) return;
  if (suppressUntilRelease_) {
    if (!suppressedPageReported_ && (M5.BtnA.isPressed() || M5.BtnB.isPressed())) {
      suppressedPageAttempt_ = true;
      suppressedPageReported_ = true;
    }
    if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed() && !M5.BtnC.isPressed()) {
      suppressUntilRelease_ = false;
      suppressedPageReported_ = false;
    }
    return;
  }
  if (M5.BtnC.wasPressed()) handler_(buttonEventForPhysical(PhysicalButton::C), context_);
  if (M5.BtnA.wasPressed()) handler_(buttonEventForPhysical(PhysicalButton::A), context_);
  if (M5.BtnB.wasPressed()) handler_(buttonEventForPhysical(PhysicalButton::B), context_);
}

bool ButtonRouter::takeSuppressedPageAttempt() {
  const bool result = suppressedPageAttempt_;
  suppressedPageAttempt_ = false;
  return result;
}

const char* ButtonRouter::name(ButtonEvent event) {
  switch (event) {
    case ButtonEvent::Voice: return "VOICE";
    case ButtonEvent::PreviousPage: return "PAGE_PREVIOUS";
    case ButtonEvent::NextPage: return "PAGE_NEXT";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
