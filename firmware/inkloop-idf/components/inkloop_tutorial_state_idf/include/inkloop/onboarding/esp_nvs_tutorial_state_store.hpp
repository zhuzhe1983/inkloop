#pragma once

#include "inkloop/onboarding/tutorial_state.hpp"

namespace inkloop::onboarding {

class EspNvsTutorialStateStore final : public ITutorialStateStore {
 public:
  TutorialStateResult load(TutorialStep& output) override;
  TutorialStateResult save(TutorialStep value) override;
};

}  // namespace inkloop::onboarding
