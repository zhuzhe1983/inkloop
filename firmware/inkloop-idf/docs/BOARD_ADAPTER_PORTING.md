# Inkloop ESP-IDF board adapter porting contract

This contract keeps a new hardware SKU below the existing Product boundary.
A board port may supply pins, buses, panel encoding, power, buttons, RGB,
audio and removable storage. It must not fork or copy MyAI, Inkloop sync,
Portal, album, settings, Recovery, OTA, scheduling or deep-sleep policy.

The current reference implementations are:

- `boards/m5_papercolor_c151`: complete physical C151 adapter;
- `boards/mock_minimal`: no-PSRAM, no-audio, no-RGB, no-SD cross-SKU proof.

## 1. Required component shape

Create `boards/<component_name>/` with at least:

```text
boards/<component_name>/
├── CMakeLists.txt
├── board.cpp
├── include/inkloop/<optional board headers>
└── sdkconfig.defaults
```

The component must link `inkloop_board` and export exactly one
`inkloop::board_adapter()` implementation. Select it at configure time rather
than editing Product code:

```sh
idf.py -B build-<component_name> \
  -DIDF_TARGET=esp32s3 \
  -DINKLOOP_BOARD=<component_name> \
  -DSDKCONFIG=build-<component_name>/sdkconfig \
  build
```

Use a fresh build directory for every SKU. An existing `sdkconfig` outranks
defaults and can silently retain another board's PSRAM, console or peripheral
wiring. Add a fail-closed board-specific check in the top-level
`CMakeLists.txt` for critical memory configuration; never put one SKU's PSRAM
mode in the shared `sdkconfig.defaults`.

## 2. Descriptor and capability truth

`BoardDescriptor` is a wire-visible product contract, not descriptive text.
Set every field to the physical truth:

- `id`: stable lowercase/hyphenated hardware SKU. It becomes MyAI/Inkloop
  metadata, prompt identity and the per-SKU OTA channel key; never rename it
  after devices ship without an identity migration.
- `width`/`height`: native stable display orientation. Portal previews, AIGC
  requested size, PNG validation and album display all derive from it.
- `palette_colors`: 1–16 indices accepted by the board-native 4-bpp frame.
- `has_psram`: controls large frame allocation policy; it must agree with the
  actual boot-time memory configuration.
- `has_sd`: true only when `prepareSdCard`, card detect and the selected IDF
  storage host are usable. No-card and unsupported are different states.
- `has_microphone`/`has_speaker`: the current Voice product is exposed only
  when both are true. Do not advertise half-configured audio.
- `rgb_pixels`: exact addressable count, currently bounded by Portal to 0–8.
- `button_mask`: only physically implemented Previous, Next and Voice keys.

An unsupported capability returns the neutral value (`nullptr`,
`GPIO_NUM_NC`, `false` or `ESP_ERR_NOT_SUPPORTED`) and is declared false/zero.
Never advertise a capability and then emulate success without hardware.
The descriptor must pass its shared `constexpr valid()` contract; the
composition root refuses invalid descriptors before hardware initialization.

## 3. Mandatory adapter behavior

Implement `IBoardAdapter` with these lifecycle rules:

1. `initialize()` is idempotent, initializes dependencies in hardware order and
   returns a real `esp_err_t`. A partial failure releases owned resources and
   leaves the adapter unavailable.
2. Initialization must not visibly refresh the e-paper. Existing stable content
   remains until Product submits a complete display transaction.
3. `display()` and `renderer()` are mandatory after successful initialization
   and return `nullptr` before initialization or after shutdown.
4. `shutdown()` is idempotent and releases resources in reverse order. Product
   unmounts removable storage before a shared SPI bus is freed.
5. Button sampling is nonblocking. GPIO ISR/debounce stays in the shared Input
   owner; the board supplies only GPIO identity and instantaneous pressed state.
6. `setRgb()` writes exactly `descriptor.rgb_pixels` pixels without delays or
   its own animation. The shared LED owner is the only status state machine.
