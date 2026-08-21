#pragma once

#include <stdint.h>

#include <string>

#include "ImageProcessing.h"

namespace inkloop {
namespace displaypower {

bool elapsedAtLeast32(uint32_t now, uint32_t since, uint32_t duration);
bool deadlineReached32(uint32_t now, uint32_t deadline);

enum class ImageLedState : uint8_t {
  Off,
  Generating,
  Downloading,
  Caching,
  // Conversion is intentionally separate from cache I/O and panel writing so
  // the image-side RGB can explain where time is being spent.
  Converting,
  Writing,
  Complete,
  Error,
};

struct LedOutput {
  bool valid;
  RgbPixel color;
  bool illuminated;

  LedOutput() : valid(false), color(), illuminated(false) {}
  LedOutput(bool isValid, const RgbPixel& value, bool isIlluminated)
      : valid(isValid), color(value), illuminated(isIlluminated) {}
};

// Generates the logical right/image LED sample. Physical left/right mapping is
// deliberately outside this module and remains a C151 calibration concern.
LedOutput imageLedOutput(
    ImageLedState state,
    uint32_t nowMilliseconds,
    uint32_t stateStartedAtMilliseconds);

enum class RefreshAcquireResult : uint8_t { Accepted, InvalidRequest, Busy, Cooldown };
enum class RefreshFinishResult : uint8_t { Finished, NotBusy, WrongTransaction };

struct RefreshRequest {
  std::string assetId;
  RenderStrategy strategy;

  RefreshRequest() : assetId(), strategy(RenderStrategy::OfficialQuality) {}
};

class RefreshArbiter;
class RefreshAcquire;
#ifdef INKLOOP_DISPLAYPOWER_TESTING
class RefreshArbiterTestAccess;
#endif

class RefreshTicket {
 public:
  RefreshTicket(const RefreshTicket& other) = default;
  RefreshTicket& operator=(const RefreshTicket& other) = delete;

  RenderStrategy strategy() const { return strategy_; }
  bool fullScreenRefreshRequired() const { return true; }

 private:
  friend class RefreshArbiter;
  friend class RefreshAcquire;

  RefreshTicket();
  RefreshTicket(
      uint64_t ownerCapability,
      uint64_t generationEpoch,
      uint64_t generation,
      RenderStrategy strategy);

  uint64_t ownerCapability_;
  uint64_t generationEpoch_;
  uint64_t generation_;
  RenderStrategy strategy_;
};

class RefreshAcquire {
 public:
  RefreshAcquire(const RefreshAcquire& other) = default;
  RefreshAcquire& operator=(const RefreshAcquire& other) = delete;

  RefreshAcquireResult result() const { return result_; }
  bool accepted() const { return hasTicket_; }
  const RefreshTicket* ticket() const { return hasTicket_ ? &ticket_ : 0; }

 private:
  friend class RefreshArbiter;

  explicit RefreshAcquire(RefreshAcquireResult result);
  RefreshAcquire(
      uint64_t ownerCapability,
      uint64_t generationEpoch,
      uint64_t generation,
      RenderStrategy strategy);

  RefreshAcquireResult result_;
  bool hasTicket_;
  RefreshTicket ticket_;
};

class RefreshArbiter {
 public:
  explicit RefreshArbiter(uint32_t cooldownMilliseconds);

  RefreshAcquire acquire(const RefreshRequest& request, uint32_t nowMilliseconds);
  RefreshFinishResult finish(
      const RefreshTicket& ticket,
      uint32_t nowMilliseconds);

  bool busy() const { return busy_; }
  bool configurationValid() const { return configurationValid_; }
  uint32_t cooldownMilliseconds() const { return cooldownMilliseconds_; }
  bool coolingDown(uint32_t nowMilliseconds) const;

 private:
#ifdef INKLOOP_DISPLAYPOWER_TESTING
  friend class RefreshArbiterTestAccess;
#endif
  void advanceGeneration();

  uint32_t cooldownMilliseconds_;
  const uint64_t ownerCapability_;
  const bool configurationValid_;
  bool busy_;
  bool hasCompleted_;
  uint64_t generationEpoch_;
  uint64_t generation_;
  RenderStrategy activeStrategy_;
  uint32_t lastFinishedAtMilliseconds_;
};

#ifdef INKLOOP_DISPLAYPOWER_TESTING
class RefreshArbiterTestAccess {
 public:
  static void forceGeneration(
      RefreshArbiter* arbiter,
      uint64_t epoch,
      uint64_t generation) {
    if (!arbiter || arbiter->busy_) return;
    arbiter->generationEpoch_ = epoch == 0 ? 1 : epoch;
    arbiter->generation_ = generation;
  }
};
#endif

}  // namespace displaypower
}  // namespace inkloop
