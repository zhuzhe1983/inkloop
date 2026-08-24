#!/usr/bin/env python3
"""Fail-closed candidate gate for the installed M5 PaperColor C151.

This tool deliberately separates read-only device capture, offline
authorization and controlled execution.  ``capture`` has a fixed allow-list of
read-only esptool commands.  A flash write is possible only through one of the
three ``execute-*`` subcommands, after the complete authorization chain,
current device identity, exact pre-write 16 MiB image and a sealed inherited
input FD all pass; each write is followed by an automatic exact full readback.

The installed unit is special: beta27 is selected in app1 and is the rollback
image.  A freshly accepted successor may therefore be staged only into
inactive app0.  ``gate-app``
does not expose selector bytes.  They are emitted by ``authorize-selector``
only after an app0 readback and all protected-region readbacks pass.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Iterator
import zlib


class GateError(RuntimeError):
    """A fail-closed gate rejection."""


@dataclasses.dataclass(frozen=True)
class GatePolicy:
    policy_id: str
    commit: str
    version: str
    candidate_sha256: str
    candidate_bytes: int
    expected_mac: str
    expected_port: str
    flash_bytes: int
    flash_sha256_before: str
    flash_manufacturer: str
    flash_device: str
    partition_table_offset: int
    partition_table_file_bytes: int
    partition_table_sha256: str
    nvs_offset: int
    nvs_bytes: int
    otadata_offset: int
    otadata_bytes: int
    app0_offset: int
    app0_bytes: int
    app0_before_sha256: str
    app1_offset: int
    app1_bytes: int
    app1_rollback_sha256: str
    littlefs_offset: int
    littlefs_bytes: int
    otadata_before_sha256: str
    current_app0_sequence: int
    current_app1_sequence: int
    next_app0_sequence: int
    next_app0_state: int
    ota_valid_state: int
    minimum_tf_image_bytes: int
    minimum_candidate_beta: int


PRODUCTION_POLICY = GatePolicy(
    policy_id="m5-papercolor-c151-fresh-candidate-inactive-app0-v2",
    commit="",
    version="",
    candidate_sha256="",
    candidate_bytes=0,
    expected_mac="28:84:85:43:da:0c",
    expected_port="/dev/cu.usbmodem21442201",
    flash_bytes=0x1000000,
    flash_sha256_before=(
        "25d169e66cc334fe219de0220cca2920d0aae8c8747d33dde3af87bd9196f76d"
    ),
    flash_manufacturer="20",
    flash_device="4018",
    partition_table_offset=0x8000,
    partition_table_file_bytes=0xC00,
    partition_table_sha256=(
        "bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce"
    ),
    nvs_offset=0x9000,
    nvs_bytes=0x5000,
    otadata_offset=0xE000,
    otadata_bytes=0x2000,
    app0_offset=0x10000,
    app0_bytes=0x640000,
    app0_before_sha256=(
        "2d6381527d541dc879e44648a5b0b1a11b12af8a848ae51ded7124714d27db49"
    ),
    app1_offset=0x650000,
    app1_bytes=0x640000,
    app1_rollback_sha256=(
        "872e1e749bc065fe0ab1b687be7dc0997953a60bcb582253957cd80c2daa2070"
    ),
    littlefs_offset=0xC90000,
    littlefs_bytes=0x360000,
    otadata_before_sha256=(
        "b7e293bb607d3bddb99b7f38a7a45afd5823c0c61e3216e67858bbc759535282"
    ),
    current_app0_sequence=1,
    current_app1_sequence=2,
    next_app0_sequence=3,
    next_app0_state=0,
    ota_valid_state=2,
    minimum_tf_image_bytes=64 * 1024 * 1024,
    minimum_candidate_beta=30,
)


READ_ONLY_ESPTOOL_OPERATIONS = frozenset(
    {"read_mac", "chip_id", "flash_id", "get_security_info", "read_flash"}
)
FORBIDDEN_ESPTOOL_OPERATIONS = frozenset(
    {
        "write_flash",
        "erase_flash",
        "erase_region",
        "write_mem",
        "write_flash_status",
        "load_ram",
        "run",
    }
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_RE = re.compile(r"^0\.4\.0-beta\.([1-9][0-9]*)$")
MAC_RE = re.compile(r"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")
PORT_RE = re.compile(r"^/dev/cu\.usbmodem[0-9]+$")
ACCEPTANCE_MAX_AGE = dt.timedelta(days=7)
ACCEPTANCE_FUTURE_SKEW = dt.timedelta(minutes=5)


def bind_release(
    base_policy: GatePolicy,
    expected_commit: str,
    expected_version: str,
    expected_candidate_sha256: str,
    expected_candidate_bytes: int,
) -> GatePolicy:
    """Bind an immutable device policy to one freshly accepted release."""
    if not isinstance(expected_commit, str) or not COMMIT_RE.fullmatch(
        expected_commit
    ):
        raise GateError("expected commit must be 40 lowercase hexadecimal characters")
    if not isinstance(expected_version, str):
        raise GateError("expected version must be a string")
    version_match = VERSION_RE.fullmatch(expected_version)
    if version_match is None:
        raise GateError("expected version must have the form 0.4.0-beta.N")
    beta_number = int(version_match.group(1))
    if beta_number < base_policy.minimum_candidate_beta:
        raise GateError(
            "candidate beta is revoked or below the minimum authorized beta: "
            f"expected beta.{base_policy.minimum_candidate_beta} or newer"
        )
    if not isinstance(
        expected_candidate_sha256, str
    ) or not SHA256_RE.fullmatch(expected_candidate_sha256):
        raise GateError("expected candidate SHA-256 must be lowercase hexadecimal")
    if (
        not isinstance(expected_candidate_bytes, int)
        or isinstance(expected_candidate_bytes, bool)
        or expected_candidate_bytes <= 0
    ):
        raise GateError("expected candidate byte count must be a positive integer")
    programmed_bytes = (expected_candidate_bytes + 0xFFF) & ~0xFFF
    if expected_candidate_bytes > base_policy.app0_bytes:
        raise GateError("expected candidate does not fit app0")
    if programmed_bytes > base_policy.app0_bytes:
        raise GateError("expected candidate erase-aligned range does not fit app0")
    return dataclasses.replace(
        base_policy,
        commit=expected_commit,
        version=expected_version,
        candidate_sha256=expected_candidate_sha256,
        candidate_bytes=expected_candidate_bytes,
    )


def _require_bound_policy(policy: GatePolicy) -> None:
    """Reject release-sensitive operations when no exact release is bound."""
    bind_release(
        policy,
        policy.commit,
        policy.version,
        policy.candidate_sha256,
        policy.candidate_bytes,
    )


def _release_cli_args(policy: GatePolicy) -> list[str]:
    _require_bound_policy(policy)
    return [
        "--expected-commit",
        policy.commit,
        "--expected-version",
        policy.version,
        "--expected-candidate-sha256",
        policy.candidate_sha256,
        "--expected-candidate-bytes",
        str(policy.candidate_bytes),
    ]


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb", buffering=1024 * 1024) as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                return digest.hexdigest()
            digest.update(chunk)


def canonical_json_sha256(value: Any) -> str:
    """Match capture_tf_whole_card_macos.mjs recursive canonicalJson()."""
    try:
        canonical = json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise GateError("canonical JSON contains an unsupported value") from exc
    return sha256_bytes(canonical)


def _require_regular_input(path_value: str | Path, label: str) -> Path:
    path = Path(path_value)
    try:
        metadata = path.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"{label} is missing: {path}") from exc
    if stat.S_ISLNK(metadata.st_mode):
        raise GateError(f"{label} must not be a symlink: {path}")
    if not stat.S_ISREG(metadata.st_mode):
        raise GateError(f"{label} must be a regular file: {path}")
    return path.resolve(strict=True)


def _read_bounded(path: Path, maximum: int, label: str) -> bytes:
    size = path.stat().st_size
    if size > maximum:
        raise GateError(f"{label} exceeds {maximum} bytes")
    return path.read_bytes()


def _read_json(path_value: str | Path, label: str) -> tuple[Path, dict[str, Any]]:
    path = _require_regular_input(path_value, label)
    try:
        value = json.loads(_read_bounded(path, 1024 * 1024, label))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise GateError(f"{label} is not valid JSON") from exc
    if not isinstance(value, dict):
        raise GateError(f"{label} must contain a JSON object")
    return path, value


def _expect(value: Any, expected: Any, label: str) -> None:
    if value != expected:
        raise GateError(f"{label} mismatch: expected {expected!r}, got {value!r}")


def _expect_sha(value: Any, expected: str, label: str) -> None:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise GateError(f"{label} is not a lowercase SHA-256")
    _expect(value, expected, label)


def _canonical_mac(value: str) -> str:
    normalized = value.strip().lower()
    if not MAC_RE.fullmatch(normalized):
        raise GateError(f"invalid MAC address: {value!r}")
    return normalized


def ota_crc(sequence: int) -> int:
    if sequence <= 0 or sequence > 0xFFFFFFFF:
        raise GateError("OTA sequence must be a nonzero uint32")
    return zlib.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF


def make_ota_entry(sequence: int, state: int) -> bytes:
    if state < 0 or state > 0xFFFFFFFF:
        raise GateError("OTA state must be a uint32")
    entry = bytearray(b"\xff" * 32)
    struct.pack_into("<I", entry, 0, sequence)
    struct.pack_into("<I", entry, 24, state)
    struct.pack_into("<I", entry, 28, ota_crc(sequence))
    return bytes(entry)


def parse_ota_entry(data: bytes, sector_offset: int) -> dict[str, Any]:
    if sector_offset < 0 or sector_offset + 32 > len(data):
        raise GateError("OTA entry is outside the supplied data")
    raw = data[sector_offset : sector_offset + 32]
    sequence, state, crc = (
        struct.unpack_from("<I", raw, 0)[0],
        struct.unpack_from("<I", raw, 24)[0],
        struct.unpack_from("<I", raw, 28)[0],
    )
    valid_crc = sequence != 0xFFFFFFFF and crc == ota_crc(sequence)
    return {
        "sequence": sequence,
        "state": state,
        "crc": crc,
        "valid_crc": valid_crc,
        "raw": raw,
    }


def validate_current_otadata(data: bytes, policy: GatePolicy) -> dict[str, Any]:
    _expect(len(data), policy.otadata_bytes, "otadata length")
    _expect_sha(sha256_bytes(data), policy.otadata_before_sha256, "otadata SHA-256")
    first = parse_ota_entry(data, 0)
    second = parse_ota_entry(data, 0x1000)
    for label, entry, sequence in (
        ("entry0", first, policy.current_app0_sequence),
        ("entry1", second, policy.current_app1_sequence),
    ):
        _expect(entry["sequence"], sequence, f"{label} sequence")
        _expect(entry["state"], policy.ota_valid_state, f"{label} state")
        _expect(entry["valid_crc"], True, f"{label} CRC")
    if any(byte != 0xFF for byte in data[32:0x1000]):
        raise GateError("entry0 selector sector has unexpected non-0xff tail bytes")
    if any(byte != 0xFF for byte in data[0x1000 + 32 : 0x2000]):
        raise GateError("entry1 selector sector has unexpected non-0xff tail bytes")
    if second["sequence"] <= first["sequence"]:
        raise GateError("app1 is not the selected OTA slot")
    return {"entry0": first, "entry1": second, "selected": "app1"}


def make_selected_app0_otadata(current: bytes, policy: GatePolicy) -> tuple[bytes, bytes]:
    validate_current_otadata(current, policy)
    selector = make_ota_entry(policy.next_app0_sequence, policy.next_app0_state)
    updated = bytearray(current)
    updated[0:0x1000] = b"\xff" * 0x1000
    updated[0:32] = selector
    first = parse_ota_entry(updated, 0)
    second = parse_ota_entry(updated, 0x1000)
    _expect(first["sequence"], policy.next_app0_sequence, "new entry0 sequence")
    _expect(first["state"], policy.next_app0_state, "new entry0 state")
    _expect(first["valid_crc"], True, "new entry0 CRC")
    if first["sequence"] <= second["sequence"]:
        raise GateError("new selector does not select app0")
    return selector, bytes(updated)


def _slice(data: bytes, offset: int, length: int, label: str) -> bytes:
    if offset < 0 or length <= 0 or offset + length > len(data):
        raise GateError(f"{label} range is outside full flash")
    return data[offset : offset + length]


def validate_flash_before(full_flash: bytes, policy: GatePolicy) -> dict[str, Any]:
    _expect(len(full_flash), policy.flash_bytes, "full flash length")
    _expect_sha(
        sha256_bytes(full_flash), policy.flash_sha256_before, "full flash SHA-256"
    )
    partition = _slice(
        full_flash,
        policy.partition_table_offset,
        policy.partition_table_file_bytes,
        "partition table",
    )
    _expect_sha(
        sha256_bytes(partition),
        policy.partition_table_sha256,
        "partition table SHA-256",
    )
    partition_sector_tail = _slice(
        full_flash,
        policy.partition_table_offset + policy.partition_table_file_bytes,
        0x1000 - policy.partition_table_file_bytes,
        "partition table sector tail",
    )
    if any(byte != 0xFF for byte in partition_sector_tail):
        raise GateError("partition table sector tail is not erased")
    app0 = _slice(full_flash, policy.app0_offset, policy.app0_bytes, "app0")
    app1 = _slice(full_flash, policy.app1_offset, policy.app1_bytes, "app1")
    otadata = _slice(
        full_flash, policy.otadata_offset, policy.otadata_bytes, "otadata"
    )
    _expect_sha(sha256_bytes(app0), policy.app0_before_sha256, "app0 SHA-256")
    _expect_sha(
        sha256_bytes(app1), policy.app1_rollback_sha256, "app1 rollback SHA-256"
    )
    ota = validate_current_otadata(otadata, policy)
    return {
        "full_flash_sha256": sha256_bytes(full_flash),
        "prefix_sha256": sha256_bytes(full_flash[: policy.app0_offset]),
        "app0_sha256": sha256_bytes(app0),
        "app1_sha256": sha256_bytes(app1),
        "littlefs_sha256": sha256_bytes(
            _slice(
                full_flash,
                policy.littlefs_offset,
                policy.littlefs_bytes,
                "LittleFS",
            )
        ),
        "otadata_sha256": sha256_bytes(otadata),
        "otadata": ota,
    }


def _parse_identity_logs(capture_dir: Path, policy: GatePolicy) -> dict[str, Any]:
    log_names = ("read-mac.log", "chip-id.log", "flash-id.log", "security-info.log")
    texts: dict[str, str] = {}
    for name in log_names:
        path = _require_regular_input(capture_dir / name, name)
        try:
            texts[name] = _read_bounded(path, 1024 * 1024, name).decode(
                "utf-8", errors="strict"
            )
        except UnicodeDecodeError as exc:
            raise GateError(f"{name} is not UTF-8") from exc
    combined = "\n".join(texts.values())
    macs = {_canonical_mac(match) for match in re.findall(r"(?im)^MAC:\s*([0-9a-f:]{17})\s*$", combined)}
    _expect(macs, {policy.expected_mac}, "captured MAC set")
    if not re.search(r"(?i)ESP32-S3.*revision\s+v?0\.2", combined):
        raise GateError("capture does not prove ESP32-S3 revision 0.2")
    flash_text = texts["flash-id.log"]
    for pattern, label in (
        (rf"(?im)^Manufacturer:\s*{re.escape(policy.flash_manufacturer)}\s*$", "flash manufacturer"),
        (rf"(?im)^Device:\s*{re.escape(policy.flash_device)}\s*$", "flash device"),
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


def _validate_acceptance(path_value: str | Path, policy: GatePolicy) -> dict[str, Any]:
    _require_bound_policy(policy)
    path, result = _read_json(path_value, "acceptance result")
    _expect(result.get("status"), "pass", "acceptance status")
    _expect(result.get("commit"), policy.commit, "acceptance commit")
    reviewed_at = result.get("reviewed_at_utc")
    if not isinstance(reviewed_at, str) or not reviewed_at.endswith("Z"):
        raise GateError("acceptance review timestamp is missing or not UTC")
    try:
        reviewed_at_value = dt.datetime.fromisoformat(
            reviewed_at.removesuffix("Z") + "+00:00"
        )
    except ValueError as exc:
        raise GateError("acceptance review timestamp is invalid") from exc
    now = dt.datetime.now(dt.timezone.utc)
    if reviewed_at_value > now + ACCEPTANCE_FUTURE_SKEW:
        raise GateError("acceptance review timestamp is in the future")
    if now - reviewed_at_value > ACCEPTANCE_MAX_AGE:
        raise GateError("acceptance result is stale; run a fresh acceptance")
    checks = result.get("checks")
    if not isinstance(checks, dict):
        raise GateError("acceptance checks are missing")
    commit_identity = checks.get("commit_identity")
    if not isinstance(commit_identity, dict):
        raise GateError("acceptance commit identity is missing")
    _expect(commit_identity.get("status"), "pass", "commit identity status")
    _expect(commit_identity.get("expected"), policy.commit, "expected commit identity")
    _expect(commit_identity.get("actual"), policy.commit, "actual commit identity")
    worktree = checks.get("worktree")
    if not isinstance(worktree, dict) or worktree.get("status") != "pass":
        raise GateError("acceptance clean-worktree proof is missing")
    reproducible = checks.get("reproducible_binaries")
    if not isinstance(reproducible, dict):
        raise GateError("reproducible binary receipt is missing")
    _expect(reproducible.get("status"), "pass", "reproducible binary status")
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
    if not isinstance(constraints, dict) or constraints.get("status") != "pass":
        raise GateError("acceptance constraint compliance is missing")
    for key in (
        "tracked_files_modified",
        "device_accessed",
        "device_written_or_flashed",
        "push_performed",
    ):
        _expect(constraints.get(key), False, f"acceptance constraint {key}")
    return {
        "path": str(path),
        "sha256": sha256_file(path),
        "reviewed_at_utc": reviewed_at,
    }


def _validate_baseline_custody(path_value: str | Path, policy: GatePolicy) -> dict[str, Any]:
    path, custody = _read_json(path_value, "baseline custody")
    _expect(custody.get("complete"), True, "baseline custody completion")
    _expect(custody.get("matching_full_reads"), True, "matching baseline reads")
    _expect(custody.get("flash_bytes"), policy.flash_bytes, "baseline flash bytes")
    _expect(_canonical_mac(str(custody.get("device_mac", ""))), policy.expected_mac, "baseline MAC")
    _expect_sha(
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
    _expect(app0.get("version"), "0.4.0-beta.25", "baseline app0 version")
    _expect_sha(app0.get("sha256"), policy.app0_before_sha256, "baseline app0 SHA-256")
    _expect(app1.get("version"), "0.4.0-beta.27", "baseline app1 version")
    _expect_sha(
        app1.get("sha256"), policy.app1_rollback_sha256, "baseline app1 SHA-256"
    )
    return {"path": str(path), "sha256": sha256_file(path)}


def _tf_safe_string(value: Any) -> str | None:
    return value if isinstance(value, str) and value else None


def _tf_safe_boolean(value: Any) -> bool | None:
    return value if isinstance(value, bool) else None


def _tf_safe_integer(value: Any) -> int | None:
    if (
        isinstance(value, int)
        and not isinstance(value, bool)
        and -(2**53 - 1) <= value <= 2**53 - 1
    ):
        return value
    return None


def _normalized_tf_member(info: dict[str, Any]) -> dict[str, Any]:
    roles = info.get("APFSVolumeRole")
    if isinstance(roles, list):
        normalized_roles: list[str] | str | None = sorted(
            entry for entry in roles if isinstance(entry, str)
        )
    else:
        normalized_roles = _tf_safe_string(roles)
    return {
        "deviceIdentifier": _tf_safe_string(info.get("DeviceIdentifier")),
        "parentWholeDisk": _tf_safe_string(info.get("ParentWholeDisk")),
        "totalSize": _tf_safe_integer(info.get("TotalSize")),
        "content": _tf_safe_string(info.get("Content")),
        "filesystemType": _tf_safe_string(info.get("FilesystemType")),
        "partitionUUID": _tf_safe_string(info.get("PartitionUUID")),
        "volumeUUID": _tf_safe_string(info.get("VolumeUUID")),
        "diskUUID": _tf_safe_string(info.get("DiskUUID")),
        "mediaUUID": _tf_safe_string(info.get("MediaUUID")),
        "mounted": _tf_safe_boolean(info.get("Mounted")),
        "mountPoint": _tf_safe_string(info.get("MountPoint")),
        "volumeRoles": normalized_roles,
    }


def _normalized_tf_identity(
    target_info: dict[str, Any],
    member_infos: list[dict[str, Any]],
    source_device: str,
) -> dict[str, Any]:
    members = sorted(
        (_normalized_tf_member(info) for info in member_infos),
        key=lambda member: member["deviceIdentifier"] or "",
    )
    return {
        "deviceIdentifier": source_device.removeprefix("/dev/"),
        "deviceNode": source_device,
        "totalSize": _tf_safe_integer(target_info.get("TotalSize")),
        "deviceBlockSize": _tf_safe_integer(target_info.get("DeviceBlockSize")),
        "mediaName": _tf_safe_string(target_info.get("MediaName")),
        "mediaUUID": _tf_safe_string(target_info.get("MediaUUID")),
        "diskUUID": _tf_safe_string(target_info.get("DiskUUID")),
        "deviceTreePath": _tf_safe_string(target_info.get("DeviceTreePath")),
        "ioRegistryEntryName": _tf_safe_string(
            target_info.get("IORegistryEntryName")
        ),
        "busProtocol": _tf_safe_string(target_info.get("BusProtocol")),
        "mediaType": _tf_safe_string(target_info.get("MediaType")),
        "removableMedia": _tf_safe_boolean(target_info.get("RemovableMedia")),
        "ejectable": _tf_safe_boolean(target_info.get("Ejectable")),
        "internal": _tf_safe_boolean(target_info.get("Internal")),
        "virtualOrPhysical": _tf_safe_string(
            target_info.get("VirtualOrPhysical")
        ),
        "members": members,
    }


def _collect_tf_device_identifiers(value: Any, output: set[str]) -> None:
    if isinstance(value, list):
        for item in value:
            _collect_tf_device_identifiers(item, output)
        return
    if not isinstance(value, dict):
        return
    for key, item in value.items():
        if (
            key
            in {
                "DeviceIdentifier",
                "ParentWholeDisk",
                "APFSPhysicalStore",
                "ContainerReference",
            }
            and isinstance(item, str)
            and re.fullmatch(r"disk(?:0|[1-9][0-9]*)(?:s[0-9]+)*", item)
        ):
            output.add(item)
        _collect_tf_device_identifiers(item, output)


def _validate_tf_custody(path_value: str | Path, policy: GatePolicy) -> dict[str, Any]:
    path, custody = _read_json(path_value, "TF custody")
    _expect(custody.get("complete"), True, "TF custody completion")
    _expect(custody.get("schema"), 1, "TF custody schema")
    _expect(custody.get("platform"), "macOS", "TF custody platform")
    _expect(
        custody.get("sourceAccess"),
        "two full read-only raw-device passes",
        "TF source access",
    )
    _expect(custody.get("sourceWritesPerformed"), False, "TF source writes")
    _expect(
        custody.get("automaticUnmountOrEjectPerformed"),
        False,
        "TF automatic unmount/eject",
    )
    _expect(
        custody.get("implicitPrivilegeEscalationPerformed"),
        False,
        "TF implicit privilege escalation",
    )
    source_device = custody.get("disk")
    raw_device = custody.get("rawDisk")
    if not isinstance(source_device, str) or not re.fullmatch(r"/dev/disk[0-9]+", source_device):
        raise GateError("TF disk must be one explicit /dev/diskN whole device")
    if raw_device != source_device.replace("/dev/disk", "/dev/rdisk", 1):
        raise GateError("TF rawDisk does not match the explicit whole disk")
    _expect(
        custody.get("identityStableAcrossSnapshots"),
        True,
        "TF stable identity evidence",
    )
    fingerprint = custody.get("fingerprint")
    if not isinstance(fingerprint, str) or not SHA256_RE.fullmatch(fingerprint):
        raise GateError("TF identity fingerprint is invalid")
    snapshots = custody.get("snapshotFingerprints")
    if (
        not isinstance(snapshots, dict)
        or set(snapshots)
        != {"before", "preRead", "betweenPasses", "after"}
        or set(snapshots.values()) != {fingerprint}
    ):
        raise GateError("TF diskutil identity changed across capture snapshots")
    image_name = custody.get("image")
    if image_name != "tf-whole-card.img":
        raise GateError("TF custody must name the canonical whole-card image")
    image_path = _require_regular_input(path.parent / image_name, "TF whole-card image")
    if image_path.parent != path.parent:
        raise GateError("TF whole-card image escaped the custody directory")
    image_bytes = image_path.stat().st_size
    if image_bytes < policy.minimum_tf_image_bytes:
        raise GateError(
            f"TF image is too small for a whole-card capture: {image_bytes} bytes"
        )
    _expect(custody.get("bytes"), image_bytes, "TF image byte count")
    declared_sha = custody.get("sha256")
    if not isinstance(declared_sha, str) or not SHA256_RE.fullmatch(declared_sha):
        raise GateError("TF custody SHA-256 is invalid")
    source_passes = custody.get("sourcePasses")
    if not isinstance(source_passes, list) or len(source_passes) != 2:
        raise GateError("TF custody requires exactly two full source passes")
    for index, source_pass in enumerate(source_passes):
        if not isinstance(source_pass, dict):
            raise GateError(f"TF source pass {index + 1} is invalid")
        _expect(source_pass.get("bytes"), image_bytes, f"TF source pass {index + 1} bytes")
        _expect_sha(
            source_pass.get("sha256"),
            declared_sha,
            f"TF source pass {index + 1} SHA-256",
        )
    identity = custody.get("identity")
    if not isinstance(identity, dict):
        raise GateError("TF disk identity is missing")
    _expect(identity.get("deviceNode"), source_device, "TF identity device node")
    _expect(identity.get("totalSize"), image_bytes, "TF identity total size")
    _expect(identity.get("internal"), False, "TF internal-media status")
    _expect(identity.get("virtualOrPhysical"), "Physical", "TF physical-media status")
    if identity.get("removableMedia") is not True and identity.get("ejectable") is not True:
        raise GateError("TF identity is not removable/ejectable")
    members = identity.get("members")
    if not isinstance(members, list) or not members:
        raise GateError("TF member inventory is missing")
    for member in members:
        if not isinstance(member, dict) or member.get("mounted") is not False or member.get("mountPoint") is not None:
            raise GateError("TF custody contains a mounted or indeterminate member")
    _expect_sha(
        fingerprint,
        canonical_json_sha256(identity),
        "TF canonical identity fingerprint",
    )
    diskutil_receipts: dict[str, dict[str, Any]] = {}
    diskutil_values: dict[str, Any] = {}
    for name in (
        "diskutil-info-before.json",
        "diskutil-list-before.json",
        "diskutil-members-before.json",
        "diskutil-info-pre-read.json",
        "diskutil-members-pre-read.json",
        "diskutil-info-between.json",
        "diskutil-members-between.json",
        "diskutil-info-after.json",
        "diskutil-members-after.json",
    ):
        receipt_path = _require_regular_input(path.parent / name, f"TF {name}")
        raw_receipt = _read_bounded(receipt_path, 16 * 1024 * 1024, f"TF {name}")
        try:
            diskutil_values[name] = json.loads(raw_receipt)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise GateError(f"TF {name} is not valid JSON") from exc
        diskutil_receipts[name] = {
            "path": str(receipt_path),
            "bytes": len(raw_receipt),
            "sha256": sha256_bytes(raw_receipt),
        }
    whole_identifier = source_device.removeprefix("/dev/")
    member_identifier_re = re.compile(
        rf"{re.escape(whole_identifier)}(?:s[0-9]+)*"
    )
    for snapshot, info_name, members_name in (
        (
            "before",
            "diskutil-info-before.json",
            "diskutil-members-before.json",
        ),
        (
            "preRead",
            "diskutil-info-pre-read.json",
            "diskutil-members-pre-read.json",
        ),
        (
            "betweenPasses",
            "diskutil-info-between.json",
            "diskutil-members-between.json",
        ),
        (
            "after",
            "diskutil-info-after.json",
            "diskutil-members-after.json",
        ),
    ):
        target_info = diskutil_values[info_name]
        member_infos = diskutil_values[members_name]
        if not isinstance(target_info, dict) or not isinstance(member_infos, list):
            raise GateError(f"TF {snapshot} diskutil receipt shape is invalid")
        if not all(isinstance(member, dict) for member in member_infos):
            raise GateError(f"TF {snapshot} diskutil members are invalid")
        _expect(
            target_info.get("DeviceIdentifier"),
            whole_identifier,
            f"TF {snapshot} whole-disk identifier",
        )
        _expect(
            target_info.get("DeviceNode"),
            source_device,
            f"TF {snapshot} whole-disk node",
        )
        _expect(target_info.get("Whole"), True, f"TF {snapshot} whole-disk flag")
        normalized = _normalized_tf_identity(
            target_info,
            member_infos,
            source_device,
        )
        _expect(normalized, identity, f"TF {snapshot} normalized identity")
        _expect_sha(
            snapshots.get(snapshot),
            canonical_json_sha256(normalized),
            f"TF {snapshot} reconstructed fingerprint",
        )
        for member in member_infos:
            identifier = member.get("DeviceIdentifier")
            if not isinstance(identifier, str) or not member_identifier_re.fullmatch(
                identifier
            ):
                raise GateError(f"TF {snapshot} member escaped the whole disk")
            if member.get("Mounted") is not False or _tf_safe_string(
                member.get("MountPoint")
            ):
                raise GateError(f"TF {snapshot} member is mounted or indeterminate")

    listed_identifiers: set[str] = set()
    _collect_tf_device_identifiers(
        diskutil_values["diskutil-list-before.json"], listed_identifiers
    )
    listed_for_whole = {
        identifier
        for identifier in listed_identifiers
        if member_identifier_re.fullmatch(identifier)
    }
    before_member_identifiers = {
        member["DeviceIdentifier"]
        for member in diskutil_values["diskutil-members-before.json"]
    }
    _expect(
        listed_for_whole,
        before_member_identifiers,
        "TF before diskutil list/member identifiers",
    )
    actual_sha = sha256_file(image_path)
    _expect(actual_sha, declared_sha, "TF whole-card SHA-256")
    sums_path = _require_regular_input(path.parent / "SHA256SUMS", "TF SHA256SUMS")
    expected_sums = f"{actual_sha}  tf-whole-card.img\n".encode("ascii")
    _expect(_read_bounded(sums_path, 1024, "TF SHA256SUMS"), expected_sums, "TF SHA256SUMS content")
    return {
        "custody_path": str(path),
        "custody_sha256": sha256_file(path),
        "image_path": str(image_path),
        "image_bytes": image_bytes,
        "image_sha256": actual_sha,
        "source_device": source_device,
        "raw_source_device": raw_device,
        "fingerprint": fingerprint,
        "source_passes": source_passes,
        "diskutil_receipts": diskutil_receipts,
    }


def _validate_capture_manifest(capture_dir: Path, policy: GatePolicy) -> dict[str, Any]:
    manifest_path, manifest = _read_json(
        capture_dir / "capture-manifest.json", "capture manifest"
    )
    _expect(manifest.get("complete"), True, "capture completion")
    _expect(manifest.get("policy_id"), policy.policy_id, "capture policy")
    _expect(manifest.get("port"), policy.expected_port, "capture port")
    operations = manifest.get("operations")
    if operations != ["read_mac", "chip_id", "flash_id", "get_security_info", "read_flash"]:
        raise GateError("capture operation list is not the fixed read-only sequence")
    if any(operation in FORBIDDEN_ESPTOOL_OPERATIONS for operation in operations):
        raise GateError("capture manifest contains a forbidden operation")
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise GateError("capture file receipt is missing")
    required = (
        "read-mac.log",
        "chip-id.log",
        "flash-id.log",
        "security-info.log",
        "full-flash-before.log",
        "full-flash-before.bin",
    )
    for name in required:
        record = files.get(name)
        if not isinstance(record, dict):
            raise GateError(f"capture receipt for {name} is missing")
        file_path = _require_regular_input(capture_dir / name, name)
        _expect(record.get("bytes"), file_path.stat().st_size, f"{name} bytes")
        _expect_sha(record.get("sha256"), sha256_file(file_path), f"{name} SHA-256")
    return {"path": str(manifest_path), "sha256": sha256_file(manifest_path)}


def _validate_candidate(path_value: str | Path, policy: GatePolicy) -> dict[str, Any]:
    _require_bound_policy(policy)
    path = _require_regular_input(path_value, "release candidate")
    _expect(path.stat().st_size, policy.candidate_bytes, "candidate byte count")
    _expect_sha(sha256_file(path), policy.candidate_sha256, "candidate SHA-256")
    with path.open("rb") as handle:
        prefix = handle.read(min(policy.candidate_bytes, 512 * 1024))
    if policy.version.encode("ascii") not in prefix:
        raise GateError("candidate does not contain the expected release version")
    if policy.candidate_bytes > policy.app0_bytes:
        raise GateError("candidate does not fit app0")
    programmed_bytes = (policy.candidate_bytes + 0xFFF) & ~0xFFF
    if programmed_bytes > policy.app0_bytes:
        raise GateError("candidate erase-aligned range does not fit app0")
    programmed_digest = hashlib.sha256()
    with path.open("rb", buffering=1024 * 1024) as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            programmed_digest.update(chunk)
    programmed_digest.update(b"\xff" * (programmed_bytes - policy.candidate_bytes))
    return {
        "path": str(path),
        "bytes": policy.candidate_bytes,
        "sha256": policy.candidate_sha256,
        "programmed_sector_bytes": programmed_bytes,
        "programmed_sector_sha256": programmed_digest.hexdigest(),
        "trailing_erased_bytes": programmed_bytes - policy.candidate_bytes,
    }


def _ensure_new_output(path_value: str | Path) -> tuple[Path, Path]:
    output = Path(path_value).resolve(strict=False)
    if output.exists() or output.is_symlink():
        raise GateError(f"output path already exists: {output}")
    parent = output.parent
    try:
        parent_meta = parent.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"output parent is missing: {parent}") from exc
    if stat.S_ISLNK(parent_meta.st_mode) or not stat.S_ISDIR(parent_meta.st_mode):
        raise GateError("output parent must be a real directory")
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=parent))
    os.chmod(temporary, 0o700)
    return output, temporary


def _write_private(path: Path, data: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        raise


def _write_json_private(path: Path, value: dict[str, Any]) -> None:
    _write_private(
        path,
        (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(
            "utf-8"
        ),
    )


def _publish_directory(output: Path, temporary: Path) -> None:
    os.rename(temporary, output)


def _esptool_base(port: str) -> list[str]:
    return [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--before",
        "no_reset",
        "--after",
        "no_reset",
    ]


def _read_command(port: str, operation: str, *arguments: str) -> list[str]:
    if operation not in READ_ONLY_ESPTOOL_OPERATIONS:
        raise GateError(f"refusing non-read-only esptool operation: {operation}")
    return _esptool_base(port) + ["--no-stub", operation, *arguments]


def _run_esptool(
    command: list[str],
    log_path: Path,
    timeout: int,
    pass_fds: tuple[int, ...] = (),
) -> None:
    operations = [
        token
        for token in command
        if token in READ_ONLY_ESPTOOL_OPERATIONS or token == "write_flash"
    ]
    if len(operations) != 1:
        raise GateError("esptool command does not contain one reviewed operation")
    operation = operations[0]
    if operation == "write_flash":
        if len(pass_fds) != 1 or f"/dev/fd/{pass_fds[0]}" not in command:
            raise GateError("write_flash input is not one inherited sealed FD")
    elif pass_fds:
        raise GateError("read-only esptool command unexpectedly inherited an input FD")
    with log_path.open("xb") as log_handle:
        os.chmod(log_path, 0o600)
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
            pass_fds=pass_fds,
        )
        log_handle.flush()
        os.fsync(log_handle.fileno())
    if completed.returncode != 0:
        raise GateError(
            f"esptool operation {operation} failed with exit {completed.returncode}"
        )


def _validate_authorized_port(port: str, policy: GatePolicy) -> None:
    if port != policy.expected_port or not PORT_RE.fullmatch(port):
        raise GateError(f"refusing unreviewed serial port: {port}")
    port_path = Path(port)
    try:
        metadata = port_path.stat()
    except FileNotFoundError as exc:
        raise GateError(f"authorized serial port is not present: {port}") from exc
    if not stat.S_ISCHR(metadata.st_mode):
        raise GateError("authorized serial port is not a character device")


def capture_read_only(port: str, output_value: str | Path, policy: GatePolicy) -> Path:
    _validate_authorized_port(port, policy)
    output, temporary = _ensure_new_output(output_value)
    manifest: dict[str, Any] = {
        "complete": False,
        "policy_id": policy.policy_id,
        "port": port,
        "captured_at_utc": utc_now(),
        "operations": [],
        "files": {},
    }
    try:
        operations: list[tuple[str, str, list[str]]] = [
            ("read_mac", "read-mac.log", []),
            ("chip_id", "chip-id.log", []),
            ("flash_id", "flash-id.log", []),
            ("get_security_info", "security-info.log", []),
            (
                "read_flash",
                "full-flash-before.log",
                ["0x0", hex(policy.flash_bytes), str(temporary / "full-flash-before.bin")],
            ),
        ]
        for operation, log_name, arguments in operations:
            if operation not in READ_ONLY_ESPTOOL_OPERATIONS:
                raise GateError(f"internal capture operation is not read-only: {operation}")
            command = _read_command(port, operation, *arguments)
            log_path = temporary / log_name
            _run_esptool(
                command,
                log_path,
                45 * 60 if operation == "read_flash" else 60,
            )
            manifest["operations"].append(operation)
        full_flash_path = temporary / "full-flash-before.bin"
        if full_flash_path.stat().st_size != policy.flash_bytes:
            raise GateError("read-only full-flash capture has the wrong byte count")
        for name in (
            "read-mac.log",
            "chip-id.log",
            "flash-id.log",
            "security-info.log",
            "full-flash-before.log",
            "full-flash-before.bin",
        ):
            path = temporary / name
            os.chmod(path, 0o600)
            manifest["files"][name] = {
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        manifest["complete"] = True
        _write_json_private(temporary / "capture-manifest.json", manifest)
        _publish_directory(output, temporary)
        return output
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _expected_app_write(candidate: dict[str, Any], policy: GatePolicy) -> dict[str, Any]:
    return {
        "kind": "application-image",
        "partition": "app0",
        "offset": policy.app0_offset,
        "input_bytes": policy.candidate_bytes,
        "input_sha256": policy.candidate_sha256,
        "flash_sectors_affected": {
            "offset": policy.app0_offset,
            "bytes": candidate["programmed_sector_bytes"],
            "expected_sha256_after": candidate["programmed_sector_sha256"],
            "trailing_erased_bytes": candidate["trailing_erased_bytes"],
        },
    }


def _expected_forbidden_ranges(
    candidate: dict[str, Any], policy: GatePolicy
) -> list[dict[str, Any]]:
    return [
        {
            "name": "bootloader-and-partition-table",
            "offset": 0,
            "end": policy.nvs_offset,
        },
        {"name": "nvs", "offset": policy.nvs_offset, "bytes": policy.nvs_bytes},
        {
            "name": "otadata-until-second-gate",
            "offset": policy.otadata_offset,
            "bytes": policy.otadata_bytes,
        },
        {
            "name": "app0-outside-programmed-range",
            "offset": policy.app0_offset + candidate["programmed_sector_bytes"],
            "end": policy.app0_offset + policy.app0_bytes,
        },
        {
            "name": "app1-rollback",
            "offset": policy.app1_offset,
            "bytes": policy.app1_bytes,
        },
        {
            "name": "littlefs",
            "offset": policy.littlefs_offset,
            "bytes": policy.littlefs_bytes,
        },
        {
            "name": "internal-flash-tail-including-coredump",
            "offset": policy.littlefs_offset + policy.littlefs_bytes,
            "end": policy.flash_bytes,
        },
        {"name": "tf-card", "scope": "entire-card"},
    ]


def _continuous_flash_ranges(
    candidate: dict[str, Any], policy: GatePolicy
) -> list[tuple[str, int, int]]:
    """Return a gap-free partitioning of every internal-flash byte."""
    programmed_bytes = candidate.get("programmed_sector_bytes")
    if not isinstance(programmed_bytes, int) or isinstance(programmed_bytes, bool):
        raise GateError("candidate programmed-sector length is invalid")
    if programmed_bytes <= 0 or programmed_bytes > policy.app0_bytes:
        raise GateError("candidate programmed-sector range is outside app0")
    if programmed_bytes % 0x1000 != 0:
        raise GateError("candidate programmed-sector range is not 4 KiB aligned")

    boundaries = (
        ("bootloader-and-partition-table", 0, policy.nvs_offset),
        ("nvs", policy.nvs_offset, policy.nvs_offset + policy.nvs_bytes),
        (
            "otadata",
            policy.otadata_offset,
            policy.otadata_offset + policy.otadata_bytes,
        ),
        (
            "app0-programmed-range",
            policy.app0_offset,
            policy.app0_offset + programmed_bytes,
        ),
        (
            "app0-suffix",
            policy.app0_offset + programmed_bytes,
            policy.app0_offset + policy.app0_bytes,
        ),
        (
            "app1-rollback",
            policy.app1_offset,
            policy.app1_offset + policy.app1_bytes,
        ),
        (
            "littlefs",
            policy.littlefs_offset,
            policy.littlefs_offset + policy.littlefs_bytes,
        ),
        (
            "internal-flash-tail-including-coredump",
            policy.littlefs_offset + policy.littlefs_bytes,
            policy.flash_bytes,
        ),
    )
    cursor = 0
    for name, start, end in boundaries:
        if start != cursor or end < start:
            raise GateError(
                f"flash policy ranges are not continuous at {name}: "
                f"expected 0x{cursor:x}, got 0x{start:x}..0x{end:x}"
            )
        cursor = end
    if cursor != policy.flash_bytes:
        raise GateError("flash policy ranges do not cover the full flash")
    return list(boundaries)


def _continuous_range_receipts(
    full_flash: bytes, candidate: dict[str, Any], policy: GatePolicy
) -> list[dict[str, Any]]:
    _expect(len(full_flash), policy.flash_bytes, "after-image full flash length")
    return [
        {
            "name": name,
            "offset": start,
            "bytes": end - start,
            "sha256": sha256_bytes(full_flash[start:end]),
        }
        for name, start, end in _continuous_flash_ranges(candidate, policy)
    ]


def _after_image_expectation(
    full_flash: bytes,
    candidate: dict[str, Any],
    policy: GatePolicy,
    permitted_delta: dict[str, Any],
) -> dict[str, Any]:
    return {
        "full_flash_bytes": policy.flash_bytes,
        "full_flash_sha256": sha256_bytes(full_flash),
        "permitted_delta": permitted_delta,
        "continuous_ranges": _continuous_range_receipts(
            full_flash, candidate, policy
        ),
    }


def _app_permitted_delta(
    candidate: dict[str, Any], policy: GatePolicy
) -> dict[str, Any]:
    return {
        "kind": "candidate-bytes-plus-required-erase-padding",
        "offset": policy.app0_offset,
        "candidate_bytes": policy.candidate_bytes,
        "candidate_sha256": policy.candidate_sha256,
        "programmed_sector_bytes": candidate["programmed_sector_bytes"],
        "padding_bytes": candidate["trailing_erased_bytes"],
        "padding_value": "ff",
    }


def _make_app_after_image(
    before_flash: bytes, candidate: dict[str, Any], policy: GatePolicy
) -> bytes:
    _expect(len(before_flash), policy.flash_bytes, "before-image full flash length")
    candidate_path_value = candidate.get("path")
    if not isinstance(candidate_path_value, str):
        raise GateError("authorized candidate path is missing")
    candidate_path = _require_regular_input(candidate_path_value, "release candidate")
    candidate_bytes = candidate_path.read_bytes()
    _expect(len(candidate_bytes), policy.candidate_bytes, "candidate byte count")
    _expect_sha(
        sha256_bytes(candidate_bytes), policy.candidate_sha256, "candidate SHA-256"
    )
    programmed_bytes = candidate.get("programmed_sector_bytes")
    if not isinstance(programmed_bytes, int) or isinstance(programmed_bytes, bool):
        raise GateError("candidate programmed-sector length is invalid")
    _continuous_flash_ranges(candidate, policy)
    expected = bytearray(before_flash)
    programmed_end = policy.app0_offset + programmed_bytes
    expected[policy.app0_offset:programmed_end] = b"\xff" * programmed_bytes
    expected[
        policy.app0_offset : policy.app0_offset + len(candidate_bytes)
    ] = candidate_bytes
    return bytes(expected)


def _make_selector_after_image(
    app_after_flash: bytes, policy: GatePolicy
) -> tuple[bytes, bytes, bytes]:
    _expect(
        len(app_after_flash), policy.flash_bytes, "post-app full flash length"
    )
    current_otadata = _slice(
        app_after_flash,
        policy.otadata_offset,
        policy.otadata_bytes,
        "post-app otadata",
    )
    selector, selected_otadata = make_selected_app0_otadata(
        current_otadata, policy
    )
    selected_flash = bytearray(app_after_flash)
    selected_flash[
        policy.otadata_offset : policy.otadata_offset + policy.otadata_bytes
    ] = selected_otadata
    return bytes(selected_flash), selector, selected_otadata


def _range_name_for_offset(
    offset: int, candidate: dict[str, Any], policy: GatePolicy
) -> str:
    for name, start, end in _continuous_flash_ranges(candidate, policy):
        if start <= offset < end:
            return name
    return "outside-full-flash"


def _validate_full_after_image(
    path_value: str | Path,
    expected: bytes,
    candidate: dict[str, Any],
    policy: GatePolicy,
    label: str,
) -> dict[str, Any]:
    """Fail closed unless a full readback byte-matches the sole after-image."""
    path = _require_regular_input(path_value, label)
    _expect(path.stat().st_size, policy.flash_bytes, f"{label} byte count")
    actual = path.read_bytes()
    _expect(len(expected), policy.flash_bytes, f"{label} expected byte count")
    if actual != expected:
        mismatch = next(
            index
            for index, (actual_byte, expected_byte) in enumerate(
                zip(actual, expected, strict=True)
            )
            if actual_byte != expected_byte
        )
        region = _range_name_for_offset(mismatch, candidate, policy)
        raise GateError(
            f"{label} differs from the authorized after-image at 0x{mismatch:x} "
            f"({region}); refusing an out-of-delta mutation"
        )
    return {
        "path": str(path),
        "bytes": policy.flash_bytes,
        "sha256": sha256_bytes(actual),
        "continuous_ranges": _continuous_range_receipts(actual, candidate, policy),
    }


@contextlib.contextmanager
def _sealed_verified_input_fd(
    path_value: str | Path,
    expected_bytes: int,
    expected_sha256: str,
    label: str,
) -> Iterator[int]:
    """Snapshot one verified regular file into an unlinked inherited FD."""
    path = Path(path_value)
    no_follow = getattr(os, "O_NOFOLLOW", None)
    if no_follow is None:
        raise GateError("this platform cannot enforce O_NOFOLLOW for write input")
    flags = os.O_RDONLY | no_follow
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        source_fd = os.open(path, flags)
    except OSError as exc:
        raise GateError(f"{label} cannot be opened without following links: {path}") from exc
    try:
        before = os.fstat(source_fd)
        if not stat.S_ISREG(before.st_mode):
            raise GateError(f"{label} must be a regular file")
        if before.st_uid != os.geteuid() or before.st_nlink != 1:
            raise GateError(f"{label} is not one private owner-controlled inode")
        if stat.S_IMODE(before.st_mode) & 0o077:
            raise GateError(f"{label} must not be accessible by group or others")
        _expect(before.st_size, expected_bytes, f"{label} byte count")
        chunks: list[bytes] = []
        remaining = expected_bytes
        while remaining:
            chunk = os.read(source_fd, min(1024 * 1024, remaining))
            if not chunk:
                raise GateError(f"{label} ended before its declared byte count")
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(source_fd, 1):
            raise GateError(f"{label} exceeds its declared byte count")
        after = os.fstat(source_fd)
        if (
            after.st_dev != before.st_dev
            or after.st_ino != before.st_ino
            or after.st_size != before.st_size
        ):
            raise GateError(f"{label} changed while it was being sealed")
        data = b"".join(chunks)
        _expect_sha(sha256_bytes(data), expected_sha256, f"{label} SHA-256")
    finally:
        os.close(source_fd)

    with tempfile.TemporaryFile(dir=path.parent) as sealed:
        if os.fstat(sealed.fileno()).st_nlink != 0:
            raise GateError(f"sealed {label} still has a filesystem name")
        sealed.write(data)
        sealed.flush()
        os.fsync(sealed.fileno())
        os.fchmod(sealed.fileno(), 0o400)
        sealed.seek(0)
        sealed_data = sealed.read(expected_bytes + 1)
        _expect(len(sealed_data), expected_bytes, f"sealed {label} byte count")
        _expect_sha(
            sha256_bytes(sealed_data), expected_sha256, f"sealed {label} SHA-256"
        )
        sealed.seek(0)
        yield sealed.fileno()


@contextlib.contextmanager
def _exclusive_port_execution(port: str) -> Iterator[None]:
    lock_name = f"inkloop-c151-{sha256_bytes(port.encode())[:16]}.lock"
    lock_path = Path(tempfile.gettempdir()) / lock_name
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(lock_path, flags, 0o600)
    except OSError as exc:
        raise GateError("cannot open the controlled serial execution lock") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise GateError("controlled serial execution lock is not private")
        os.fchmod(descriptor, 0o600)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise GateError("another controlled execution owns this serial port") from exc
        yield
    finally:
        os.close(descriptor)


def _write_fd_command(port: str, offset: int, input_fd: int) -> list[str]:
    if offset < 0:
        raise GateError("reviewed write offset is invalid")
    return _esptool_base(port) + [
        "write_flash",
        "--flash_mode",
        "keep",
        "--flash_freq",
        "keep",
        "--flash_size",
        "keep",
        hex(offset),
        f"/dev/fd/{input_fd}",
    ]


def _capture_execution_identity(
    port: str, directory: Path, policy: GatePolicy
) -> dict[str, Any]:
    for operation, log_name in (
        ("read_mac", "read-mac.log"),
        ("chip_id", "chip-id.log"),
        ("flash_id", "flash-id.log"),
        ("get_security_info", "security-info.log"),
    ):
        _run_esptool(_read_command(port, operation), directory / log_name, 60)
    return _parse_identity_logs(directory, policy)


def _read_execution_full_flash(
    port: str,
    binary_path: Path,
    log_path: Path,
    policy: GatePolicy,
) -> None:
    _run_esptool(
        _read_command(
            port,
            "read_flash",
            "0x0",
            hex(policy.flash_bytes),
            str(binary_path),
        ),
        log_path,
        45 * 60,
    )


def _execute_reviewed_write(
    *,
    port: str,
    input_path: str | Path,
    input_bytes: int,
    input_sha256: str,
    offset: int,
    expected_before: bytes,
    expected_after: bytes,
    candidate: dict[str, Any],
    output_value: str | Path,
    after_name: str,
    phase: str,
    authorization_receipt: dict[str, Any],
    policy: GatePolicy,
) -> Path:
    _require_bound_policy(policy)
    with _exclusive_port_execution(port), _sealed_verified_input_fd(
        input_path,
        input_bytes,
        input_sha256,
        f"{phase} staged input",
    ) as input_fd:
        _validate_authorized_port(port, policy)
        output, temporary = _ensure_new_output(output_value)
        try:
            identity = _capture_execution_identity(port, temporary, policy)
            before_path = temporary / "full-flash-before-write.bin"
            _read_execution_full_flash(
                port,
                before_path,
                temporary / "full-flash-before-write.log",
                policy,
            )
            before_receipt = _validate_full_after_image(
                before_path,
                expected_before,
                candidate,
                policy,
                f"{phase} pre-write full flash",
            )
            before_receipt["path"] = str(output / before_path.name)

            immediate_mac_log = temporary / "read-mac-immediate-pre-write.log"
            _run_esptool(
                _read_command(port, "read_mac"), immediate_mac_log, 60
            )
            immediate_mac_text = immediate_mac_log.read_text(
                encoding="utf-8", errors="strict"
            )
            immediate_macs = {
                _canonical_mac(match)
                for match in re.findall(
                    r"(?im)^MAC:\s*([0-9a-f:]{17})\s*$", immediate_mac_text
                )
            }
            _expect(
                immediate_macs,
                {policy.expected_mac},
                f"{phase} immediate pre-write MAC set",
            )
            _run_esptool(
                _write_fd_command(port, offset, input_fd),
                temporary / "reviewed-write.log",
                45 * 60,
                (input_fd,),
            )
            after_path = temporary / after_name
            _read_execution_full_flash(
                port,
                after_path,
                temporary / f"{Path(after_name).stem}.log",
                policy,
            )
            after_receipt = _validate_full_after_image(
                after_path,
                expected_after,
                candidate,
                policy,
                f"{phase} post-write full flash",
            )
            after_receipt["path"] = str(output / after_name)
            receipt = {
                "status": "pass",
                "policy_id": policy.policy_id,
                "phase": phase,
                "completed_at_utc": utc_now(),
                "commit": policy.commit,
                "version": policy.version,
                "device": identity,
                "authorization": authorization_receipt,
                "sealed_input": {
                    "source_path": str(Path(input_path)),
                    "bytes": input_bytes,
                    "sha256": input_sha256,
                    "path_reopened_by_writer": False,
                },
                "offset": offset,
                "before": before_receipt,
                "after": after_receipt,
            }
            _write_json_private(temporary / "execution-receipt.json", receipt)
            _publish_directory(output, temporary)
            return output
        except BaseException:
            shutil.rmtree(temporary, ignore_errors=True)
            raise


def gate_app(
    capture_dir_value: str | Path,
    candidate_value: str | Path,
    acceptance_value: str | Path,
    baseline_value: str | Path,
    tf_custody_value: str | Path,
    output_value: str | Path,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    _require_bound_policy(policy)
    capture_dir = Path(capture_dir_value)
    try:
        capture_meta = capture_dir.lstat()
    except FileNotFoundError as exc:
        raise GateError(f"capture directory is missing: {capture_dir}") from exc
    if stat.S_ISLNK(capture_meta.st_mode) or not stat.S_ISDIR(capture_meta.st_mode):
        raise GateError("capture directory must be a real directory")
    capture_dir = capture_dir.resolve(strict=True)
    capture_receipt = _validate_capture_manifest(capture_dir, policy)
    identity = _parse_identity_logs(capture_dir, policy)
    full_flash_path = _require_regular_input(
        capture_dir / "full-flash-before.bin", "fresh full flash"
    )
    full_flash = full_flash_path.read_bytes()
    flash_state = validate_flash_before(full_flash, policy)
    source_candidate = _validate_candidate(candidate_value, policy)
    acceptance = _validate_acceptance(acceptance_value, policy)
    baseline = _validate_baseline_custody(baseline_value, policy)
    tf_custody = _validate_tf_custody(tf_custody_value, policy)
    app_after_flash = _make_app_after_image(full_flash, source_candidate, policy)
    app_after_expectation = _after_image_expectation(
        app_after_flash,
        source_candidate,
        policy,
        _app_permitted_delta(source_candidate, policy),
    )

    output, temporary = _ensure_new_output(output_value)
    try:
        candidate_name = "candidate-app0.bin"
        staged_candidate_bytes = Path(source_candidate["path"]).read_bytes()
        _expect(
            len(staged_candidate_bytes),
            policy.candidate_bytes,
            "staged candidate byte count",
        )
        _expect_sha(
            sha256_bytes(staged_candidate_bytes),
            policy.candidate_sha256,
            "staged candidate SHA-256",
        )
        _write_private(temporary / candidate_name, staged_candidate_bytes)
        candidate = {
            **source_candidate,
            "path": str(output / candidate_name),
        }
        authorization = {
            "status": "authorized-app0-only",
            "policy_id": policy.policy_id,
            "created_at_utc": utc_now(),
            "commit": policy.commit,
            "version": policy.version,
            "device": identity,
            "capture": capture_receipt,
            "baseline": baseline,
            "acceptance": acceptance,
            "tf_custody": tf_custody,
            "before": {
                key: value
                for key, value in flash_state.items()
                if key != "otadata"
            },
            "slot_proof": {
                "selected": "app1",
                "inactive_target": "app0",
                "app0_before_sha256": policy.app0_before_sha256,
                "app1_rollback_sha256": policy.app1_rollback_sha256,
                "entry0_sequence": policy.current_app0_sequence,
                "entry1_sequence": policy.current_app1_sequence,
                "state": "VALID",
            },
            "candidate": candidate,
            "authorized_write": _expected_app_write(candidate, policy),
            "expected_after_app_write": app_after_expectation,
            "selector_authorized": False,
            "forbidden_ranges": _expected_forbidden_ranges(candidate, policy),
        }
        _write_json_private(temporary / "app-stage-authorization.json", authorization)
        execution_output = output / "app-execution"
        full_flash_readback = str(execution_output / "full-flash-after-app.bin")
        suggested_selector_output = str(
            output.parent / f"{output.name}-selector"
        )
        plan = {
            "status": "controlled-execution-only",
            "warning": (
                "Run only execute_app_argv. It revalidates and seals the exact "
                "input FD, then automatically verifies a full-flash readback."
            ),
            "port": policy.expected_port,
            "authorized_mutation": {
                "phase": 1,
                "offset": policy.app0_offset,
                "input_bytes": policy.candidate_bytes,
                "input_sha256": policy.candidate_sha256,
                "flash_sectors_affected": {
                    "bytes": candidate["programmed_sector_bytes"],
                    "expected_sha256_after": candidate[
                        "programmed_sector_sha256"
                    ],
                },
            },
            "execute_app_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "execute-app",
                *_release_cli_args(policy),
                "--app-authorization",
                str(output / "app-stage-authorization.json"),
                "--port",
                policy.expected_port,
                "--output-dir",
                str(execution_output),
            ],
            "automatic_full_flash_readback": full_flash_readback,
            "next_gate_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "authorize-selector",
                *_release_cli_args(policy),
                "--app-authorization",
                str(output / "app-stage-authorization.json"),
                "--full-flash-readback",
                full_flash_readback,
                "--output-dir",
                suggested_selector_output,
            ],
            "expected": app_after_expectation,
            "direct_esptool_write_forbidden": True,
            "generic_idf_flash_forbidden": True,
        }
        _write_json_private(temporary / "app-stage-plan.json", plan)
        _publish_directory(output, temporary)
        return output
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _load_app_authorization(
    path_value: str | Path, policy: GatePolicy
) -> tuple[Path, dict[str, Any]]:
    _require_bound_policy(policy)
    path, auth = _read_json(path_value, "app-stage authorization")
    _expect(auth.get("status"), "authorized-app0-only", "app authorization status")
    _expect(auth.get("policy_id"), policy.policy_id, "app authorization policy")
    _expect(auth.get("commit"), policy.commit, "app authorization commit")
    _expect(auth.get("version"), policy.version, "app authorization version")
    _expect(auth.get("selector_authorized"), False, "selector preauthorization")
    candidate = auth.get("candidate")
    if not isinstance(candidate, dict):
        raise GateError("app authorization candidate is missing")
    candidate_path = candidate.get("path")
    if not isinstance(candidate_path, str):
        raise GateError("authorized candidate path is missing")
    _expect(
        candidate,
        _validate_candidate(candidate_path, policy),
        "authorized candidate receipt",
    )
    _expect(
        auth.get("authorized_write"),
        _expected_app_write(candidate, policy),
        "app authorized-write receipt",
    )
    _expect(
        auth.get("forbidden_ranges"),
        _expected_forbidden_ranges(candidate, policy),
        "app forbidden-range receipt",
    )
    capture = auth.get("capture")
    if not isinstance(capture, dict) or not isinstance(capture.get("path"), str):
        raise GateError("app authorization capture receipt is missing")
    capture_manifest_path = _require_regular_input(
        capture["path"], "authorized capture manifest"
    )
    _expect_sha(
        sha256_file(capture_manifest_path),
        str(capture.get("sha256", "")),
        "authorized capture manifest SHA-256",
    )
    capture_dir = capture_manifest_path.parent
    _expect(
        capture,
        _validate_capture_manifest(capture_dir, policy),
        "app authorization capture receipt",
    )
    full_flash_path = _require_regular_input(
        capture_dir / "full-flash-before.bin", "authorized fresh full flash"
    )
    before_flash = full_flash_path.read_bytes()
    flash_state = validate_flash_before(before_flash, policy)
    before = auth.get("before")
    if not isinstance(before, dict):
        raise GateError("app authorization before-state is missing")
    expected_before = {
        key: value for key, value in flash_state.items() if key != "otadata"
    }
    _expect(before, expected_before, "app authorization before-state")
    expected_app_after = _make_app_after_image(before_flash, candidate, policy)
    _expect(
        auth.get("expected_after_app_write"),
        _after_image_expectation(
            expected_app_after,
            candidate,
            policy,
            _app_permitted_delta(candidate, policy),
        ),
        "app authorization expected after-image",
    )
    for label, key, validator in (
        ("acceptance", "acceptance", _validate_acceptance),
        ("baseline custody", "baseline", _validate_baseline_custody),
        ("TF custody", "tf_custody", _validate_tf_custody),
    ):
        receipt = auth.get(key)
        if not isinstance(receipt, dict) or not isinstance(
            receipt.get("path") or receipt.get("custody_path"), str
        ):
            raise GateError(f"app authorization {label} receipt is missing")
        source_path = receipt.get("path") or receipt.get("custody_path")
        _expect(
            receipt,
            validator(source_path, policy),
            f"app authorization {label} receipt",
        )
    slot = auth.get("slot_proof")
    if not isinstance(slot, dict):
        raise GateError("app authorization slot proof is missing")
    _expect(
        slot,
        {
            "selected": "app1",
            "inactive_target": "app0",
            "app0_before_sha256": policy.app0_before_sha256,
            "app1_rollback_sha256": policy.app1_rollback_sha256,
            "entry0_sequence": policy.current_app0_sequence,
            "entry1_sequence": policy.current_app1_sequence,
            "state": "VALID",
        },
        "app authorization slot proof",
    )
    return path, auth


def _authorized_before_flash(
    app_auth: dict[str, Any], policy: GatePolicy
) -> bytes:
    capture = app_auth.get("capture")
    if not isinstance(capture, dict) or not isinstance(capture.get("path"), str):
        raise GateError("app authorization capture receipt is missing")
    manifest_path = _require_regular_input(
        capture["path"], "authorized capture manifest"
    )
    full_flash_path = _require_regular_input(
        manifest_path.parent / "full-flash-before.bin",
        "authorized fresh full flash",
    )
    before_flash = full_flash_path.read_bytes()
    validate_flash_before(before_flash, policy)
    return before_flash


def execute_app(
    authorization_value: str | Path,
    port: str,
    output_value: str | Path,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    app_auth_path, app_auth = _load_app_authorization(authorization_value, policy)
    candidate = app_auth.get("candidate")
    if not isinstance(candidate, dict) or not isinstance(candidate.get("path"), str):
        raise GateError("app authorization staged candidate is missing")
    before_flash = _authorized_before_flash(app_auth, policy)
    app_after_flash = _make_app_after_image(before_flash, candidate, policy)
    return _execute_reviewed_write(
        port=port,
        input_path=candidate["path"],
        input_bytes=policy.candidate_bytes,
        input_sha256=policy.candidate_sha256,
        offset=policy.app0_offset,
        expected_before=before_flash,
        expected_after=app_after_flash,
        candidate=candidate,
        output_value=output_value,
        after_name="full-flash-after-app.bin",
        phase="app0",
        authorization_receipt={
            "path": str(app_auth_path),
            "sha256": sha256_file(app_auth_path),
        },
        policy=policy,
    )


def _selector_delta(
    selector: bytes, selected_otadata: bytes, policy: GatePolicy
) -> dict[str, Any]:
    return {
        "kind": "selector-new-entry-plus-required-sector-erase",
        "offset": policy.otadata_offset,
        "input_bytes": len(selector),
        "input_sha256": sha256_bytes(selector),
        "erased_sector_bytes": 0x1000,
        "erased_tail_bytes": 0x1000 - len(selector),
        "erased_tail_value": "ff",
        "retained_entry1_sha256": sha256_bytes(selected_otadata[0x1000:0x2000]),
    }


def _rollback_delta(
    rollback_entry: bytes, current_otadata: bytes, policy: GatePolicy
) -> dict[str, Any]:
    return {
        "kind": "restore-original-selector-entry-plus-required-sector-erase",
        "offset": policy.otadata_offset,
        "input_bytes": len(rollback_entry),
        "input_sha256": sha256_bytes(rollback_entry),
        "erased_sector_bytes": 0x1000,
        "erased_tail_bytes": 0x1000 - len(rollback_entry),
        "erased_tail_value": "ff",
        "retained_entry1_sha256": sha256_bytes(current_otadata[0x1000:0x2000]),
    }


def authorize_selector(
    authorization_value: str | Path,
    full_flash_readback_value: str | Path,
    output_value: str | Path,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    _require_bound_policy(policy)
    app_auth_path, app_auth = _load_app_authorization(authorization_value, policy)
    candidate = app_auth.get("candidate")
    if not isinstance(candidate, dict):
        raise GateError("app authorization candidate is missing")
    before_flash = _authorized_before_flash(app_auth, policy)
    app_after_flash = _make_app_after_image(before_flash, candidate, policy)
    app_readback = _validate_full_after_image(
        full_flash_readback_value,
        app_after_flash,
        candidate,
        policy,
        "full flash after app write",
    )
    selected_flash, selector, selected_otadata = _make_selector_after_image(
        app_after_flash, policy
    )
    current_otadata = _slice(
        app_after_flash,
        policy.otadata_offset,
        policy.otadata_bytes,
        "post-app otadata",
    )
    original_entry = current_otadata[:32]
    expected_after = _after_image_expectation(
        selected_flash,
        candidate,
        policy,
        _selector_delta(selector, selected_otadata, policy),
    )
    expected_rollback_after = _after_image_expectation(
        app_after_flash,
        candidate,
        policy,
        _rollback_delta(original_entry, current_otadata, policy),
    )

    output, temporary = _ensure_new_output(output_value)
    try:
        selector_name = "selector-entry0-seq3-new.bin"
        rollback_name = "rollback-entry0-seq1-valid.bin"
        _write_private(temporary / selector_name, selector)
        _write_private(temporary / rollback_name, original_entry)
        selector_auth = {
            "status": "authorized-selector-after-app-readback",
            "policy_id": policy.policy_id,
            "created_at_utc": utc_now(),
            "commit": policy.commit,
            "version": policy.version,
            "app_authorization": {
                "path": str(app_auth_path),
                "sha256": sha256_file(app_auth_path),
            },
            "verified_after_app_write": {"full_flash": app_readback},
            "authorized_write": {
                "kind": "ota-selector-entry",
                "offset": policy.otadata_offset,
                "input_path": str(output / selector_name),
                "input_bytes": 32,
                "input_sha256": sha256_bytes(selector),
                "flash_sector_affected": {
                    "offset": policy.otadata_offset,
                    "bytes": 0x1000,
                    "expected_sha256_after": sha256_bytes(
                        selected_otadata[:0x1000]
                    ),
                },
                "entry": {
                    "sequence": policy.next_app0_sequence,
                    "state": "NEW",
                    "crc32_le": f"{ota_crc(policy.next_app0_sequence):08x}",
                    "selects": "app0",
                },
            },
            "expected_after": expected_after,
            "rollback": {
                "offset": policy.otadata_offset,
                "input_path": str(output / rollback_name),
                "input_bytes": 32,
                "input_sha256": sha256_bytes(original_entry),
                "restores_selected": "app1",
                "expected_after": expected_rollback_after,
            },
        }
        _write_json_private(temporary / "selector-authorization.json", selector_auth)
        selector_execution = output / "selector-execution"
        final_full_flash = str(selector_execution / "full-flash-final.bin")
        rollback_execution = output / "rollback-execution"
        rollback_full_flash = str(
            rollback_execution / "full-flash-after-rollback.bin"
        )
        plan = {
            "status": "controlled-execution-only",
            "warning": (
                "Run only the controlled execute argv entries. They seal exact "
                "32-byte inputs and automatically verify full-flash readbacks."
            ),
            "port": policy.expected_port,
            "authorized_selector_mutation": {
                "phase": 2,
                "offset": policy.otadata_offset,
                "input_bytes": 32,
                "input_sha256": sha256_bytes(selector),
            },
            "execute_selector_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "execute-selector",
                *_release_cli_args(policy),
                "--selector-authorization",
                str(output / "selector-authorization.json"),
                "--port",
                policy.expected_port,
                "--output-dir",
                str(selector_execution),
            ],
            "automatic_final_full_flash_readback": final_full_flash,
            "final_verification_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "verify-selector",
                *_release_cli_args(policy),
                "--selector-authorization",
                str(output / "selector-authorization.json"),
                "--full-flash-readback",
                final_full_flash,
                "--expect",
                "app0",
                "--output",
                str(output / "selector-verification.json"),
            ],
            "authorized_rollback_mutation_if_candidate_fails": {
                "offset": policy.otadata_offset,
                "input_bytes": 32,
                "input_sha256": sha256_bytes(original_entry),
            },
            "execute_rollback_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "execute-rollback",
                *_release_cli_args(policy),
                "--selector-authorization",
                str(output / "selector-authorization.json"),
                "--port",
                policy.expected_port,
                "--output-dir",
                str(rollback_execution),
            ],
            "automatic_rollback_full_flash_readback": rollback_full_flash,
            "rollback_verification_argv": [
                sys.executable,
                str(Path(__file__).resolve()),
                "verify-selector",
                *_release_cli_args(policy),
                "--selector-authorization",
                str(output / "selector-authorization.json"),
                "--full-flash-readback",
                rollback_full_flash,
                "--expect",
                "rollback-app1",
                "--output",
                str(output / "rollback-verification.json"),
            ],
            "direct_esptool_write_forbidden": True,
            "generic_idf_flash_forbidden": True,
        }
        _write_json_private(temporary / "selector-stage-plan.json", plan)
        _publish_directory(output, temporary)
        return output
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def verify_selector(
    selector_authorization_value: str | Path,
    full_flash_readback_value: str | Path,
    output_value: str | Path,
    expect_rollback: bool,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    _require_bound_policy(policy)
    auth_path, auth = _read_json(
        selector_authorization_value, "selector authorization"
    )
    _expect(
        auth.get("status"),
        "authorized-selector-after-app-readback",
        "selector authorization status",
    )
    _expect(
        auth.get("policy_id"), policy.policy_id, "selector authorization policy"
    )
    _expect(auth.get("commit"), policy.commit, "selector authorization commit")
    _expect(auth.get("version"), policy.version, "selector authorization version")
    app_receipt = auth.get("app_authorization")
    if not isinstance(app_receipt, dict) or not isinstance(
        app_receipt.get("path"), str
    ):
        raise GateError("selector authorization app receipt is missing")
    app_auth_path = _require_regular_input(
        app_receipt["path"], "selector's app authorization"
    )
    _expect_sha(
        app_receipt.get("sha256"),
        sha256_file(app_auth_path),
        "selector's app authorization SHA-256",
    )
    _, app_auth = _load_app_authorization(app_auth_path, policy)
    candidate = app_auth.get("candidate")
    if not isinstance(candidate, dict):
        raise GateError("selector's app authorization candidate is missing")
    before_flash = _authorized_before_flash(app_auth, policy)
    app_after_flash = _make_app_after_image(before_flash, candidate, policy)

    verified = auth.get("verified_after_app_write")
    if not isinstance(verified, dict):
        raise GateError("selector's post-app readback receipt is missing")
    stored_full_flash = verified.get("full_flash")
    if not isinstance(stored_full_flash, dict) or not isinstance(
        stored_full_flash.get("path"), str
    ):
        raise GateError("selector's post-app full-flash receipt is missing")
    expected_verified = {
        "full_flash": _validate_full_after_image(
            stored_full_flash["path"],
            app_after_flash,
            candidate,
            policy,
            "authorized full flash after app write",
        )
    }
    _expect(verified, expected_verified, "selector's post-app readback receipt")

    selected_flash, selector, selected_otadata = _make_selector_after_image(
        app_after_flash, policy
    )
    current_otadata = _slice(
        app_after_flash,
        policy.otadata_offset,
        policy.otadata_bytes,
        "selector's original otadata",
    )
    rollback_entry = current_otadata[:32]
    selector_path = _require_regular_input(
        auth_path.parent / "selector-entry0-seq3-new.bin", "authorized selector bytes"
    )
    _expect(selector_path.read_bytes(), selector, "authorized selector bytes")
    rollback_path = _require_regular_input(
        auth_path.parent / "rollback-entry0-seq1-valid.bin",
        "authorized rollback selector bytes",
    )
    _expect(
        rollback_path.read_bytes(),
        rollback_entry,
        "authorized rollback selector bytes",
    )

    expected_after = _after_image_expectation(
        selected_flash,
        candidate,
        policy,
        _selector_delta(selector, selected_otadata, policy),
    )
    expected_rollback_after = _after_image_expectation(
        app_after_flash,
        candidate,
        policy,
        _rollback_delta(rollback_entry, current_otadata, policy),
    )
    _expect(auth.get("expected_after"), expected_after, "selector expected after-image")
    stored_rollback = auth.get("rollback")
    if not isinstance(stored_rollback, dict) or not isinstance(
        stored_rollback.get("input_path"), str
    ):
        raise GateError("selector rollback receipt is missing")
    _expect(
        _require_regular_input(
            stored_rollback["input_path"], "selector rollback input"
        ),
        rollback_path,
        "selector rollback input path",
    )
    rollback = {
        "offset": policy.otadata_offset,
        "input_path": stored_rollback["input_path"],
        "input_bytes": 32,
        "input_sha256": sha256_bytes(rollback_entry),
        "restores_selected": "app1",
        "expected_after": expected_rollback_after,
    }
    _expect(auth.get("rollback"), rollback, "selector rollback receipt")

    authorized_write = auth.get("authorized_write")
    if not isinstance(authorized_write, dict):
        raise GateError("selector authorized-write receipt is missing")
    selector_input = authorized_write.get("input_path")
    if not isinstance(selector_input, str):
        raise GateError("selector input path is missing")
    _expect(
        _require_regular_input(selector_input, "selector input"),
        selector_path,
        "selector input path",
    )
    expected_write = {
        "kind": "ota-selector-entry",
        "offset": policy.otadata_offset,
        "input_path": selector_input,
        "input_bytes": 32,
        "input_sha256": sha256_bytes(selector),
        "flash_sector_affected": {
            "offset": policy.otadata_offset,
            "bytes": 0x1000,
            "expected_sha256_after": sha256_bytes(selected_otadata[:0x1000]),
        },
        "entry": {
            "sequence": policy.next_app0_sequence,
            "state": "NEW",
            "crc32_le": f"{ota_crc(policy.next_app0_sequence):08x}",
            "selects": "app0",
        },
    }
    _expect(authorized_write, expected_write, "selector authorized-write receipt")

    expected_flash = app_after_flash if expect_rollback else selected_flash
    full_flash = _validate_full_after_image(
        full_flash_readback_value,
        expected_flash,
        candidate,
        policy,
        "full flash after rollback" if expect_rollback else "final full flash",
    )
    full_flash_data = Path(full_flash["path"]).read_bytes()
    ota = _slice(
        full_flash_data, policy.otadata_offset, policy.otadata_bytes, "final otadata"
    )
    if expect_rollback:
        state = validate_current_otadata(ota, policy)
        selected = "app1"
    else:
        first = parse_ota_entry(ota, 0)
        second = parse_ota_entry(ota, 0x1000)
        _expect(first["sequence"], policy.next_app0_sequence, "final app0 sequence")
        _expect(first["state"], policy.next_app0_state, "final app0 state")
        _expect(first["valid_crc"], True, "final app0 selector CRC")
        _expect(
            second["sequence"], policy.current_app1_sequence, "retained app1 sequence"
        )
        _expect(second["state"], policy.ota_valid_state, "retained app1 state")
        _expect(second["valid_crc"], True, "retained app1 selector CRC")
        if first["sequence"] <= second["sequence"]:
            raise GateError("final selector does not select app0")
        _expect_sha(
            sha256_bytes(ota),
            sha256_bytes(selected_otadata),
            "final otadata SHA-256",
        )
        state = {"entry0": first, "entry1": second}
        selected = "app0"
    result = {
        "status": "pass",
        "policy_id": policy.policy_id,
        "verified_at_utc": utc_now(),
        "commit": policy.commit,
        "selected": selected,
        "selector_authorization": {
            "path": str(auth_path),
            "sha256": sha256_file(auth_path),
        },
        "readbacks": {"full_flash": full_flash},
        "otadata": {
            "entry0_sequence": state["entry0"]["sequence"],
            "entry1_sequence": state["entry1"]["sequence"],
            "sha256": sha256_bytes(ota),
        },
    }
    output = Path(output_value).absolute()
    if output.exists() or output.is_symlink():
        raise GateError(f"verification output already exists: {output}")
    parent = output.parent
    if not parent.is_dir() or parent.is_symlink():
        raise GateError("verification output parent must be a real directory")
    _write_json_private(output, result)
    return output


def _validated_selector_execution_context(
    selector_authorization_value: str | Path,
    policy: GatePolicy,
) -> dict[str, Any]:
    auth_path, auth = _read_json(
        selector_authorization_value, "selector authorization"
    )
    verified = auth.get("verified_after_app_write")
    if not isinstance(verified, dict):
        raise GateError("selector's post-app readback receipt is missing")
    full_flash = verified.get("full_flash")
    if not isinstance(full_flash, dict) or not isinstance(full_flash.get("path"), str):
        raise GateError("selector's post-app full-flash receipt is missing")
    with tempfile.TemporaryDirectory(prefix="inkloop-selector-auth-check-") as check:
        verify_selector(
            auth_path,
            full_flash["path"],
            Path(check) / "validated.json",
            True,
            policy,
        )

    app_receipt = auth.get("app_authorization")
    if not isinstance(app_receipt, dict) or not isinstance(
        app_receipt.get("path"), str
    ):
        raise GateError("selector authorization app receipt is missing")
    app_auth_path, app_auth = _load_app_authorization(app_receipt["path"], policy)
    _expect_sha(
        app_receipt.get("sha256"),
        sha256_file(app_auth_path),
        "selector's app authorization SHA-256",
    )
    candidate = app_auth.get("candidate")
    if not isinstance(candidate, dict):
        raise GateError("selector's app authorization candidate is missing")
    before_flash = _authorized_before_flash(app_auth, policy)
    app_after_flash = _make_app_after_image(before_flash, candidate, policy)
    selected_flash, selector, _ = _make_selector_after_image(
        app_after_flash, policy
    )
    current_otadata = _slice(
        app_after_flash,
        policy.otadata_offset,
        policy.otadata_bytes,
        "selector's original otadata",
    )
    rollback_entry = current_otadata[:32]
    return {
        "auth_path": auth_path,
        "auth_sha256": sha256_file(auth_path),
        "candidate": candidate,
        "app_after_flash": app_after_flash,
        "selected_flash": selected_flash,
        "selector": selector,
        "selector_path": auth_path.parent / "selector-entry0-seq3-new.bin",
        "rollback_entry": rollback_entry,
        "rollback_path": auth_path.parent / "rollback-entry0-seq1-valid.bin",
    }


def execute_selector(
    selector_authorization_value: str | Path,
    port: str,
    output_value: str | Path,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    context = _validated_selector_execution_context(
        selector_authorization_value, policy
    )
    return _execute_reviewed_write(
        port=port,
        input_path=context["selector_path"],
        input_bytes=32,
        input_sha256=sha256_bytes(context["selector"]),
        offset=policy.otadata_offset,
        expected_before=context["app_after_flash"],
        expected_after=context["selected_flash"],
        candidate=context["candidate"],
        output_value=output_value,
        after_name="full-flash-final.bin",
        phase="selector",
        authorization_receipt={
            "path": str(context["auth_path"]),
            "sha256": context["auth_sha256"],
        },
        policy=policy,
    )


def execute_rollback(
    selector_authorization_value: str | Path,
    port: str,
    output_value: str | Path,
    policy: GatePolicy = PRODUCTION_POLICY,
) -> Path:
    context = _validated_selector_execution_context(
        selector_authorization_value, policy
    )
    return _execute_reviewed_write(
        port=port,
        input_path=context["rollback_path"],
        input_bytes=32,
        input_sha256=sha256_bytes(context["rollback_entry"]),
        offset=policy.otadata_offset,
        expected_before=context["selected_flash"],
        expected_after=context["app_after_flash"],
        candidate=context["candidate"],
        output_value=output_value,
        after_name="full-flash-after-rollback.bin",
        phase="rollback",
        authorization_receipt={
            "path": str(context["auth_path"]),
            "sha256": context["auth_sha256"],
        },
        policy=policy,
    )


def _add_release_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-candidate-sha256", required=True)
    parser.add_argument("--expected-candidate-bytes", required=True, type=int)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fail-closed accepted-candidate inactive-app0 physical staging gate"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture = subparsers.add_parser(
        "capture", help="capture a fixed read-only device snapshot"
    )
    capture.add_argument("--port", required=True)
    capture.add_argument("--output-dir", required=True)

    gate = subparsers.add_parser(
        "gate-app", help="authorize only one freshly accepted app0 image write"
    )
    gate.add_argument("--capture-dir", required=True)
    gate.add_argument("--candidate", required=True)
    gate.add_argument("--acceptance-result", required=True)
    gate.add_argument("--baseline-custody", required=True)
    gate.add_argument("--tf-custody", required=True)
    gate.add_argument("--output-dir", required=True)
    _add_release_arguments(gate)

    execute_app_parser = subparsers.add_parser(
        "execute-app",
        help="revalidate, seal and execute the sole authorized app0 write",
    )
    execute_app_parser.add_argument("--app-authorization", required=True)
    execute_app_parser.add_argument("--port", required=True)
    execute_app_parser.add_argument("--output-dir", required=True)
    _add_release_arguments(execute_app_parser)

    selector = subparsers.add_parser(
        "authorize-selector",
        help="emit selector bytes only after a full-flash after-image passes",
    )
    selector.add_argument("--app-authorization", required=True)
    selector.add_argument("--full-flash-readback", required=True)
    selector.add_argument("--output-dir", required=True)
    _add_release_arguments(selector)

    for command, help_text in (
        (
            "execute-selector",
            "revalidate, seal and execute the sole authorized selector write",
        ),
        (
            "execute-rollback",
            "revalidate, seal and execute the sole authorized rollback write",
        ),
    ):
        execute_selector_parser = subparsers.add_parser(command, help=help_text)
        execute_selector_parser.add_argument(
            "--selector-authorization", required=True
        )
        execute_selector_parser.add_argument("--port", required=True)
        execute_selector_parser.add_argument("--output-dir", required=True)
        _add_release_arguments(execute_selector_parser)

    verify = subparsers.add_parser(
        "verify-selector", help="verify final app0 selection or reviewed rollback"
    )
    verify.add_argument("--selector-authorization", required=True)
    verify.add_argument("--full-flash-readback", required=True)
    verify.add_argument("--expect", choices=("app0", "rollback-app1"), default="app0")
    verify.add_argument("--output", required=True)
    _add_release_arguments(verify)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(list(argv) if argv is not None else None)
    try:
        if arguments.command == "capture":
            result = capture_read_only(arguments.port, arguments.output_dir, PRODUCTION_POLICY)
        else:
            policy = bind_release(
                PRODUCTION_POLICY,
                arguments.expected_commit,
                arguments.expected_version,
                arguments.expected_candidate_sha256,
                arguments.expected_candidate_bytes,
            )
            if arguments.command == "gate-app":
                result = gate_app(
                    arguments.capture_dir,
                    arguments.candidate,
                    arguments.acceptance_result,
                    arguments.baseline_custody,
                    arguments.tf_custody,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "execute-app":
                result = execute_app(
                    arguments.app_authorization,
                    arguments.port,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "authorize-selector":
                result = authorize_selector(
                    arguments.app_authorization,
                    arguments.full_flash_readback,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "execute-selector":
                result = execute_selector(
                    arguments.selector_authorization,
                    arguments.port,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "execute-rollback":
                result = execute_rollback(
                    arguments.selector_authorization,
                    arguments.port,
                    arguments.output_dir,
                    policy,
                )
            elif arguments.command == "verify-selector":
                result = verify_selector(
                    arguments.selector_authorization,
                    arguments.full_flash_readback,
                    arguments.output,
                    arguments.expect == "rollback-app1",
                    policy,
                )
            else:  # pragma: no cover - argparse makes this unreachable.
                raise GateError("unknown command")
        print(result)
        return 0
    except GateError as exc:
        print(f"BLOCKED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