7. Board calls never perform DNS, HTTP, WebSocket, filesystem transactions,
   Portal handling, prompt decisions or cross-owner waits.

Power sequencing, shared-bus arbitration and electrical delays live inside the
adapter, but any delay must remain bounded and must not run from Input, Voice or
LED callbacks.

## 4. Display and renderer contract

The shared display pipeline supplies full-frame RGB scanlines to
`IBoardRenderer` and a board-native `Palette4Bpp` frame to `IBoardDisplay`.

- `renderStrategyCatalog()` is fixed-size, valid and contains
  `official-quality`; IDs are stable lowercase/hyphenated storage/API values.
- `supportsRenderStrategy()` and `renderRgbFullFrame()` agree exactly. An
  unknown strategy fails rather than silently selecting another algorithm.
- RGB input is `width × height × 3`, row-major, with an explicit row stride.
- Native output is exactly `(width × height + 1) / 2` bytes, high nibble first.
  Every nibble is less than `palette_colors`.
- `writeFullFrame()` rejects the wrong geometry, length, format or palette and
  returns only after the controller has accepted the complete transaction.
- `busy()` reflects a real physical write; `sleep()` puts the panel/controller
  into its stable low-power state.
- Provisioning/pairing frames are optional. If unsupported, return
  `ESP_ERR_NOT_SUPPORTED`; Product must not fabricate a board layout.

Palette definitions, dithering/solid-color optimization, orientation, panel
waveforms and controller commands remain entirely inside the board component.

## 5. Audio, storage, wake and diagnostics

For Voice-capable hardware, provide a real `IAudioCodecControl` and
`EspI2sAudioConfig` matching native 16 kHz mono PCM capture. Playback accepts
the negotiated 8–48 kHz mono/stereo PCM stream and the shared device expands or
scales it for the physical stereo slot. DMA and backpressure remain in shared
audio components; the board owns codec registers, pins and clocks only.

For removable storage, `prepareSdCard()` powers/configures the physical slot and
`sharedStorageSpiHost()` returns the already initialized bus. It must never
mount, format or select a filesystem; shared Storage owns those actions.

New wake pins/capabilities must be expressed through the shared power adapter
and tested for first-key consumption, panel preservation and timer wake. Do not
add a second sleep policy to the board.

Secondary serial diagnostics are optional. If enabled, use a separate USB
Serial/JTAG data interface and the existing bounded diagnostics component. A
board must not add an unrestricted shell, raw token/transcript output or a
parallel command path around Product owners.

## 6. Required digital gates

Before any physical flash:

- extend `esp-idf-cross-sku-board.test.mjs` with the new descriptor,
  unsupported-capability and invalid-frame cases;
- extend scaffold/source tests for the component selection and board-specific
  memory/console constraints;
- run the complete `node --test tests/esp-idf-*.test.mjs` suite;
- complete a clean official ESP-IDF build for both the new SKU and at least one
  existing SKU, in separate build directories;
- verify no board source includes Arduino, M5Unified, `HTTPClient`,
  `WebServer`, `WiFiManager` or `Preferences`;
- package/sign under a distinct immutable per-SKU OTA directory without
  changing another SKU's stable channel.

If flash size or partition requirements differ from the retained 16 MiB
layout, stop. Add an explicit layout, migration/rollback design and tests; do
not silently reuse or resize NVS/LittleFS/OTA partitions.

## 7. Physical acceptance

A compile proves only the interface boundary. Repeat every applicable row in
the C151 checklist using a SKU-specific evidence directory, especially:

- rail/bus/panel initialization and shutdown;
- orientation, palette, terminal frame and refresh duration;
- every key's latency under network/storage/display load;
- microphone/speaker quality and cancellation if advertised;
- physical RGB order/brightness if advertised;
- storage detect/remove/full/power-cut behavior if advertised;
- deep-sleep current and every wake source;
- exact MyAI/Inkloop SKU, geometry and capability metadata;
- signed OTA forward/rollback/power-cut without data loss.

Until that evidence passes, the new SKU remains digitally supported only.
