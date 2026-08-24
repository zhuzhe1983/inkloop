#!/usr/bin/env python3
"""Fail-closed app0-only staging while C151 TF custody is deferred.

This is deliberately a different policy and CLI from
``c151_inactive_app0_gate.py``.  It permits one reviewed, erase-aligned app0
candidate write while the TF card is witnessed physically absent.  It never
emits or executes selector, reset or boot actions.  Stage B is intentionally
not implemented: completing Stage A does not authorize selecting or booting
the candidate.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
from typing import Any, Iterable, Iterator


def _load_original_gate() -> Any:
    """Load the reviewed byte/range primitives without changing its policy."""
    module_name = "_inkloop_c151_inactive_app0_gate_core"
    loaded = sys.modules.get(module_name)
    if loaded is not None:
        return loaded
    path = Path(__file__).with_name("c151_inactive_app0_gate.py")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - installation defect.
        raise RuntimeError(f"cannot load original C151 gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


core = _load_original_gate()
GateError = core.GateError
GatePolicy = core.GatePolicy

POLICY_ID = "m5-papercolor-c151-beta31-app0-stage-deferred-tf-v1"
APP_STAGE_STATUS = "authorized-app0-stage-only-deferred-tf"
OPERATOR_STATUS = "operator-authorized-app0-stage-only-deferred-tf"
STAGE_B_STATUS = "not-authorized-not-implemented"
TERMINAL_SCOPE = "download-mode-no-reset"
OPERATOR_DECISION = (
    "authorize one app0-only staging attempt while TF custody remains deferred"
)
AUTHORIZATION_SCHEMA = 1
STAGING_RECEIPT_SCHEMA = 1
AUTHORIZATION_MAX_AGE = dt.timedelta(hours=24)
STAGING_RECEIPT_MAX_AGE = dt.timedelta(days=7)
FUTURE_SKEW = dt.timedelta(minutes=5)
ATTEMPT_MARKER_NAME = "deferred-tf-app0-stage-attempt.json"
AUTHORIZATION_ID_RE = re.compile(r"^[0-9a-f]{64}$")
CARD_CHAIN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{15,127}$")
PLACEHOLDER_RE = re.compile(
    r"(?i)^(?:operator|reviewer|unknown|unset|none|null|n/?a|todo|tbd|replace(?:-?me)?|example)$"
)
UTC_TIMESTAMP_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?(?:Z|\+00:00)$"
)
REPO_ROOT = Path(__file__).resolve().parents[3]

DEFERRED_TF_POLICY = dataclasses.replace(
    core.PRODUCTION_POLICY,
    policy_id=POLICY_ID,
)


def _strict_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise GateError(f"{label} fields mismatch: missing={missing}, extra={extra}")


def _utc_timestamp(value: Any, label: str) -> dt.datetime:
    if not isinstance(value, str) or not UTC_TIMESTAMP_RE.fullmatch(value):
        raise GateError(
            f"{label} must be an explicit UTC timestamp ending in Z or +00:00"
        )
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = dt.datetime.fromisoformat(normalized)
    except ValueError as exc:
        raise GateError(f"{label} is invalid") from exc
    if parsed.tzinfo is None or parsed.utcoffset() != dt.timedelta(0):
        raise GateError(f"{label} is not UTC")
    return parsed


def _fresh_window(
    created_value: Any,
    expires_value: Any,
    *,
    now: dt.datetime,
    maximum_age: dt.timedelta,
    label: str,
) -> tuple[dt.datetime, dt.datetime]:
    created = _utc_timestamp(created_value, f"{label} created_at_utc")
    expires = _utc_timestamp(expires_value, f"{label} expires_at_utc")
    if expires <= created:
        raise GateError(f"{label} expiry must be after creation")
    if expires - created > maximum_age:
        raise GateError(f"{label} validity exceeds {maximum_age}")
    if created > now + FUTURE_SKEW:
        raise GateError(f"{label} was created in the future")
    if now < created - FUTURE_SKEW or now > expires:
        raise GateError(f"{label} is stale or not yet valid")
    return created, expires


def _fresh_observation(
    value: Any,
    *,
    now: dt.datetime,
    maximum_age: dt.timedelta,
    label: str,
) -> dt.datetime:
    observed = _utc_timestamp(value, label)
    if observed > now + FUTURE_SKEW:
        raise GateError(f"{label} is in the future")
    if now - observed > maximum_age:
        raise GateError(f"{label} is stale")
    return observed


def _identity(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise GateError(f"{label} must be a string")
    normalized = value.strip()
    if len(normalized) < 3 or len(normalized) > 128 or PLACEHOLDER_RE.fullmatch(
        normalized
    ):
        raise GateError(f"{label} is empty or a placeholder")
    return normalized


@dataclasses.dataclass(frozen=True)
class FileSnapshot:
    path: Path
    data: bytes
    sha256: str
    device: int
    inode: int

    def binding(self) -> dict[str, Any]:
        return {
            "path": str(self.path),
            "bytes": len(self.data),
            "sha256": self.sha256,
        }


def _read_snapshot_fd(
    descriptor: int,
    *,
    opened: os.stat_result,
    maximum_bytes: int,
    label: str,
) -> tuple[bytes, str]:
    if opened.st_size > maximum_bytes:
        raise GateError(f"{label} exceeds {maximum_bytes} bytes")
    chunks: list[bytes] = []
    remaining = opened.st_size
    while remaining:
        chunk = os.read(descriptor, min(1024 * 1024, remaining))
        if not chunk:
            raise GateError(f"{label} ended before its recorded byte count")
        chunks.append(chunk)
        remaining -= len(chunk)
    if os.read(descriptor, 1):
        raise GateError(f"{label} exceeds its recorded byte count")
    data = b"".join(chunks)
    return data, core.sha256_bytes(data)


def _validate_snapshot_metadata(
    metadata: os.stat_result, *, require_private: bool, label: str
) -> None:
    if not stat.S_ISREG(metadata.st_mode):
        raise GateError(f"{label} must be a regular file")
    if require_private:
        if metadata.st_uid != os.geteuid():
            raise GateError(f"{label} must be owned by the executing user")
        if metadata.st_nlink != 1:
            raise GateError(f"{label} must have exactly one filesystem link")
        if stat.S_IMODE(metadata.st_mode) & 0o077:
            raise GateError(f"{label} must be private mode 0600 or stricter")


def _snapshot_regular(
    path_value: str | Path,
    label: str,
    *,
    maximum_bytes: int,
    require_private: bool,
) -> FileSnapshot:
    path = Path(path_value).absolute()
    try:
        before_path = path.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"{label} is missing: {path}") from exc
    if stat.S_ISLNK(before_path.st_mode):
        raise GateError(f"{label} must not be a symlink: {path}")
    _validate_snapshot_metadata(
        before_path, require_private=require_private, label=label
    )
    no_follow = getattr(os, "O_NOFOLLOW", None)
    if no_follow is None:
        raise GateError(f"platform cannot securely snapshot {label}")
    flags = os.O_RDONLY | no_follow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise GateError(f"cannot securely open {label}: {path}") from exc
    try:
        opened = os.fstat(descriptor)
        _validate_snapshot_metadata(
            opened, require_private=require_private, label=label
        )
        if (opened.st_dev, opened.st_ino) != (
            before_path.st_dev,
            before_path.st_ino,
        ):
            raise GateError(f"{label} changed while it was opened")
        data, digest = _read_snapshot_fd(
            descriptor,
            opened=opened,
            maximum_bytes=maximum_bytes,
            label=label,
        )
        after_fd = os.fstat(descriptor)
        if (
            after_fd.st_dev,
            after_fd.st_ino,
            after_fd.st_size,
            after_fd.st_mtime_ns,
            after_fd.st_ctime_ns,
        ) != (
            opened.st_dev,
            opened.st_ino,
            opened.st_size,
            opened.st_mtime_ns,
            opened.st_ctime_ns,
        ):
            raise GateError(f"{label} changed while it was snapshotted")
        try:
            after_path = path.lstat()
        except FileNotFoundError as exc:
            raise GateError(f"{label} path changed while it was snapshotted") from exc
        if (after_path.st_dev, after_path.st_ino) != (
            opened.st_dev,
            opened.st_ino,
        ):
            raise GateError(f"{label} path changed while it was snapshotted")
        resolved = path.resolve(strict=True)
        resolved_metadata = resolved.stat()
        if (resolved_metadata.st_dev, resolved_metadata.st_ino) != (
            opened.st_dev,
            opened.st_ino,
        ):
            raise GateError(f"{label} resolved path changed while it was snapshotted")
        return FileSnapshot(
            path=resolved,
            data=data,
            sha256=digest,
            device=opened.st_dev,
            inode=opened.st_ino,
        )
    finally:
        os.close(descriptor)


def _snapshot_regular_at(
    directory_fd: int,
    directory_identity: dict[str, Any],
    name: str,
    label: str,
    *,
    maximum_bytes: int,
    require_private: bool,
) -> FileSnapshot:
    if Path(name).name != name or name in {"", ".", ".."}:
        raise GateError(f"{label} has an invalid capture member name")
    try:
        before_path = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError as exc:
        raise GateError(f"{label} is missing from capture directory") from exc
    if stat.S_ISLNK(before_path.st_mode):
        raise GateError(f"{label} must not be a symlink")
    _validate_snapshot_metadata(
        before_path, require_private=require_private, label=label
    )
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    no_follow = getattr(os, "O_NOFOLLOW", None)
    if no_follow is None:
        raise GateError(f"platform cannot securely snapshot {label}")
    flags |= no_follow
    try:
        descriptor = os.open(name, flags, dir_fd=directory_fd)
    except OSError as exc:
        raise GateError(f"cannot securely open capture member {label}") from exc
    try:
        opened = os.fstat(descriptor)
        _validate_snapshot_metadata(
            opened, require_private=require_private, label=label
        )
        if (opened.st_dev, opened.st_ino) != (
            before_path.st_dev,
            before_path.st_ino,
        ):
            raise GateError(f"{label} changed while it was opened")
        data, digest = _read_snapshot_fd(
            descriptor,
            opened=opened,
            maximum_bytes=maximum_bytes,
            label=label,
        )
        after_fd = os.fstat(descriptor)
        if (
            after_fd.st_dev,
            after_fd.st_ino,
            after_fd.st_size,
            after_fd.st_mtime_ns,
            after_fd.st_ctime_ns,
        ) != (
            opened.st_dev,
            opened.st_ino,
            opened.st_size,
            opened.st_mtime_ns,
            opened.st_ctime_ns,
        ):
            raise GateError(f"{label} changed while it was snapshotted")
        after_path = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if (after_path.st_dev, after_path.st_ino) != (
            opened.st_dev,
            opened.st_ino,
        ):
            raise GateError(f"{label} path changed while it was snapshotted")
        return FileSnapshot(
            path=Path(directory_identity["path"]) / name,
            data=data,
            sha256=digest,
            device=opened.st_dev,
            inode=opened.st_ino,
        )
    finally:
        os.close(descriptor)


def _json_from_snapshot(
    snapshot: FileSnapshot, label: str
) -> dict[str, Any]:
    try:
        value = json.loads(
            snapshot.data.decode("utf-8", errors="strict"),
            parse_constant=lambda constant: (_ for _ in ()).throw(
                ValueError(f"invalid JSON constant {constant}")
            ),
        )
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as exc:
        raise GateError(f"{label} is not strict JSON") from exc
    if not isinstance(value, dict):
        raise GateError(f"{label} must contain a JSON object")
    return value


def _snapshot_json(
    path_value: str | Path,
    label: str,
    *,
    require_private: bool,
) -> tuple[FileSnapshot, dict[str, Any]]:
    snapshot = _snapshot_regular(
        path_value,
        label,
        maximum_bytes=1024 * 1024,
        require_private=require_private,
    )
    return snapshot, _json_from_snapshot(snapshot, label)


def _binding(path_value: str | Path, label: str) -> dict[str, Any]:
    return _snapshot_regular(
        path_value,
        label,
        maximum_bytes=64 * 1024 * 1024,
        require_private=False,
    ).binding()


def _expect_binding(
    actual: Any, expected: dict[str, Any], label: str
) -> dict[str, Any]:
    if not isinstance(actual, dict):
        raise GateError(f"{label} binding is missing")
    _strict_keys(actual, {"path", "bytes", "sha256"}, f"{label} binding")
    core._expect(actual, expected, f"{label} binding")
    return actual


def _require_external(path_value: str | Path, label: str) -> Path:
    path = _snapshot_regular(
        path_value,
        label,
        maximum_bytes=64 * 1024 * 1024,
        require_private=False,
    ).path
    try:
        path.relative_to(REPO_ROOT)
    except ValueError:
        return path
    raise GateError(f"{label} must remain outside the repository")


def _require_private_external(path_value: str | Path, label: str) -> Path:
    snapshot = _snapshot_regular(
        path_value,
        label,
        maximum_bytes=64 * 1024 * 1024,
        require_private=True,
    )
    try:
        snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        return snapshot.path
    raise GateError(f"{label} must remain outside the repository")


def _validate_acceptance_no_removable_access(
    path_value: str | Path, policy: GatePolicy
) -> dict[str, Any]:
    core._require_bound_policy(policy)
    snapshot, result = _snapshot_json(
        path_value, "acceptance result", require_private=False
    )
    try:
        snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        pass
    else:
        raise GateError("acceptance result must remain outside the repository")
    core._expect(result.get("status"), "pass", "acceptance status")
    core._expect(result.get("commit"), policy.commit, "acceptance commit")
    reviewed_at = result.get("reviewed_at_utc")
    if not isinstance(reviewed_at, str) or not reviewed_at.endswith("Z"):
        raise GateError("acceptance review timestamp is missing or not UTC")
    reviewed = _utc_timestamp(reviewed_at, "acceptance reviewed_at_utc")
    now = dt.datetime.now(dt.timezone.utc)
    if reviewed > now + core.ACCEPTANCE_FUTURE_SKEW:
        raise GateError("acceptance review timestamp is in the future")
    if now - reviewed > core.ACCEPTANCE_MAX_AGE:
        raise GateError("acceptance result is stale; run a fresh acceptance")
    checks = result.get("checks")
    if not isinstance(checks, dict):
        raise GateError("acceptance checks are missing")
    commit_identity = checks.get("commit_identity")
    if not isinstance(commit_identity, dict):
        raise GateError("acceptance commit identity is missing")
    core._expect(commit_identity.get("status"), "pass", "commit identity status")
    core._expect(commit_identity.get("expected"), policy.commit, "expected commit identity")
    core._expect(commit_identity.get("actual"), policy.commit, "actual commit identity")
    worktree = checks.get("worktree")
    if not isinstance(worktree, dict) or worktree.get("status") != "pass":
        raise GateError("acceptance clean-worktree proof is missing")
    reproducible = checks.get("reproducible_binaries")
    if not isinstance(reproducible, dict):
        raise GateError("reproducible binary receipt is missing")
    core._expect(reproducible.get("status"), "pass", "reproducible binary status")
    artifacts = reproducible.get("artifacts")
    if not isinstance(artifacts, list):
        raise GateError("reproducible artifacts are missing")
    matching = [
        item
        for item in artifacts
        if isinstance(item, dict)
        and item.get("sha256") == policy.candidate_sha256
        and item.get("bytes") == policy.candidate_bytes
        and str(item.get("name", "")).startswith("c151-")
    ]
    if len(matching) != 2 or len({item.get("name") for item in matching}) != 2:
        raise GateError("acceptance does not contain both exact reproducible C151 binaries")
    constraints = checks.get("constraint_compliance")
    if not isinstance(constraints, dict):
        raise GateError("acceptance constraint compliance is missing")
    core._expect(constraints.get("status"), "pass", "constraint compliance status")
    for key in (
        "tracked_files_modified",
        "device_accessed",
        "device_written_or_flashed",
        "push_performed",
        "removable_media_accessed",
    ):
        core._expect(constraints.get(key), False, f"acceptance constraint {key}")
    return {**snapshot.binding(), "reviewed_at_utc": reviewed_at}


def _validate_staging_receipt(
    path_value: str | Path,
    candidate_binding: dict[str, Any],
    acceptance_binding: dict[str, Any],
    policy: GatePolicy,
    *,
    now: dt.datetime,
) -> dict[str, Any]:
    snapshot, receipt = _snapshot_json(
        path_value, "deferred-TF staging receipt", require_private=True
    )
    try:
        snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        pass
    else:
        raise GateError("deferred-TF staging receipt must remain outside the repository")
    _strict_keys(
        receipt,
        {
            "schema",
            "policy_id",
            "release_train",
            "authorized_for_app0_stage_only",
            "authorized_for_selector",
            "authorized_for_reset",
            "authorized_for_boot",
            "authorized_for_tf_access",
            "reviewed_at_utc",
            "reviewer",
            "source",
            "application",
            "acceptance",
        },
        "deferred-TF staging receipt",
    )
    core._expect(receipt.get("schema"), STAGING_RECEIPT_SCHEMA, "staging schema")
    core._expect(receipt.get("policy_id"), POLICY_ID, "staging policy")
    core._expect(receipt.get("release_train"), "beta31", "staging release train")
    for key, expected in (
        ("authorized_for_app0_stage_only", True),
        ("authorized_for_selector", False),
        ("authorized_for_reset", False),
        ("authorized_for_boot", False),
        ("authorized_for_tf_access", False),
    ):
        core._expect(receipt.get(key), expected, f"staging {key}")
    reviewed_at = _fresh_observation(
        receipt.get("reviewed_at_utc"),
        now=now,
        maximum_age=STAGING_RECEIPT_MAX_AGE,
        label="staging reviewed_at_utc",
    )
    reviewer = receipt.get("reviewer")
    if not isinstance(reviewer, dict):
        raise GateError("staging reviewer is missing")
    _strict_keys(reviewer, {"identity", "independent"}, "staging reviewer")
    _identity(reviewer.get("identity"), "staging reviewer identity")
    core._expect(reviewer.get("independent"), True, "independent staging review")

    source = receipt.get("source")
    if not isinstance(source, dict):
        raise GateError("staging source binding is missing")
    _strict_keys(source, {"commit", "version"}, "staging source")
    core._expect(source.get("commit"), policy.commit, "staging source commit")
    core._expect(source.get("version"), policy.version, "staging source version")

    _expect_binding(receipt.get("application"), candidate_binding, "staging application")
    acceptance = receipt.get("acceptance")
    if not isinstance(acceptance, dict):
        raise GateError("staging acceptance binding is missing")
    _strict_keys(acceptance, {"path", "bytes", "sha256", "status"}, "staging acceptance")
    core._expect(acceptance.get("status"), "PASS", "staging acceptance status")
    _expect_binding(
        {key: acceptance.get(key) for key in ("path", "bytes", "sha256")},
        acceptance_binding,
        "staging acceptance",
    )
    return {
        **snapshot.binding(),
        "reviewed_at_utc": reviewed_at.isoformat().replace("+00:00", "Z"),
    }


@contextlib.contextmanager
def _opened_capture_directory(
    capture_dir_value: str | Path,
) -> Iterator[tuple[int, Path, dict[str, Any]]]:
    capture_dir = Path(capture_dir_value).absolute()
    try:
        before = capture_dir.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"capture directory is missing: {capture_dir}") from exc
    if stat.S_ISLNK(before.st_mode) or not stat.S_ISDIR(before.st_mode):
        raise GateError("capture directory must be a real directory")
    no_follow = getattr(os, "O_NOFOLLOW", None)
    directory_flag = getattr(os, "O_DIRECTORY", None)
    if no_follow is None or directory_flag is None:
        raise GateError("platform cannot securely bind the capture directory inode")
    flags = os.O_RDONLY | no_follow | directory_flag
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        descriptor = os.open(capture_dir, flags)
    except OSError as exc:
        raise GateError("cannot securely open the capture directory") from exc
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISDIR(opened.st_mode):
            raise GateError("opened capture evidence is not a directory")
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise GateError("capture directory changed while it was opened")
        if opened.st_uid != os.geteuid():
            raise GateError("capture directory must be owned by the executing user")
        if stat.S_IMODE(opened.st_mode) & 0o077:
            raise GateError("capture directory must be private mode 0700 or stricter")
        resolved = capture_dir.resolve(strict=True)
        resolved_metadata = resolved.stat()
        if (resolved_metadata.st_dev, resolved_metadata.st_ino) != (
            opened.st_dev,
            opened.st_ino,
        ):
            raise GateError("resolved capture directory does not match its held inode")
        identity = {
            "path": str(resolved),
            "device": opened.st_dev,
            "inode": opened.st_ino,
        }
        yield descriptor, resolved, identity
    finally:
        os.close(descriptor)


def _parse_capture_identity(
    snapshots: dict[str, FileSnapshot], policy: GatePolicy
) -> dict[str, Any]:
    texts: dict[str, str] = {}
    for name in ("read-mac.log", "chip-id.log", "flash-id.log", "security-info.log"):
        try:
            texts[name] = snapshots[name].data.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise GateError(f"{name} is not UTF-8") from exc
    combined = "\n".join(texts.values())
    macs = {
        core._canonical_mac(match)
        for match in re.findall(r"(?im)^MAC:\s*([0-9a-f:]{17})\s*$", combined)
    }
    core._expect(macs, {policy.expected_mac}, "captured MAC set")
    if not re.search(r"(?i)ESP32-S3.*revision\s+v?0\.2", combined):
        raise GateError("capture does not prove ESP32-S3 revision 0.2")
    flash_text = texts["flash-id.log"]
    for pattern, label in (
        (
            rf"(?im)^Manufacturer:\s*{re.escape(policy.flash_manufacturer)}\s*$",
            "flash manufacturer",
        ),
        (
            rf"(?im)^Device:\s*{re.escape(policy.flash_device)}\s*$",
            "flash device",
        ),
        (r"(?im)^Detected flash size:\s*16MB\s*$", "16 MiB flash size"),
    ):
        if not re.search(pattern, flash_text):
            raise GateError(f"capture does not prove expected {label}")
    security = texts["security-info.log"]
    if not re.search(r"(?im)^Secure Boot:\s*Disabled\s*$", security):
        raise GateError("Secure Boot is enabled or unproven")
    if not re.search(r"(?im)^Flash Encryption:\s*Disabled\s*$", security):
        raise GateError("Flash Encryption is enabled or unproven")
    return {
        "mac": policy.expected_mac,
        "chip": "ESP32-S3 revision 0.2",
        "flash_bytes": policy.flash_bytes,
        "secure_boot": "disabled",
        "flash_encryption": "disabled",
    }


def _validate_fresh_capture(
    capture_dir_value: str | Path,
    policy: GatePolicy,
    *,
    now: dt.datetime,
    capture_directory_fd: int | None = None,
    capture_directory_identity: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], dict[str, Any], bytes, dict[str, Any]]:
    if capture_directory_fd is None:
        with _opened_capture_directory(capture_dir_value) as (
            descriptor,
            capture_dir,
            directory_identity,
        ):
            return _validate_fresh_capture(
                capture_dir,
                policy,
                now=now,
                capture_directory_fd=descriptor,
                capture_directory_identity=directory_identity,
            )
    if capture_directory_identity is None:
        raise GateError("held capture directory identity is missing")
    capture_dir = Path(capture_directory_identity["path"])
    opened = os.fstat(capture_directory_fd)
    if (opened.st_dev, opened.st_ino) != (
        capture_directory_identity.get("device"),
        capture_directory_identity.get("inode"),
    ):
        raise GateError("held capture directory inode no longer matches authorization")
    names = (
        "read-mac.log",
        "chip-id.log",
        "flash-id.log",
        "security-info.log",
        "full-flash-before.log",
        "full-flash-before.bin",
    )
    snapshots = {
        name: _snapshot_regular_at(
            capture_directory_fd,
            capture_directory_identity,
            name,
            name,
            maximum_bytes=policy.flash_bytes if name.endswith(".bin") else 1024 * 1024,
            require_private=True,
        )
        for name in names
    }
    manifest_snapshot = _snapshot_regular_at(
        capture_directory_fd,
        capture_directory_identity,
        "capture-manifest.json",
        "capture manifest",
        maximum_bytes=1024 * 1024,
        require_private=True,
    )
    manifest = _json_from_snapshot(manifest_snapshot, "capture manifest")
    core._expect(manifest.get("complete"), True, "capture completion")
    core._expect(manifest.get("policy_id"), policy.policy_id, "capture policy")
    core._expect(manifest.get("port"), policy.expected_port, "capture port")
    operations = manifest.get("operations")
    expected_operations = [
        "read_mac",
        "chip_id",
        "flash_id",
        "get_security_info",
        "read_flash",
    ]
    core._expect(operations, expected_operations, "capture operation list")
    if any(operation in core.FORBIDDEN_ESPTOOL_OPERATIONS for operation in operations):
        raise GateError("capture manifest contains a forbidden operation")
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise GateError("capture file receipt is missing")
    for name, snapshot in snapshots.items():
        record = files.get(name)
        if not isinstance(record, dict):
            raise GateError(f"capture receipt for {name} is missing")
        core._expect(record.get("bytes"), len(snapshot.data), f"{name} bytes")
        core._expect_sha(record.get("sha256"), snapshot.sha256, f"{name} SHA-256")
    captured_at = _fresh_observation(
        manifest.get("captured_at_utc"),
        now=now,
        maximum_age=AUTHORIZATION_MAX_AGE,
        label="capture captured_at_utc",
    )
    identity = _parse_capture_identity(snapshots, policy)
    full_flash_snapshot = snapshots["full-flash-before.bin"]
    full_flash = full_flash_snapshot.data
    flash_state = core.validate_flash_before(full_flash, policy)
    try:
        after = capture_dir.stat()
    except FileNotFoundError as exc:
        raise GateError("capture directory path changed during validation") from exc
    if (after.st_dev, after.st_ino) != (opened.st_dev, opened.st_ino):
        raise GateError("capture directory path changed during validation")
    return (
        {
            "capture_directory": capture_directory_identity,
            "manifest_path": str(manifest_snapshot.path),
            "manifest_bytes": len(manifest_snapshot.data),
            "manifest_sha256": manifest_snapshot.sha256,
            "captured_at_utc": manifest["captured_at_utc"],
            "full_flash_path": str(full_flash_snapshot.path),
            "full_flash_bytes": len(full_flash_snapshot.data),
            "full_flash_sha256": full_flash_snapshot.sha256,
        },
        identity,
        full_flash,
        flash_state,
    )


def _operator_binding_payload(authorization: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value
        for key, value in authorization.items()
        if key != "binding_sha256"
    }


def _validate_tf_absence(
    value: Any,
    *,
    now: dt.datetime,
    authorization_created: dt.datetime,
    capture_created: dt.datetime,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise GateError("TF-absence attestation is missing")
    _strict_keys(
        value,
        {
            "removed_at_utc",
            "powered_off_before_removal",
            "physically_removed",
            "sequestered",
            "no_tf_device_or_host_access_since_removal",
            "continuous_witness",
            "card_chain_id",
        },
        "TF-absence attestation",
    )
    removed_at = _fresh_observation(
        value.get("removed_at_utc"),
        now=now,
        maximum_age=AUTHORIZATION_MAX_AGE,
        label="TF removed_at_utc",
    )
    if removed_at > capture_created + FUTURE_SKEW:
        raise GateError("TF removal was not witnessed before the fresh capture")
    if removed_at > authorization_created + FUTURE_SKEW:
        raise GateError("TF removal is later than operator authorization")
    for key in (
        "powered_off_before_removal",
        "physically_removed",
        "sequestered",
        "no_tf_device_or_host_access_since_removal",
    ):
        core._expect(value.get(key), True, f"TF absence {key}")
    chain_id = value.get("card_chain_id")
    if not isinstance(chain_id, str) or not CARD_CHAIN_ID_RE.fullmatch(chain_id):
        raise GateError("TF card_chain_id is missing, a placeholder or malformed")
    witness = value.get("continuous_witness")
    if not isinstance(witness, dict):
        raise GateError("continuous TF-absence witness binding is missing")
    witness_path_value = witness.get("path")
    if not isinstance(witness_path_value, str):
        raise GateError("continuous TF-absence witness path is missing")
    witness_snapshot = _snapshot_regular(
        witness_path_value,
        "continuous TF-absence witness",
        maximum_bytes=64 * 1024 * 1024,
        require_private=True,
    )
    try:
        witness_snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        pass
    else:
        raise GateError("continuous TF-absence witness must remain outside the repository")
    expected_witness = witness_snapshot.binding()
    if expected_witness["bytes"] <= 0:
        raise GateError("continuous TF-absence witness is empty")
    _expect_binding(witness, expected_witness, "continuous TF-absence witness")
    return {
        **value,
        "removed_at_utc": removed_at.isoformat().replace("+00:00", "Z"),
        "continuous_witness": expected_witness,
    }


def _validate_operator_authorization(
    path_value: str | Path,
    *,
    source_candidate: dict[str, Any],
    acceptance: dict[str, Any],
    baseline: dict[str, Any],
    staging_receipt: dict[str, Any],
    capture: dict[str, Any],
    policy: GatePolicy,
    now: dt.datetime,
) -> dict[str, Any]:
    snapshot, authorization = _snapshot_json(
        path_value, "operator authorization", require_private=True
    )
    try:
        snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        pass
    else:
        raise GateError("operator authorization must remain outside the repository")
    _strict_keys(
        authorization,
        {
            "schema",
            "policy_id",
            "status",
            "authorization_id",
            "decision",
            "created_at_utc",
            "expires_at_utc",
            "operator",
            "reviewer",
            "source",
            "application",
            "acceptance",
            "staging_receipt",
            "device",
            "baseline",
            "capture",
            "tf_absence",
            "authorized_for_app0_stage_only",
            "authorized_for_selector",
            "authorized_for_reset",
            "authorized_for_boot",
            "authorized_for_tf_access",
            "terminal_scope",
            "tf_custody_status",
            "binding_sha256",
        },
        "operator authorization",
    )
    core._expect(authorization.get("schema"), AUTHORIZATION_SCHEMA, "authorization schema")
    core._expect(authorization.get("policy_id"), POLICY_ID, "authorization policy")
    core._expect(authorization.get("status"), OPERATOR_STATUS, "authorization status")
    authorization_id = authorization.get("authorization_id")
    if not isinstance(authorization_id, str) or not AUTHORIZATION_ID_RE.fullmatch(
        authorization_id
    ):
        raise GateError("authorization_id must be 64 lowercase hexadecimal characters")
    core._expect(authorization.get("decision"), OPERATOR_DECISION, "operator decision")
    created, expires = _fresh_window(
        authorization.get("created_at_utc"),
        authorization.get("expires_at_utc"),
        now=now,
        maximum_age=AUTHORIZATION_MAX_AGE,
        label="operator authorization",
    )
    operator = authorization.get("operator")
    reviewer = authorization.get("reviewer")
    if not isinstance(operator, dict) or not isinstance(reviewer, dict):
        raise GateError("operator and reviewer acknowledgements are required")
    _strict_keys(operator, {"identity", "acknowledged_at_utc"}, "operator acknowledgement")
    _strict_keys(
        reviewer,
        {"identity", "acknowledged_at_utc", "independent"},
        "reviewer acknowledgement",
    )
    operator_identity = _identity(operator.get("identity"), "operator identity")
    reviewer_identity = _identity(reviewer.get("identity"), "reviewer identity")
    if reviewer_identity == operator_identity:
        raise GateError("operator and reviewer must be different identities")
    core._expect(reviewer.get("independent"), True, "independent reviewer acknowledgement")
    for party, acknowledgement in (("operator", operator), ("reviewer", reviewer)):
        acknowledged = _utc_timestamp(
            acknowledgement.get("acknowledged_at_utc"),
            f"{party} acknowledged_at_utc",
        )
        if acknowledged > now + FUTURE_SKEW:
            raise GateError(f"{party} acknowledgement is in the future")
        if acknowledged < created - FUTURE_SKEW or acknowledged > expires:
            raise GateError(f"{party} acknowledgement is outside authorization validity")

    source = authorization.get("source")
    if not isinstance(source, dict):
        raise GateError("operator source binding is missing")
    _strict_keys(source, {"commit", "version"}, "operator source")
    core._expect(source.get("commit"), policy.commit, "operator source commit")
    core._expect(source.get("version"), policy.version, "operator source version")
    expected_application = {
        "path": source_candidate["path"],
        "bytes": policy.candidate_bytes,
        "sha256": policy.candidate_sha256,
    }
    _expect_binding(authorization.get("application"), expected_application, "operator application")
    _expect_binding(
        authorization.get("acceptance"),
        {key: acceptance[key] for key in ("path", "bytes", "sha256")},
        "operator acceptance",
    )
    _expect_binding(
        authorization.get("staging_receipt"),
        {key: staging_receipt[key] for key in ("path", "bytes", "sha256")},
        "operator staging receipt",
    )
    baseline_expected = {key: baseline[key] for key in ("path", "bytes", "sha256")}
    _expect_binding(authorization.get("baseline"), baseline_expected, "operator baseline")

    device = authorization.get("device")
    if not isinstance(device, dict):
        raise GateError("operator device binding is missing")
    _strict_keys(device, {"mac", "port", "flash_bytes"}, "operator device")
    core._expect(device.get("mac"), policy.expected_mac, "operator device MAC")
    core._expect(device.get("port"), policy.expected_port, "operator device port")
    core._expect(device.get("flash_bytes"), policy.flash_bytes, "operator flash bytes")

    capture_auth = authorization.get("capture")
    if not isinstance(capture_auth, dict):
        raise GateError("operator capture binding is missing")
    core._expect(capture_auth, capture, "operator capture binding")
    capture_created = _utc_timestamp(capture["captured_at_utc"], "capture captured_at_utc")
    if capture_created > created + FUTURE_SKEW:
        raise GateError("operator authorization predates the fresh capture")
    tf_absence = _validate_tf_absence(
        authorization.get("tf_absence"),
        now=now,
        authorization_created=created,
        capture_created=capture_created,
    )
    for key, expected in (
        ("authorized_for_app0_stage_only", True),
        ("authorized_for_selector", False),
        ("authorized_for_reset", False),
        ("authorized_for_boot", False),
        ("authorized_for_tf_access", False),
        ("terminal_scope", TERMINAL_SCOPE),
        ("tf_custody_status", "deferred"),
    ):
        core._expect(authorization.get(key), expected, f"operator authorization {key}")
    expected_binding_sha = core.canonical_json_sha256(
        _operator_binding_payload(authorization)
    )
    core._expect_sha(
        authorization.get("binding_sha256"),
        expected_binding_sha,
        "operator authorization binding SHA-256",
    )
    return {
        **snapshot.binding(),
        "authorization_id": authorization_id,
        "created_at_utc": created.isoformat().replace("+00:00", "Z"),
        "expires_at_utc": expires.isoformat().replace("+00:00", "Z"),
        "operator": operator_identity,
        "reviewer": reviewer_identity,
        "tf_absence": tf_absence,
    }


def _forbidden_ranges(candidate: dict[str, Any], policy: GatePolicy) -> list[dict[str, Any]]:
    programmed = candidate["programmed_sector_bytes"]
    return [
        {"name": "bootloader-and-partition-table", "offset": 0, "end": policy.nvs_offset},
        {"name": "nvs", "offset": policy.nvs_offset, "bytes": policy.nvs_bytes},
        {"name": "otadata-no-selector", "offset": policy.otadata_offset, "bytes": policy.otadata_bytes},
        {
            "name": "app0-outside-programmed-range",
            "offset": policy.app0_offset + programmed,
            "end": policy.app0_offset + policy.app0_bytes,
        },
        {"name": "app1-beta27-rollback", "offset": policy.app1_offset, "bytes": policy.app1_bytes},
        {"name": "littlefs", "offset": policy.littlefs_offset, "bytes": policy.littlefs_bytes},
        {
            "name": "internal-flash-tail-including-coredump",
            "offset": policy.littlefs_offset + policy.littlefs_bytes,
            "end": policy.flash_bytes,
        },
        {"name": "tf-card-physically-absent", "scope": "entire-card"},
    ]


def _scope() -> dict[str, Any]:
    return {
        "authorized_for_app0_stage_only": True,
        "authorized_for_selector": False,
        "authorized_for_reset": False,
        "authorized_for_boot": False,
        "authorized_for_tf_access": False,
        "terminal_scope": TERMINAL_SCOPE,
        "tf_custody_status": "deferred",
    }


def _baseline_receipt(path_value: str | Path, policy: GatePolicy) -> dict[str, Any]:
    snapshot, custody = _snapshot_json(
        path_value, "baseline custody", require_private=False
    )
    core._expect(custody.get("complete"), True, "baseline custody completion")
    core._expect(custody.get("matching_full_reads"), True, "matching baseline reads")
    core._expect(custody.get("flash_bytes"), policy.flash_bytes, "baseline flash bytes")
    core._expect(
        core._canonical_mac(str(custody.get("device_mac", ""))),
        policy.expected_mac,
        "baseline MAC",
    )
    core._expect_sha(
        custody.get("full_flash_sha256"),
        policy.flash_sha256_before,
        "baseline full flash SHA-256",
    )
    slots = custody.get("slot_state")
    if not isinstance(slots, dict):
        raise GateError("baseline slot state is missing")
    app0 = slots.get("app0")
    app1 = slots.get("app1")
    if not isinstance(app0, dict) or not isinstance(app1, dict):
        raise GateError("baseline app slot evidence is missing")
    core._expect(app0.get("version"), "0.4.0-beta.25", "baseline app0 version")
    core._expect_sha(
        app0.get("sha256"), policy.app0_before_sha256, "baseline app0 SHA-256"
    )
    core._expect(app1.get("version"), "0.4.0-beta.27", "baseline app1 version")
    core._expect_sha(
        app1.get("sha256"),
        policy.app1_rollback_sha256,
        "baseline app1 SHA-256",
    )
    return snapshot.binding()


def _validated_candidate_snapshot(
    path_value: str | Path, policy: GatePolicy
) -> tuple[dict[str, Any], FileSnapshot]:
    snapshot = _snapshot_regular(
        path_value,
        "release candidate",
        maximum_bytes=policy.app0_bytes,
        require_private=True,
    )
    try:
        snapshot.path.relative_to(REPO_ROOT)
    except ValueError:
        pass
    else:
        raise GateError("release candidate must remain outside the repository")
    core._expect(len(snapshot.data), policy.candidate_bytes, "candidate byte count")
    core._expect_sha(
        snapshot.sha256, policy.candidate_sha256, "candidate snapshot SHA-256"
    )
    prefix = snapshot.data[: min(policy.candidate_bytes, 512 * 1024)]
    if policy.version.encode("ascii") not in prefix:
        raise GateError("candidate does not contain the expected release version")
    programmed_bytes = (policy.candidate_bytes + 0xFFF) & ~0xFFF
    if policy.candidate_bytes > policy.app0_bytes or programmed_bytes > policy.app0_bytes:
        raise GateError("candidate erase-aligned range does not fit app0")
    programmed = snapshot.data + b"\xff" * (programmed_bytes - len(snapshot.data))
    receipt = {
        "path": str(snapshot.path),
        "bytes": policy.candidate_bytes,
        "sha256": policy.candidate_sha256,
        "programmed_sector_bytes": programmed_bytes,
        "programmed_sector_sha256": core.sha256_bytes(programmed),
        "trailing_erased_bytes": programmed_bytes - policy.candidate_bytes,
    }
    return receipt, snapshot


def _make_app_after_image_from_snapshot(
    before_flash: bytes,
    candidate: dict[str, Any],
    snapshot: FileSnapshot,
    policy: GatePolicy,
) -> bytes:
    core._expect(len(before_flash), policy.flash_bytes, "before-image full flash length")
    core._expect(len(snapshot.data), policy.candidate_bytes, "candidate byte count")
    core._expect_sha(snapshot.sha256, policy.candidate_sha256, "candidate SHA-256")
    core._continuous_flash_ranges(candidate, policy)
    expected = bytearray(before_flash)
    programmed_end = policy.app0_offset + candidate["programmed_sector_bytes"]
    expected[policy.app0_offset:programmed_end] = b"\xff" * candidate[
        "programmed_sector_bytes"
    ]
    expected[
        policy.app0_offset : policy.app0_offset + len(snapshot.data)
    ] = snapshot.data
    return bytes(expected)


def authorize_app0_stage_deferred_tf(
    capture_dir_value: str | Path,
    candidate_value: str | Path,
    acceptance_value: str | Path,
    staging_receipt_value: str | Path,
    baseline_value: str | Path,
    operator_authorization_value: str | Path,
    output_value: str | Path,
    policy: GatePolicy = DEFERRED_TF_POLICY,
    *,
    now: dt.datetime | None = None,
) -> Path:
    """Emit one app0-only authorization; never emit selector/reset/boot data."""
    core._require_bound_policy(policy)
    current_time = now or dt.datetime.now(dt.timezone.utc)
    source_candidate, source_candidate_snapshot = _validated_candidate_snapshot(
        candidate_value, policy
    )
    acceptance = _validate_acceptance_no_removable_access(acceptance_value, policy)
    baseline = _baseline_receipt(baseline_value, policy)
    staging_receipt = _validate_staging_receipt(
        staging_receipt_value,
        {key: source_candidate[key] for key in ("path", "bytes", "sha256")},
        {key: acceptance[key] for key in ("path", "bytes", "sha256")},
        policy,
        now=current_time,
    )
    capture, identity, full_flash, flash_state = _validate_fresh_capture(
        capture_dir_value, policy, now=current_time
    )
    operator_authorization = _validate_operator_authorization(
        operator_authorization_value,
        source_candidate=source_candidate,
        acceptance=acceptance,
        baseline=baseline,
        staging_receipt=staging_receipt,
        capture=capture,
        policy=policy,
        now=current_time,
    )
    output, temporary = core._ensure_new_output(output_value)
    try:
        candidate_name = "candidate-app0.bin"
        copy_candidate, copy_snapshot = _validated_candidate_snapshot(
            source_candidate["path"], policy
        )
        core._expect(copy_candidate, source_candidate, "candidate copy-boundary receipt")
        core._expect(
            copy_snapshot.sha256,
            source_candidate_snapshot.sha256,
            "candidate copy-boundary snapshot",
        )
        core._write_private(temporary / candidate_name, copy_snapshot.data)
        staged_candidate, staged_snapshot = _validated_candidate_snapshot(
            temporary / candidate_name, policy
        )
        core._expect(
            {key: staged_candidate[key] for key in staged_candidate if key != "path"},
            {key: source_candidate[key] for key in source_candidate if key != "path"},
            "staged candidate snapshot",
        )
        candidate = {
            **staged_candidate,
            "path": str(output / candidate_name),
        }
        expected_after_bytes = _make_app_after_image_from_snapshot(
            full_flash, staged_candidate, staged_snapshot, policy
        )
        expected_after = core._after_image_expectation(
            expected_after_bytes,
            candidate,
            policy,
            core._app_permitted_delta(candidate, policy),
        )
        authorization = {
            "schema": AUTHORIZATION_SCHEMA,
            "status": APP_STAGE_STATUS,
            "policy_id": POLICY_ID,
            "created_at_utc": core.utc_now().replace("+00:00", "Z"),
            "expires_at_utc": operator_authorization["expires_at_utc"],
            "commit": policy.commit,
            "version": policy.version,
            "device": identity,
            "source_application": {
                "path": source_candidate["path"],
                "bytes": policy.candidate_bytes,
                "sha256": policy.candidate_sha256,
            },
            "candidate": candidate,
            "acceptance": acceptance,
            "staging_receipt": staging_receipt,
            "baseline": baseline,
            "capture": capture,
            "operator_authorization": operator_authorization,
            "before": {key: value for key, value in flash_state.items() if key != "otadata"},
            "slot_proof": {
                "selected": "app1",
                "inactive_target": "app0",
                "entry0_sequence": policy.current_app0_sequence,
                "entry1_sequence": policy.current_app1_sequence,
                "state": "VALID",
                "app1_beta27_rollback_sha256": policy.app1_rollback_sha256,
            },
            "scope": _scope(),
            "authorized_write": core._expected_app_write(candidate, policy),
            "expected_after_app_write": expected_after,
            "forbidden_ranges": _forbidden_ranges(candidate, policy),
            "attempt_marker": {
                "capture_directory": capture["capture_directory"],
                "name": ATTEMPT_MARKER_NAME,
            },
            "stage_b": {
                "status": STAGE_B_STATUS,
                "authorization_emitted": False,
                "implementation_available": False,
            },
        }
        core._write_json_private(
            temporary / "app0-stage-authorization.json", authorization
        )
        execution_output = output / "app0-execution"
        plan = {
            "status": "controlled-execution-only-app0-stage-deferred-tf",
            "policy_id": POLICY_ID,
            "warning": (
                "This plan authorizes one app0-only attempt. It does not authorize "
                "selector, reset, boot or TF access. Any failed attempt consumes it."
            ),
            "scope": _scope(),
            "port": policy.expected_port,
            "authorized_mutation": {
                "partition": "app0",
                "offset": policy.app0_offset,
                "input_bytes": policy.candidate_bytes,
                "input_sha256": policy.candidate_sha256,
                "flash_sectors_affected": {
                    "offset": policy.app0_offset,
                    "bytes": candidate["programmed_sector_bytes"],
                    "alignment_bytes": 0x1000,
                    "expected_sha256_after": candidate["programmed_sector_sha256"],
                },
            },
            "execute_app0_stage_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "execute-app0-stage-deferred-tf",
                *core._release_cli_args(policy),
                "--app-authorization",
                str(output / "app0-stage-authorization.json"),
                "--port",
                policy.expected_port,
                "--output-dir",
                str(execution_output),
            ],
            "automatic_full_flash_readback": str(
                execution_output / "full-flash-after-app0-stage.bin"
            ),
            "expected_after": expected_after,
            "failure_rule": "authorization-consumed-no-blind-retry",
            "stage_b": {
                "status": STAGE_B_STATUS,
                "authorization_emitted": False,
                "argv_emitted": False,
            },
        }
        core._write_json_private(temporary / "app0-stage-plan.json", plan)
        final_candidate, _ = _validated_candidate_snapshot(
            temporary / candidate_name, policy
        )
        core._expect(
            final_candidate,
            staged_candidate,
            "final staged candidate before publication",
        )
        core._publish_directory(output, temporary)
        return output
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _load_app_stage_authorization(
    path_value: str | Path,
    policy: GatePolicy,
    *,
    now: dt.datetime | None = None,
    capture_directory_fd: int | None = None,
    capture_directory_identity: dict[str, Any] | None = None,
) -> tuple[Path, dict[str, Any], dict[str, Any]]:
    core._require_bound_policy(policy)
    current_time = now or dt.datetime.now(dt.timezone.utc)
    authorization_snapshot, authorization = _snapshot_json(
        path_value, "deferred-TF app0 authorization", require_private=True
    )
    path = authorization_snapshot.path
    _strict_keys(
        authorization,
        {
            "schema",
            "status",
            "policy_id",
            "created_at_utc",
            "expires_at_utc",
            "commit",
            "version",
            "device",
            "source_application",
            "candidate",
            "acceptance",
            "staging_receipt",
            "baseline",
            "capture",
            "operator_authorization",
            "before",
            "slot_proof",
            "scope",
            "authorized_write",
            "expected_after_app_write",
            "forbidden_ranges",
            "attempt_marker",
            "stage_b",
        },
        "deferred-TF app0 authorization",
    )
    core._expect(authorization.get("schema"), AUTHORIZATION_SCHEMA, "app0 authorization schema")
    core._expect(authorization.get("status"), APP_STAGE_STATUS, "app0 authorization status")
    core._expect(authorization.get("policy_id"), POLICY_ID, "app0 authorization policy")
    core._expect(authorization.get("commit"), policy.commit, "app0 authorization commit")
    core._expect(authorization.get("version"), policy.version, "app0 authorization version")
    core._expect(authorization.get("scope"), _scope(), "app0 authorization scope")
    stage_b = authorization.get("stage_b")
    core._expect(
        stage_b,
        {
            "status": STAGE_B_STATUS,
            "authorization_emitted": False,
            "implementation_available": False,
        },
        "Stage B status",
    )

    source_application = authorization.get("source_application")
    if not isinstance(source_application, dict) or not isinstance(
        source_application.get("path"), str
    ):
        raise GateError("source application binding is missing")
    source_candidate, _ = _validated_candidate_snapshot(
        source_application["path"], policy
    )
    core._expect(
        source_application,
        {
            "path": source_candidate["path"],
            "bytes": policy.candidate_bytes,
            "sha256": policy.candidate_sha256,
        },
        "source application binding",
    )
    candidate = authorization.get("candidate")
    if not isinstance(candidate, dict) or not isinstance(candidate.get("path"), str):
        raise GateError("staged candidate binding is missing")
    staged_candidate, staged_snapshot = _validated_candidate_snapshot(
        candidate["path"], policy
    )
    core._expect(candidate, staged_candidate, "staged candidate binding")
    core._expect(
        authorization.get("authorized_write"),
        core._expected_app_write(candidate, policy),
        "authorized app0 write",
    )
    core._expect(
        authorization.get("forbidden_ranges"),
        _forbidden_ranges(candidate, policy),
        "forbidden ranges",
    )

    acceptance_stored = authorization.get("acceptance")
    if not isinstance(acceptance_stored, dict) or not isinstance(
        acceptance_stored.get("path"), str
    ):
        raise GateError("acceptance receipt is missing")
    acceptance = _validate_acceptance_no_removable_access(
        acceptance_stored["path"], policy
    )
    core._expect(acceptance_stored, acceptance, "acceptance receipt")
    baseline_stored = authorization.get("baseline")
    if not isinstance(baseline_stored, dict) or not isinstance(
        baseline_stored.get("path"), str
    ):
        raise GateError("baseline receipt is missing")
    baseline = _baseline_receipt(baseline_stored["path"], policy)
    core._expect(baseline_stored, baseline, "baseline receipt")
    staging_stored = authorization.get("staging_receipt")
    if not isinstance(staging_stored, dict) or not isinstance(
        staging_stored.get("path"), str
    ):
        raise GateError("staging receipt is missing")
    staging = _validate_staging_receipt(
        staging_stored["path"],
        {key: source_candidate[key] for key in ("path", "bytes", "sha256")},
        {key: acceptance[key] for key in ("path", "bytes", "sha256")},
        policy,
        now=current_time,
    )
    core._expect(staging_stored, staging, "staging receipt")
    capture_stored = authorization.get("capture")
    if not isinstance(capture_stored, dict) or not isinstance(
        capture_stored.get("manifest_path"), str
    ):
        raise GateError("capture receipt is missing")
    capture, identity, before_flash, flash_state = _validate_fresh_capture(
        Path(capture_stored["manifest_path"]).parent,
        policy,
        now=current_time,
        capture_directory_fd=capture_directory_fd,
        capture_directory_identity=capture_directory_identity,
    )
    core._expect(capture_stored, capture, "capture receipt")
    core._expect(authorization.get("device"), identity, "captured device identity")
    operator_stored = authorization.get("operator_authorization")
    if not isinstance(operator_stored, dict) or not isinstance(
        operator_stored.get("path"), str
    ):
        raise GateError("operator authorization receipt is missing")
    operator = _validate_operator_authorization(
        operator_stored["path"],
        source_candidate=source_candidate,
        acceptance=acceptance,
        baseline=baseline,
        staging_receipt=staging,
        capture=capture,
        policy=policy,
        now=current_time,
    )
    core._expect(operator_stored, operator, "operator authorization receipt")
    app_created = _utc_timestamp(
        authorization.get("created_at_utc"), "app0 authorization created_at_utc"
    )
    operator_created = _utc_timestamp(
        operator["created_at_utc"], "operator authorization created_at_utc"
    )
    operator_expires = _utc_timestamp(
        operator["expires_at_utc"], "operator authorization expires_at_utc"
    )
    if app_created < operator_created - FUTURE_SKEW or app_created > operator_expires:
        raise GateError("app0 authorization time is outside operator authorization validity")
    core._expect(
        authorization.get("expires_at_utc"),
        operator["expires_at_utc"],
        "app0 authorization expiry",
    )
    expected_before = {key: value for key, value in flash_state.items() if key != "otadata"}
    core._expect(authorization.get("before"), expected_before, "authorized before state")
    slot_proof = {
        "selected": "app1",
        "inactive_target": "app0",
        "entry0_sequence": policy.current_app0_sequence,
        "entry1_sequence": policy.current_app1_sequence,
        "state": "VALID",
        "app1_beta27_rollback_sha256": policy.app1_rollback_sha256,
    }
    core._expect(authorization.get("slot_proof"), slot_proof, "slot proof")
    expected_after = _make_app_after_image_from_snapshot(
        before_flash, candidate, staged_snapshot, policy
    )
    core._expect(
        authorization.get("expected_after_app_write"),
        core._after_image_expectation(
            expected_after,
            candidate,
            policy,
            core._app_permitted_delta(candidate, policy),
        ),
        "expected app0 after-image",
    )
    expected_marker = {
        "capture_directory": capture["capture_directory"],
        "name": ATTEMPT_MARKER_NAME,
    }
    core._expect(authorization.get("attempt_marker"), expected_marker, "attempt marker path")
    return path, authorization, {
        "candidate": candidate,
        "before_flash": before_flash,
        "expected_after": expected_after,
        "capture": capture,
        "attempt_marker": expected_marker,
        "capture_directory": capture["capture_directory"],
        "authorization_binding": authorization_snapshot.binding(),
    }


def _preflight_new_output(path_value: str | Path) -> Path:
    output = Path(path_value).resolve(strict=False)
    if output.exists() or output.is_symlink():
        raise GateError(f"output path already exists: {output}")
    parent = output.parent
    try:
        metadata = parent.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"output parent is missing: {parent}") from exc
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise GateError("output parent must be a real directory")
    return output


def _capture_directory_path_from_app_authorization(
    authorization_value: str | Path,
) -> Path:
    _, authorization = _snapshot_json(
        authorization_value,
        "deferred-TF app0 authorization",
        require_private=True,
    )
    capture = authorization.get("capture")
    if not isinstance(capture, dict):
        raise GateError("capture receipt is missing")
    directory = capture.get("capture_directory")
    if not isinstance(directory, dict) or not isinstance(directory.get("path"), str):
        raise GateError("capture directory binding is missing")
    return Path(directory["path"])


def _consume_attempt(
    capture_directory_fd: int,
    capture_directory_identity: dict[str, Any],
    authorization_binding: dict[str, Any],
    output: Path,
    policy: GatePolicy,
) -> None:
    directory = os.fstat(capture_directory_fd)
    if (directory.st_dev, directory.st_ino) != (
        capture_directory_identity.get("device"),
        capture_directory_identity.get("inode"),
    ):
        raise GateError("held capture directory changed before authorization consume")
    marker_value = {
        "schema": 1,
        "status": "consumed-before-device-execution",
        "policy_id": POLICY_ID,
        "consumed_at_utc": core.utc_now(),
        "capture_directory": capture_directory_identity,
        "authorization_path": authorization_binding["path"],
        "authorization_sha256": authorization_binding["sha256"],
        "intended_output": str(output),
        "commit": policy.commit,
        "version": policy.version,
        "failure_rule": "never delete this marker; obtain a fresh capture and authorization",
    }
    marker_bytes = (
        json.dumps(marker_value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    no_follow = getattr(os, "O_NOFOLLOW", None)
    if no_follow is None:
        raise GateError("platform cannot securely create the attempt marker")
    flags |= no_follow
    try:
        marker_fd = os.open(
            ATTEMPT_MARKER_NAME,
            flags,
            0o600,
            dir_fd=capture_directory_fd,
        )
    except FileExistsError as exc:
        raise GateError(
            "deferred-TF app0 authorization was already attempted or consumed"
        ) from exc
    except OSError as exc:
        raise GateError("cannot atomically consume deferred-TF authorization") from exc
    try:
        written = 0
        while written < len(marker_bytes):
            count = os.write(marker_fd, marker_bytes[written:])
            if count <= 0:
                raise GateError("attempt marker write made no progress")
            written += count
        os.fsync(marker_fd)
    finally:
        os.close(marker_fd)
    try:
        os.fsync(capture_directory_fd)
    except OSError as exc:
        raise GateError("attempt marker parent directory could not be synced") from exc


def execute_app0_stage_deferred_tf(
    authorization_value: str | Path,
    port: str,
    output_value: str | Path,
    policy: GatePolicy = DEFERRED_TF_POLICY,
    *,
    now: dt.datetime | None = None,
) -> Path:
    capture_directory_path = _capture_directory_path_from_app_authorization(
        authorization_value
    )
    with _opened_capture_directory(capture_directory_path) as (
        capture_directory_fd,
        _,
        capture_directory_identity,
    ):
        _, _, context = _load_app_stage_authorization(
            authorization_value,
            policy,
            now=now,
            capture_directory_fd=capture_directory_fd,
            capture_directory_identity=capture_directory_identity,
        )
        core._expect(
            context["capture_directory"],
            capture_directory_identity,
            "execution capture directory inode",
        )
        core._expect(port, policy.expected_port, "execution port")
        output = _preflight_new_output(output_value)
        _consume_attempt(
            capture_directory_fd,
            capture_directory_identity,
            context["authorization_binding"],
            output,
            policy,
        )
        candidate = context["candidate"]
        return core._execute_reviewed_write(
            port=port,
            input_path=candidate["path"],
            input_bytes=policy.candidate_bytes,
            input_sha256=policy.candidate_sha256,
            offset=policy.app0_offset,
            expected_before=context["before_flash"],
            expected_after=context["expected_after"],
            candidate=candidate,
            output_value=output,
            after_name="full-flash-after-app0-stage.bin",
            phase="app0-stage-only-deferred-tf-no-reset",
            authorization_receipt={
                **context["authorization_binding"],
                "capture_directory": capture_directory_identity,
                "authorized_for_selector": False,
                "authorized_for_reset": False,
                "authorized_for_boot": False,
                "authorized_for_tf_access": False,
            },
            policy=policy,
        )


def authorize_stage_b(*_: Any, **__: Any) -> Path:
    raise GateError(
        "Stage B is NOT AUTHORIZED / NOT IMPLEMENTED. Stage A emits no selector "
        "bytes or argv and authorizes no reset, boot or TF access. A future Stage B "
        "requires a separately reviewed schema, complete custody prerequisites, "
        "fresh preboot full-flash proof and explicit boot-health evidence."
    )


def _add_release_arguments(parser: argparse.ArgumentParser) -> None:
    core._add_release_arguments(parser)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fail-closed C151 app0-only staging with TF physically absent"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture = subparsers.add_parser(
        "capture", help="capture a fresh fixed read-only C151 snapshot"
    )
    capture.add_argument("--port", required=True)
    capture.add_argument("--output-dir", required=True)

    authorize = subparsers.add_parser(
        "authorize-app0-stage-deferred-tf",
        help="authorize one app0-only attempt and no selector/reset/boot action",
    )
    authorize.add_argument("--capture-dir", required=True)
    authorize.add_argument("--candidate", required=True)
    authorize.add_argument("--acceptance-result", required=True)
    authorize.add_argument("--staging-receipt", required=True)
    authorize.add_argument("--baseline-custody", required=True)
    authorize.add_argument("--operator-authorization", required=True)
    authorize.add_argument("--output-dir", required=True)
    _add_release_arguments(authorize)

    execute = subparsers.add_parser(
        "execute-app0-stage-deferred-tf",
        help="consume authorization, write only app0, then compare all 16 MiB",
    )
    execute.add_argument("--app-authorization", required=True)
    execute.add_argument("--port", required=True)
    execute.add_argument("--output-dir", required=True)
    _add_release_arguments(execute)

    stage_b = subparsers.add_parser(
        "authorize-stage-b",
        help="always fail closed; Stage B is not authorized or implemented",
    )
    stage_b.add_argument("--app-execution-receipt", required=True)
    stage_b.add_argument("--fresh-preboot-full-flash", required=True)
    stage_b.add_argument("--operator-authorization", required=True)
    stage_b.add_argument("--boot-health-evidence-plan", required=True)
    stage_b.add_argument("--output-dir", required=True)
    _add_release_arguments(stage_b)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(list(argv) if argv is not None else None)
    try:
        if arguments.command == "capture":
            result = core.capture_read_only(
                arguments.port, arguments.output_dir, DEFERRED_TF_POLICY
            )
        else:
            policy = core.bind_release(
                DEFERRED_TF_POLICY,
                arguments.expected_commit,
                arguments.expected_version,
                arguments.expected_candidate_sha256,
                arguments.expected_candidate_bytes,
            )
            if arguments.command == "authorize-app0-stage-deferred-tf":
                result = authorize_app0_stage_deferred_tf(
                    arguments.capture_dir,
                    arguments.candidate,
                    arguments.acceptance_result,
                    arguments.staging_receipt,
                    arguments.baseline_custody,
                    arguments.operator_authorization,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "execute-app0-stage-deferred-tf":
                result = execute_app0_stage_deferred_tf(
                    arguments.app_authorization,
                    arguments.port,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "authorize-stage-b":
                result = authorize_stage_b()
            else:  # pragma: no cover - argparse makes this unreachable.
                raise GateError("unknown command")
        print(result)
        return 0
    except GateError as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
