# Inkloop ESP-IDF firmware

This is the new reusable firmware root. It deliberately does not depend on
Arduino, M5Unified, `HTTPClient`, `WebServer`, `WiFiManager`, `Preferences`, or
Arduino `String`.

Status: the native Product graph now composes the C151 board, bounded dual-core
runtime, Wi-Fi/Portal, storage, display, Inkloop, MyAI voice/AIGC, settings,
power, Recovery and signed-OTA owners without an Arduino compatibility layer.
Host fault matrices and official ESP-IDF v6.0.2 C151/mock links prove the
digital composition. Native beta10 additionally has retained device evidence
for boot, saved Wi-Fi, Portal health, deep sleep and timer panel-preserving
wake; it does **not** prove the remaining physical or public-service behavior.

Do not treat this tree as release-ready until the exact candidate's all-tree
gate, fresh independent acceptance and the authorized attached-C151 run in
[the physical acceptance checklist](docs/C151_PHYSICAL_ACCEPTANCE.md) pass.
The per-capability split between implemented digital behavior and pending
physical/live evidence is [the migration matrix](docs/MIGRATION_MATRIX.md).
The checked-in OTA URL and public key are intentionally empty, so normal OTA
is fail-closed until a reviewed external signing/channel configuration exists.

## Layout

- `components/inkloop_contracts`: bounded cross-task messages and ownership
  vocabulary shared by every board.
- `components/inkloop_runtime`: dual-core topology and validation. Product
  state remains message-driven and single-owner.
- `boards/<board>`: pins, PMIC, panel, audio, storage and wake adapters only.
- `main`: composition root; it must not contain product logic.
- `docs`: migration evidence, sequencing and acceptance requirements.

## Build (after installing ESP-IDF)

The supported toolchain is pinned in `.idf-version` (`v6.0.2`). The wrapper
fails closed when `IDF_PATH` points at another revision:

```sh
IDF_PATH=/path/to/esp-idf ./tools/build.sh
```

Use `./tools/build.sh --clean` when changing target/default Kconfig values; the
normal command preserves incremental objects.

The equivalent manual commands are:

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

Select another board without changing service code:

```sh
idf.py -DINKLOOP_BOARD=my_new_board reconfigure
```

## Non-negotiable scheduling order

1. GPIO input capture
2. voice capture/playback/session progression
3. product control state
4. LED status state machine
5. display/storage/network background work
6. WebUI (event-driven and normally asleep)

No task may synchronously call into another task owner. Commands and results
cross bounded queues; timeouts, cancellation and generation IDs are explicit.

## Physical acceptance tools

The repository-root scripts are intentionally separate from the firmware
runtime:

- `scripts/papercolor_portal_physical_acceptance.py` exercises the authenticated
  local Portal using an interactive, non-argv password and restores temporary
  settings/assets/current-image changes.
- `scripts/papercolor_myai_physical_acceptance.py` uses only the bounded
  secondary-USB commands `status`, `album-status`, `voice-tap` and `aigc-test`.
  It applies only after this diagnostic branch is merged, assigned a version
  strictly above beta11, rebuilt, signed and freshly accepted.

Neither tool prints credentials, pairing codes, transcripts or raw responses.
Their machine verdicts do not replace the checklist's human audio/RGB/panel
observations.
