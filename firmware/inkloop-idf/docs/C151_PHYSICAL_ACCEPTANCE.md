# M5 PaperColor C151 physical acceptance

Status: **PARTIAL PHYSICAL EVIDENCE — RELEASE STILL PENDING.** beta27 has a
retained inactive-app1 readback/boot and Recovery-path pass with app0 beta25
preserved. It does not have a Product pass: three valid divergent TF album
indexes remain behind an explicit operator choice. The previously reviewed
beta29 artifact is **revoked for physical staging after the complete audit**;
it must not be flashed. The next eligible candidate is beta30 or newer and must
have a fresh exact-commit acceptance result plus two byte-identical C151 builds.
Its commit, version, application SHA-256 and byte count remain deliberately
unbound until those new artifacts exist. No older physical observation
transfers to that candidate.

Current pre-candidate internal-flash custody: two 16 MiB reads are byte-identical
with SHA-256
`25d169e66cc334fe219de0220cca2920d0aae8c8747d33dde3af87bd9196f76d`.
They preserve beta25 app0, selected beta27 app1, otadata, NVS and LittleFS. The
device was left in ROM bootloader after the reads; no deliberate device or TF
mutation command was issued. This does not establish TF byte custody: the
existing firmware mounts TF writable through `esp_vfs_fat_sdspi_mount`, and no
offline whole-card image has yet been captured. Physical candidate staging is
therefore **BLOCKED** until that image and its hash are verified and a fresh
beta30-or-newer release acceptance is bound.

This is the release-blocking procedure for the currently attached,
user-authorized C151. The unit is currently in Download mode, but future runs
must re-enumerate and verify it rather than assume that state. Run a candidate
only after its exact shared-tree tests, clean build and fresh independent
acceptance pass. Do not modify MyAI/AaaS, deploy a server, publish a release,
erase a chip, resolve a TF candidate or format media merely because this
document exists.

## 1. Verdict and evidence contract

- `PASS`: every mandatory item has a retained artifact and meets its stated
  limit. A warning, inferred result or missing observation is not a pass.
- `FAIL`: any safety, data-integrity, auth, latency, audio, panel, recovery or
  rollback item fails. Stop rollout and preserve the device/logs.
- `BLOCKED`: the authorized device or its required full/NVS/LittleFS/TF backup,
  credential, test endpoint, power control or measurement tool is unavailable.
  Do not skip the item.
- Never put SSIDs, Wi-Fi/local passwords, pairing/device tokens, cookies,
  Authorization headers, signing private keys or raw MyAI response bodies in
  an evidence bundle. Keep an unshared original locally if diagnosis requires
  it; publish only a redacted copy plus its hash.
- One operator executes; a second reviewer verifies the artifacts and signs
  the final table. A PASS applies only to the recorded firmware SHA-256, board
  identity, configuration and service environment.

Use one directory per run. From the repository root:

```sh
export INKLOOP_C151_PORT=/dev/cu.usbmodem-REPLACE_ME
export INKLOOP_C151_DIAG_PORT=/dev/cu.usbmodem-SECONDARY-REPLACE_ME
export INKLOOP_EVIDENCE_DIR="$PWD/.butler/evidence/c151-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$INKLOOP_EVIDENCE_DIR"
git rev-parse HEAD >"$INKLOOP_EVIDENCE_DIR/git-head.txt"
git status --short >"$INKLOOP_EVIDENCE_DIR/git-status.txt"
shasum -a 256 firmware/inkloop-idf/build/inkloop_idf.bin \
  >"$INKLOOP_EVIDENCE_DIR/firmware-sha256.txt"
```

If the accepted build directory is not `build`, substitute its exact path and
record it. Do not use an old binary merely because that path exists.

For each section retain:

- UTC/CST start/end, operator and reviewer;
- C151 serial/hardware identifier, canonical MAC and board SKU;
- IDF version, sdkconfig hash, ELF/bin/partition hashes and build log;
- redacted serial log, browser screenshots or screen recording;
- `measurements.csv` with scenario, event timestamp, completion timestamp,
  latency, sample count, p50, p95, p99, maximum and verdict;
