# Inkloop ESP-IDF firmware

This is the new reusable firmware root. It deliberately does not depend on
Arduino, M5Unified, `HTTPClient`, `WebServer`, `WiFiManager`, `Preferences`, or
Arduino `String`.

Status: the native Product graph now composes the C151 board, bounded dual-core
runtime, Wi-Fi/Portal, storage, display, Inkloop, MyAI voice/AIGC, settings,
power, Recovery and signed-OTA owners without an Arduino compatibility layer.
Host fault matrices and official ESP-IDF v6.0.2 C151/mock links prove the
digital composition. The current candidate worktree is `0.4.0-beta.30`; the
complete repository suite passes 481/481 and lint has zero errors (19
warnings). The exact beta28 independent gate failed on TF export, flash
custody, build reproducibility, failed-wake admission and an undeclared host
dependency;
beta29 contains remediations for those findings. Development defaults now
remove time/path inputs, and new candidate hashes must come from the two-build
check below. A clean exact-commit reproducibility receipt and fresh independent
beta30-or-newer gate are still required. Beta29 was never flashed or promoted
and is revoked; beta30 has not been flashed or promoted. The attached device
currently has only a beta27 Recovery-path physical pass;
Product Voice/AIGC/album/display evidence remains behind an explicit,
operator-gated TF transaction choice.

Do not treat this tree as release-ready until the exact candidate's all-tree
gate, fresh independent acceptance and the authorized attached-C151 run in
[the physical acceptance checklist](docs/C151_PHYSICAL_ACCEPTANCE.md) pass.
The per-capability split between implemented digital behavior and pending
physical/live evidence is [the migration matrix](docs/MIGRATION_MATRIX.md).
The required boundary and gates for another hardware SKU are in
[the board adapter porting contract](docs/BOARD_ADAPTER_PORTING.md).
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

### Verify byte-reproducible C151 and mock builds

`sdkconfig.defaults` enables ESP-IDF's reproducible-build mode and explicitly
disables application and bootloader compile-time dates. The verifier performs
two fresh C151 builds and two fresh mock-minimal builds in four isolated build
directories, checks every generated sdkconfig/cache, and rejects either target
when its two application sizes or SHA-256 digests differ.

Use a new repository-external evidence path and the pinned ESP-IDF revision:

```sh
proof_parent="$(mktemp -d /tmp/inkloop-repro.XXXXXX)"
IDF_PATH=/absolute/path/to/esp-idf-v6.0.2 \
  ./tools/verify_reproducible_builds.sh "$proof_parent/evidence"
```

The output root must not exist before invocation. After the normal build
transcript, a successful run prints the stable C151/mock application sizes and
hashes plus the retained evidence root. This is a host build proof; it neither
flashes a device nor upgrades digital evidence to physical acceptance.

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

Recovery mode also supports an authenticated, bounded logical export of all
three divergent TF album indexes and the union of their referenced assets. See
[`docs/TF_ALBUM_RECOVERY_EXPORT.md`](docs/TF_ALBUM_RECOVERY_EXPORT.md). A
complete export never selects a slot, but it is not pre-flash physical custody:
the current SDSPI/FAT mount is writable. An offline whole-card image is
mandatory before any candidate device write.
