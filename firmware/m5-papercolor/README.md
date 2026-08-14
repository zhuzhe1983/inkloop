# Inkloop M5 PaperColor thin client

This firmware is the device-side adapter for the M5Stack PaperColor C151. It keeps the browser UX unchanged for Bluetooth devices while moving Wi-Fi scheduling onto the ESP32 device.

## First boot

1. The display opens the `Inkloop-XXXX` Wi-Fi provisioning portal.
2. After Wi-Fi is configured, the firmware registers its hardware identity over HTTPS.
3. The display shows a six-digit pairing code and asks the user to bind it through Inkloop's **Add Device** flow.
4. Once bound, the device polls the server every 15 seconds, atomically replaces its local task manifest when the revision changes, and executes schedules locally.

Task metadata is persisted in LittleFS. Frame PNGs are authenticated and fetched only when a task is due, so an unpowered/offline device simply does not refresh. Server-side deletion increments the desired revision and is therefore removed from the local manifest on the next online sync.

## Build

```sh
pio run -d firmware/m5-papercolor
```

The web flasher publishes the bootloader, partition table, boot app and application image under `public/firmware/m5-papercolor/`.

The PlatformIO target follows M5Stack's official PaperColor settings: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM and the M5Unified display driver.