- photos/video for LEDs, panel and wake; current/power trace where required;
- before/after hashes or inventories for NVS/LittleFS/TF data;
- exact failure code and a tracked defect for every non-PASS result.

## 2. Preconditions and safe candidate flash

Historical beta29 digital evidence is not an eligible physical target after the
complete audit. The next candidate must be beta30 or newer, pass the complete
repository suite and lint on its exact commit, and produce two byte-identical
official ESP-IDF v6.0.2 C151 builds. A fresh independent result must prove the
same commit and artifacts. The operator must bind every physical result to that
exact committed source, version, binary SHA/size, acceptance file and physical
run. A beta27 Recovery pass is not Product evidence for the new candidate.

- [ ] the candidate's exact source commit, version, C151 binary hash, signed
  manifest hash and fresh-acceptance report all match; no unversioned branch
  build is used.
- [ ] `node --test tests/esp-idf-*.test.mjs` is PASS on the exact tree and its
  output is retained.
- [ ] official ESP-IDF v6.0.2 clean C151 and mock-SKU builds are PASS; archive
  `sdkconfig`, `.map`, `.elf`, `.bin`, partitions and hashes.
- [ ] the target is the currently attached, explicitly authorized Download-mode
  C151; record its MAC, flash size, board revision and TF-card identity.
- [ ] read and hash the full 16 MiB flash, NVS and LittleFS before any write.
  Power down, remove the TF card, capture and hash an offline whole-card byte
  image before any candidate write. Verify every output is nonempty and
  readable; if any required backup/read fails or the whole-card image is
  absent, mark `BLOCKED` and do not flash.
- [ ] review the partition table against the backup. The normal migration
  flash must not include `erase-flash`, an automatic NVS erase, LittleFS
  format or TF format.
- [ ] production OTA Kconfig remains empty for all non-OTA sections. For OTA
  sections, use only a reviewed stable HTTPS C151 channel and its reviewed raw
  Ed25519 public key; the private key remains external.
- [ ] provide a controlled 2.4 GHz AP, a public MyAI/Inkloop test identity,
  Chrome/Edge, USB serial, current meter and repeatable power interruption.
  Do not change MyAI server code or create a developer-key path.

After exporting ESP-IDF v6.0.2, capture the three flash backups before the
build/flash commands below. The offsets match `partitions.csv`; a mismatch in
the connected chip or accepted partition table is `BLOCKED`:

```sh
esptool.py --chip esp32s3 -p "$INKLOOP_C151_PORT" read_flash \
  0x000000 0x1000000 "$INKLOOP_EVIDENCE_DIR/full-flash-before.bin"
esptool.py --chip esp32s3 -p "$INKLOOP_C151_PORT" read_flash \
  0x009000 0x005000 "$INKLOOP_EVIDENCE_DIR/nvs-before.bin"
esptool.py --chip esp32s3 -p "$INKLOOP_C151_PORT" read_flash \
  0xc90000 0x360000 "$INKLOOP_EVIDENCE_DIR/littlefs-before.bin"
shasum -a 256 "$INKLOOP_EVIDENCE_DIR"/*-before.bin \
  >"$INKLOOP_EVIDENCE_DIR/preflash-sha256.txt"
```

The mandatory pre-flash TF backup must be an offline whole-card byte image from
an explicitly validated card device, captured after powering down and removing
the card. Never infer a TF device from an unresolved variable or broad disk
path. Use the inspect-then-confirm macOS whole-card tool documented in
[TF_ALBUM_RECOVERY_EXPORT.md](TF_ALBUM_RECOVERY_EXPORT.md), and require its
final `custody.json` to contain `complete: true` with two matching source-pass
hashes and the matching image hash. A firmware HTTP export cannot satisfy this
gate because current
`esp_vfs_fat_sdspi_mount` usage mounts FAT writable. For divergent album slots,
after the whole-card custody exists, the logical exporter in the same document
may additionally retain all
three candidate indexes and the referenced-asset union for selection. A
`manifest.json` with `complete: true` is logical export evidence only.

