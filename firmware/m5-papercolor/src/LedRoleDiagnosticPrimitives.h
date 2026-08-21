#pragma once

#include <stdint.h>

namespace inkloop {

enum class LedDiagnosticRole : uint8_t { None, PowerProof, Voice, Image };

struct LedRoleDiagnosticFrame {
  bool active;
  bool complete;
  bool illuminated;
  uint8_t phase;
  uint8_t cycle;
  LedDiagnosticRole role;
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  LedRoleDiagnosticFrame()
      : active(false),
        complete(false),
        illuminated(false),
        phase(8),
        cycle(0),
        role(LedDiagnosticRole::None),
        red(0),
        green(0),
        blue(0) {}
};

static const uint32_t kLedDiagnosticPowerProofMilliseconds = 800;
static const uint32_t kLedDiagnosticOnMilliseconds = 800;
static const uint32_t kLedDiagnosticOffMilliseconds = 150;
static const uint32_t kLedDiagnosticCycleMilliseconds =
    kLedDiagnosticOnMilliseconds + kLedDiagnosticOffMilliseconds;
static const uint32_t kLedDiagnosticRoleMilliseconds =
    kLedDiagnosticCycleMilliseconds * 2U;
static const uint32_t kLedDiagnosticTotalMilliseconds =
    kLedDiagnosticPowerProofMilliseconds + kLedDiagnosticRoleMilliseconds * 2U;
static const uint8_t kLedDiagnosticBrightness = 144;

inline LedRoleDiagnosticFrame ledRoleDiagnosticFrame(uint32_t elapsedMilliseconds) {
  LedRoleDiagnosticFrame frame;
  if (elapsedMilliseconds >= kLedDiagnosticTotalMilliseconds) {
    frame.complete = true;
    return frame;
  }
  frame.active = true;
  if (elapsedMilliseconds < kLedDiagnosticPowerProofMilliseconds) {
    frame.illuminated = true;
    frame.role = LedDiagnosticRole::PowerProof;
    frame.phase = 0;
    frame.red = 255;
    frame.green = 255;
    frame.blue = 255;
    return frame;
  }
  elapsedMilliseconds -= kLedDiagnosticPowerProofMilliseconds;
  const bool image = elapsedMilliseconds >= kLedDiagnosticRoleMilliseconds;
  const uint32_t roleElapsed = image
      ? elapsedMilliseconds - kLedDiagnosticRoleMilliseconds
      : elapsedMilliseconds;
  frame.cycle = static_cast<uint8_t>(roleElapsed / kLedDiagnosticCycleMilliseconds);
  const uint32_t cycleElapsed = roleElapsed % kLedDiagnosticCycleMilliseconds;
  frame.illuminated = cycleElapsed < kLedDiagnosticOnMilliseconds;
  frame.role = image ? LedDiagnosticRole::Image : LedDiagnosticRole::Voice;
  frame.phase = static_cast<uint8_t>(1U + (image ? 4U : 0U) +
      static_cast<uint32_t>(frame.cycle) * 2U +
      (frame.illuminated ? 0U : 1U));
  if (!frame.illuminated) return frame;
  if (!image && frame.cycle == 0) {
    frame.red = 0;
    frame.green = 110;
    frame.blue = 255;
  } else if (!image) {
    frame.red = 0;
    frame.green = 255;
    frame.blue = 255;
  } else if (frame.cycle == 0) {
    frame.red = 255;
    frame.green = 210;
    frame.blue = 0;
  } else {
    frame.red = 255;
    frame.green = 110;
    frame.blue = 0;
  }
  return frame;
}

}  // namespace inkloop
