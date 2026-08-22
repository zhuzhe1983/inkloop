#!/usr/bin/env python3
"""Verify and atomically promote one immutable Inkloop OTA release."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile

from package_ota_release import (
    BOARD_SKU,
    MANIFEST_KEYS,
    MAXIMUM_IMAGE_BYTES,
    SIGNATURE_POLICY,
    ReleaseError,
    SemVersion,
    _absolute_without_traversal,
    _directory_without_symlink,
    _inside,
    _join_public_url,
    _public_base_url,
    _same_file_snapshot,
)
from sign_ota_manifest import SPKI_PREFIX, canonical_bytes


RECEIPT_KEYS = (
    "schema_version",
    "board_sku",
    "firmware_version",
    "image_path",
    "image_size",
    "image_sha256",
    "manifest_path",
    "manifest_size",
    "manifest_sha256",
)
SHA256_HEX = re.compile(r"^[0-9a-f]{64}$")
SIGNATURE_HEX = re.compile(r"^[0-9a-f]{128}$")


class JsonObjectPairs(list[tuple[str, object]]):
    """Distinguish a JSON object from an array while retaining key order."""


@dataclass(frozen=True)
class FileSnapshot:
    path: Path
    metadata: os.stat_result


@dataclass(frozen=True)
class ValidatedRelease:
    version: SemVersion
    manifest: dict[str, object]
    manifest_bytes: bytes
    manifest_sha256: str
    snapshots: tuple[FileSnapshot, ...]


def _open_flags() -> int:
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    return flags


def _stable_regular_bytes(
    path: Path, label: str, maximum: int
) -> tuple[bytes, os.stat_result]:
    try:
        before = path.lstat()
        if (
            stat.S_ISLNK(before.st_mode)
            or not stat.S_ISREG(before.st_mode)
            or before.st_size <= 0
            or before.st_size > maximum
        ):
            raise ReleaseError(f"invalid_{label}")
        descriptor = os.open(path, _open_flags())
    except (OSError, ReleaseError) as error:
        if isinstance(error, ReleaseError):
            raise
        raise ReleaseError(f"invalid_{label}") from error
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or not _same_file_snapshot(before, opened):
            raise ReleaseError(f"{label}_changed")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(64 * 1024, maximum + 1 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > maximum:
                raise ReleaseError(f"invalid_{label}")
        after_open = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    try:
        after_path = path.lstat()
    except OSError as error:
        raise ReleaseError(f"{label}_changed") from error
    if (
        total <= 0
        or not _same_file_snapshot(opened, after_open)
        or not _same_file_snapshot(opened, after_path)
    ):
        raise ReleaseError(f"{label}_changed")
    return b"".join(chunks), opened


def _stable_regular_hash(
    path: Path, label: str, maximum: int
) -> tuple[str, int, os.stat_result]:
    try:
        before = path.lstat()
        if (
            stat.S_ISLNK(before.st_mode)
            or not stat.S_ISREG(before.st_mode)
            or before.st_size <= 0
            or before.st_size > maximum
        ):
            raise ReleaseError(f"invalid_{label}")
        descriptor = os.open(path, _open_flags())
    except (OSError, ReleaseError) as error:
        if isinstance(error, ReleaseError):
            raise
        raise ReleaseError(f"invalid_{label}") from error
    digest = hashlib.sha256()
    total = 0
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or not _same_file_snapshot(before, opened):
            raise ReleaseError(f"{label}_changed")
        while True:
            chunk = os.read(descriptor, 64 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > maximum:
                raise ReleaseError(f"invalid_{label}")
            digest.update(chunk)
        after_open = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    try:
        after_path = path.lstat()
    except OSError as error:
        raise ReleaseError(f"{label}_changed") from error
    if (
        total <= 0
        or not _same_file_snapshot(opened, after_open)
        or not _same_file_snapshot(opened, after_path)
    ):
        raise ReleaseError(f"{label}_changed")
    return digest.hexdigest(), total, opened


def _ordered_json(data: bytes, expected: tuple[str, ...], label: str) -> dict[str, object]:
    try:
        parsed = json.loads(
            data.decode("ascii"), object_pairs_hook=JsonObjectPairs
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseError(f"invalid_{label}") from error
    if not isinstance(parsed, JsonObjectPairs):
        raise ReleaseError(f"invalid_{label}")
    keys = tuple(key for key, _ in parsed)
    if keys != expected or len(set(keys)) != len(keys):
        raise ReleaseError(f"invalid_{label}")
    return dict(parsed)


def _manifest_contract(
    data: bytes,
    board_sku: str,
    version: SemVersion,
    public_base: str,
) -> dict[str, object]:
    manifest = _ordered_json(data, MANIFEST_KEYS, "manifest")
    image_name = f"inkloop-idf-{board_sku}-{version.path_component}.bin"
    image_relative = PurePosixPath(board_sku, version.path_component, image_name)
    if (
        type(manifest["schema_version"]) is not int
        or manifest["schema_version"] != 1
        or manifest["board_sku"] != board_sku
        or manifest["firmware_version"] != version.value
        or manifest["image_url"] != _join_public_url(public_base, image_relative)
        or type(manifest["image_size"]) is not int
        or not 0 < manifest["image_size"] <= MAXIMUM_IMAGE_BYTES
        or not isinstance(manifest["image_sha256"], str)
        or not SHA256_HEX.fullmatch(manifest["image_sha256"])
        or manifest["signature_policy"] != SIGNATURE_POLICY
        or not isinstance(manifest["detached_signature"], str)
        or not SIGNATURE_HEX.fullmatch(manifest["detached_signature"])
    ):
        raise ReleaseError("invalid_manifest")
    return manifest


def _receipt_contract(
    data: bytes,
    board_sku: str,
    version: SemVersion,
    image_size: int,
    image_sha256: str,
    manifest_size: int,
    manifest_sha256: str,
) -> dict[str, object]:
    receipt = _ordered_json(data, RECEIPT_KEYS, "receipt")
    image_name = f"inkloop-idf-{board_sku}-{version.path_component}.bin"
    image_relative = PurePosixPath(board_sku, version.path_component, image_name)
    manifest_relative = PurePosixPath(board_sku, version.path_component, "manifest.json")
    if (
        type(receipt["schema_version"]) is not int
        or receipt["schema_version"] != 1
        or receipt["board_sku"] != board_sku
        or receipt["firmware_version"] != version.value
        or receipt["image_path"] != image_relative.as_posix()
        or type(receipt["image_size"]) is not int
        or receipt["image_size"] != image_size
        or receipt["image_sha256"] != image_sha256
        or receipt["manifest_path"] != manifest_relative.as_posix()
        or type(receipt["manifest_size"]) is not int
        or receipt["manifest_size"] != manifest_size
        or receipt["manifest_sha256"] != manifest_sha256
    ):
        raise ReleaseError("invalid_receipt")
    return receipt


def _verify_signature(
    manifest: dict[str, object], public_key: bytes
) -> None:
    digest = bytes.fromhex(str(manifest["image_sha256"]))
    try:
        message = canonical_bytes(
            str(manifest["board_sku"]),
            str(manifest["firmware_version"]),
            int(manifest["image_size"]),
            digest,
        ) + digest
    except (UnicodeError, ValueError) as error:
        raise ReleaseError("invalid_manifest") from error
    signature = bytes.fromhex(str(manifest["detached_signature"]))
    with tempfile.TemporaryDirectory(prefix="inkloop-ota-verify-") as temporary_name:
        temporary = Path(temporary_name)
        public_der = temporary / "release-public.der"
        signed_message = temporary / "signed-message.bin"
        signature_path = temporary / "signature.bin"
        public_der.write_bytes(SPKI_PREFIX + public_key)
        signed_message.write_bytes(message)
        signature_path.write_bytes(signature)
        try:
            verified = subprocess.run(
                (
                    "openssl",
                    "pkeyutl",
                    "-verify",
                    "-rawin",
                    "-pubin",
                    "-keyform",
                    "DER",
                    "-inkey",
                    str(public_der),
                    "-sigfile",
                    str(signature_path),
                    "-in",
                    str(signed_message),
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        except OSError as error:
            raise ReleaseError("signature_verification_failed") from error
    if verified.returncode != 0:
        raise ReleaseError("signature_verification_failed")


def _validated_release(
    output_root: Path,
    board_sku: str,
    version: SemVersion,
    public_base: str,
    public_key: bytes,
) -> ValidatedRelease:
    release_directory_path = output_root / board_sku / version.path_component
    release_directory = _directory_without_symlink(
        release_directory_path, "release_directory"
    )
    directory_before = release_directory.lstat()
    image_name = f"inkloop-idf-{board_sku}-{version.path_component}.bin"
    expected_names = {image_name, "manifest.json", "release-receipt.json"}
    try:
        entries = {entry.name for entry in release_directory.iterdir()}
    except OSError as error:
        raise ReleaseError("invalid_release_directory") from error
    if entries != expected_names:
        raise ReleaseError("ambiguous_release_directory")

    manifest_path = release_directory / "manifest.json"
    receipt_path = release_directory / "release-receipt.json"
    image_path = release_directory / image_name
    manifest_bytes, manifest_metadata = _stable_regular_bytes(
        manifest_path, "manifest", 4096
    )
    manifest = _manifest_contract(manifest_bytes, board_sku, version, public_base)
    image_sha256, image_size, image_metadata = _stable_regular_hash(
        image_path, "image", MAXIMUM_IMAGE_BYTES
    )
    if (
        image_size != manifest["image_size"]
        or image_sha256 != manifest["image_sha256"]
    ):
        raise ReleaseError("image_manifest_mismatch")
    receipt_bytes, receipt_metadata = _stable_regular_bytes(
        receipt_path, "receipt", 4096
    )
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    _receipt_contract(
        receipt_bytes,
        board_sku,
        version,
        image_size,
        image_sha256,
        len(manifest_bytes),
        manifest_sha256,
    )
    _verify_signature(manifest, public_key)
    try:
        directory_after = release_directory.lstat()
    except OSError as error:
        raise ReleaseError("release_changed") from error
    if not _same_file_snapshot(directory_before, directory_after):
        raise ReleaseError("release_changed")
    return ValidatedRelease(
        version=version,
        manifest=manifest,
        manifest_bytes=manifest_bytes,
        manifest_sha256=manifest_sha256,
        snapshots=(
            FileSnapshot(release_directory, directory_after),
            FileSnapshot(manifest_path, manifest_metadata),
            FileSnapshot(receipt_path, receipt_metadata),
            FileSnapshot(image_path, image_metadata),
        ),
    )


def _scan_board_history(board_directory: Path, board_sku: str, public_base: str) -> None:
    for entry in board_directory.iterdir():
        if entry.name == "manifest.json":
            _stable_regular_bytes(entry, "channel_manifest", 4096)
            continue
        if entry.name.startswith(".") or entry.is_symlink() or not entry.is_dir():
            raise ReleaseError("ambiguous_release_history")
        manifest_bytes, _ = _stable_regular_bytes(
            entry / "manifest.json", "existing_manifest", 4096
        )
        partial = _ordered_json(manifest_bytes, MANIFEST_KEYS, "existing_manifest")
        if partial.get("board_sku") != board_sku or not isinstance(
            partial.get("firmware_version"), str
        ):
            raise ReleaseError("ambiguous_release_history")
        try:
            version = SemVersion(partial["firmware_version"])
            _manifest_contract(manifest_bytes, board_sku, version, public_base)
        except ReleaseError as error:
            raise ReleaseError("ambiguous_release_history") from error
        if entry.name != version.path_component:
            raise ReleaseError("ambiguous_release_history")
        image_name = f"inkloop-idf-{board_sku}-{version.path_component}.bin"
        if {child.name for child in entry.iterdir()} != {
            image_name,
            "manifest.json",
            "release-receipt.json",
        }:
            raise ReleaseError("ambiguous_release_history")
        for name, maximum in (
            (image_name, MAXIMUM_IMAGE_BYTES),
            ("manifest.json", 4096),
            ("release-receipt.json", 4096),
        ):
            try:
                metadata = (entry / name).lstat()
            except OSError as error:
                raise ReleaseError("ambiguous_release_history") from error
            if (
                stat.S_ISLNK(metadata.st_mode)
                or not stat.S_ISREG(metadata.st_mode)
                or metadata.st_size <= 0
                or metadata.st_size > maximum
            ):
                raise ReleaseError("ambiguous_release_history")


def _read_public_key(path: Path) -> tuple[bytes, os.stat_result]:
    data, metadata = _stable_regular_bytes(path, "public_key", 65)
    if not re.fullmatch(rb"[0-9a-f]{64}\n?", data):
        raise ReleaseError("invalid_public_key")
    return bytes.fromhex(data.rstrip(b"\n").decode("ascii")), metadata


def _verify_snapshots(snapshots: tuple[FileSnapshot, ...]) -> None:
    for snapshot in snapshots:
        try:
            current = snapshot.path.lstat()
        except OSError as error:
            raise ReleaseError("release_changed") from error
        if not _same_file_snapshot(snapshot.metadata, current):
            raise ReleaseError("release_changed")


def _atomic_channel_replace(
    board_directory: Path,
    board_descriptor: int,
    data: bytes,
    previous: os.stat_result | None,
    snapshots: tuple[FileSnapshot, ...],
) -> None:
    channel_path = board_directory / "manifest.json"
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".inkloop-ota-channel-", dir=board_directory
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o644)
        output = os.fdopen(descriptor, "wb")
        descriptor = -1
        with output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        _verify_snapshots(snapshots)
        try:
            current = channel_path.lstat()
        except FileNotFoundError:
            current = None
        except OSError as error:
            raise ReleaseError("channel_changed") from error
        if (previous is None) != (current is None) or (
            previous is not None
            and current is not None
            and not _same_file_snapshot(previous, current)
        ):
            raise ReleaseError("channel_changed")
        os.replace(temporary, channel_path)
        os.fsync(board_descriptor)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def promote(arguments: argparse.Namespace) -> dict[str, object]:
    output_path = _absolute_without_traversal(arguments.output_root, "output_root")
    public_key_path = _absolute_without_traversal(arguments.public_key, "public_key")
    output_root = _directory_without_symlink(output_path, "output_root")
    public_key_real = public_key_path.resolve(strict=True)
    if _inside(public_key_real, output_root):
        raise ReleaseError("public_key_inside_output")
    public_key, public_key_metadata = _read_public_key(public_key_path)
    if not BOARD_SKU.fullmatch(arguments.board_sku):
        raise ReleaseError("invalid_board_sku")
    target_version = SemVersion(arguments.firmware_version)
    public_base = _public_base_url(arguments.public_base_url)
    board_directory = _directory_without_symlink(
        output_root / arguments.board_sku, "board_directory"
    )
    directory_flags = _open_flags()
    if hasattr(os, "O_DIRECTORY"):
        directory_flags |= os.O_DIRECTORY
    board_descriptor = os.open(board_directory, directory_flags)
    try:
        try:
            fcntl.flock(board_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ReleaseError("concurrent_publication") from error
        _scan_board_history(board_directory, arguments.board_sku, public_base)
        target = _validated_release(
            output_root,
            arguments.board_sku,
            target_version,
            public_base,
            public_key,
        )
        channel_path = board_directory / "manifest.json"
        channel_metadata: os.stat_result | None = None
        snapshots = list(target.snapshots)
        if channel_path.exists() or channel_path.is_symlink():
            channel_bytes, channel_metadata = _stable_regular_bytes(
                channel_path, "channel_manifest", 4096
            )
            channel_manifest = _ordered_json(
                channel_bytes, MANIFEST_KEYS, "channel_manifest"
            )
            if channel_manifest.get("board_sku") != arguments.board_sku or not isinstance(
                channel_manifest.get("firmware_version"), str
            ):
                raise ReleaseError("invalid_channel_manifest")
            current_version = SemVersion(channel_manifest["firmware_version"])
            current = _validated_release(
                output_root,
                arguments.board_sku,
                current_version,
                public_base,
                public_key,
            )
            if channel_bytes != current.manifest_bytes:
                raise ReleaseError("channel_release_mismatch")
            if target_version.compare(current_version) <= 0:
                raise ReleaseError("non_increasing_promotion")
            snapshots.extend(current.snapshots)
        snapshots.append(FileSnapshot(public_key_path, public_key_metadata))
        retained_snapshots = tuple(snapshots)
        if arguments.verify_only:
            _verify_snapshots(retained_snapshots)
        else:
            _atomic_channel_replace(
                board_directory,
                board_descriptor,
                target.manifest_bytes,
                channel_metadata,
                retained_snapshots,
            )
        channel_relative = PurePosixPath(arguments.board_sku, "manifest.json")
        return {
            "schema_version": 1,
            "operation": "verify" if arguments.verify_only else "promote",
            "board_sku": arguments.board_sku,
            "firmware_version": target_version.value,
            "channel_path": channel_relative.as_posix(),
            "channel_url": _join_public_url(public_base, channel_relative),
            "manifest_sha256": target.manifest_sha256,
        }
    finally:
        try:
            fcntl.flock(board_descriptor, fcntl.LOCK_UN)
        finally:
            os.close(board_descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Verify an immutable signed release, then atomically replace its "
            "per-SKU stable manifest channel."
        )
    )
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--board-sku", required=True)
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument("--public-base-url", required=True)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="validate without creating or replacing the stable channel",
    )
    arguments = parser.parse_args()
    try:
        result = promote(arguments)
    except (ReleaseError, OSError) as error:
        code = str(error) if isinstance(error, ReleaseError) else "filesystem_error"
        print(f"error: {code}", file=sys.stderr)
        return 2
    print(json.dumps(result, separators=(",", ":"), ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