Build only after the preconditions pass and no gated watcher owns the device.
Use only the exact freshly accepted external build selected by the gate, not an
arbitrary rebuild, and archive it before staging:

```sh
cd firmware/inkloop-idf
IDF_PATH=/Users/zhuzhe/.espressif/frameworks/esp-idf-v6.0.2 ./tools/build.sh --clean
. /Users/zhuzhe/.espressif/frameworks/esp-idf-v6.0.2/export.sh
idf.py -p "$INKLOOP_C151_PORT" monitor
```

Do **not** run the generic `idf.py flash` target on an installed dual-slot
device. Its normal flash arguments write `app0 @ 0x10000` and the initial OTA
selector at `0xe000`; that can overwrite the retained rollback application and
reset the currently reviewed selector. `erase-flash` is also forbidden.

Candidate staging needs a fresh independent gate tied to the exact binary and
the just-read physical state. That gate must identify the running and inactive
slots, retain the running image as rollback, authorize only the inactive app
range, verify a full readback hash, and use either the product's signed
inactive-slot OTA path or one reviewed selector update. If the active slot,
selector sequence/state, partition table, internal backup hashes, offline TF
whole-card hash or target image hash differs from the gate, stop before the
first write. Factory flashing of a
verified blank board is a separate procedure and is not evidence for this
installed unit.

### 2.1 Installed-unit beta30+ inactive-app0 gate

For this one installed unit, use
`tools/c151_inactive_app0_gate.py`. The tool is pinned to the authorized MAC,
port, 16 MiB pre-candidate baseline, beta25 app0, selected beta27 app1 rollback,
partition table and current seq1/seq2 `VALID` otadata. It is deliberately not
pinned to a release: every release-sensitive phase requires the same explicit
beta30-or-newer commit, version, C151 SHA-256 and byte count, and `gate-app`
also requires a fresh acceptance file proving that exact tuple. The revoked
beta29 tuple is rejected before any authorization is emitted. Its `capture`
subcommand has a fixed read-only esptool allow-list. Writes are possible only
through the generated `execute-app`, `execute-selector` and `execute-rollback`
commands; never copy or improvise a raw esptool write. Do not run `capture`
until the device is powered down, the TF card is removed and its offline
whole-card image/custody JSON exists.

The `--tf-custody` input must be the unmodified `custody.json` emitted by
`capture_tf_whole_card_macos.mjs` next to its `tf-whole-card.img` and
`SHA256SUMS`. The app0 gate re-hashes the image and requires `complete: true`,
two matching full source passes, matching byte counts, unchanged diskutil
fingerprints, an explicit external physical whole disk, unmounted members and
the tool's no-source-write/no-automatic-unmount/no-implicit-elevation claims.
It reconstructs the normalized identity independently from every
before/pre-read/between/after diskutil info/member receipt, checks the initial
disk list/member inventory, recomputes each fingerprint from recursively
key-sorted canonical JSON, and binds every receipt by path, size and SHA-256; a
merely well-formed or self-consistent 64-hex fingerprint is not sufficient.
Device shutdown and physical card removal remain witnessed operator
preconditions; a logical album export or mounted FAT copy cannot satisfy them.

Run the read-only capture, then the offline first-stage gate:

