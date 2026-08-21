# InkloopVoice

Hardware-independent PaperColor push-to-talk controls and local command safety
layer. This library owns no HTTP, WebSocket, Wi-Fi, endpoint, token, or remote
service implementation. A firmware integration supplies the conversation,
display, storage, LED, and audio adapters.

## Button, event, and half-duplex contract

- Button C in `Ready` starts one PCM16 microphone turn only after packaged
  playback reports a successful synchronous stop.
- A second tap stops capture and sends the turn. Button C in `Thinking` or
  `Speaking` cancels the active turn; in `Error` it retries bounded audio
  cleanup and returns to `Ready` only if the session is still ready.
- Button C never starts capture during a panel refresh. It emits only a
  throttled `display.please_wait` cue bound to `refreshGeneration()`; queued or
  playing wait audio is discarded/stopped when that generation ends.
- Before streamed TTS may enter `Speaking`, any live capture must report a
  successful `stopListeningAndSend()` and packaged playback must stop. The
  integration must not enable its streamed speaker until `onTtsStart()` returns
  success.
- Cancellation operations are idempotent release barriers: successful
  `cancelTurn()` means microphone capture and the outstanding response are
  quiescent; successful `cancelTts()` means streamed playback is quiescent;
  successful prompt-player `stop()` means packaged playback is quiescent.
- A transport that must close its whole socket/lease on cancel returns true
  from `cancelTurnClosesSession()`. The runtime invalidates session readiness
  before cleanup completes and publishes only Error/reconnecting until a fresh
  `onSessionReady()` callback; it never flashes a false Ready state.
- Every streamed callback carries `activeTurnGeneration()`. Late callbacks
  after cancellation, transport loss, or reconnect are ignored.

Listening, response, and TTS deadlines release tracked audio and enter a visible
recoverable `Error`. The left LED receives semantic `Listening`, `Thinking`,
`Speaking`, `Error`, or `Off`; physical pixels/colors remain the LED owner's job.

## Safe command and confirmation contract

The bounded Chinese/English grammar covers free-space queries, image listing and
selection, stable-ID deletion, clear-all, volume 0–100, SD/internal format,
assistant prompt, image size/model/negative-prompt settings, and Wi-Fi or voice
identity reset. IDs reject dot segments, hidden/path-like shapes, separators,
encoded separators, globs, and drive-letter tokens.

Delete requires an exact spoken confirmation. Clear-all, storage format, and
identity reset additionally require a fresh trusted physical Button C callback.
Every pending destructive intent stores the immutable target ID, active album
ID, album revision, session generation, and expiry. The runtime re-reads the
album revision before each confirmation stage and clears intent on any mismatch,
transport error, disconnect, reconnect/session-ready callback, disable, expiry,
or explicit revision-change callback. Cleared intent is never restored.

## Display-commit-bound ordinal prompts

`selectImage()` returns an album/frame/revision ticket. Selection only calls
`queueOrdinal()`; it cannot play “first/second/third.” Playback is authorized
only by an exact `onDisplayCommitSuccess(frame, revision, index)` callback.
Commit failure, cancellation, mismatch, or supersession produces no ordinal.

## Offline prompt package

`assets/prompts.v1.json` is a versioned manifest for 19 PCM16 mono 16 kHz WAV
prompts, including first/second/third, wait, listening, and error. Main assets
were generated locally with macOS offline voice `Tingting`; every entry also has
a deterministic short-tone fallback. The manifest records SHA-256 for both.

Run the strict gate locally with:

```sh
node scripts/generate-prompts.mjs --verify
python3 scripts/platformio_prompt_check.py
```

`library.json` registers the Python verifier as a PlatformIO pre-build action,
so a missing, corrupt, path-escaping, or checksum-mismatched asset fails the
build. Regeneration is offline-only:

```sh
node scripts/generate-prompts.mjs --generate
```

On macOS with `say` and `afconvert`, regeneration creates spoken WAVs; otherwise
it creates deterministic tone mains as well as fallbacks. Final speaker level,
pronunciation, audibility during a Spectra 6 refresh, and prompt quality remain
a C151 physical acceptance gate. The tone fallback is a safety cue, not a
voice-quality substitute.
