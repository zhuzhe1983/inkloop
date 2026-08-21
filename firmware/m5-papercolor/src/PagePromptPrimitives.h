#pragma once

#include <stddef.h>
#include <stdint.h>

namespace inkloop {

struct PagePromptPlan {
  const char* prompts[7];
  uint8_t count;

  PagePromptPlan() : prompts(), count(0) {}

  bool append(const char* prompt) {
    if (!prompt || count >= sizeof(prompts) / sizeof(prompts[0])) return false;
    prompts[count++] = prompt;
    return true;
  }
};

inline const char* pageDigitPrompt(uint8_t digit) {
  static const char* const prompts[] = {
      "ordinal.digit_zero", "ordinal.digit_one", "ordinal.digit_two",
      "ordinal.digit_three", "ordinal.digit_four", "ordinal.digit_five",
      "ordinal.digit_six", "ordinal.digit_seven", "ordinal.digit_eight",
      "ordinal.digit_nine"};
  return digit < sizeof(prompts) / sizeof(prompts[0]) ? prompts[digit] : 0;
}

inline PagePromptPlan pageOrdinalPromptPlan(
    size_t oneBasedOrdinal, bool includeRefreshStart) {
  PagePromptPlan plan;
  if (includeRefreshStart && !plan.append("display.refresh_start")) return plan;
  if (oneBasedOrdinal == 1) {
    plan.append("ordinal.first");
    return plan;
  }
  if (oneBasedOrdinal == 2) {
    plan.append("ordinal.second");
    return plan;
  }
  if (oneBasedOrdinal == 3) {
    plan.append("ordinal.third");
    return plan;
  }
  if (oneBasedOrdinal == 0 || oneBasedOrdinal > 99) {
    plan.append("ordinal.number");
    return plan;
  }

  plan.append("ordinal.prefix");
  const uint8_t tens = static_cast<uint8_t>(oneBasedOrdinal / 10U);
  const uint8_t ones = static_cast<uint8_t>(oneBasedOrdinal % 10U);
  if (tens == 0) {
    plan.append(pageDigitPrompt(ones));
  } else {
    if (tens > 1) plan.append(pageDigitPrompt(tens));
    plan.append("ordinal.ten");
    if (ones) plan.append(pageDigitPrompt(ones));
  }
  plan.append("ordinal.suffix");
  return plan;
}

}  // namespace inkloop
