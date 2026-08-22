# Inkloop bounded OTA staging and boot-health cores

This isolated component stages a caller-supplied, reviewed OTA stream into the
inactive update partition and confirms or rolls back an image that ESP-IDF
reports as `PENDING_VERIFY`. It is not wired into `app_main`.

`OtaStagingCore` accepts only a bounded schema-v1 manifest whose board SKU
matches the device. The canonical signed fields bind the domain, schema, board
SKU, firmware version, exact image size, expected SHA-256 and the exact
`inkloop-pinned-ed25519-sha256-v1` policy identifier. The detached signature
is bounded but deliberately excluded from the canonical payload. Incremental
hashing requires an exact byte count before an injected verifier may approve
the canonical manifest plus the streamed digest.

`EspOtaEd25519Verifier` is the fail-closed ESP-IDF implementation of that
verification seam. It uses the official `espressif/libsodium` 1.0.22 managed
component, accepts only an exact 32-byte caller-provided public key, validates
the encoded Ed25519 point and verifies only the exact staging policy. It owns
and scrubs its bounded public-key copy and scrubs the temporary signed-message
buffer after every verification attempt. No key is compiled into this
component; without production composition supplying a valid pinned public key,
the verifier remains unavailable and staging cannot advance.

`EspOtaStagingAdapter` validates the manifest and verifier seam before asking
for a partition. Its production function table obtains only
`esp_ota_get_next_update_partition(nullptr)`, rejects the running partition or
insufficient capacity, and bounds each write to 64 KiB. Selection occurs only
after content, signature and `esp_ota_end` checks. Once a handle exists, every
failure/explicit-abort path attempts `esp_ota_abort` at most once and never
selects the target. No production key, downloader, URL, credential, retry,
reboot or storage mutation is present.

`OtaHttpsAcquisition` is a separate caller-driven, one-shot acquisition
layer. It accepts no default endpoint: the caller must supply an exact HTTPS
manifest URL, board SKU, current semantic version and total deadline. Its
strict, fixed-capacity JSON parser rejects missing, duplicate, unknown,
escaped, oversized or non-canonical fields; requires the exact schema, board,
newer semantic version, image length, lowercase SHA-256, pinned signing policy
and 64-byte Ed25519 signature; and requires the image URL to use the exact
reviewed manifest origin. The manifest is capped at 4 KiB and the image is
streamed in chunks of at most 4 KiB into `EspOtaStagingAdapter`; no full-image
buffer exists.

`EspOtaHttpsTransport` uses the ESP-IDF certificate bundle with hostname
verification enabled, disables redirects and authorization retries, checks the
actual connected IPv4/IPv6 peer is public, accepts only HTTP 200 with a
positive exact `Content-Length`, and reapplies the one shared monotonic
deadline before headers and every bounded read. A timeout, redirect, private
peer, missing/mismatched length, truncation or sink fault fails closed and the
acquisition layer aborts any already-open staging handle. URLs permit neither
credentials, query strings nor fragments, and this component never logs them.

`OtaBootHealthCore` is board-neutral. It requires fresh storage/upgrade,
board, runtime, fatal-status, and mandatory supervisor-lane evidence for a
continuous bounded soak window. Network, Wi-Fi, cloud and MyAI state are not
inputs. A pending deadline or explicit fatal condition requests rollback.

`EspOtaBootHealthAdapter` rereads the running partition state immediately
before either irreversible action and latches each action attempt. Production
functions are isolated in `esp_ota_system_api.cpp`; host tests use an injected
function table.

Integration must supply a reviewed manifest endpoint and a
production-provisioned pinned public key, compose the acquisition, verifier,
staging flow and boot-health gate, and
supply real boot-health evidence. Rollback support is enabled in
`sdkconfig.defaults`; regenerating a clean build configuration makes staged
images use pending-verify semantics. All app composition, key provisioning,
downloading, reboot and physical-device validation remain outside this
component.
