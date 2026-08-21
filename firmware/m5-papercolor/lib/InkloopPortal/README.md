# InkloopPortal

Hardware-independent PaperColor onboarding and local-settings core. It does not
open sockets, modify MyAI/AaaS, write storage, drive LEDs, or execute destructive
operations itself. The firmware supplies an `IPortalAdapter`, routes WebServer
requests into `InkloopPortal::handle`, and forwards trusted device callbacks.

## Boot and persistence contract

The portal starts fail-closed. After loading durable state, firmware must call
`hydrate()` with a complete schema-v1 `PortalPersistedSnapshot`; a new device
uses `makeFreshPortalSnapshot()` only after that fresh snapshot is durably
created. Missing presence bits, unsupported/future schemas, invalid enums,
inconsistent onboarding stages, unsafe settings, and revision overflow are
rejected without partially restoring state. An expired unbound code is cleared
through an atomic persistence patch before hydration succeeds.

Every trusted onboarding transition, tutorial step, and settings patch is
copy-on-write. `persistPortalSnapshot` receives the expected/next revision, an
exact dirty-field mask, and the fully merged hydrated snapshot. Only a
successful atomic adapter return updates RAM. A failed adapter call must leave
durable bytes unchanged and the portal retains its prior state. This prevents a
one-field form from replacing previously saved values with constructor
defaults. An activated/bound hydrated device rejects pairing replay.

## Onboarding contract

1. Firmware keeps the existing `Inkloop-XXXX` Wi-Fi AP and calls
   `onWifiConfigured(true)` after provisioning.
2. `POST /api/onboarding/myai/start` asks the adapter to begin ordinary
   third-party MyAI pairing with public `app_id=inkloop`.
3. The MyAI client supplies the authoritative six-digit `onboarding_code` to
   `onAuthoritativeMyAiCode`. While Inkloop is not yet bound, the same value is
   offered to Inkloop via `requestInkloopCodeReuse`. Reuse failure fails closed;
   there is no automatic second-code path. After Inkloop is bound, later MyAI
   rotations never mutate or re-present the historical Inkloop binding value.
4. The presentation exposes the fixed public registration target
   `https://myai.mess.host/?device_code=<six digits>#devices`, the visible code,
   activation state, replayable voice tutorial, and settings-ready state.

No pairing token, device token, gateway token, Inkloop secret, password,
transcript, or audio is represented by the module's public DTOs or renderers.
Diagnostics additionally normalize case and separators before redacting token,
Authorization/Bearer, pairing, session, password, cookie, API-key, and secret
families. Arbitrary adapter strings remain untrusted.

Album access is page-based. `readAlbumPage` must enforce the request's item,
field, and aggregate byte bounds while reading storage; it must not construct a
complete album and truncate it later. The portal independently revalidates the
page, caps JSON at 12 KiB and dashboard HTML at 32 KiB, and returns deterministic
413/422 responses for excessive/invalid adapter data. `findAlbumItem` performs
the exact bounded lookup used before preparing a delete action.

## Routes

Public: `GET /health`, `GET /` (login form), `POST /api/session`.

Authenticated reads: `/`, `/api/state`, `/api/settings`, `/api/album`,
`/api/diagnostics`, `/api/serial-log`.

Authenticated mutations: `/api/onboarding/myai/start`, tutorial advance/
complete/restart, `/api/settings`, and the destructive-action
preparation/confirmation endpoints. Every mutation requires an exact allowed
Origin and CSRF token. Bodies are bounded form data.

`POST /api/settings` accepts only these bounded fields: `storage`
(`auto|internal|sd`), `volume` (0–100), `led_brightness` (1–100; changing it
automatically starts the non-blocking RGB role diagnostic), `assistant_prompt` (512 bytes),
`image_size` (`400x600|600x400`), `image_steps` (1–50), `negative_prompt`
(384 bytes), `led_swap` (`0|1`), `refresh_mode`
(`official|experimental-six-color`), `power_mode`
(`compatibility|battery`), and `idle_timeout` (120–3600 seconds). Unknown or
duplicate fields fail closed. Official refresh and compatibility power remain
the defaults.

Every request must carry the socket-derived `peerIp`. A bounded in-memory gate
keys budgets by peer, current session scope, normalized route, and request
class. Read, write, and destructive budgets are separate. Exhaustion returns
HTTP 429 plus `PortalResponse::retryAfterSeconds`, which the WebServer adapter
must emit as `Retry-After`. The uint32 monotonic-window arithmetic handles
counter wrap and fails closed on a backwards/ambiguous clock. Rate limiting is
additive: bootstrap nonce, session expiry, same-origin, and CSRF checks remain
mandatory. At capacity, only entries whose own windows have expired are
recycled; live budgets are never evicted.

`POST /api/actions/prepare` accepts `format_sd`, `clear_album`, or
`delete_asset` with a typed asset ID. It returns the nonce, exact repeat phrase,
expiry, and `physicalConfirmationRequired=true`. `POST /api/actions/confirm`
can only move that same action to `awaiting_physical_confirmation`; it cannot
execute it. `confirmPhysical` consumes the nonce before dispatching the typed
callback, so retries cannot replay an operation. `clear_album` is scoped to the
user album; integration must preserve factory/tutorial assets.

Destructive actions have three distinct stages: prepare with a per-action nonce,
repeat the exact phrase through the web session, then call `confirmPhysical`
from the device button path before the typed adapter callback can run. There is
no web execution route.