```sh
export BETA30_COMMIT=REPLACE_WITH_40_LOWERCASE_HEX
export BETA30_VERSION=0.4.0-beta.30
export BETA30_SHA256=REPLACE_WITH_64_LOWERCASE_HEX
export BETA30_BYTES=REPLACE_WITH_DECIMAL_BYTE_COUNT

python3 firmware/inkloop-idf/tools/c151_inactive_app0_gate.py capture \
  --port /dev/cu.usbmodem21442201 \
  --output-dir /ABSOLUTE/NEW/c151-candidate-readonly-capture

python3 firmware/inkloop-idf/tools/c151_inactive_app0_gate.py gate-app \
  --capture-dir /ABSOLUTE/NEW/c151-candidate-readonly-capture \
  --candidate /ABSOLUTE/BETA30-C151/inkloop_idf.bin \
  --acceptance-result /ABSOLUTE/BETA30-FRESH-ACCEPTANCE/result.json \
  --baseline-custody /Users/zhuzhe/.local/share/inkloop/device-evidence/c151-20260824-pre-beta29-current/custody.json \
  --tf-custody /ABSOLUTE/TF-CUSTODY.json \
  --expected-commit "$BETA30_COMMIT" \
  --expected-version "$BETA30_VERSION" \
  --expected-candidate-sha256 "$BETA30_SHA256" \
  --expected-candidate-bytes "$BETA30_BYTES" \
  --output-dir /ABSOLUTE/NEW/c151-candidate-app0-gate
```

`gate-app` is fail-closed if any current byte or identity differs. Its first
plan authorizes exactly one mutation: the explicitly bound candidate bytes at
inactive `app0 @ 0x10000`. Its recorded affected length is the minimum 4 KiB
erase-aligned range containing the candidate; every byte after the exact image
within that range must read back as `0xff`. It emits no selector bytes and no raw
esptool write argv. Run only `execute_app_argv` from `app-stage-plan.json`. At
that execution boundary the tool reloads the complete authorization chain,
rejects a replaced, extended or symlinked staged candidate, snapshots the exact
bytes into an unlinked inherited `/dev/fd/N`, rechecks MAC/chip/security/flash
identity, and compares a fresh current 16 MiB read with the expected baseline.
Only then can it write app0. It automatically reads all 16 MiB back and compares
every byte with the unique after-image: only the exact candidate bytes and their
required 4 KiB erase-padding may differ. That continuous check covers the
bootloader/partition prefix, NVS, unchanged otadata, the app0 suffix, app1
rollback, LittleFS and the flash tail including coredump.

Only after those files exist may the second gate run:

```sh
python3 firmware/inkloop-idf/tools/c151_inactive_app0_gate.py authorize-selector \
  --app-authorization /ABSOLUTE/NEW/c151-candidate-app0-gate/app-stage-authorization.json \
  --full-flash-readback /ABSOLUTE/NEW/c151-candidate-app0-gate/app-execution/full-flash-after-app.bin \
  --expected-commit "$BETA30_COMMIT" \
  --expected-version "$BETA30_VERSION" \
  --expected-candidate-sha256 "$BETA30_SHA256" \
  --expected-candidate-bytes "$BETA30_BYTES" \
  --output-dir /ABSOLUTE/NEW/c151-candidate-selector-gate
```

That gate emits one 32-byte seq3/`NEW`/CRC-correct entry for `0xe000`. The new
image is a Product candidate and rollback is enabled, so `NEW` is mandatory: the
bootloader must advance it to `PENDING_VERIFY`, after which the committed
boot-health gate either confirms it or rolls back to beta27 app1. Reusing the
beta27 Recovery-only `VALID` selector would bypass that automatic rollback and
is forbidden. The selector must be exactly
`03000000ffffffffffffffffffffffffffffffffffffffff0000000011504aed`
(SHA-256
`9e10bbc4ceebe605fde303fe22ce077af5f7471fcc607e6ee43b63207bd63e58`). The
flash device necessarily erases the containing 4 KiB entry0 sector, whose
remaining bytes are required to be `0xff`; it does not touch entry1 at
`0xf000`. Entry1 remains seq2/`VALID` and therefore remains a rollback selector
if entry0 is lost during a power cut. The gate also emits the exact original
32-byte seq1 entry as the only reviewed rollback mutation. Run only the plan's
`execute_selector_argv`; it repeats the live identity and exact pre-write 16 MiB
checks, seals the exact 32-byte selector into an inherited FD, writes it, and
automatically verifies the final full image. Then run the recorded
`final_verification_argv`. The verifier byte-compares the whole flash against
the app-stage after-image plus only the reviewed seq3/`NEW` selector delta. The
controlled `execute_rollback_argv` applies the same protections to the exact
seq1 entry, and its verification compares the whole flash against the exact
post-app image. Thus both paths also prove the app0 suffix, flash tail/coredump,
prefix, NVS, app1 and LittleFS. All execute and verify commands repeat the four
release-binding arguments. If rollback is required, use only those two recorded
rollback commands; never improvise an otadata erase/write.

