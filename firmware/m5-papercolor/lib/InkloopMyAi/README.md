# InkloopMyAi

Hardware-independent MyAI third-party client core for M5 PaperColor. It fixes
the ordinary-client identity to `app_id=inkloop` and Center/Login Server to
`https://myai.mess.host`. It contains no developer key, provider credential,
account-management route, or MyAI service mutation.

Integration supplies adapters for HTTPS/TLS, WebSocket, persistent credentials,
clock, streaming image decode, audio playback, local transcript interception,
and UI events. The default codec implements the documented public request and
event shapes without depending on Arduino or a WebSocket library.

## Onboarding order

1. Generate a six-digit candidate locally and call `startPairing` before
   registering/binding Inkloop.
2. Once MyAI accepts it, always reuse `PairingView.onboardingCode` for Inkloop's
   loose six-digit UX. A rejected, missing, mismatched, or non-six-digit MyAI
   response blocks Inkloop binding and requires a fresh MyAI candidate; there is
   no ordinary two-code fallback.
3. Display the returned `bindingUrl` as the QR target and poll `pollPairing`.
4. Stop immediately after the first token-bearing success. The credential-store
   adapter atomically promotes the returned device token and clears the pending
   pairing token.

The stable installation fingerprint, pending pairing token, durable device
token, and Inkloop credential are distinct values and namespaces. Gateway tokens
exist only in `GatewayLease` RAM and are overwritten on disconnect/destruction.
The selected gateway—not Center—receives `/api/v1/gateway/sessions/start`, and
that request carries only `X-Gateway-Session-Token` plus its JSON content type.
Heartbeat and disconnect remain Center operations authenticated by the durable
device credential.

## Voice and images

Realtime voice is half duplex: open a routed voice session, wait for
`session.ready`, send `audio.start`, raw PCM16 16 kHz mono frames, and
`audio.stop`. `auto_response=false` allows the injected local interceptor to
handle bounded device commands; unmatched final transcripts are returned with
`response.create`. Binary WebSocket messages during `tts.start`/`tts.stop` are
streamed to the audio adapter.

Image generation uses a separate routed `aigc` lease. The output adapter must
tokenize the JSON and decode `content_base64` incrementally into `IImageSink`,
enforcing encoded and decoded caps without holding the whole image in RAM.

The injected endpoint-security adapter must resolve every A/AAAA answer, reject
private, loopback, link-local, unspecified, multicast, or otherwise non-public
answers, and require certificate-chain and hostname validation. HTTP adapters
must not follow redirects. The core also rejects non-HTTPS, private-literal,
`.local`, `.lan`, and `.internal` gateway URLs. Credential operations are
fail-closed: failed writes, clears, reloads, or postcondition checks put the
client in storage recovery instead of transitioning or silently re-pairing.
Every durable read must match exactly one complete state: fresh, pending, or
bound. Pending/bound states require the same exact six-numeric-digit device ID,
are mutually exclusive, and runtime clear removes the temporary device ID as
well as the token and active bit. Empty/partial secrets and incomplete selected
gateway leases are rejected before persistence, session start, WebSocket, or
business traffic. On reboot, a bound `active=true` snapshot restores `Bound`;
a bound `active=false` snapshot keeps its credential but restores
`PaymentRequired` until a later online authorization check succeeds.
Center device tokens are stable for the lifetime of a bound device. A 401 from
an authenticated device request therefore clears the unusable runtime
credential atomically while preserving the installation fingerprint; the
firmware then starts a fresh six-digit pairing flow. A failed clear remains a
storage-recovery error and never silently creates a second credential.
