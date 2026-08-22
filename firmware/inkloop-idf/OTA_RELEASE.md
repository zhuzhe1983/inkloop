# Static OTA release procedure

This procedure creates a same-origin static release tree. It does not deploy,
flash, reboot or trigger an update. The release operator remains responsible for
reviewing the endpoint, public key, target board and firmware provenance.

## Preconditions

- Start with the device on reviewed version `beta.N`. The target must be a
  strictly newer SemVer such as `beta.N+1`; build metadata alone is not newer.
- Use ESP-IDF `v6.0.2`, the board `m5_papercolor_c151`, and a clean source tree
  whose `version.txt` is the exact target SemVer.
- Keep the Ed25519 private PEM outside the repository and outside every static
  output/deployment tree. It must be owned by the current user with mode `0600`.
- Select an existing absolute output root that maps byte-for-byte to one
  reviewed HTTPS public base URL. For production, the Next.js `public/ota`
  directory maps to `https://inkloop.mess.host/ota`. The host must be
  lower-case public DNS; redirects, credentials, query strings, fragments and
  IP literals are invalid.
- Keep the reviewed 32-byte Ed25519 public key outside the output tree in a
  file containing exactly 64 lower-case hex characters and one optional final
  newline. Compare it byte-for-byte with the raw public key pinned in the
  production firmware before every release ceremony.
- Keep release-build defaults outside the repository and static tree. The base
  `sdkconfig.defaults` intentionally has no OTA assignments, so normal
  development builds remain OTA-disabled.

Do not install a test public key in production firmware. Confirm separately
that the reviewed public key compiled into the target firmware belongs to the
external private key used here.

## 1. Prepare public release defaults

Set `version.txt` to the exact next version, for example `0.4.0-beta.2`, review
the source, then create an external ASCII defaults file with exactly these
three lines in this order. Replace the public-key placeholder with the reviewed
64-character lower-case raw Ed25519 public key; an all-zero or placeholder key
is invalid.

```text
CONFIG_INKLOOP_OTA_MANIFEST_URL="https://inkloop.mess.host/ota/m5-papercolor-c151/manifest.json"
CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX="<reviewed-64-lowercase-hex-public-key>"
CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS=120000
```

This file is public configuration, not a private signing key, but it still
belongs in the reviewed release system rather than source control. There must
be exactly one assignment for URL, key and deadline; comments, blank lines,
extra Kconfig values, version-specific manifest URLs and another host/SKU are
rejected.

## 2. Build one clean OTA-enabled C151 release

Choose an absolute output path outside the repository whose parent exists but
whose final directory does not. Run the pinned IDF and explicit version through
the release wrapper:

```sh
python3 firmware/inkloop-idf/tools/build_ota_release.py \
  --idf-path /Users/zhuzhe/.espressif/frameworks/esp-idf-v6.0.2 \
  --public-defaults /external/reviewed/inkloop-ota-release.defaults \
  --output-dir /absolute/release-work/build-c151-beta.2 \
  --board-sku m5-papercolor-c151 \
  --firmware-version 0.4.0-beta.2
```

The wrapper rejects an existing/stale output directory and configures from
scratch into the new isolated directory. It pins IDF `v6.0.2`, ESP32-S3, C151,
the exact `version.txt`, a build-local sdkconfig, and the external public
defaults. Command success alone is insufficient: it then verifies generated
sdkconfig assignments, CMake target/SKU, project version, ESP image header,
embedded URL and public-key text, and the standard bootloader/partition/OTA
data/app flash arguments and artifacts.

Any failure removes the directory the wrapper created. Success writes
`release-build-receipt.json` and prints the same credential-free JSON. It
contains exact SKU/version/target, app size and SHA-256, stable manifest URL,
the SHA-256 fingerprint of the raw public key, deadline and hashes of generated
sdkconfig/flash arguments. It contains no raw key, private key, filesystem path,
timestamp or git state.

The input to packaging is the verified
`/absolute/release-work/build-c151-beta.2/inkloop_idf.bin`. Preserve source
revision and the build receipt in the release system. Do not use the normal
development `build.sh` output for a production OTA package: its empty OTA
defaults deliberately keep acquisition disabled.

## 3. Package `beta.N+1`

The command below creates
`<output-root>/<sku>/<version>/`. Use absolute paths for all filesystem inputs.
The output root must already exist.

```sh
python3 firmware/inkloop-idf/tools/package_ota_release.py \
  --image /absolute/release-work/build-c151-beta.2/inkloop_idf.bin \
  --board-sku m5-papercolor-c151 \
  --firmware-version 0.4.0-beta.2 \
  --public-base-url https://inkloop.mess.host/ota \
  --private-key /external/secure/path/inkloop-release-ed25519.pem \
  --output-root /absolute/path/to/next-app/public/ota
```

The packager copies the image to a deterministic versioned filename, invokes
`sign_ota_manifest.py` without changing its
`canonical-manifest || streamed-image-digest` signature contract, and emits:

