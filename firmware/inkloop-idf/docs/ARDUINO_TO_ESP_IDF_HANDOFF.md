# Arduino → ESP-IDF migration handoff

Date: 2026-08-23

Target: M5 PaperColor C151 first, reusable for later Inkloop hardware

MyAI role: third-party client only; **no MyAI/AaaS server modification**

## 1. Why this migration is now required

The current firmware already runs on Arduino-ESP32 over ESP-IDF/FreeRTOS, but
its product architecture is still a cooperative Arduino loop. Moving selected
calls to another FreeRTOS task improved GPIO capture without eliminating the
root problem: callers still synchronously wait for DNS, TLS, HTTP, filesystem,
WebSocket and panel operations while product state is held on the stack.

Physical evidence from the connected C151:

- button GPIO capture on Core 1 / priority 4 was immediate;
- MyAI gateway/session selection performed multiple sequential TLS operations
  and delayed an interactive request by roughly 45 seconds;
- a long MyAI TTS burst filled the 384 KiB audio queue, disconnected the
  gateway and produced slow/distorted playback;
- WebUI preview and state handlers, album commits, SD access and display writes
  all originated from the same cooperative state owner;
- boot currently reports an ambiguous display transaction
  (`backend=sd`, stage `1`, asset prefix `eeb0617236f4`) that must not be
  silently resolved;
- the live device otherwise mounted LittleFS and SD, connected to
  `192.168.199.156`, loaded a bound MyAI credential, and preserved NVS.

Adding more Arduino callbacks, delays or larger buffers would conceal rather
than solve these ownership and scheduling failures.

## 2. Current device safety state

Current update (2026-08-24): source remediation is now `0.4.0-beta.29`. The
worktree passes 424/424 repository tests and lint with zero errors/19 warnings.
The exact beta28 independent gate failed on five findings: no read-only TF
asset export, unsafe generic dual-slot flash guidance, non-reproducible build
identity, fail-open admission after wake rollback failure, and an undeclared
ignored ArduinoJson host dependency. Beta29 remediates all five, adds bounded
low-priority MyAI lease heartbeats plus WSS Pong supervision, and adds remote
Recovery logical export. That HTTP path uses the current writable SDSPI/FAT
mount and cannot replace mandatory pre-flash offline whole-card custody.
Exact-commit reproducible C151/mock receipts and a
fresh independent beta29 gate remain pending; beta29 has not been flashed,
signed or promoted. The authorized C151 retains beta27 in app1 and beta25 in
app0 because three valid divergent TF album indexes still require an explicit
operator choice.

Historical retained evidence: the authorized C151 previously ran the native
ESP-IDF `0.4.0-beta.11` image from
commit `57ec42b`. It was flashed through the standard four-image boundaries and
each written range was read back and verified. Two matching 16 MiB pre-flash
backups were retained with restricted permissions, and NVS, LittleFS and TF
were not written, erased or formatted. Physical evidence covers native boot,
saved Wi-Fi, Portal health and session/Host boundaries, compatible protected
storage, roughly two-minute deep sleep and a timer wake that preserves the
panel before sleeping again. The fail-closed first-cycle evaluator passed with
no panic, watchdog, unexpected reset or credential leak. This remains an
engineering test image, not a release, and does not prove the remaining
button/audio/LED/display/Portal/MyAI/OTA rows.

The beta11 C151 application SHA-256 is
`67d71ebf3d7e1982d413e671c9939c96ed1f86fdb9b68c1ffd3e201ac411b89b`;
the signed manifest SHA-256 is
`ff02c70072cca92d382bb7dcf59e3d8f64b26507cb94d4b408ec34355285e188`.
Its independent digital candidate gate and guarded physical first-cycle gate
both passed. The 100-cycle beta10 baseline completed 100/100 before beta11 was
allowed to replace it.

Historical development note: main later included the former
`codex/esp-idf-serial-diagnostics` work through
`6f06d01`: bounded secret-free diagnostics, authenticated Portal acceptance,
the reusable board-porting contract and fail-closed descriptor validation. Its
pre-merge complete ESP-IDF host suite was 271/271 PASS and both C151 and mock
SKU linked. Source is now versioned `0.4.0-beta.17`. The beta16 physical run
proved that a manual AIGC command could report `QUEUED`, then expire behind a
synchronous Network-lane gateway call because it inherited a one-second queue
deadline. Beta17 makes that ticket-bound admission durable until the Network
owner consumes or explicitly cancels it, retains the atomic queued-to-handoff
sleep blocker, and emits a terminal diagnostic when a test admission fails. It
must receive a fresh clean build, signature and independent candidate gate
before any flash or OTA.

## 3. Target ownership model

