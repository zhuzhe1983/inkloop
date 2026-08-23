# M5 PaperColor C151 physical acceptance

Status: **PARTIAL PHYSICAL EVIDENCE — RELEASE STILL PENDING.** beta27 has a
retained inactive-app1 readback/boot and Recovery-path pass with app0 beta25
preserved. It does not have a Product pass: three valid divergent TF album
indexes remain behind an explicit operator choice. `0.4.0-beta.28` is the
current digital candidate (403/403 tests, lint zero errors, clean C151/mock
links, C151 SHA-256
`cd9498e006693b7b3ea61c143dbd230f61873f9f1a0d41a5dff020327f14091a`)
and has not been flashed. No older physical observation transfers to beta28.

This is the release-blocking procedure for the currently attached,
user-authorized C151. The device normally deep-sleeps and is not assumed to be
in Download mode. Run a candidate only after its exact shared-tree tests, clean
build and fresh independent acceptance pass. Do not interfere with the active
beta10 endurance/gated-beta11 watcher, modify MyAI/AaaS, deploy a server,
publish a release, erase a chip, resolve a TF candidate or format media merely because this document
exists.

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

Current digital evidence: beta28 passes the complete repository suite 403/403,
lint with zero errors, and clean official ESP-IDF v6.0.2 C151/mock builds. The
operator must still bind every result to the exact committed source, binary,
fresh independent gate and physical run selected for flashing. A beta27
Recovery pass is not beta28 Product evidence.

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
  Independently back up and hash the TF card. Verify every output is nonempty
  and readable; if any required backup/read fails, mark `BLOCKED` and do not
  flash.
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

The TF backup must use a read-only, explicitly validated card-device or mount
path and include a byte image plus file inventory/hashes. Never infer a TF
device from an unresolved variable or broad disk path.

Build and flash only after the preconditions pass and no gated watcher owns the
device. For beta11 use its retained accepted external build, not an arbitrary
rebuild. For a later candidate, archive the exact clean build before flashing:

```sh
cd firmware/inkloop-idf
IDF_PATH=/Users/zhuzhe/.espressif/frameworks/esp-idf-v6.0.2 ./tools/build.sh --clean
. /Users/zhuzhe/.espressif/frameworks/esp-idf-v6.0.2/export.sh
idf.py -p "$INKLOOP_C151_PORT" flash
idf.py -p "$INKLOOP_C151_PORT" monitor
```

`idf.py flash` must show the exact accepted images/offsets. Do not substitute
`erase-flash`. Save the full monitor output from reset through stable normal or
Recovery mode. If the boot loops, reports an ambiguous audit without the
expected Recovery path, formats media, shows a terminal black panel or emits a
secret, stop and mark `FAIL`.

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

After beta11 first-cycle evaluation succeeds, the committed Portal harness may
be run from the isolated worktree. It prompts for the local password through
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
- [ ] 30-second heartbeat is deferred throughout capture/active TTS and sent
  after audio becomes idle without losing the session.
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
| Digital final gate and clean builds | PASS (WS43/44/46; shared suite 261/261) | Bind exact flash artifact/hash before device write. |
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
