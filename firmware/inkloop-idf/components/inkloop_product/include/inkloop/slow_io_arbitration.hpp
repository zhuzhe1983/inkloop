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

  // NativeInkloopService raises portal_operation_active before entering its
  // admitted Portal-lane section.  That flag protects callers on other lanes,
  // but it must not make the admitted owner reject itself.  Keep the public
  // busy view and the already-admitted view explicit so they cannot be
  // accidentally collapsed back into the circular self-gate.
  static constexpr bool inkloopOwnerBusy(bool storage_maintenance,
                                         bool portal_operation_active,
                                         bool display_mailbox_busy) {
    return storage_maintenance || portal_operation_active ||
        display_mailbox_busy;
  }

  static constexpr bool inkloopOwnerAdmittedBusy(bool storage_maintenance,
                                                 bool display_mailbox_busy) {
    return storage_maintenance || display_mailbox_busy;
  }
};

}  // namespace inkloop
