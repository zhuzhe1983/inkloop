# Inkloop M5 PaperColor thin client

This firmware is the device-side adapter for the M5Stack PaperColor C151. It keeps the browser UX unchanged for Bluetooth devices while moving Wi-Fi scheduling onto the ESP32 device.

## First boot

1. The display opens the `Inkloop-XXXX` Wi-Fi provisioning portal.
2. After Wi-Fi is configured, the firmware registers its hardware identity over HTTPS.
3. The display shows a six-digit pairing code and asks the user to bind it through Inkloop's **Add Device** flow.
4. Once bound, the device polls the server every 15 seconds, atomically replaces its local task manifest when the revision changes, and executes schedules locally.

The web flasher selects the serial device before downloading, performs a hard reset after writing, then reconnects at 115200 baud. It keeps showing structured `INKLOOP_*` boot logs and detects `INKLOOP_PAIR_CODE:123456` so the browser can offer one-click binding.

## Serial diagnostics

The firmware exposes a newline-delimited debug console at 115200 baud. It never prints the device secret. These commands are intended for hardware bring-up and AI-assisted debugging:

- `help` — list commands
- `status` / `diag` — print firmware, board, PM1, Wi-Fi, pairing, revision, heap and PSRAM status as JSON
- `pair-code` — repeat the current six-digit pairing code
- `led-test`, `sound-test`, `screen-test` — test individual output hardware
- `reboot` — restart into the application firmware

Boot stages such as `INKLOOP_PM1`, `INKLOOP_DISPLAY_READY`, `INKLOOP_WIFI_AP`, `INKLOOP_REGISTER_HTTP`, errors and heartbeats are machine-readable so copied logs can be analyzed without guessing which stage failed.

Task metadata is persisted in LittleFS. Frame PNGs are authenticated and fetched only when a task is due, so an unpowered/offline device simply does not refresh. Server-side deletion increments the desired revision and is therefore removed from the local manifest on the next online sync.

## Build

```sh
pio run -d firmware/m5-papercolor
```

The web flasher publishes the bootloader, partition table, boot app and application image under `public/firmware/m5-papercolor/`.

The PlatformIO target follows M5Stack's official PaperColor settings: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM and the M5Unified display driver.