No phase authorizes bootloader, partition-table, NVS, app1, LittleFS, coredump
or TF writes. No phase authorizes generic `idf.py flash`, `erase-flash`, a raw
esptool write, a different port/MAC, a rebuilt candidate, or a selector write
before the app after-image passes.

Save the full monitor output from reset through stable normal or Recovery mode.
If the boot loops, reports an ambiguous audit without the expected Recovery
path, formats media, shows a terminal black panel or emits a secret, stop and
mark `FAIL`.

## 3. Boot, identity and scheduling baseline

- [ ] Cold boot reaches one stable terminal state without splash, transient
  “connecting” pages or repeated e-paper refresh. Valid saved Wi-Fi leaves the
  existing panel unchanged.
- [ ] Missing/invalid saved Wi-Fi eventually shows the Settings AP page once;
  no reboot loop occurs.
- [ ] serial, Portal, MyAI and Inkloop report the same canonical MAC, SKU and
  six-digit identity where applicable.
- [ ] serial/Portal telemetry shows every required lane started, advancing and
  within its stack/queue bounds; watchdog, heap and PSRAM stay healthy for a
  30-minute idle baseline.
- [ ] no secret appears in serial, Portal state, Recovery diagnostics, crash
  output or the redacted evidence bundle.

## 4. Input latency, priority and LEDs

Collect at least 200 presses per physical key in each scenario: idle; MyAI TLS
or WSS work; TF write; 1.5 MiB Portal preview/upload; PNG decode/conversion; and
full ED2208 refresh. Correlate the GPIO/ISR event timestamp with Control
admission and first visible/audible acknowledgement.

- [ ] all three keys are detected once with correct gesture/debounce behavior;
  no lost, duplicate or stale-generation action occurs.
- [ ] button event p99 is ≤20 ms in every scenario.
- [ ] eligible voice-start acknowledgement is ≤100 ms.
- [ ] opening/using Portal increases button or voice p99 by ≤10 ms versus the
  paired no-Portal run; closing the last session returns Portal to idle work.
- [ ] queue-full, expiry and cancellation are typed/observable and do not block
  the ISR, Voice or Control lanes.
- [ ] left LED: listening green, thinking state as documented by the runtime,
  speaking state as documented, blocked/error red. Right LED: generation,
  download and panel-write states are distinct and terminal idle is off.
- [ ] left/right physical order is correct. Changing maximum brightness runs
  the complete bounded LED preview and persists across reboot; no standalone
  test button is required.

## 5. Wi-Fi and authenticated Portal

- [ ] with valid saved credentials, three cold resets and three deep-sleep
  wakes reconnect without asking for Wi-Fi again or leaving the Settings AP
  permanently enabled.
- [ ] with deliberately invalid credentials, the Settings AP appears and the
  screen shows enough non-secret access information. Configure new credentials
  and verify they persist after reset.
- [ ] `http://inkloop.local/` and the actual IP open the same Portal. Login,
  session reuse/expiry, CSRF, host/origin rejection and logout behave exactly;
  no ordinary local access path requires an unexplained strong generated
  password.
- [ ] tabs group device, album, MyAI and settings functions coherently. Saved
  and immediate settings have explicit behavior; free/total storage is correct.
- [ ] `/api/state`, album page and preview remain available during a 30-minute
  active-browser run. Record error rate and peak latency; any repeatable 409,
  owner starvation, reset or “album unavailable” without a true maintenance
  gate is `FAIL`.
- [ ] volume adjustment plays the local preview; brightness adjustment runs
  the LED preview. Voice-assistance off disables optional prompts, not safety
  errors.
