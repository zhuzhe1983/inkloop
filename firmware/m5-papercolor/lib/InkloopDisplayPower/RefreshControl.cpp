#include "RefreshControl.h"

#include <stdint.h>

#include <atomic>
#include <ctype.h>

namespace inkloop {
namespace displaypower {

bool elapsedAtLeast32(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}

bool deadlineReached32(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

namespace {

std::atomic<uint64_t> nextArbiterOwnerCapability(1);

uint64_t allocateOwnerCapability() {
  uint64_t value = nextArbiterOwnerCapability.fetch_add(1, std::memory_order_relaxed);
  if (value == 0) {
    value = nextArbiterOwnerCapability.fetch_add(1, std::memory_order_relaxed);
  }
  return value;
}

uint8_t scaleChannel(uint8_t channel, uint8_t scale) {
  return static_cast<uint8_t>((static_cast<uint16_t>(channel) * scale + 127U) / 255U);
}

RgbPixel scaleColor(const RgbPixel& color, uint8_t scale) {
  return RgbPixel(
      scaleChannel(color.red, scale),
      scaleChannel(color.green, scale),
      scaleChannel(color.blue, scale));
}

uint8_t triangleBrightness(uint32_t elapsed, uint32_t period, uint8_t minimum) {
  if (period < 2U) return 255;
  const uint32_t half = period / 2U;
  const uint32_t phase = elapsed % period;
  const uint32_t rising = phase <= half ? phase : period - phase;
  const uint32_t range = static_cast<uint32_t>(255U - minimum);
  return static_cast<uint8_t>(minimum + (range * rising) / half);
}

bool validAssetId(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (!isalnum(character) && character != ':' && character != '-' &&
        character != '_') {
      return false;
    }
  }
  return true;
}

}  // namespace

LedOutput imageLedOutput(
    ImageLedState state,
    uint32_t nowMilliseconds,
    uint32_t stateStartedAtMilliseconds) {
  const uint32_t elapsed = static_cast<uint32_t>(
      nowMilliseconds - stateStartedAtMilliseconds);
  switch (state) {
    case ImageLedState::Generating:
      return LedOutput(true, scaleColor(RgbPixel(0, 255, 60),
          triangleBrightness(elapsed, 2000, 48)), true);
    case ImageLedState::Downloading:
      return LedOutput(true, scaleColor(RgbPixel(0, 90, 255),
          triangleBrightness(elapsed, 600, 64)), true);
    case ImageLedState::Caching:
      return LedOutput(true, scaleColor(RgbPixel(255, 190, 0),
          triangleBrightness(elapsed, 800, 64)), true);
    case ImageLedState::Converting:
      return LedOutput(true, scaleColor(RgbPixel(0, 220, 160),
          triangleBrightness(elapsed, 1000, 56)), true);
    case ImageLedState::Writing: {
      const bool on = (elapsed % 500U) < 250U;
      return LedOutput(true, on ? RgbPixel(255, 120, 0) : RgbPixel(), on);
    }
    case ImageLedState::Complete: {
      const bool on = elapsed < 2000U;
      return LedOutput(true, on ? RgbPixel(0, 255, 60) : RgbPixel(), on);
    }
    case ImageLedState::Error: {
      const bool inPattern = elapsed < 1200U;
      const bool on = inPattern && ((elapsed / 200U) % 2U == 0U);
      return LedOutput(true, on ? RgbPixel(255, 0, 0) : RgbPixel(), on);
    }
    case ImageLedState::Off:
      return LedOutput(true, RgbPixel(), false);
  }
  return LedOutput(false, RgbPixel(), false);
}

RefreshTicket::RefreshTicket()
    : ownerCapability_(0),
      generationEpoch_(0),
      generation_(0),
      strategy_(RenderStrategy::OfficialQuality) {}

RefreshTicket::RefreshTicket(
    uint64_t ownerCapability,
    uint64_t generationEpoch,
    uint64_t generation,
    RenderStrategy strategy)
    : ownerCapability_(ownerCapability),
      generationEpoch_(generationEpoch),
      generation_(generation),
      strategy_(strategy) {}

RefreshAcquire::RefreshAcquire(RefreshAcquireResult result)
    : result_(result), hasTicket_(false), ticket_() {}

RefreshAcquire::RefreshAcquire(
    uint64_t ownerCapability,
    uint64_t generationEpoch,
    uint64_t generation,
    RenderStrategy strategy)
    : result_(RefreshAcquireResult::Accepted),
      hasTicket_(true),
      ticket_(ownerCapability, generationEpoch, generation, strategy) {}

RefreshArbiter::RefreshArbiter(uint32_t cooldownMilliseconds)
    : cooldownMilliseconds_(cooldownMilliseconds),
      ownerCapability_(allocateOwnerCapability()),
      configurationValid_(cooldownMilliseconds < 0x80000000UL),
      busy_(false),
      hasCompleted_(false),
      generationEpoch_(1),
      generation_(0),
      activeStrategy_(RenderStrategy::OfficialQuality),
      lastFinishedAtMilliseconds_(0) {}

void RefreshArbiter::advanceGeneration() {
  ++generation_;
  if (generation_ == 0) {
    ++generationEpoch_;
    if (generationEpoch_ == 0) generationEpoch_ = 1;
    generation_ = 1;
  }
}

bool RefreshArbiter::coolingDown(uint32_t nowMilliseconds) const {
  return hasCompleted_ && cooldownMilliseconds_ > 0 &&
      !elapsedAtLeast32(
          nowMilliseconds, lastFinishedAtMilliseconds_, cooldownMilliseconds_);
}

RefreshAcquire RefreshArbiter::acquire(
    const RefreshRequest& request,
    uint32_t nowMilliseconds) {
  if (!configurationValid_ || !validAssetId(request.assetId)) {
    return RefreshAcquire(RefreshAcquireResult::InvalidRequest);
  }
  if (!validRenderStrategy(request.strategy)) {
    return RefreshAcquire(RefreshAcquireResult::InvalidRequest);
  }
  if (busy_) {
    return RefreshAcquire(RefreshAcquireResult::Busy);
  }
  if (coolingDown(nowMilliseconds)) {
    return RefreshAcquire(RefreshAcquireResult::Cooldown);
  }
  advanceGeneration();
  busy_ = true;
  activeStrategy_ = request.strategy;
  return RefreshAcquire(
      ownerCapability_, generationEpoch_, generation_, request.strategy);
}

RefreshFinishResult RefreshArbiter::finish(
    const RefreshTicket& ticket,
    uint32_t nowMilliseconds) {
  if (!busy_) return RefreshFinishResult::NotBusy;
  if (ticket.ownerCapability_ != ownerCapability_) {
    return RefreshFinishResult::WrongTransaction;
  }
  if (ticket.generationEpoch_ != generationEpoch_ ||
      ticket.generation_ != generation_ || ticket.strategy_ != activeStrategy_ ||
      !validRenderStrategy(ticket.strategy_)) {
    return RefreshFinishResult::WrongTransaction;
  }
  busy_ = false;
  hasCompleted_ = true;
  lastFinishedAtMilliseconds_ = nowMilliseconds;
  return RefreshFinishResult::Finished;
}

}  // namespace displaypower
}  // namespace inkloop