Every mutable subsystem has exactly one owner. Cross-owner work uses bounded
queues, immutable IDs, deadlines, cancellation and generation numbers.

| Priority | Task | Core | Sole ownership |
|---|---|---:|---|
| 22 | input | 1 | GPIO ISR/debounce and gesture timestamps |
| 21 | control | 1 | device/product state machine, button semantics and arbitration |
| 20 | voice | 1 | capture/playback state, VAD, TTS pacing, interactive intent |
| 8 | LED | 1 | two-pixel desired-state animation only |
| 7 | storage | 0 | NVS, LittleFS, SD, album/index/task transactions |
| 6 | display | 0 | decode/quantize/panel refresh; sole panel writer |
| 5 | network | 0 | Inkloop/MyAI HTTP, TLS, WebSocket and leases |
| 3 | Portal | 0 | ESP HTTP server; sleeps blocked on sockets when unused |

ESP-IDF system/Wi-Fi tasks retain their configured priorities. Application
tasks must not raise themselves above system tasks on Core 0.

### Required priority behavior

- A button event is timestamped even during TLS, storage or panel work.
- A voice gesture can cancel/defer lower-priority work; it never waits behind
  Portal polling or LED animation.
- LED code receives desired states and advances by timer/event; no delay,
  network, storage or diagnostic loop is permitted in the LED task.
- WebUI does no periodic server polling when nobody is connected. ESP
  `httpd` blocks on sockets. Browser state refresh should use one SSE/WebSocket
  subscription while visible, with a bounded fallback poll only when needed.

## 4. Service cadence

- Inkloop task sync: 30 seconds, with immediate event-driven sync after a user
  mutation or wake; never every 15 seconds.
- AIGC status: no timer while idle. After a generation is accepted, poll every
  5 seconds until a terminal state, then stop.
- MyAI voice heartbeat: retain 30 seconds because that is the public client
  lease contract. Send only from the network owner and defer during capture or
  active TTS; do not guess a longer interval without a server-provided lease.
- Portal: no artificial high-frequency device loop. HTTP server task sleeps
  until a socket arrives; an authenticated active session may temporarily
  receive richer state updates.

## 5. MyAI client port (no server changes)

Reusable protocol/business code lives in the current
`firmware/m5-papercolor/lib/InkloopMyAi` package, but only files without Arduino
types or concrete transports should be copied. Preserve:

- exact `app_id=inkloop`;
- six-digit device identity and canonical public API field names;
- durable device token, active/inactive/recovery states and fail-closed NVS;
- 30-second lease heartbeat;
- `device_token` authentication (never pairing token);
- immediate stop of pairing polling after a device token is accepted;
- bounded status/error bodies and original 401/402 diagnostics;
- `device_token` only on authenticated Center requests;
- short-lived `probe_token` only on bounded candidate `HEAD` probes, with
  `X-Device-ID`, `X-Device-MAC` and adapter-owned `X-Gateway-ID`; the ESP-IDF
  client intentionally probes one candidate at a time under one fair global
  deadline because simultaneous lwIP/TLS state machines were unstable under
  concurrent Voice/AIGC load;
- selected `gateway_token` on Gateway start, business HTTP, Voice WebSocket and
  AIGC, using `Authorization`, `X-Gateway-Session-Token`,
  `X-Gateway-Session-ID` and `X-Gateway-ID` together;
- heartbeat and disconnect remain Center calls authenticated by `device_token`;
- gateway lease/session selection, public DNS/connected-peer validation,
  certificate validation for TLS endpoints and redirect rejection.

Replace adapters with native implementations:

- `esp_http_client` for HTTP with event-driven completion and cancellation;
- `esp_websocket_client` (or a small native WebSocket component) owned only by
  the network task;
- `esp_crt_bundle`/mbedTLS verification and explicit public-address policy;
- NVS component with dual-slot records and CRC/generation;
- I2S standard driver with DMA rings for microphone and speaker.

Do not call the MyAI client recursively from a wait-pump. Network results are
messages delivered to voice/control. Gateway probes need one bounded total
deadline; the ESP-IDF implementation is deliberately serial and divides the
remaining deadline fairly, so it never degenerates into N sequential
8-second waits. Cache a valid selected gateway for its lease lifetime.

## 6. TTS/audio design

The old implementation let WebSocket ingress outrun the speaker until a large
PSRAM queue filled. The IDF implementation must use true backpressure:

1. network task writes PCM into a bounded DMA-facing ring;
2. speaker/I2S task drains at the hardware sample rate;
3. high/low watermarks pause and resume WebSocket reads/TCP receive credit;
4. one answer has an explicit duration/byte limit;
5. cancel closes the active generation and flushes only its buffers;
6. buffer depth, underruns, overruns and first-audio latency are observable;
7. input/voice control queues never share the PCM data queue.