- [ ] upload a valid representative PNG and reject corrupt/oversized input
  cleanly. Preview, delete, render-strategy change and “上屏” work after reload
  without stretching the thumbnail to 600 px CSS height.

After the exact candidate boots Product and its first-cycle evaluation
succeeds, the committed Portal harness may be run from the isolated worktree.
It prompts for the local password through
`getpass`; never put the password in argv, environment variables or evidence:

```sh
python3 scripts/papercolor_portal_physical_acceptance.py \
  --base-url http://inkloop.local --read-only
python3 scripts/papercolor_portal_physical_acceptance.py \
  --base-url http://inkloop.local
```

The full form temporarily changes volume/brightness/render strategy, uploads
and displays a generated diagnostic PNG, then must restore the previous
settings/current image and delete the temporary asset. A cleanup failure is a
failed acceptance, not a warning.

## 6. Storage, upgrade data and transactions

Use the verified backups to prepare identifiable legacy tasks, album
assets/current image, chat, settings,
Wi-Fi, MyAI credential journal and one known ambiguous display transaction.
Hash raw backups before the test.

- [ ] read-only boot audit inventories every protected namespace/file before a
  normal writer starts. Known clean data starts normally; corruption or the
  ambiguous transaction enters Recovery without changing bytes.
- [ ] internal LittleFS and inserted TF mount on their expected partitions;
  backend selection is fixed consistently for album/chat/task owners. Missing
  TF falls back without silently rewriting the preference.
- [ ] Portal capacity equals an independent filesystem observation within
  allocation-unit rounding.
- [ ] album upload, AIGC commit, Inkloop frame, task replacement/delete, chat
  append and settings/token journal each survive reset at every defined
  transaction stage. The result is exactly old or new, never mixed/missing.
- [ ] confirmed TF format drains owners, formats only the removable target,
  remounts/rebuilds caches and resumes. Cancel/no-confirmation does nothing.
  Internal NVS/LittleFS hashes remain unchanged.
- [ ] sudden TF removal, full media and corrupt records fail closed without
  blocking input/voice or automatically erasing/formatting anything.

## 7. MyAI pairing, voice, chat and AIGC

Use only public client APIs and `app_id=inkloop`. Preserve network timestamps
or redacted client diagnostics; do not change MyAI/AaaS server code.

- [ ] with no credential, obtain one six-digit MyAI code, display it and bind
  it. The client stops pairing polling immediately after receiving the latest
  `device_token`; it never authenticates with `pairing_token`.
- [ ] `/api/v1/devices/check` is authorized for that token/device/MAC. Reboot
  retains it. A deliberate revoke preserves original 401/402 diagnostics and
  leads to an understandable rebind flow; fresh rebind succeeds.
- [ ] bounded gateway probes choose a reachable public peer, reuse its valid
  lease and fail over within one total deadline. DNS answer and connected TLS
  peer are public; redirect/private-peer/certificate failures send no token.
- [ ] one short top-button press starts listening; the defined second action
  stops/cancels. Record acknowledgement, capture start, final ASR, first TTS
  and completion timestamps.
- [ ] final ASR → assistant → TTS completes over public WSS. Local chat stores
  only final non-empty ASR/assistant/tool/AIGC text; it contains no audio,
  partial ASR or `blank_audio` and survives reboot/rotation.
- [ ] a 60-second TTS stream has zero input-ring overrun, playback underrun and
  audible corruption. Cancel completes ≤100 ms and flushes only its generation.
- [ ] the 30-second lease heartbeat is prepared by Network ownership and runs
  as bounded low-priority Portal-lane HTTP while capture/WSS/TTS remains
  responsive; stale session/gateway completions are rejected.
- [ ] credential-free WSS Ping receives its exact Pong within the bounded
  timeout; a missing/mismatched Pong closes with reconnect semantics without
  starving capture or playback.
- [ ] Portal manual image text and a voice image request use the saved agent
  and AIGC prompt templates. No AIGC status request occurs while idle; active
  polls are spaced 5 seconds until terminal state.
