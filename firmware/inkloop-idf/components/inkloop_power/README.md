# Inkloop native power core

This component owns board-neutral sleep admission, typed blocker state,
pre-sleep revalidation, bounded retry/log decisions, and wake recovery order.
It contains no PaperColor, C151, display-page, MyAI, Portal, network, or storage
implementation.

Integration owners provide these seams:

- Update `PowerActivityState` for user activity and every typed blocker. A
  blocker transition resets the complete 120-second idle interval.
- Capture a fresh `PowerInputs` before and after quiescence. The final snapshot
  is authoritative and aborts sleep if new work appeared.
- Implement `ISleepPreparation`; any failed or cancelled preparation calls
  `restoreAwakeServices()`.
- Implement `IDeepSleepDriver` with the selected board's wake capabilities.
- On deep-sleep wake, start `WakeRecoveryRuntime` before dispatching buttons.
  A button wake is consumed, requests the awake indicator, restores services
  while preserving panel RAM/content, reconciles metadata without dispatching
  due work, waits for release/debounce, rearms input, and may queue the local
  `device_restored` prompt. There is intentionally no callback that can render
  a status page, current image, or refresh the panel.

`SleepAttemptRuntime::poll()` returns `Transition`, `Summary`, or `None`; the
composition root maps those to its diagnostics sink. Repeated operational
failures back off from 5 to 60 seconds, while ordinary blockers recheck once a
second. All millisecond intervals are safe across one `uint32_t` wrap.
