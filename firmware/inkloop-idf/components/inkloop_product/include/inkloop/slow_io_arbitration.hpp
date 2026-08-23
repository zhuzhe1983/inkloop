#pragma once

namespace inkloop {

// Every slow owner below is serviced by the same Portal lane.  The gates are
// deliberately asymmetric: an active AIGC transaction must not stop Portal
// from draining work that was already accepted, otherwise AIGC Download and
// the Portal mutation queue wait on each other forever.  AIGC may mutate the
// album only after that bounded queue has drained.
class SlowIoArbitration final {
 public:
  static constexpr bool portalMayDrain(bool display_busy,
                                       bool inkloop_busy) {
    return !display_busy && !inkloop_busy;
  }

  static constexpr bool aigcMayMutate(bool display_busy,
                                      bool portal_mutation_busy,
                                      bool inkloop_busy) {
    return !display_busy && !portal_mutation_busy && !inkloop_busy;
  }

  static constexpr bool inkloopMayRun(bool aigc_busy, bool display_busy,
                                      bool portal_mutation_busy) {
    return !aigc_busy && !display_busy && !portal_mutation_busy;
  }
};

}  // namespace inkloop