PaperColor C151 is a fixed physical-output exception: ES8311 TX runs at
44.1 kHz stereo. Local/MyAI PCM stays in its declared 8--48 kHz source format
and passes through the allocation-free streaming resampler. TX is preloaded for
60 ms before enable, retains roughly 93 ms of DMA lead, and waits for the tail
to drain before disabling the codec. This avoids the earlier 16 kHz clock
mismatch and 10 ms zero-fill gaps heard in the local “已恢复” prompt.

Changing the volume in WebUI sends a short local preview command to the voice
task; it does not run in an HTTP handler.

## 7. Inkloop, album and AIGC data flow

- Network task downloads into a bounded buffer pool or stream pipe.
- Storage task validates PNG structure/hash/dimensions and atomically commits
  `.part → asset → index`; it alone mutates album metadata.
- AIGC success is not reported until the storage task returns the durable asset
  ID. The asset then appears in the same album as uploads and Inkloop pushes.
- Display task reads an immutable committed asset and reports start/progress/
  completion. No metadata is committed from the display task.
- Buttons change a desired album cursor immediately. A one-second settle timer
  emits one display request; returning to the current image emits none.
- Preview streaming opens a read lease from storage and streams through the
  Portal task without blocking input/voice/control.
- MyAI chat/debug records are append-only bounded JSONL/TXT on the selected
  persistent backend, rotated by size, and readable from the MyAI tab. The
  device log is the only history source: the Portal never fetches chat history
  from MyAI. Persist final non-empty ASR text, final assistant text and typed
  tool/AIGC states; discard partial ASR, `blank_audio` markers and audio bytes.
- Native AIGC output must use `EspAigcOutputTransport`: public-DNS and connected
  TLS-peer checks happen before any credential-bearing request body is sent;
  HTTP framing is stripped by `esp_http_client`, while JSON/Base64 is decoded
  incrementally into a bounded storage-owner sink. Never buffer the complete
  response or decoded image on the network owner.

## 8. Display and e-paper policy

- Only stable terminal screens are written. Skip splash, “connecting Wi-Fi”,
  polling and other transient states.
- Wake by any physical key preserves the current panel. Acknowledge only with
  LED and optional local audio.
- The display task is the sole panel writer and never owns product state.
- Keep the official full refresh as the default strategy. Additional six-color
  dither/solid strategies are pure render adapters selected by per-asset
  metadata.
- Track conversion time separately from physical ED2208 refresh time.
- Preserve exact 400×600 portrait semantics; layout coordinates always derive
  from the active board descriptor.

## 9. Board abstraction required for future hardware

Each `boards/<sku>` component implements the same interfaces:

- immutable display metadata: width, height, palette, orientation;
- PMIC/LDO and safe power sequencing;
- GPIO input and wake sources;
- panel submit/complete/cancel capability;
- microphone and speaker/I2S formats;
- LED count/order/driver;
- internal flash/NVS and removable-storage capabilities;
- battery, RTC/deep sleep and wake reason;
- immutable hardware identity and canonical network MAC.

No board component may contain Inkloop API, MyAI protocol, Portal HTML, album
policy or scheduling logic.

## 10. Persistence and painless upgrade

The partition table intentionally matches the current 16 MiB layout. The first
IDF build must mount existing NVS/LittleFS/SD read-only, classify every record,
and emit a migration plan before writing anything.

Required rules:

- never erase or format automatically;
- preserve Wi-Fi credentials, device identity/token, Portal settings, tasks,
  album, current image, chat history and display journal;
- use schema version + CRC + generation + dual slots;
- migrate one namespace at a time with rollback marker;
- do not clear an ambiguous display transaction automatically;
- a failed migration boots the old OTA slot or enters a read-only recovery
  mode with serial diagnostics.

Current digital state: the composition root runs the protected NVS/file audit
before normal writers; compatibility probes, marker/executor/recovery planner,
legacy display/file recovery and exact current/next/previous Recovery actions
have host fault matrices. Automatic boot deliberately does not guess an
ambiguous legacy transaction or format media. A physical legacy-data run,
power-cut matrix and rollback observation are still mandatory before upgrading
the existing unit.

## 11. Migration sequence

