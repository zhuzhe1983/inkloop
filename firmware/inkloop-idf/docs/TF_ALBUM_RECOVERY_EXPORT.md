# TF album recovery export

This procedure is mandatory before choosing a removable-album recovery slot
when `index.json`, `index.next`, and `index.prev` are all valid and divergent.
Only the offline whole-card workflow below satisfies the pre-flash physical TF
custody gate. A receipt-bound beta31-or-newer accepted candidate's HTTP path is an
additional logical export for selection and content verification after that
exact candidate is already running; it does not replace the whole-card image.

The current firmware mounts TF through `esp_vfs_fat_sdspi_mount`, which is a
writable FAT mount. Although the HTTP exporter exposes no mutation operation
and the export implementation intends only reads, mounting/using that
filesystem is not equivalent to a hardware- or VFS-enforced read-only source.
Filesystem metadata writes cannot be excluded by this workflow. Therefore do
not describe the HTTP path as a read-only custody path, do not use it as proof
that TF bytes were unchanged, and do not flash a beta31-or-newer candidate in
order to obtain it before the offline image exists.

## Beta31+ authenticated HTTP logical export (additional evidence only)

Prerequisite: the pre-flash whole-card image and its hash have already been
captured and verified through the offline boundary below. If they do not
exist, physical candidate staging remains `BLOCKED`.

Use the exact three lowercase SHA-256 values displayed for removable Current,
Next, and Previous in the authenticated Recovery page. Keep the device in
Recovery mode and use its port-8080 origin; port 80 is the Wi-Fi settings
service, not this API. First open the exact private IPv4 origin shown on the
device screen, log in through the Recovery page, and copy the short-lived
`inkloop_recovery_session` cookie value and the
`inkloop-recovery-csrf` Session Storage value from the browser developer
storage view. The exporter does not accept or transmit the local management
password and never calls `/api/session`.

Pass the two values on stdin only: session cookie first, CSRF token second.
Do not put either value in command-line arguments, exported environment
variables, shell history, logs, or the manifest. Start the export immediately;
if the browser session expires, log in again and copy both new values.

```sh
export INKLOOP_TF_EXPORT_DIR='/absolute/new/backup-directory'
export INKLOOP_RECOVERY_URL='http://192.168.4.1:8080/'
IFS= read -r -s INKLOOP_RECOVERY_SESSION
IFS= read -r -s INKLOOP_RECOVERY_CSRF
printf '%s\n%s\n' "$INKLOOP_RECOVERY_SESSION" "$INKLOOP_RECOVERY_CSRF" | \
  node firmware/inkloop-idf/tools/export_tf_album_recovery_http.mjs \
    --remote-url "$INKLOOP_RECOVERY_URL" \
    --output "$INKLOOP_TF_EXPORT_DIR" \
    --auth-stdin \
    --expect-current-sha256 '<Current SHA-256 from Recovery>' \
    --expect-next-sha256 '<Next SHA-256 from Recovery>' \
    --expect-previous-sha256 '<Previous SHA-256 from Recovery>'
unset INKLOOP_RECOVERY_SESSION INKLOOP_RECOVERY_CSRF
```

Replace the example address with the exact RFC1918 IPv4 shown on the device
screen. The production target must have the form
`http://<private-ipv4>:8080/`. The exporter deliberately rejects mDNS names
including `inkloop.local`, public, loopback and link-local addresses, alternate
ports, URL credentials, and any path/query/fragment;
`http://localhost:8080/` is reserved for the isolated test harness. Recovery
still enforces its local-peer, HttpOnly session, allowed-Origin and CSRF checks,
but the exporter reuses the already authenticated short session instead of
submitting the management password. Prepare is bound to all three expected
hashes.
The asset union is paginated and bounded; files are transferred sequentially,
the device yields between 4 KiB chunks, and both device and host hash every
file. The device export session and the host's final-verification wait each use
a 30-minute window, sized for the maximum 288 assets plus the second complete
hash pass. The host also parses the downloaded indexes and requires their union,
sizes, and candidate masks to exactly match the server inventory.

After every file is copied, the host calls the final Recovery verification.
The device re-hashes all three indexes and every referenced asset in a second
bounded pass before answering `verified`; a stale session, disconnect, changed
source, short response, bad hash, inconsistent inventory, or local write error
fails closed. `manifest.json` with `complete: true` is the last durable write.
Any failure after output creation leaves `INCOMPLETE.json` and no complete
manifest. Do not use that directory as backup evidence.
The exporter canonicalizes the existing output parent, appends only the
requested leaf name, and claims that new directory exclusively before its
first network request. A pre-existing file, directory, or symlink is refused.

This is a complete logical export of the three indexes and their referenced
asset union, not physical pre-flash custody, not proof of source immutability,
and not a raw image of unused sectors or unrelated TF files. The offline
whole-card workflow below is mandatory before writing any beta31-or-newer
candidate to the device.

## Mandatory pre-flash offline whole-card safety boundary

1. Shut the device down and remove the TF card. Do not resolve a Recovery
   action first.
2. Insert the removed card through a known card reader. Prefer a reader/adapter
   with hardware write protection. Identify the exact external whole disk and
   manually unmount every volume on it. Do not format, repair, initialize or
   mount it writable. The repository tool deliberately does not unmount,
   eject, elevate privileges or choose a disk for you.
3. Capture and hash a whole-card byte image with the approved macOS tool below.
   Keep that image outside the repository with restricted permissions.
   `custody.json` must say `complete: true`; merely having an `.img` or
   `.partial` file is not custody evidence.
4. Mount the byte image, or the card if an image mount is unavailable, as an
   explicitly read-only filesystem. Record the exact mounted device and mount
   root. A directory inside a mount is not acceptable.