- [ ] successful AIGC output is streamed, durably committed, visible in local
  chat/album/preview and can be written to the panel. Generation/download/write
  LED states and failures are distinguishable. Restart during each stage
  leaves a coherent album.
- [ ] local voice tools report space, delete one image, clear album, change
  volume, change agent/AIGC prompts and request TF format. Destructive actions
  require explicit confirmation and report exact local outcomes.

Only a merged, strictly versioned and freshly accepted beta12-or-newer image
contains the bounded secondary-USB diagnostic protocol. When public MyAI Voice
and T2I gateways are available, run its exact four-command harness against the
explicit secondary serial port:

```sh
python3 scripts/papercolor_myai_physical_acceptance.py \
  --port "$INKLOOP_C151_DIAG_PORT"
```

The harness proves only the emitted machine events and requires a human check
of audible quality, correct left/right RGB roles and the final lighthouse
image. `INKLOOP_C151_DIAG_PORT` is the secondary USB Serial/JTAG data interface,
not the primary boot/monitor console in `INKLOOP_C151_PORT`; both must be
resolved explicitly after enumeration. The harness never prints transcripts,
codes, credentials or raw serial lines.

## 8. Inkloop tasks, album navigation and display

- [ ] bind the same C151 on Inkloop using the reused six-digit identity. Device
  metadata, 400×600 portrait canvas and render capabilities match the board.
- [ ] steady sync occurs every 30 seconds, not 15. Wake/dirty work triggers one
  bounded coalesced immediate sync. Offline time causes no busy loop; reconnect
  catches up without duplicate delivery.
- [ ] create, replace and delete a scheduled image task. The frame becomes a
  durable previewable album asset; acknowledgement matches the exact revision
  only after successful panel completion. A failed/missing frame is not acked.
- [ ] upload, Inkloop and AIGC assets coexist and persist. Logical duplicates
  can be independently deleted or assigned render strategies without deleting
  shared content too early.
- [ ] official full refresh is the default. Compare official, classic,
  reflectance and solid on the same photo, saturated artwork, text/table and
  large color blocks. Record decode, six-color conversion and physical ED2208
  refresh separately, plus photos and operator quality notes.
- [ ] portrait output is exactly 400×600 with the physical bottom edge down;
  no content leaves the canvas and the terminal frame is not black.
- [ ] with at least eight images, rapid previous/next presses announce every
  candidate. Only the selection stable for ≥1 second refreshes. The “开始刷新第
  n 张” prompt begins with the refresh; returning to the current image causes
  no refresh.
- [ ] while the panel is writing, navigation is safely deferred/rejected with
  the configured prompt. The right LED indicates the whole physical write.
- [ ] ordinary reboot and button wake preserve the existing panel; transient
  Wi-Fi/MyAI/AIGC progress never replaces a stable image.

## 9. Deep sleep and wake

- [ ] after two minutes with no blocker, the device enters deep sleep. Record
  sleep current after radios/peripherals settle and compare it with the board
  power budget; unexplained periodic wake is `FAIL`.
- [ ] Voice, previous, next and RTC/timer each wake the device; combined/held
  inputs do not repeat the wake action after input is rearmed.
- [ ] a button wake does not refresh or enter a system page. It only shows the
  allowed LED and optional “已恢复” prompt, reconnects Wi-Fi and schedules
  metadata/task sync. The first post-wake key action is consumed as wake, not a
  voice/page action.
- [ ] timer wake is silent and panel-preserving; pending scheduled Inkloop work
  is fetched/applied according to policy, then idle sleep resumes.
- [ ] test 100 sleep/wake cycles and three abrupt power cycles. Panel content,
  settings, credentials, album current index and task state remain coherent.

## 10. Recovery mode

Use restorable copies derived from the verified backups for clean, corrupt and
ambiguous state. Do not corrupt the only original. Recovery must remain
local/read-only until an authenticated exact action is confirmed.

- [ ] trigger boot-audit, migration, storage-integrity, OTA-health and
  post-Product-failure Recovery reasons. The diagnostic reason/phase/outcome
  and counts are exact, bounded and secret-free.
