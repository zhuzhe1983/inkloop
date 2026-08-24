# C151 deferred-TF app0-only staging gate

Status: **Stage A implemented for review; Stage B NOT AUTHORIZED / NOT
IMPLEMENTED.** This document and tool do not authorize a physical run by
themselves. Do not execute either stage until the exact external receipts have
been independently reviewed and the operator deliberately starts that stage.

This is a separately named, narrower policy for one exceptional operation:
staging an exact beta31 application into inactive app0 while the TF card is
continuously witnessed physically absent and its whole-card custody is still
deferred. It does not modify or relax
[`C151_PHYSICAL_ACCEPTANCE.md`](C151_PHYSICAL_ACCEPTANCE.md) or the existing
`c151_inactive_app0_gate.py` whole-card policy. Complete whole-card custody is
still mandatory before any later TF access and, under the current safety
review, before any selector or boot authorization.

## Hard boundary

`tools/c151_deferred_tf_app0_gate.py` has a distinct policy ID:

```text
m5-papercolor-c151-beta31-app0-stage-deferred-tf-v1
```

Stage A authorizes at most one attempted mutation:

- the exact externally receipt-bound beta31 application at inactive
  `app0 @ 0x10000`;
- the affected byte count is the smallest 4 KiB-aligned range containing the
  candidate; and
- bytes between the exact candidate end and aligned range end must read back
  as `0xff`.

Every other internal-flash byte must match the fresh pre-write 16 MiB image.
That includes the bootloader and partition table, NVS, all otadata, the app0
suffix, beta27 app1 rollback, LittleFS and the flash tail/coredump. TF must
remain absent and inaccessible.

Stage A emits no selector or rollback bytes and no selector, reset or boot
argv. Its scope values are exact and fail closed:

```json
{
  "authorized_for_app0_stage_only": true,
  "authorized_for_selector": false,
  "authorized_for_reset": false,
  "authorized_for_boot": false,
  "authorized_for_tf_access": false,
  "terminal_scope": "download-mode-no-reset",
  "tf_custody_status": "deferred"
}
```

The original tool's `authorize-selector` rejects this policy and status. Do
not translate, rename or copy a deferred-TF authorization into the original
whole-card workflow.

## Required Stage A evidence

The authorizer requires every input explicitly; there are no default receipt,
candidate, capture or authorization paths.

1. A private, single-link, owner-controlled external staging receipt for this
   policy. It binds the exact beta31 commit, version, candidate absolute path,
   byte count and SHA-256 plus the exact fresh independent PASS acceptance
   result. It authorizes app0 staging only and explicitly denies selector,
   reset, boot and TF access.
2. The exact candidate and acceptance files. The candidate must be one private,
   single-link, owner-controlled inode so it can be snapshotted without path
   substitution. In addition to the existing acceptance checks,
   `constraint_compliance.removable_media_accessed` must be present and exactly
   `false`; missing, null or true is rejected.
3. The established internal baseline custody proving the exact 16 MiB baseline,
   beta25 app0 and beta27 app1 rollback.
4. A fresh capture made under the separate policy. It must contain the fixed
   read-only identity sequence plus a complete 16 MiB image that exactly
   matches the internal baseline. The authorization binds the capture
   directory's canonical path, filesystem device and inode.
5. A private, single-link, owner-controlled external operator authorization,
   valid for no more than 24 hours. A different operator and independent
   reviewer must acknowledge the same binding.
6. A private continuous-absence witness artifact bound by path, bytes and
   SHA-256. The authorization must attest that the unit was powered off before
   removal, the card is physically removed and sequestered, and neither its
   device nor host interface has been accessed since removal.

The external operator authorization schema is strict: unknown and missing
fields fail. It binds `schema`, policy/status, a 64-hex authorization ID, the
exact decision phrase, creation/expiry, distinct operator/reviewer
acknowledgements, source/application/acceptance/staging/baseline/capture/device
bindings, the TF-absence evidence and every scope value above. Its
`binding_sha256` is the canonical JSON SHA-256 of all fields except
`binding_sha256` itself.

Every JSON authority is opened once with `O_NOFOLLOW`, bounded to 1 MiB, read
from that one descriptor, and checked with before/after `fstat`. JSON semantics,
byte count and SHA-256 all come from the same immutable snapshot; the path must
still name the opened inode after hashing. This applies to staging, acceptance,
baseline, operator and generated app authorization. Capture manifest, identity
logs and full-flash bytes use the equivalent relative-open snapshot under the
held capture-directory descriptor. A same-length path swap cannot combine the
semantics of one file with the hash of another.

