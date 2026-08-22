# Native device settings

`inkloop_settings` is a portable, SKU-neutral settings model and atomic journal.
`inkloop_settings_idf` owns the native NVS adapter. The model persists:

- volume and maximum LED brightness (`0..100`);
- voice-assistance enablement;
- non-empty assistant and AIGC prompt templates (both at most 512 UTF-8 bytes);
- optional negative prompt (at most 384 UTF-8 bytes);
- automatic/internal/removable asset-storage preference;
- an adapter-owned render-strategy ID.

The 512/512/384 byte limits are the end-to-end Portal bounds and are below
MyAI's 2048-byte limit. The local ASR command parser's 256-byte input limit does
not reduce values written through WebUI or another trusted settings client.

The native journal uses namespace `ink-settings-v1`, CRC32-protected binary
records, alternating slots selected by generation parity, and a separately
committed `head` plus `initialized` marker. Only the selected, verified slot is
visible. An interrupted first save is fresh-with-orphan; an interrupted later
save leaves the previous head authoritative. The adapter writes `head` before
the advisory marker; a reset between them leaves a checksum-valid selected
record and is therefore accepted, while a present-but-invalid marker, bad head,
or invalid selected record fails closed.

## Arduino compatibility

Released Arduino firmware stored these operational settings in NVS namespace
`ink-portal`, keys `initialized=0xA5`, `head=1|2`, and `snap-a`/`snap-b`. Each
slot is a JSON envelope with an escaped canonical `payload` and its SHA-256.
`EspNvsReadOnlyLegacyPortalSource` opens that namespace with `NVS_READONLY` only.
`inspectLegacyPortalSettings()` verifies the envelope and returns a candidate;
it never writes, erases, repairs, or automatically imports legacy settings.

The older `inkloop-v2` namespace is deliberately not imported here: it contains
feature flags and LED role calibration, not this complete settings contract.

## Wiring API

1. The SKU adapter supplies reviewed defaults (`makePaperColorDefaults()` is a
   convenience profile builder) and constructs `EspNvsSettingsJournalStore`
   plus `SettingsStoreCore` on the single settings/storage owner task.
2. Call `SettingsStoreCore::load()` once. Generation `0` means fresh defaults;
   a nonzero generation is a committed native record. Do not silently continue
   on any non-success status.
3. When generation is `0`, optionally call
   `inspectLegacyPortalSettings(EspNvsReadOnlyLegacyPortalSource,
   EspPsaLegacySha256Verifier, defaults, candidate)`. A candidate is data only;
   product policy decides whether to show/accept it. Nothing is auto-written.
4. Persist an accepted candidate or Portal patch with
   `SettingsStoreCore::save(values, expected_generation, committed)`. A
   `Conflict` requires reload/rebase; never retry with a guessed generation.
5. Publish the committed immutable snapshot to audio/LED/voice/AIGC/storage/
   display owners through bounded messages. Pass assistant/template strings
   byte-for-byte; do not truncate. AIGC request assembly must separately reject
   an expanded `{prompt}` request that exceeds the MyAI request limit.