```text
m5-papercolor-c151/0.4.0-beta.2/
  inkloop-idf-m5-papercolor-c151-0.4.0-beta.2.bin
  release-receipt.json
  manifest.json
```

It completes the image and credential-free receipt in a hidden staging
directory, creates `manifest.json` last, fsyncs the tree, then atomically renames
the completed version directory into place. Existing, equal, older or ambiguous
release histories are refused. A failed package may not be repaired by copying
individual files into the public tree; investigate it and package into a clean
reviewed root.

## 4. Verify the immutable release locally

Run the promotion tool in verification-only mode before changing the stable
channel. This is a complete second read of the immutable directory: exact file
set and ordered JSON contracts, board/version and URL, receipt-to-manifest
size/hash, streamed image size/hash, and the Ed25519 signature over the frozen
`canonical-manifest || streamed-image-digest` bytes. It also validates the
existing channel, if any, and rejects an equal or older target. It writes
nothing to the output root.

```sh
python3 firmware/inkloop-idf/tools/promote_ota_channel.py \
  --output-root /absolute/path/to/next-app/public/ota \
  --board-sku m5-papercolor-c151 \
  --firmware-version 0.4.0-beta.2 \
  --public-base-url https://inkloop.mess.host/ota \
  --public-key /external/reviewed/inkloop-release-ed25519-public.raw.hex \
  --verify-only
```

A successful result has `"operation":"verify"`. Any receipt, image,
signature, same-origin URL, public-key, symlink, unexpected entry or concurrent
publisher failure exits nonzero and leaves the channel unchanged. Do not edit
an immutable version directory to repair a failure.

## 5. Promote the stable per-SKU channel

Repeat the same command without `--verify-only`:

```sh
python3 firmware/inkloop-idf/tools/promote_ota_channel.py \
  --output-root /absolute/path/to/next-app/public/ota \
  --board-sku m5-papercolor-c151 \
  --firmware-version 0.4.0-beta.2 \
  --public-base-url https://inkloop.mess.host/ota \
  --public-key /external/reviewed/inkloop-release-ed25519-public.raw.hex
```

The tool revalidates every target-release byte under a nonblocking exclusive
per-SKU lock, then fsyncs a temporary copy and atomically replaces only:

```text
<output-root>/m5-papercolor-c151/manifest.json
```

It never writes inside a versioned directory. The fixed production URL pinned
for the lifetime of this SKU is therefore:

```text
https://inkloop.mess.host/ota/m5-papercolor-c151/manifest.json
```

The stable file is byte-identical to the promoted version's signed manifest;
its same-origin `image_url` continues to name the immutable versioned image.
Equal/downgrade promotion, a corrupt current channel, and a second publisher
holding the SKU lock are refused without waiting or replacing the pointer.

## 6. Deploy and verify static hosting

Have the release system deploy the complete promoted tree as one atomic Next
release. If the hosting system cannot switch the tree atomically, publish the
new immutable version directory first and the per-SKU channel manifest last.
Never overwrite a versioned image/manifest/receipt and never place either key
in the static root. Promotion proves local filesystem state, not CDN behavior.

Download without following redirects and retain headers:

```sh
channel_url=https://inkloop.mess.host/ota/m5-papercolor-c151/manifest.json
release_url=https://inkloop.mess.host/ota/m5-papercolor-c151/0.4.0-beta.2
curl --fail --silent --show-error --proto '=https' --tlsv1.2 \
  --max-redirs 0 --dump-header /tmp/inkloop-manifest.headers \
  --output /tmp/inkloop-manifest.json "$channel_url"
curl --fail --silent --show-error --proto '=https' --tlsv1.2 \
  --max-redirs 0 --dump-header /tmp/inkloop-image.headers \
  --output /tmp/inkloop-image.bin \
  "$release_url/inkloop-idf-m5-papercolor-c151-0.4.0-beta.2.bin"
```

Verify both responses are direct HTTP 200 responses with a positive,
non-chunked `Content-Length`; image length must equal `image_size`, its SHA-256
must equal `image_sha256`, and the downloaded manifest SHA-256 and size must
equal the immutable version's `release-receipt.json`. The downloaded stable
manifest must be byte-identical to that version's manifest. Confirm it has
exactly the frozen eight fields, the exact board/version, and an image URL on
the byte-exact manifest origin/effective port. Verify its detached Ed25519
signature using the reviewed raw public key before exposing the action.

## 7. Deliberately request OTA

Firmware keeps the fixed channel URL above and matching production public key;
they do not change for each version. Only after the HTTPS and cryptographic
checks pass, open the authenticated normal Portal, inspect the displayed
current/target versions and deliberately invoke the OTA action once. Do not use
Recovery, an unauthenticated request or an automatic background trigger as a
substitute.

Observe acquisition, inactive-slot selection and reboot logs. The new image
must produce fresh local health evidence before confirmation; otherwise the
existing 120-second pending-image policy must roll it back. A successful host
package or HTTPS download is not physical proof: complete the serial-backed
power-cut, rollback and post-boot functional acceptance on C151 separately.