5. Put the export destination on a different filesystem. It must be a new,
   non-existing path.

## Approved macOS whole-card custody tool

The tool accepts only an explicit whole-device `/dev/diskN`; it rejects
`/dev/rdiskN`, partitions, globs, internal/boot/virtual media, APFS or
system-role media, indeterminate/non-removable media, and any mounted member.
It also rejects an output inside this repository, an existing output, a broad
output path, and insufficient destination capacity. No default disk or
environment-variable fallback exists.

Use macOS inventory only to identify and visually confirm the card, then
explicitly unmount that exact whole disk. Replace `N` only after comparing the
reported size/media with the physical card. These are separate operator
commands; the capture tool will not run them for you:

```sh
/usr/sbin/diskutil list external physical
/usr/sbin/diskutil info /dev/diskN
/usr/sbin/diskutil unmountDisk /dev/diskN
```

First inspect metadata. This phase does not open the raw device or create the
output directory:

```sh
node firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs \
  --mode inspect \
  --disk /dev/diskN \
  --output /absolute/new/custody-directory-outside-the-repository
```

Visually compare `identity`, `expectSizeBytes`, the physical card label and the
reader you just inserted. Never copy values from an older run or guess a disk
number. Then copy the exact byte count and lowercase fingerprint printed by
that same inspection into capture mode:

```sh
node firmware/inkloop-idf/tools/capture_tf_whole_card_macos.mjs \
  --mode capture \
  --disk /dev/diskN \
  --output /absolute/new/custody-directory-outside-the-repository \
  --expect-size-bytes '<exact expectSizeBytes from inspect>' \
  --confirm-fingerprint '<exact confirmFingerprint from inspect>'
```

The program never invokes `sudo`. If the raw device cannot be opened with the
current account, it fails after leaving `custody.json` as `complete: false`;
do not reuse that output directory. Privilege changes are an explicit operator
decision outside this tool, never an automatic retry. Re-run both phases and
re-confirm the live identity if anything was unplugged, remounted or changed.

Capture performs one complete raw-device read into `tf-whole-card.img`, a
second complete raw-device read into SHA-256, and an independent image hash.
All three digests must match the exact `diskutil` capacity. It re-runs and
compares the normalized `diskutil info` identity before, between and after the
reads. The final durable write atomically changes `custody.json` from
`complete: false` to `complete: true`. A short read, changed identity, remount,
hash mismatch, destination race or write failure therefore cannot authorize
flashing.

An incomplete run retains `captureStage`, the available source/image digest
summaries and their pairwise agreement in `custody.json`. These fields are
diagnostic evidence only: they can distinguish unstable source reads from a
changed destination image, but `complete: false` never authorizes staging.

Retain together, outside the repository:

- `tf-whole-card.img` and `SHA256SUMS`;
- `custody.json` with `complete: true`, exact byte size, source-pass hashes and
  stable fingerprint;
- the captured before/pre-read/between/after `diskutil` target and member
  metadata.

The repository tool never writes the source. It refuses a writable mount, an
unexpected mounted device, a destination on the source filesystem, a missing
or changed candidate, non-divergent candidates, an unsafe album path, a
missing asset, an asset size disagreement, or a content hash mismatch.

## Offline export command

Copy the three lowercase SHA-256 values from the authenticated Recovery page.
They identify the physical Current, Next, and Previous files; they do not
assert ordering or recency.

```sh
export INKLOOP_TF_READONLY_MOUNT='/absolute/read-only/mount'
export INKLOOP_TF_MOUNTED_DEVICE='/dev/exact-mounted-partition'
export INKLOOP_TF_EXPORT_DIR='/absolute/separate-volume/new-export-directory'

node firmware/inkloop-idf/tools/export_tf_album_recovery.mjs \
  --source "$INKLOOP_TF_READONLY_MOUNT" \
  --output "$INKLOOP_TF_EXPORT_DIR" \
  --expect-device "$INKLOOP_TF_MOUNTED_DEVICE" \
  --expect-current-sha256 '<Current SHA-256 from Recovery>' \
  --expect-next-sha256 '<Next SHA-256 from Recovery>' \
  --expect-previous-sha256 '<Previous SHA-256 from Recovery>'
```

Do not use a guessed device, unresolved variable, `/`, a home directory, or a
broad workspace path. The CLI checks the live mount table and requires the
source to be its exact read-only root.

## Required evidence

A successful export contains:

- `candidates/index.json`, `candidates/index.next`, and
  `candidates/index.prev`, copied byte-for-byte;
- the deduplicated union of every asset referenced by all three candidates in
  `assets/`;
- `SHA256SUMS` for every copied candidate and asset;
- `manifest.json` with `complete: true`, all three candidate hashes, per-slot
  asset counts, the unique asset count, and every copied file hash.

The exporter re-reads and re-hashes the complete source set before and after
copying. A failure after destination creation leaves `INCOMPLETE.json`; that
directory is not valid backup evidence and must never authorize selection.

Only after the whole-card image and its hash are retained may a beta31-or-newer
image be staged. After the exact accepted candidate's Recovery boots, the HTTP
logical export may be used as additional selection evidence; the offline
exporter may instead validate the mounted image. Before submitting exactly one
Current/Next/Previous choice, confirm that the mandatory external whole-card
custody exists. The
removable-album resolve API rejects requests without
`backup=verified_external`. Other recovery domains require
`backup=not_required` so the assertion cannot be accidentally reused.

The checkbox is an operator assertion, not proof that bytes were copied. Keep
the raw whole-card image and its digest as primary custody. Also keep
`manifest.json` and `SHA256SUMS` from whichever logical export is used.
