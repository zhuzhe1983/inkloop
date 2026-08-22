#!/usr/bin/env python3
"""Package one signed, same-origin Inkloop OTA release without deploying it."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit, urlunsplit


MAXIMUM_IMAGE_BYTES = 8 * 1024 * 1024
BOARD_SKU = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9_.-]{0,30}[A-Za-z0-9])?$")
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)
DNS_LABEL = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
URL_PATH = re.compile(r"^/[A-Za-z0-9/_.~-]*$")
MANIFEST_KEYS = (
    "schema_version",
    "board_sku",
    "firmware_version",
    "image_url",
    "image_size",
    "image_sha256",
    "signature_policy",
    "detached_signature",
)
SIGNATURE_POLICY = "inkloop-pinned-ed25519-sha256-v1"


class ReleaseError(RuntimeError):
    """A deliberately path- and credential-free packaging failure."""


class SemVersion:
    def __init__(self, value: str) -> None:
        match = SEMVER.fullmatch(value)
        if not match or len(value.encode("ascii", "strict")) > 64:
            raise ReleaseError("invalid_firmware_version")
        core = tuple(int(part) for part in match.group(1, 2, 3))
        if any(part > 0xFFFFFFFF for part in core):
            raise ReleaseError("invalid_firmware_version")
        prerelease = match.group(4)
        identifiers: list[tuple[bool, int | str]] = []
        if prerelease:
            for identifier in prerelease.split("."):
                numeric = identifier.isdigit()
                if numeric and len(identifier) > 1 and identifier[0] == "0":
                    raise ReleaseError("invalid_firmware_version")
                identifiers.append((numeric, int(identifier) if numeric else identifier))
        self.value = value
        self.core = core
        self.prerelease = identifiers if prerelease is not None else None

    def compare(self, other: "SemVersion") -> int:
        if self.core != other.core:
            return -1 if self.core < other.core else 1
        if self.prerelease is None and other.prerelease is None:
            return 0
        if self.prerelease is None:
            return 1
        if other.prerelease is None:
            return -1
        for left, right in zip(self.prerelease, other.prerelease):
            if left == right:
                continue
            if left[0] != right[0]:
                return -1 if left[0] else 1
            return -1 if left[1] < right[1] else 1
        if len(self.prerelease) == len(other.prerelease):
            return 0
        return -1 if len(self.prerelease) < len(other.prerelease) else 1

    @property
    def path_component(self) -> str:
        # '+' is valid SemVer but is outside the device's deliberately narrow
        # URL path alphabet. SemVer itself cannot contain '_', so this mapping
        # is deterministic and collision-free.
        return self.value.replace("+", "_build_")


def _absolute_without_traversal(path: Path, label: str) -> Path:
    if not path.is_absolute() or ".." in path.parts:
        raise ReleaseError(f"invalid_{label}_path")
    return path


def _regular_file(path: Path, label: str, maximum: int | None = None) -> os.stat_result:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise ReleaseError(f"invalid_{label}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise ReleaseError(f"invalid_{label}")
    if metadata.st_size <= 0 or (maximum is not None and metadata.st_size > maximum):
        raise ReleaseError(f"invalid_{label}")
    return metadata


def _directory_without_symlink(path: Path, label: str) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise ReleaseError(f"invalid_{label}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise ReleaseError(f"invalid_{label}")
    return path.resolve(strict=True)


def _inside(candidate: Path, root: Path) -> bool:
    try:
        candidate.relative_to(root)
        return True
    except ValueError:
        return False


def _public_base_url(value: str) -> str:
    try:
        if len(value.encode("ascii", "strict")) > 384:
            raise ReleaseError("invalid_public_base_url")
        parsed = urlsplit(value)
        port = parsed.port
    except (UnicodeError, ValueError) as error:
        raise ReleaseError("invalid_public_base_url") from error
    hostname = parsed.hostname or ""
    labels = hostname.split(".")
    if (
        parsed.scheme != "https"
        or not hostname
        or parsed.netloc != parsed.netloc.lower()
        or hostname != hostname.lower()
        or len(labels) < 2
        or not any(character.isalpha() for character in hostname)
        or any(not DNS_LABEL.fullmatch(label) for label in labels)
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or (port is not None and not 1 <= port <= 65535)
    ):
        raise ReleaseError("invalid_public_base_url")
    path = parsed.path
    if path and (
        not URL_PATH.fullmatch(path)
        or "//" in path
        or any(part in (".", "..") for part in path.split("/"))
    ):
        raise ReleaseError("invalid_public_base_url")
    canonical_path = path.rstrip("/")
    return urlunsplit(("https", parsed.netloc, canonical_path, "", ""))


def _join_public_url(base: str, relative: PurePosixPath) -> str:
    return f"{base}/{relative.as_posix()}"


def _sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def _atomic_write(path: Path, data: bytes, mode: int = 0o644) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def _same_file_snapshot(left: os.stat_result, right: os.stat_result) -> bool:
    return (
        left.st_dev == right.st_dev
        and left.st_ino == right.st_ino
        and left.st_size == right.st_size
        and left.st_mtime_ns == right.st_mtime_ns
        and left.st_ctime_ns == right.st_ctime_ns
        and stat.S_IMODE(left.st_mode) == stat.S_IMODE(right.st_mode)
    )


def _copy_image(
    source_path: Path, destination: Path, expected: os.stat_result
) -> tuple[str, int]:
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    source_descriptor = os.open(source_path, flags)
    temporary = destination.with_name(f".{destination.name}.partial")
    digest = hashlib.sha256()
    size = 0
    try:
        source_metadata = os.fstat(source_descriptor)
        if (
            not stat.S_ISREG(source_metadata.st_mode)
            or not _same_file_snapshot(source_metadata, expected)
        ):
            raise ReleaseError("image_changed_during_copy")
        output_descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644
        )
        try:
            while True:
                chunk = os.read(source_descriptor, 64 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                if size > MAXIMUM_IMAGE_BYTES:
                    raise ReleaseError("invalid_image")
                digest.update(chunk)
                view = memoryview(chunk)
                while view:
                    written = os.write(output_descriptor, view)
                    view = view[written:]
            if size == 0:
                raise ReleaseError("invalid_image")
            os.fsync(output_descriptor)
        finally:
            os.close(output_descriptor)
        if not _same_file_snapshot(source_metadata, os.fstat(source_descriptor)):
            raise ReleaseError("image_changed_during_copy")
        os.replace(temporary, destination)
    finally:
        os.close(source_descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return digest.hexdigest(), size


def _load_existing_manifest(version_directory: Path, board_sku: str) -> SemVersion:
    if version_directory.is_symlink() or not version_directory.is_dir():
        raise ReleaseError("ambiguous_existing_release")
    manifest_path = version_directory / "manifest.json"
    _regular_file(manifest_path, "existing_manifest", 4096)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseError("ambiguous_existing_release") from error
    if (
        not isinstance(manifest, dict)
        or tuple(manifest) != MANIFEST_KEYS
        or manifest.get("board_sku") != board_sku
        or not isinstance(manifest.get("firmware_version"), str)
    ):
        raise ReleaseError("ambiguous_existing_release")
    version = SemVersion(manifest["firmware_version"])
    if version_directory.name != version.path_component:
        raise ReleaseError("ambiguous_existing_release")
    return version


def _check_release_history(board_directory: Path, target: SemVersion) -> None:
    for entry in board_directory.iterdir():
        if entry.name.startswith(".inkloop-ota-stage-"):
            raise ReleaseError("ambiguous_existing_release")
        # The per-SKU stable channel is a copy of one already-complete,
        # versioned manifest. It is the only non-directory entry allowed in a
        # board tree and is never treated as a release version.
        if entry.name == "manifest.json":
            _regular_file(entry, "channel_manifest", 4096)
            continue
        existing = _load_existing_manifest(entry, board_directory.name)
        if target.compare(existing) <= 0:
            raise ReleaseError("non_increasing_release")


def _validate_signed_manifest(
    path: Path,
    board_sku: str,
    firmware_version: str,
    image_url: str,
    image_size: int,
    image_sha256: str,
) -> bytes:
    _regular_file(path, "signed_manifest", 4096)
    data = path.read_bytes()
    try:
        manifest = json.loads(data.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseError("signer_contract_mismatch") from error
    if (
        not isinstance(manifest, dict)
        or tuple(manifest) != MANIFEST_KEYS
        or manifest["schema_version"] != 1
        or manifest["board_sku"] != board_sku
        or manifest["firmware_version"] != firmware_version
        or manifest["image_url"] != image_url
        or manifest["image_size"] != image_size
        or manifest["image_sha256"] != image_sha256
        or manifest["signature_policy"] != SIGNATURE_POLICY
        or not isinstance(manifest["detached_signature"], str)
        or not re.fullmatch(r"[0-9a-f]{128}", manifest["detached_signature"])
    ):
        raise ReleaseError("signer_contract_mismatch")
    return data


def package(arguments: argparse.Namespace) -> dict[str, object]:
    image_path = _absolute_without_traversal(arguments.image, "image")
    key_path = _absolute_without_traversal(arguments.private_key, "private_key")
    output_path = _absolute_without_traversal(arguments.output_root, "output_root")
    image_metadata = _regular_file(image_path, "image", MAXIMUM_IMAGE_BYTES)
    key_metadata = _regular_file(key_path, "private_key", 64 * 1024)
    if key_metadata.st_mode & 0o077 or key_metadata.st_uid != os.geteuid():
        raise ReleaseError("weak_private_key_permissions")
    output_root = _directory_without_symlink(output_path, "output_root")
    image_real = image_path.resolve(strict=True)
    key_real = key_path.resolve(strict=True)
    if _inside(key_real, output_root):
        raise ReleaseError("private_key_inside_output")

    if not BOARD_SKU.fullmatch(arguments.board_sku):
        raise ReleaseError("invalid_board_sku")
    version = SemVersion(arguments.firmware_version)
    public_base = _public_base_url(arguments.public_base_url)
    relative_directory = PurePosixPath(arguments.board_sku, version.path_component)
    image_name = (
        f"inkloop-idf-{arguments.board_sku}-{version.path_component}.bin"
    )
    image_relative = relative_directory / image_name
    manifest_relative = relative_directory / "manifest.json"
    receipt_relative = relative_directory / "release-receipt.json"
    final_directory = output_root.joinpath(*relative_directory.parts)
    final_image = final_directory / image_name
    if image_real == final_image.resolve(strict=False):
        raise ReleaseError("source_output_alias")
    if final_image.exists():
        try:
            if os.path.samefile(image_real, final_image):
                raise ReleaseError("source_output_alias")
        except OSError:
            pass

    board_directory = output_root / arguments.board_sku
    if board_directory.exists():
        if board_directory.is_symlink() or not board_directory.is_dir():
            raise ReleaseError("invalid_output_tree")
    else:
        board_directory.mkdir(mode=0o755)
    board_directory = board_directory.resolve(strict=True)
    board_descriptor = os.open(board_directory, os.O_RDONLY)
    staging_directory: Path | None = None
    try:
        fcntl.flock(board_descriptor, fcntl.LOCK_EX)
        _check_release_history(board_directory, version)
        if final_directory.exists():
            raise ReleaseError("release_already_exists")
        staging_directory = Path(
            tempfile.mkdtemp(prefix=".inkloop-ota-stage-", dir=board_directory)
        )
        os.chmod(staging_directory, 0o755)
        staged_image = staging_directory / image_name
        image_sha256, image_size = _copy_image(
            image_path, staged_image, image_metadata
        )
        if image_size != image_metadata.st_size:
            raise ReleaseError("image_changed_during_copy")

        image_url = _join_public_url(public_base, image_relative)
        signed_manifest = staging_directory / ".manifest.signed.json"
        signer = Path(__file__).with_name("sign_ota_manifest.py")
        signed = subprocess.run(
            (
                sys.executable,
                str(signer),
                "--image",
                str(staged_image),
                "--image-url",
                image_url,
                "--board-sku",
                arguments.board_sku,
                "--firmware-version",
                arguments.firmware_version,
                "--private-key",
                str(key_path),
                "--manifest-out",
                str(signed_manifest),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if signed.returncode != 0 or signed.stdout:
            raise ReleaseError("signer_failed")
        try:
            current_key_metadata = key_path.lstat()
        except OSError as error:
            raise ReleaseError("private_key_changed_during_signing") from error
        if not _same_file_snapshot(key_metadata, current_key_metadata):
            raise ReleaseError("private_key_changed_during_signing")
        manifest_bytes = _validate_signed_manifest(
            signed_manifest,
            arguments.board_sku,
            arguments.firmware_version,
            image_url,
            image_size,
            image_sha256,
        )
        manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
        receipt = {
            "schema_version": 1,
            "board_sku": arguments.board_sku,
            "firmware_version": arguments.firmware_version,
            "image_path": image_relative.as_posix(),
            "image_size": image_size,
            "image_sha256": image_sha256,
            "manifest_path": manifest_relative.as_posix(),
            "manifest_size": len(manifest_bytes),
            "manifest_sha256": manifest_sha256,
        }
        receipt_bytes = (
            json.dumps(receipt, separators=(",", ":"), ensure_ascii=True) + "\n"
        ).encode("ascii")
        _atomic_write(staging_directory / receipt_relative.name, receipt_bytes)
        os.chmod(signed_manifest, 0o644)
        # This is deliberately the last public filename created inside the
        # completed staging tree.
        os.replace(signed_manifest, staging_directory / manifest_relative.name)
        staging_descriptor = os.open(staging_directory, os.O_RDONLY)
        try:
            os.fsync(staging_descriptor)
        finally:
            os.close(staging_descriptor)
        if final_directory.exists():
            raise ReleaseError("release_already_exists")
        os.rename(staging_directory, final_directory)
        staging_directory = None
        os.fsync(board_descriptor)
        return receipt
    finally:
        if staging_directory is not None:
            shutil.rmtree(staging_directory)
        fcntl.flock(board_descriptor, fcntl.LOCK_UN)
        os.close(board_descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create, but do not deploy, one static Inkloop OTA release tree."
    )
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--board-sku", required=True)
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument("--public-base-url", required=True)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        receipt = package(arguments)
    except (ReleaseError, OSError) as error:
        code = str(error) if isinstance(error, ReleaseError) else "filesystem_error"
        print(f"error: {code}", file=sys.stderr)
        return 2
    print(json.dumps(receipt, separators=(",", ":"), ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