The exact decision phrase is:

```text
authorize one app0-only staging attempt while TF custody remains deferred
```

An unsigned JSON file cannot prove that the physical statements are true. The
tool proves that two explicitly named people bound the same fresh evidence;
the people remain responsible for witnessing physical removal and continuous
absence. Placeholder identities or a single person in both roles are rejected.

## Review and controlled execution

The offline authorizer command is
`authorize-app0-stage-deferred-tf`. On success it emits only:

- `candidate-app0.bin`, a private staged copy;
- `app0-stage-authorization.json`; and
- `app0-stage-plan.json` containing the sole controlled Stage A argv.

The only write-capable command in this policy is
`execute-app0-stage-deferred-tf`. Before a device operation it reloads and
re-hashes the entire authorization chain, checks freshness, revalidates the
exact candidate and expected full-flash images, and atomically creates
`deferred-tf-app0-stage-attempt.json` inside the fresh capture directory. That
marker consumes the capture/authorization before device execution. The
executor holds an `O_DIRECTORY|O_NOFOLLOW` descriptor for the validated
capture-directory inode, creates the marker relative to that descriptor with
`O_EXCL|O_NOFOLLOW`, then fsyncs both marker and directory. Renaming the capture
path to a decoy cannot redirect the marker. Never delete it. A failure,
interruption or incomplete post-readback requires a new read-only capture and
new operator authorization; blind retry is forbidden.

Candidate staging reads one exact verified snapshot from a single
`O_NOFOLLOW` file descriptor, writes that snapshot into the private temporary
gate directory, verifies its size/hash/version, constructs the after-image
from that staged copy, and revalidates the staged copy immediately before the
directory is published.

The controlled executor reuses the original gate's reviewed execution
primitive. It locks the exact port, verifies MAC/chip/flash/security identity,
reads and byte-compares all 16 MiB immediately before the write, seals the
private candidate into an unlinked inherited FD, uses `no_reset` before and
after the sole app0 write, reads all 16 MiB again, and rejects any byte outside
the unique app0 after-image. It cannot accept this authorization at another
offset.

After a Stage A PASS, leave the device in Download mode with TF absent. Stage A
completion is not a candidate selection, first boot, boot-health pass, Product
pass, TF custody pass or release pass.

## Stage B contract — deliberately non-executable

The `authorize-stage-b` command always exits blocked and creates no output.
There is no selector binary, selector argv, reset argv or boot argv in this
policy. This remains true even if every command-line path appears valid.

A future independent review must finalize and implement a separate Stage B
policy. At minimum it must require and mechanically bind:

- the exact successful Stage A execution receipt and full 16 MiB post-readback;
- a new fresh 16 MiB preboot capture exactly equal to the Stage A after-image;
- completed whole-card TF custody and same-card chain evidence required by the
  current safety review, while the card remains physically absent for first
  boot;
- a new, non-replayable operator/reviewer authorization that explicitly
  accepts the reviewed otadata update and acknowledges that normal pending
  candidate boot can write NVS and LittleFS;
- a reviewed seq3/`NEW` selector plan that preserves seq2/`VALID` and the exact
  beta27 app1 rollback bytes;
- a bounded first-boot/30-second boot-health evidence plan, automatic rollback
  evidence on failure, and post-boot evidence rules that distinguish permitted
  otadata/NVS/LittleFS changes from every other protected range; and
- invalidation after any reset, boot attempt, TF contact, otadata transition,
  unexpected byte drift or evidence mismatch.

Those post-boot mutation and evidence rules are not safely represented by the
Stage A exact-after-image model. Until the separate schema receives independent
approval and tests, Stage B remains `not-authorized-not-implemented`.

## Offline regression tests

`tests/test_c151_deferred_tf_app0_gate.py` is entirely simulated and never
opens a device or removable medium. It covers:

- distinct policy/status and rejection by the original selector authorizer;
- exact aligned app0 range plus byte-for-byte preservation of every other
  range and full-flash mismatch rejection;
- stale, fake, mismatched and scope-escalated authorization;
- mandatory `removable_media_accessed: false`;
- repository-internal, public-mode and hard-linked authority rejection;
- one-time attempt consumption/replay rejection;
- capture-parent rename/decoy restoration replay rejection and candidate
  mutation at the staging-copy boundary; and
- an exact same-length staging-receipt `authorized_for_selector: false` to
  `true ` swap at the
  former semantic/hash boundary, rejected before output and before execution;
- unconditional Stage B blocking with no selector/reset/boot command surface.