- [ ] only Recovery owns Wi-Fi/HTTP after a successful Product shutdown. Port
  8080 coexists with provisioning where applicable; the displayed actual IP
  and access code authenticate repeatedly according to the documented policy.
- [ ] there is no outbound MyAI/Inkloop/OTA work and no normal storage/display
  writer in Recovery.
- [ ] current/next/previous actions require explicit choice and confirmation,
  reject stale request identity and mutate only the typed transaction target.
  After success, a fresh exact audit is clean before the response/reboot.
- [ ] inject every practical HTTP/Wi-Fi teardown fault. A failed stop retains
  the same owner and can retry; only a real successful shutdown reports
  success. If a native driver fault cannot be injected physically, record it
  as `BLOCKED`, not PASS from the host fault matrix.
- [ ] reset/power loss before action, during action, after audit and before
  reboot always returns to a coherent old/new/Recovery state without erase.

## 11. Signed OTA, boot health and rollback

Run only after the non-OTA sections pass. Configure an ephemeral reviewed
external signing key, a stable same-origin C151 manifest URL and a strictly
newer SemVer build. Archive public artifacts/receipts; never copy the private
key into firmware, repository or evidence.

- [ ] package, sign, verify and promote an immutable version directory. The
  stable `<sku>/manifest.json` changes atomically only after the version is
  complete. Equal/downgrade, corrupt receipt/image/signature, symlink,
  traversal, ambiguous tree and concurrent promotion publish nothing.
- [ ] authenticated Portal requires explicit confirmation and immediately
  tells the user the device will go offline for verified installation. It does
  not promise unreachable live download/staging progress.
- [ ] manifest/image HTTPS requires public DNS and connected peer, CA/hostname,
  HTTP 200, exact length, same origin, board, strict newer version, image size,
  SHA-256 and Ed25519. Exercise every rejection, redirect, private peer,
  timeout and truncated body; current slot/data remain selected and intact.
- [ ] valid acquisition writes only the inactive slot, selects it and reboots
  after both Product shutdown phases. Cut power during manifest, download,
  erase/write, finish, boot selection and reboot handoff; each boot resolves to
  old valid image, pending new image or Recovery with no ambiguous success.
- [ ] new pending image starts boot health before board/storage/Product. Eight
  mandatory local lanes remain fresh for the 30-second soak and the image is
  confirmed before the 120-second deadline. Wi-Fi/MyAI/cloud success is not
  accepted as boot-health evidence.
- [ ] inject fatal reset, missing/stale lane, board/storage/Product failure and
  confirmation failure. The bootloader rolls back to the previous valid slot;
  panel and persistent data remain coherent.
- [ ] after reconnect, authenticated Portal exposes one bounded credential-free
  selected/acquisition-failed/confirmed/rollback outcome and expires it by the
  documented boot-age rule. Torn/corrupt/stale journal records fail closed and
  never expose URL, key, manifest, signature, token or response body.
- [ ] complete one forward upgrade and one controlled rollback drill from the
  stable channel. Record running/boot partitions, versions, image hashes,
  reboot reason, outcome and preserved data hashes at every boot.

## 12. Sign-off

| Gate | Result | Evidence path / defect |
|---|---|---|
| Digital final gate and clean builds | PENDING (beta29 target revoked) | Produce beta30-or-newer exact-commit reproducible C151/mock artifacts and pass a fresh independent gate before any write. |
| First flash, boot and identity | PENDING | |
| Input latency and LEDs | PENDING | |
| Wi-Fi and Portal | PENDING | |
| Storage and migration safety | PENDING | |
| MyAI voice/chat/AIGC | PENDING | |
| Inkloop/album/display | PENDING | |
| Deep sleep/wake | PENDING | |
| Recovery | PENDING | |
| Signed OTA/rollback | PENDING | |

Final verdict: **PENDING**

Operator/date: ____________________  Reviewer/date: ____________________

An existing-device flash, OTA channel enablement or public release is allowed
only after the final verdict is changed to `PASS` with both signatures and all
referenced artifacts present.
