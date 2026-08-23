#pragma once

#include <cstdint>

namespace inkloop::onboarding {

enum class TutorialStep : std::uint8_t {
  PressToTalk = 0,
  VoiceLedStates,
  GalleryPaging,
  DisplayBusyGuard,
  LocalPortal,
  Complete,
};

enum class TutorialStateResult : std::uint8_t {
  Ok = 0,
  Absent,
  Corrupt,
  Storage,
  InvalidArgument,
};

class ITutorialStateStore {
 public:
  virtual ~ITutorialStateStore() = default;
  virtual TutorialStateResult load(TutorialStep& output) = 0;
  virtual TutorialStateResult save(TutorialStep value) = 0;
};

// Board-neutral durable state. Wi-Fi, MyAI credentials and Inkloop identity
// remain authoritative in their existing owners; onboarding persists only
// whether the optional voice tutorial still needs to run.
class TutorialStateCore final {
 public:
  explicit TutorialStateCore(ITutorialStateStore& store) : store_(store) {}

  TutorialStateResult initialize();
  TutorialStateResult set(TutorialStep value);
  TutorialStep step() const { return step_; }
  bool complete() const { return step_ == TutorialStep::Complete; }
  bool persistenceError() const { return persistence_error_; }

 private:
  ITutorialStateStore& store_;
  TutorialStep step_ = TutorialStep::PressToTalk;
  bool persistence_error_ = false;
  bool initialized_ = false;
};

bool validTutorialStep(TutorialStep value);
const char* tutorialStepName(TutorialStep value);

}  // namespace inkloop::onboarding
