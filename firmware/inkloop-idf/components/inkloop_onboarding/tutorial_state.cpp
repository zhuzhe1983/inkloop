#include "inkloop/onboarding/tutorial_state.hpp"

namespace inkloop::onboarding {

bool validTutorialStep(TutorialStep value) {
  return static_cast<std::uint8_t>(value) <=
      static_cast<std::uint8_t>(TutorialStep::Complete);
}

const char* tutorialStepName(TutorialStep value) {
  switch (value) {
    case TutorialStep::PressToTalk: return "press_to_talk";
    case TutorialStep::VoiceLedStates: return "voice_led_states";
    case TutorialStep::GalleryPaging: return "gallery_paging";
    case TutorialStep::DisplayBusyGuard: return "display_busy_guard";
    case TutorialStep::LocalPortal: return "local_portal";
    case TutorialStep::Complete: return "complete";
  }
  return "press_to_talk";
}

TutorialStateResult TutorialStateCore::initialize() {
  if (initialized_) return TutorialStateResult::Ok;
  TutorialStep restored = TutorialStep::PressToTalk;
  const TutorialStateResult result = store_.load(restored);
  if (result == TutorialStateResult::Ok && validTutorialStep(restored)) {
    step_ = restored;
    persistence_error_ = false;
  } else {
    // Missing or damaged tutorial metadata must never brick the product or
    // skip first-use guidance. Replaying the tutorial is the safe fallback.
    step_ = TutorialStep::PressToTalk;
    persistence_error_ = result != TutorialStateResult::Absent;
  }
  initialized_ = true;
  return result;
}

TutorialStateResult TutorialStateCore::set(TutorialStep value) {
  if (!initialized_ || !validTutorialStep(value))
    return TutorialStateResult::InvalidArgument;
  const TutorialStateResult result = store_.save(value);
  persistence_error_ = result != TutorialStateResult::Ok;
  if (result == TutorialStateResult::Ok) step_ = value;
  return result;
}

}  // namespace inkloop::onboarding
