#!/usr/bin/env python3
"""Build an Inkloop signed OTA manifest without storing private keys.

The private Ed25519 PEM remains caller-owned. This tool emits the exact
canonical byte contract consumed by inkloop_ota and optionally the raw public
key as lowercase hex for firmware provisioning.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from urllib.parse import urlsplit


DOMAIN = b"INKLOOP-OTA-MANIFEST-V1"
SCHEMA_VERSION = 1
POLICY = "inkloop-pinned-ed25519-sha256-v1"
MAXIMUM_IMAGE_BYTES = 8 * 1024 * 1024
SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
TOKEN = re.compile(r"^[A-Za-z0-9_.-]+$")
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)
URL_PATH = re.compile(r"^/[A-Za-z0-9/_.~-]+$")


def bounded_token(value: str, maximum: int, label: str) -> bytes:
    encoded = value.encode("ascii", "strict")
    if not encoded or len(encoded) > maximum or not TOKEN.fullmatch(value):
        raise ValueError(f"invalid_{label}")
    return encoded


def checked_image_url(value: str) -> str:
    if len(value.encode("ascii", "strict")) > 512:
        raise ValueError("invalid_image_url")
    parsed = urlsplit(value)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.hostname != parsed.hostname.lower()
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or not URL_PATH.fullmatch(parsed.path)
    ):
        raise ValueError("invalid_image_url")
    try:
        port = parsed.port
    except ValueError as error:
        raise ValueError("invalid_image_url") from error
    if port is not None and not (1 <= port <= 65535):
        raise ValueError("invalid_image_url")
    return value


def canonical_bytes(
    board_sku: str, firmware_version: str, image_size: int, digest: bytes
) -> bytes:
    board = bounded_token(board_sku, 32, "board_sku")
    version = bounded_token(firmware_version, 64, "firmware_version")
    if not SEMVER.fullmatch(firmware_version):
        raise ValueError("invalid_firmware_version")
    policy = POLICY.encode("ascii")
    if not (0 < image_size <= MAXIMUM_IMAGE_BYTES) or len(digest) != 32:
        raise ValueError("invalid_image")
    return b"".join(
        (
            DOMAIN,
            struct.pack("<H", SCHEMA_VERSION),
            bytes((len(board),)),
            board,
            bytes((len(version),)),
            version,
            struct.pack("<Q", image_size),
            digest,
            bytes((len(policy),)),
            policy,
        )
    )


def openssl(*arguments: str, input_bytes: bytes | None = None) -> bytes:
    result = subprocess.run(
        ("openssl", *arguments),
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError("openssl_operation_failed")
    return result.stdout


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--image-url", required=True)
    parser.add_argument("--board-sku", required=True)
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--manifest-out", required=True, type=Path)
    parser.add_argument("--public-key-out", type=Path)
    arguments = parser.parse_args()

    image_url = checked_image_url(arguments.image_url)
    image = arguments.image.read_bytes()
    if not image or len(image) > MAXIMUM_IMAGE_BYTES:
        raise ValueError("invalid_image")
    digest = hashlib.sha256(image).digest()
    canonical = canonical_bytes(
        arguments.board_sku,
        arguments.firmware_version,
        len(image),
        digest,
    )
    # WS31 verifier signs the canonical manifest plus the independently
    # streamed image digest. Keeping both inputs makes the verifier's streaming
    # evidence explicit even though the digest is also a manifest field.
    signed_message = canonical + digest
    # Ed25519 is a one-shot algorithm in OpenSSL and therefore requires a
    # seekable input file instead of stdin.
    with tempfile.TemporaryDirectory(prefix="inkloop-ota-canonical-") as temporary:
        canonical_path = Path(temporary) / "manifest.canonical"
        canonical_path.write_bytes(signed_message)
        signature = openssl(
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(arguments.private_key),
            "-in",
            str(canonical_path),
        )
    if len(signature) != 64:
        raise RuntimeError("invalid_ed25519_signature")

    public_der = openssl(
        "pkey", "-in", str(arguments.private_key), "-pubout", "-outform", "DER"
    )
    if len(public_der) != len(SPKI_PREFIX) + 32 or not public_der.startswith(
        SPKI_PREFIX
    ):
        raise RuntimeError("private_key_is_not_ed25519")
    public_key = public_der[len(SPKI_PREFIX) :]

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "board_sku": arguments.board_sku,
        "firmware_version": arguments.firmware_version,
        "image_url": image_url,
        "image_size": len(image),
        "image_sha256": digest.hex(),
        "signature_policy": POLICY,
        "detached_signature": signature.hex(),
    }
    atomic_write(
        arguments.manifest_out,
        (json.dumps(manifest, separators=(",", ":"), ensure_ascii=True) + "\n").encode(
            "ascii"
        ),
    )
    if arguments.public_key_out:
        atomic_write(arguments.public_key_out, (public_key.hex() + "\n").encode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
