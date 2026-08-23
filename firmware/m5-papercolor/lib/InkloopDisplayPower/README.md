# InkloopDisplayPower

Hardware-independent PaperColor image, refresh, LED, and power policy services.
They contain no panel, M5, Wi-Fi, storage, RTC, MyAI/AaaS, or server calls.

## Image and refresh contract

- Encoded refresh requests carry only an asset ID, exact PNG bytes, and a
  strategy. `PngAttestation` parses the PNG signature/chunks/CRC/order and
  accepts only a supported 400×600 IHDR; a SHA-256 digest then binds the exact
  bytes handed to the decoder or official renderer. Caller-provided dimension
  and "already quantized" booleans are not part of the runtime API.
- Accepted decoded target is exact scanline RGB at 400×600.
- Geometry planning supports centered cover/crop and contain/letterbox.
- `papercolor-m5gfx-quality-v1` reproduces M5GFX 0.2.27 ED2208
  `epd_quality` RGB-pair dithering as a bounded scanline stream before the
  native panel sink. M5GFX/LovyanGFX and its FreeBSD licence remain attributed
  in `ImageProcessing.cpp`.
- `papercolor-sixcolor-prequant-v1` is experimental deterministic
  Floyd-Steinberg preprocessing restricted to black, white, yellow, red, blue,
  and green values verified for PaperColor.
- Both strategies explicitly require a full-screen refresh and explicitly do
  not support partial refresh. This module makes no speed claim.
- Unknown image-format, fit, render, and LED enum values fail closed. An invalid
  render descriptor is labeled `invalid-render-strategy`, consumes no pixels,
  and cannot acquire a refresh ticket.
- `RefreshArbiter` owns one logical transaction at a time, rejects busy or
  cooldown requests, and only the matching opaque owner capability can finish
  it. Tickets have private construction/state, no public transaction ID, no
  assignment mutation, are owner-bound and generation-bound, and are single-use.
- `imageLedOutput` generates the logical right/image RGB states; physical LED
  index mapping remains a device calibration responsibility.
- Converting is a first-class image LED state, distinct from generation,
  download, cache I/O, and panel writing.
- `DisplayRefreshRuntime` is disabled by default, claims one application-lifetime
  physical writer, and presents an unforgeable call-scoped capability to the
  renderer. Official M5GFX quality refresh is the default. Experimental
  prequantization is separately opt-in: the runtime decodes the attested bytes,
  performs deterministic six-color conversion itself, and hands a sealed
  palette frame to the no-second-dither writer. Conversion progress drives the
  distinct Converting LED state while panel transfer drives Writing.
- The M5 image-role LED adapter samples bounded patterns at 50 ms. Generation,
  download, cache, conversion, and write states animate independently; completion
  and error are finite patterns.

## Power contract

- Default mode is always awake. Battery mode is explicit opt-in and rejects an
  eligible-idle threshold shorter than 120 seconds.
- Audio, generation, download, conversion, panel write, active portal, and task
  finalization are independent no-sleep blockers. The accepted 0.2 aggregate
  voice/display/journal/task blockers remain available at the integration seam.
- RTC/NTP synchronization and all GPIO1/9/10 buttons released are also required
  before a battery-mode sleep plan is emitted. A supplied heartbeat is bounded
  by the configured periodic heartbeat deadline.
- A timer wake is the earlier adjusted deadline from the next local task and
  heartbeat. The connection margin is subtracted before sleep; due or too-near
  work keeps the device awake.
- `prepareAndExecuteSleep` takes an initial eligibility snapshot, finalizes the
  task/display transaction, quiesces audio, image RGB, and network, then takes a
  second snapshot before configuring wake sources. Any new blocker fails closed
  without entering deep sleep.
- The wake plan is ESP32-S3 deep sleep EXT1 ANY_LOW over GPIO1/9/10 plus the RTC
  timer. It is not a PM1 hard-shutdown wake promise.
- After wake, input remains unavailable through hardware reinitialization,
  Wi-Fi reconnect, Inkloop synchronization, debounced release of all three wake
  keys, and explicit input re-arm; only `Ready` re-enables it. Invalid wake enums
  or illegal transitions fail into/retain a non-ready state.

## ESP32/M5 adapter boundary

`src/DisplayPowerAdapters.*` is the narrow hardware layer. The legacy Arduino
official path delegates to `DisplayController` in `epd_quality`; the portable
renderer also exposes the matching bounded RGB-pair quantizer for native
ESP-IDF. The experimental sealed palette path selects the pinned ED2208
`epd_fastest` row converter, whose pinned implementation uses
`_dither_row_none`, and then restores `epd_quality`.
Image-role status delegates to `LedStatusController`, and stale input suppression
to `ButtonRouter`. The ESP32-S3 implementation uses timer wake plus EXT1 ANY_LOW
for GPIO1/9/10, matching the pinned M5Unified board definition. Wake recovery
invokes caller-provided Wi-Fi reconnect and Inkloop schedule-sync hooks, then
requires a stable all-key release before rearming.

Compile success does not establish physical C151 behavior. Three-key wake,
panel retention after deep sleep, current draw, and LED left/right mapping must
remain explicit on-device acceptance gates. Existing `experimentalRenderEnabled`
and `deepSleepEnabled` flags stay off until integration and hardware acceptance.

Monotonic 32-bit millisecond calculations use subtraction so idle/cooldown/LED
timing remains correct across the usual counter wrap, within durations below
2^31 milliseconds. RTC task/heartbeat deadlines use 64-bit epoch seconds.
