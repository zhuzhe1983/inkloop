# Native device settings

`inkloop_settings` is a portable, SKU-neutral settings model and atomic journal.
`inkloop_settings_idf` owns the native NVS adapter. The model persists:

- volume and maximum LED brightness (`0..100`);
- voice-assistance enablement;
- non-empty assistant and AIGC prompt templates (both at most 512 UTF-8 bytes);
- AIGC inference steps (`1..50`, default `20`);
- optional negative prompt (at most 384 UTF-8 bytes);
- automatic/internal/removable asset-storage preference;
- an adapter-owned render-strategy ID.

The 512/512/384 byte limits are the end-to-end Portal bounds and are below
MyAI's 2048-byte limit. The local ASR command parser's 256-byte input limit does
not reduce values written through WebUI or another trusted settings client.

The retained beta27 rollback image (`219d001`) accepts only main wire schemas
1/2, the voice flag, and a zero reserved byte. Every beta31 main-journal write
therefore remains schema 2. Schema 3 is read-only compatibility for an
unreleased development record; an explicit save rewrites it as schema 2.

The main journal uses namespace `ink-settings-v1`, CRC32-protected `slot0` /
`slot1`, a generation-selected `head`, and an advisory `initialized` marker.
The inactive slot is written and verified before the head selector. NVS set
operations are not treated as a multi-key transaction: after an uncertain
selector write, readback determines the authoritative record.

LED-role swap and AIGC steps are stored in a rollback-ignored extension journal
in the same namespace: `ext0`, `ext1`, and the sole commit selector `ext-head`.
Each CRC-protected extension record has its own sequence plus the compatible
main generation. Missing `ext-head` materializes the legacy LED/default-20
values; a corrupt selected extension fails closed, while unselected orphan
slots are ignored. A selected extension may name an older main generation
after beta27 performs a main-only save, but may never name a future generation.

Extension-only changes prepare one inactive extension slot then publish the
single `ext-head` key without advancing the main generation. Main-only changes
leave `ext-head` untouched. Mixed changes prepare the extension, publish and
verify the schema-2 main record, then publish `ext-head`. If the final selector
fails, the caller receives failure plus an authoritative reload (new main, old
extension); it must not report the requested LED/steps as saved. Retrying is
idempotent. Migration recovery covers this exact split boundary without
rewriting the already-selected main record.

## Arduino compatibility

Released Arduino firmware stored these operational settings in NVS namespace
`ink-portal`, keys `initialized=0xA5`, `head=1|2`, and `snap-a`/`snap-b`. Each
slot is a JSON envelope with an escaped canonical `payload` and its SHA-256.
`EspNvsReadOnlyLegacyPortalSource` opens that namespace with `NVS_READONLY` only.
`inspectLegacyPortalSettings()` verifies the envelope and returns a candidate;
it never writes, erases, repairs, or automatically imports legacy settings.

The older `inkloop-v2` LED calibration remains a separately verified migration
input; it is never treated as a complete settings snapshot. Legacy `steps`,
when present, must be `1..50`; absence deterministically materializes `20`.

## Wiring API

1. The SKU adapter supplies reviewed defaults (`makePaperColorDefaults()` is a
   convenience profile builder) and constructs both NVS journal adapters plus
   `SettingsStoreCore` and `SettingsExtensionStoreCore` on the single settings/
   storage owner task.
2. Call `loadRollbackCompatibleSettings()` once. Generation `0` means fresh
   defaults; a nonzero generation is a committed main record. Do not silently
   continue on any non-success status. Direct `SettingsStoreCore` calls are for
   canonical schema-2 main values only, not Product mutations.
3. When generation is `0`, optionally call
   `inspectLegacyPortalSettings(EspNvsReadOnlyLegacyPortalSource,
   EspPsaLegacySha256Verifier, defaults, candidate)`. A candidate is data only;
   product policy decides whether to show/accept it. Nothing is auto-written.
4. Persist an accepted candidate or Portal patch with
   `saveRollbackCompatibleSettings(main, extension, values,
   expected_generation, authoritative)`. A `Conflict` requires reload/rebase;
   never retry with a guessed generation. On failure, use a valid returned
   authoritative snapshot only as the actual partial state, never as success.
5. Publish the committed immutable snapshot to audio/LED/voice/AIGC/storage/
   display owners through bounded messages. Pass assistant/template strings
   byte-for-byte; do not truncate. AIGC request assembly must separately reject
   an expanded `{prompt}` request that exceeds the MyAI request limit.
