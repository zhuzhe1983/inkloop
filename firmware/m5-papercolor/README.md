# Inkloop M5 PaperColor thin client

This firmware is the device-side adapter for the M5Stack PaperColor C151. It keeps the browser UX unchanged for Bluetooth devices while moving Wi-Fi scheduling onto the ESP32 device.

## First boot

1. The display opens the `Inkloop-XXXX` Wi-Fi provisioning portal.
2. After Wi-Fi is configured, the device starts a password-protected `Inkloop-XXXX-Settings` AP and the `http://inkloop.local` portal. The access code printed over serial and shown on the display is both the AP password and one-time portal bootstrap code.
3. Firmware automatically starts MyAI pairing first with third-party `app_id=inkloop`. The exact six ASCII digits accepted and returned by MyAI are submitted to Inkloop as `pairingCode`; firmware never creates or shows an ordinary second Inkloop code. The code, a QR for the MyAI binding URL, and the same serial `PAIR_CODE` event remain visible after a pending-pairing reboot.
4. MyAI activation, Inkloop binding, the MyAI-spoken voice tutorial, and settings readiness are separate durable onboarding stages. Once bound, Inkloop polling atomically replaces the local task manifest and schedules execute locally.

The web flasher selects the serial device before downloading, performs a hard reset after writing, then reconnects at 115200 baud. It keeps showing structured `INKLOOP_*` boot logs and detects `INKLOOP_PAIR_CODE:123456` so the browser can offer one-click binding.

## Serial diagnostics

The firmware exposes a newline-delimited debug console at 115200 baud. It never prints the device secret. These commands are intended for hardware bring-up and AI-assisted debugging:

- `help` — list commands
- `status` / `diag` — print firmware, board, PM1, Wi-Fi, pairing, revision, heap and PSRAM status as JSON
- `pair-code` — repeat the current six-digit pairing code
- `album-status`, `display-txn` — inspect the pinned album backend and any unfinished display metadata transaction
- `display-recover target|previous` — resolve the deliberately fail-closed ambiguity if power is lost after a refresh was prepared but before its completion could be journaled
- `led-test`, `sound-test`, `screen-test` — test individual output hardware
- `led-map 0|1|swap|auto` — record, reverse, or clear the physical voice/image LED mapping after real-device calibration
- `myai-enable` / `myai-disable` — opt an upgraded 0.2 device into or out of the integrated runtime; reboot afterward
- `reboot` — restart into the application firmware

Boot stages such as `INKLOOP_PM1`, `INKLOOP_DISPLAY_READY`, `INKLOOP_WIFI_AP`, `INKLOOP_REGISTER_HTTP`, errors and heartbeats are machine-readable so copied logs can be analyzed without guessing which stage failed.

Task metadata is persisted at the existing `/tasks.json` LittleFS path. Frame PNGs are authenticated and fetched only when a task is due, so an unpowered/offline device simply does not refresh. Server-side deletion increments the desired revision and is therefore removed from the local manifest on the next online sync.

Downloaded frames are transactionally cached before display. A mounted SD card is preferred unless the portal selects internal storage; otherwise a bounded LittleFS cache keeps at most two frames within a 3 MB ceiling. Before admitting a frame, firmware measures the actual next task manifest, predicted album index, and worst serialized display journal, accounts for their temp/backup copies, and requires 320 KiB to remain afterward (1 MiB on the SD data volume). The calculation has no arbitrary task-count limit. LittleFS is never formatted automatically. SD FAT formatting exists only behind the portal/voice destructive confirmation gates. Every cache/page/display/current operation remains pinned to the backend that supplied the frame. Full/corrupt/removed media or a failed write leaves a recoverable index; once a panel refresh succeeds, current-pointer and task acknowledgement failures are retried from a durable journal without drawing the frame again.

Button A selects the previous cached frame and Button B the next. A low-priority raw-GPIO capture task records A/B/C attempts even while the synchronous quality refresh blocks the application loop; attempts receive one nonblocking wait prompt and never queue another refresh. A completed page refresh returns immediately and starts a 30-second arbitration window before a due task may refresh the panel.

