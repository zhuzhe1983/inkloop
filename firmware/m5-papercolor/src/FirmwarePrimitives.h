#pragma once

#include <stdint.h>

namespace inkloop {

enum class ButtonEvent : uint8_t { Voice, PreviousPage, NextPage };
enum class PhysicalButton : uint8_t { A, B, C };

constexpr ButtonEvent buttonEventForPhysical(PhysicalButton button) {
  return button == PhysicalButton::C
    ? ButtonEvent::Voice
    : (button == PhysicalButton::A ? ButtonEvent::PreviousPage : ButtonEvent::NextPage);
}

enum class LedRole : uint8_t { Voice, Image };
enum class LedState : uint8_t {
  Off,
  Connecting,
  Listening,
  Thinking,
  Speaking,
  Generating,
  Downloading,
  Caching,
  Writing,
  Complete,
  Error,
};

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  constexpr RgbColor(uint8_t redValue = 0, uint8_t greenValue = 0, uint8_t blueValue = 0)
    : red(redValue), green(greenValue), blue(blueValue) {}
};

struct LedFrame {
  RgbColor pixels[2];

  constexpr LedFrame(RgbColor first = RgbColor(), RgbColor second = RgbColor())
    : pixels{first, second} {}
};

constexpr RgbColor ledStateColor(LedState state) {
  return state == LedState::Connecting ? RgbColor{0, 100, 255}
    : state == LedState::Listening ? RgbColor{0, 255, 60}
    : state == LedState::Thinking ? RgbColor{150, 30, 255}
    : state == LedState::Speaking ? RgbColor{0, 255, 70}
    : state == LedState::Generating ? RgbColor{0, 255, 60}
    : state == LedState::Downloading ? RgbColor{0, 90, 255}
    : state == LedState::Caching ? RgbColor{255, 190, 0}
    : state == LedState::Writing ? RgbColor{255, 120, 0}
    : state == LedState::Complete ? RgbColor{0, 255, 60}
    : state == LedState::Error ? RgbColor{255, 0, 0}
    : RgbColor{0, 0, 0};
}

constexpr uint8_t ledStatePriority(LedState state) {
  return state == LedState::Error ? 100
    : state == LedState::Writing ? 90
    : (state == LedState::Speaking || state == LedState::Listening) ? 80
    : (state == LedState::Downloading || state == LedState::Caching || state == LedState::Generating) ? 60
    : (state == LedState::Thinking || state == LedState::Connecting) ? 40
    : state == LedState::Complete ? 20
    : 0;
}

constexpr LedFrame resolveLedFrame(
  bool calibrated,
  uint8_t voiceLedIndex,
  uint8_t ledCount,
  LedState voiceState,
  LedState imageState
) {
  return (!calibrated || ledCount < 2)
    ? LedFrame(
        ledStateColor(ledStatePriority(imageState) > ledStatePriority(voiceState) ? imageState : voiceState),
        ledStateColor(ledStatePriority(imageState) > ledStatePriority(voiceState) ? imageState : voiceState)
      )
    : ((voiceLedIndex & 1U) == 0
        ? LedFrame(ledStateColor(voiceState), ledStateColor(imageState))
        : LedFrame(ledStateColor(imageState), ledStateColor(voiceState)));
}

struct StorageSelectionInput {
  bool attached;
  bool removable;
  bool writable;
  bool mounted;

  constexpr StorageSelectionInput(
    bool attachedValue = false,
    bool removableValue = false,
    bool writableValue = false,
    bool mountedValue = false
  ) : attached(attachedValue), removable(removableValue), writable(writableValue), mounted(mountedValue) {}
};

constexpr bool optionalStorageEligible(StorageSelectionInput input) {
  return input.attached && input.removable && input.writable && input.mounted;
}

struct PersistenceReadiness {
  bool settingsReady;
  bool identityReady;

  constexpr PersistenceReadiness(bool settings = false, bool identity = false)
    : settingsReady(settings), identityReady(identity) {}

  constexpr bool safeToStartNetwork() const { return settingsReady && identityReady; }
};

}  // namespace inkloop