| Stage | Current status | Remaining gate |
|---|---|---|
| Pinned ESP-IDF project and compatible partitions | **Digitally implemented.** `.idf-version`, `tools/build.sh`, dual OTA slots and the compatible 16 MiB layout are tested by `esp-idf-scaffold.test.mjs`. | Re-run clean C151 and mock-SKU builds after final integration. |
| Bounded queues, owner lifecycle and telemetry | **Digitally implemented.** `inkloop_runtime` and Product lifecycle/fault tests cover the graph. | Measure latency, stack/heap/PSRAM, watchdog and overload on C151. |
| Native C151 hardware adapters | **Digitally implemented and target-linked.** PM1, RGB, buttons, I2S, SD and ED2208 contain no Arduino dependency. | Run electrical, audio, LED, key, storage and panel checks. |
| Persistence and upgrade safety | **Digitally implemented with an explicit Recovery gate.** read-only boot audit, compatibility, fault-tested transaction recovery, typed Recovery choices and authenticated three-index/asset logical export are composed. | Before any candidate write, capture an offline whole-card TF image; the writable SDSPI/FAT-mounted HTTP exporter is only additional logical evidence. Then run real legacy-media and every power-cut/rollback case; no automatic choice. |
| Inkloop task/album delivery | **Digitally implemented.** binding, 30 s/event sync, tasks, frame storage, display and correlated ack are Product-owned. | Run public Inkloop schedule/delete/offline/reconnect scenarios. |
| MyAI voice/AIGC | **Digitally implemented.** public pairing/token/gateway/WSS/audio/AIGC flow, local final-text history, low-priority 30 s lease heartbeat and exact WSS Ping/Pong timeout are composed. | Run public MyAI pairing, voice, 60 s TTS, heartbeat/Pong failure, cancellation and AIGC end to end. |
| Portal/settings/local tools | **Digitally implemented.** native authenticated Portal, upload/preview, settings, volume preview, local tools and confirmed TF maintenance are Product-owned. | Run a real browser against IP/mDNS and all storage/confirmation flows. |
| Render/display/stable-screen behavior | **Digitally implemented.** official/classic/reflectance/solid strategies, journaled display, one-second page settle and panel-preserving wake are composed. | Compare physical quality/timing and power-cut behavior. |
| Recovery and OTA | **Digitally verified through WS46.** signed HTTPS/Ed25519 staging, boot health, truthful accepted/offline Portal UX plus bounded post-reboot outcome, two-phase quiesce, Recovery teardown fault handling and immutable per-SKU promotion exist. WS43 targeted 19/19 and C151 clean/full link PASS; WS44 promoter 6/6 and OTA regression 36/36 PASS; WS46 real clean release build and OTA/scaffold 50/50 PASS. | Configure/deploy the reviewed public-key channel and execute live signed update/rollback. |
| Authorized attached C151 | **Physical tranche remains Recovery-only and staging is BLOCKED.** beta27 is selected in app1 and beta25 remains in app0. Two byte-identical current 16 MiB internal-flash reads plus NVS/LittleFS/slot custody are retained. No deliberate TF mutation was requested, but writable FAT mounting is not proof of unchanged TF bytes. | Capture/hash an offline whole-card TF image before any candidate write. Then pass the fresh beta29 gate, stage only the proven inactive slot with readback, use the HTTP logical export for additional selection evidence, make the explicit slot choice, and run Product acceptance. |

## 12. Acceptance gates before the next candidate and release

The executable procedure and required evidence bundle are in
[C151_PHYSICAL_ACCEPTANCE.md](C151_PHYSICAL_ACCEPTANCE.md). Historical boot,
Wi-Fi, Portal-health, deep-sleep and timer-wake evidence exists; every item
below remains **PENDING physical/live** unless its retained evidence bundle
explicitly records a passing result for the exact candidate. This list is a
release gate, not an inference from host tests.

- button event p99 ≤ 20 ms during TLS, SD write, preview and full refresh;
- voice start acknowledgement ≤ 100 ms when no higher safety blocker exists;
- Portal cannot change button/voice p99 by more than 10 ms;
- no TTS overrun/underrun in a 60-second burst; cancel ≤ 100 ms;
- AIGC poll is absent while idle and exactly 5 seconds while active;
- Inkloop sync cadence is 30 seconds and event-driven when dirty;
- no task queue is unbounded; queue-full behavior is typed and observable;
- watchdog, stack high-water and heap/PSRAM telemetry remain healthy;
- album commit survives reset at every transaction stage;
- current e-paper content survives wake and ordinary reboot;
- NVS/LittleFS/SD migration is non-destructive and rollback-tested;
- MyAI server repository and live service show no firmware-driven mutation.

## 13. Explicit non-reuse list

Do not transplant these concrete Arduino-era pieces:

- the monolithic `loop()` orchestration;
- synchronous `ResponsiveWorkExecutor::execute` wait-pumping;
- `HTTPClient`, Arduino `WebServer`, `WiFiManager`, `Preferences` adapters;
- M5Unified button/audio/display ownership;
- any WebUI handler that directly formats storage, writes an album, starts
  network work, drives LEDs or plays audio;
- large “just increase the queue” TTS buffering without TCP/DMA backpressure.

The algorithms and contracts are useful; the cooperative adapters are not.