Page selection is debounced for one second. Each accepted press announces the
new ordinal, but physical refresh starts only after the selection remains
stable. If a rapid sequence wraps around and settles on the already displayed
asset, firmware records `PAGE_SKIPPED:ALREADY_CURRENT` and performs no panel
write. Otherwise `display.refresh_start` plus the complete ordinal phrase is
queued immediately before the display transaction begins.

## Render strategies

Each Inkloop task and cached album asset carries one stable strategy identifier.
The local album UI can override it per image; the Portal default is used only
when an older asset has no explicit value.

| id | local label | behavior |
| --- | --- | --- |
| `official-quality` | 官方画质 | M5GFX `epd_quality`; compatibility default |
| `classic-six-color` | 经典六色 | deterministic RGB Floyd–Steinberg diffusion to six inks |
| `reflectance-photo` | 反射率照片 | measured Lab ink anchors, Yule–Nielsen pseudo-reflectance and serpentine Stucki diffusion |
| `solid-clean` | 纯色清晰 | perceptual nearest-ink mapping with no diffusion, for text, tables and large flat fills |

The reflectance-domain design and measured PaperColor palette are adapted from
[`MarsTechHAN/PaperColor-Frame`](https://github.com/MarsTechHAN/PaperColor-Frame)
commit `304ac82a507ad59cc9fdfb4c82512543d7e21e1a`, principally
`main/dither.c`, `main/color_pipeline.c`, and `main/palette.c`. Upstream is
published under GPL-3.0. Preserve that attribution and the applicable source /
distribution notice with firmware artifacts; the project owner has separately
reported additional permission from the upstream author, but that private grant
is not represented as a repository license file here.

## Build

```sh
pio run -d firmware/m5-papercolor
```

The published stable `0.2.0` web-flash manifest remains the compatible four-image
package under `public/firmware/m5-papercolor/`; it is not rewritten by development
packaging. A future test-channel complete-flash package is produced locally with:

```sh
node scripts/package-papercolor-test-channel.mjs \
  --version 0.2.0-slice1r2-dev-c151
```

The command writes only under the ignored `outputs/` tree by default. Its v2
manifest is self-describing (`completeFlash: true`) and requires every segment's
exact address, byte size, and SHA-256:

| role | address | allowed written interval | contents |
| --- | ---: | --- | --- |
| bootloader | `0x000000` | `[0x000000, 0x008000)` | ESP32-S3 second-stage bootloader |
| partitions | `0x008000` | `[0x008000, 0x009000)` | pinned `default_16MB.csv` partition table |
| boot_app0 | `0x00E000` | `[0x00E000, 0x010000)` | OTA boot selector |
| app | `0x010000` | `[0x010000, 0x650000)` | Inkloop application; never OTA app1 |
| littlefs | `0xC90000` | exactly `[0xC90000, 0xFF0000)` | deterministic empty `0x360000`-byte LittleFS |

The packager validates both the CSV and compiled partition binary, rejects overlap
or a missing/hash-mismatched segment, and expands a pinned empty LittleFS template.
The template is already a legal mounted-empty filesystem; it is pinned because the
upstream image creator embeds wall-clock creation metadata and therefore does not
produce identical bytes on repeated invocations.

The stable four-entry legacy manifest remains accepted without adding `role` or
`size`. WebSerial first downloads all four files, then uses their actual byte lengths
to enforce the same role intervals, flash boundary, and adjacent-segment overlap
rules before hashing, patching, or writing anything. In particular, a partition
table may not extend into NVS at `0x009000`, and the app may not extend into the
second OTA slot at `0x650000`.

NVS at `0x009000` is deliberately **not** a packaged image and WebSerial must never
write a reusable NVS blob. After a factory erase those bytes remain erased; the
firmware initializes its Preferences/NVS namespaces on first boot. This avoids
shipping credentials, identity, activation state, or another device's calibration.

The PlatformIO target follows M5Stack's official PaperColor settings: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM and the M5Unified display driver.

Fresh test flashes default the MyAI integration on. An existing schema-v2 NVS record keeps its stored feature flag, so upgrading a deployed 0.2 device does not silently change its behavior; use the serial opt-in command when that device is ready for the new flow. Disabling the flag restores legacy registration/sync and direct display behavior without erasing identity, tasks, or album data.

## Integrated PaperColor runtime

The development build identifies itself as `0.2.0-slice1r2-dev` over serial while retaining `0.2.0` on the existing Inkloop register/sync wire contract. The published `0.2.0` manifest and binaries remain unchanged.

- `DisplayRefreshRuntime` claims the physical writer capability after the boot/onboarding status screen. Inkloop tasks, page changes, and MyAI images then share one attested 400×600 PNG ingress. Official full-screen quality is the default; portal-selected deterministic six-color prequantization takes effect after reboot and uses the no-second-dither panel path.
- `ButtonRouter` reports semantic `VOICE`, `PAGE_PREVIOUS`, and `PAGE_NEXT` events. A/B page the transactional cache and speak the ordinal only after the exact display journal commits. C starts/stops 16 kHz mono PCM capture or confirms a pending destructive voice tool.
- `LedStatusController` owns both RGB pixels behind one internal FreeRTOS mutex. Product output stays mirrored until a real C151 records which physical pixel is the voice side.
- The local portal uses a checksummed dual-slot `ink-portal` snapshot, mDNS/AP access, a one-time session cookie, CSRF and origin/host checks, rate limits, bounded album reads, and web+physical confirmation for destructive operations. It configures storage, SD format, album deletion, volume, agent prompt, image settings, LED swap, render strategy, and battery mode.
- `StorageManager` mounts LittleFS without format-on-failure and prefers an officially wired PaperColor SD backend when present. `AlbumStore` writes and verifies an asset temp file, promotes it, then commits a validated index last while retaining a previous-index recovery copy.
- `DisplayTransaction` persists prepared/displayed/current/task-ack stages on LittleFS. Displayed or later stages retry metadata without redrawing; a prepared stage after reboot requires an explicit serial decision because firmware cannot truthfully infer whether the physical panel completed.
- `TaskStore` writes `/tasks.next`, preserves `/tasks.json` as `/tasks.prev`, and promotes only after a full byte verification. Boot/load recovers a valid next or previous manifest, so an active display journal can keep retrying acknowledgement instead of deadlocking on a missing canonical file.
- `MyAiClient` changes no MyAI server code. It owns fail-closed dual-slot credentials, public-only DNS/TLS endpoint checks, Center gateway probing/selection, session start/heartbeat/disconnect, WSS voice, streaming TTS, and gateway-only AIGC generation/status/output. MyAI and Inkloop images share the same transactional album.
- Packaged 16 kHz prompts cover busy feedback, first/second/third ordinals, confirmation, storage/image tools, settings, and voice errors. PlatformIO validates the manifest, WAV headers and SHA-256 values before dependency resolution; a missing required WAV aborts the project build.
- Local voice tools query space, list/select/delete/clear images, change volume/prompt/image settings, and format the exact `sd` target. Destructive commands bind to an immutable album revision and require the configured spoken/physical confirmation sequence.
- Battery mode uses a two-minute-or-longer idle policy, drains the task/display journal, audio, image RGB and network before sleep, and wakes on any of the three buttons or RTC. The next local schedule and heartbeat both constrain the wake deadline; input remains suppressed until Wi-Fi, Inkloop sync, and key-release debounce complete.
- The right/image RGB role exposes download (blue), cache (yellow), display write (orange), ready (green), and error (red) states. The reversible LED mapping remains the physical calibration boundary.
- `InkloopClient` and `TaskStore` preserve the current v1 API, `inkloop` NVS keys, `/tasks.json`, 15-second polling, and schedule behavior. The optional `pairingCode` overload is backward compatible; malformed, mismatched, expired, colliding, or owned-code responses fail closed.

This workspace firmware is a compile-tested integration candidate. Flashing, microphone/speaker gain, C151 LED-side identity, SD format behavior, panel quality modes, deep-sleep wake pins, and long-running realtime voice/AIGC still require the physical PaperColor acceptance matrix before a public binary is promoted.

If either the settings or Inkloop identity NVS namespace cannot be opened or durably initialized, startup remains offline. The device retries three times, shows one persistent storage error, then retries every 30 seconds without repeatedly refreshing the E Ink panel; it restarts only after both namespaces recover. This prevents an empty or volatile identity from reaching Wi-Fi registration.
