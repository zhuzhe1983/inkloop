# ESP32 device architecture

## Compatibility boundary

Inkloop treats a physical model as a **SKU** plus a runtime **adapter**:

- `app/lib/device-catalog.ts` owns declarative SKU metadata: family, screen technology, pixel dimensions, orientations, render output, color optimization, transport, write strategy and schedule owner.
- `DEVICE_ADAPTERS` owns runtime behavior switches: browser vs device execution, calibration support, driver requirements and orientation-specific render targets.
- Bluetooth keeps the existing TodooCard browser/GATT path (`todoocard-frame`, 528 × 792, browser scheduling).
- M5 PaperColor uses the Wi-Fi adapter (PNG, 400 × 600 or 600 × 400, HTTPS pull, device scheduling).

Adding another SKU should therefore add catalog metadata and an adapter, without adding model-name conditionals throughout the studio.

## Provisioning and identity

```text
Web Serial flash
  -> firmware receives the current Inkloop /api/devices URL
  -> first boot opens Inkloop-XXXX Wi-Fi captive portal
  -> device creates a 256-bit secret and stable hardware ID
  -> POST register over HTTPS (or explicitly allowed RFC1918 HTTP)
  -> server returns a 10-minute six-digit pairing code
  -> user enters code in Add Device
  -> device is bound to the current Inkloop owner
```

The secret never enters the browser account flow. It remains in ESP32 Preferences and is stored server-side only as SHA-256. Device requests use `Authorization: InkloopDevice <deviceId>:<secret>`. Pairing attempts are rate-limited and temporarily locked after repeated failures.

## Task synchronization

The server maintains `desired_revision`; the device reports `applied_revision` every 15 seconds. When revisions differ, the response is a full replacement manifest rather than a patch. The firmware atomically replaces `/tasks.json` in LittleFS and only then persists the new applied revision.

Full replacement is deliberate: a server-side deletion cannot be resurrected by a stale device. If the device is off, no pull occurs. On the next boot it receives the latest complete manifest, including removal of deleted tasks.

Task metadata and last-run state live on the device. Authenticated PNG frames remain in R2 and are fetched only when a local schedule is due. A frame is validated as 400 × 600 or 600 × 400 before storage and is capped at 1.5 MB.

## Adding a new ESP32 SKU

1. Add its physical/render/write metadata to `DEVICE_SKUS`.
2. Add a runtime entry to `DEVICE_ADAPTERS`.
3. Add a firmware target and signed/checksummed web-flash manifest.
4. Allow the SKU in device registration and validate its exact frame dimensions.
5. Keep the sync envelope stable; put SKU-specific execution behind firmware and render adapters.

