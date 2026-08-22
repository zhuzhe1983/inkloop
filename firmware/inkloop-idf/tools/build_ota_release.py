#!/usr/bin/env python3
"""Build one clean OTA-enabled C151 image and emit a public receipt."""

from __future__ import annotations

import argparse
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

from package_ota_release import (
    MAXIMUM_IMAGE_BYTES,
    ReleaseError,
    SemVersion,
    _absolute_without_traversal,
    _atomic_write,
    _directory_without_symlink,
    _inside,
    _same_file_snapshot,
)
from promote_ota_channel import JsonObjectPairs, _ordered_json, _stable_regular_bytes


BOARD_SKU = "m5-papercolor-c151"
IDF_TARGET = "esp32s3"
PROJECT_NAME = "inkloop_idf"
RECEIPT_NAME = "release-build-receipt.json"
OTA_URL_KEY = "CONFIG_INKLOOP_OTA_MANIFEST_URL"
OTA_PUBLIC_KEY = "CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX"
OTA_DEADLINE_KEY = "CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS"
OTA_DEFAULT_KEYS = (OTA_URL_KEY, OTA_PUBLIC_KEY, OTA_DEADLINE_KEY)
PUBLIC_KEY_HEX = re.compile(r"^[0-9a-f]{64}$")
CONFIG_LINE = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
STANDARD_FLASH_FILES = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xe000": "ota_data_initial.bin",
    "0x10000": "inkloop_idf.bin",
}
STANDARD_FLASH_ARGS = (
    "--flash-mode dio --flash-freq 80m --flash-size 16MB\n"
    "0x0 bootloader/bootloader.bin\n"
    "0x8000 partition_table/partition-table.bin\n"
    "0xe000 ota_data_initial.bin\n"
    "0x10000 inkloop_idf.bin\n"
).encode("ascii")
FLASHER_KEYS = (
    "write_flash_args",
    "flash_settings",
    "flash_files",
    "bootloader",
    "partition-table",
    "otadata",
    "app",
    "extra_esptool_args",
)


class ReleaseBuildError(ReleaseError):
    """A deliberately path- and key-free release-build failure."""


def _snapshot(path: Path, label: str, maximum: int) -> tuple[bytes, os.stat_result]:
    try:
        return _stable_regular_bytes(path, label, maximum)
    except ReleaseError as error:
        raise ReleaseBuildError(str(error)) from error


def _quoted(value: str) -> str:
    return f'"{value}"'


def _parse_public_defaults(
    data: bytes, board_sku: str
) -> tuple[str, str, int]:
    try:
        text = data.decode("ascii")
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_public_defaults") from error
    lines = text.splitlines()
    if len(lines) != 3:
        raise ReleaseBuildError("invalid_public_defaults")
    parsed: list[tuple[str, str]] = []
    for line in lines:
        match = CONFIG_LINE.fullmatch(line)
        if not match:
            raise ReleaseBuildError("invalid_public_defaults")
        parsed.append((match.group(1), match.group(2)))
    if tuple(key for key, _ in parsed) != OTA_DEFAULT_KEYS:
        raise ReleaseBuildError("invalid_public_defaults")
    assignments = dict(parsed)
    manifest_url = (
        f"https://inkloop.mess.host/ota/{board_sku}/manifest.json"
    )
    if assignments[OTA_URL_KEY] != _quoted(manifest_url):
        raise ReleaseBuildError("invalid_ota_manifest_url")
    public_key_value = assignments[OTA_PUBLIC_KEY]
    if (
        len(public_key_value) != 66
        or public_key_value[0] != '"'
        or public_key_value[-1] != '"'
        or not PUBLIC_KEY_HEX.fullmatch(public_key_value[1:-1])
        or set(public_key_value[1:-1]) == {"0"}
    ):
        raise ReleaseBuildError("invalid_ota_public_key")
    deadline_value = assignments[OTA_DEADLINE_KEY]
    if not re.fullmatch(r"[1-9][0-9]*", deadline_value):
        raise ReleaseBuildError("invalid_ota_deadline")
    deadline = int(deadline_value)
    if deadline > 120000:
        raise ReleaseBuildError("invalid_ota_deadline")
    return manifest_url, public_key_value[1:-1], deadline


def _parse_exact_assignments(data: bytes, names: tuple[str, ...]) -> dict[str, str]:
    try:
        text = data.decode("ascii")
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_generated_sdkconfig") from error
    found: dict[str, list[str]] = {name: [] for name in names}
    for line in text.splitlines():
        match = CONFIG_LINE.fullmatch(line)
        if match and match.group(1) in found:
            found[match.group(1)].append(match.group(2))
    if any(len(found[name]) != 1 for name in names):
        raise ReleaseBuildError("invalid_generated_sdkconfig")
    return {name: found[name][0] for name in names}


def _verify_generated_sdkconfig(
    output_directory: Path,
    manifest_url: str,
    public_key_hex: str,
    deadline_ms: int,
) -> tuple[bytes, os.stat_result]:
    data, metadata = _snapshot(
        output_directory / "sdkconfig", "generated_sdkconfig", 2 * 1024 * 1024
    )
    names = OTA_DEFAULT_KEYS + (
        "CONFIG_IDF_TARGET",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
        "CONFIG_SPIRAM",
        "CONFIG_SPIRAM_MODE_OCT",
        "CONFIG_SPIRAM_TYPE_AUTO",
        "CONFIG_SPIRAM_SPEED_80M",
        "CONFIG_SPIRAM_BOOT_INIT",
        "CONFIG_SPIRAM_USE_MALLOC",
        "CONFIG_SPIRAM_MEMTEST",
        "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL",
        "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL",
        "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC",
        "CONFIG_LITTLEFS_MULTIVERSION",
        "CONFIG_LITTLEFS_DISK_VERSION_2_0",
    )
    assignments = _parse_exact_assignments(data, names)
    expected = {
        OTA_URL_KEY: _quoted(manifest_url),
        OTA_PUBLIC_KEY: _quoted(public_key_hex),
        OTA_DEADLINE_KEY: str(deadline_ms),
        "CONFIG_IDF_TARGET": _quoted(IDF_TARGET),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_SPIRAM_TYPE_AUTO": "y",
        "CONFIG_SPIRAM_SPEED_80M": "y",
        "CONFIG_SPIRAM_BOOT_INIT": "y",
        "CONFIG_SPIRAM_USE_MALLOC": "y",
        "CONFIG_SPIRAM_MEMTEST": "y",
        "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "4096",
        "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
        "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC": "y",
        "CONFIG_LITTLEFS_MULTIVERSION": "y",
        "CONFIG_LITTLEFS_DISK_VERSION_2_0": "y",
    }
    if (assignments != expected or b"CONFIG_SPIRAM_MODE_QUAD=y" in data or
            b"CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y" in data):
        raise ReleaseBuildError("generated_sdkconfig_mismatch")
    return data, metadata


def _verify_cache(output_directory: Path) -> os.stat_result:
    data, metadata = _snapshot(
        output_directory / "CMakeCache.txt", "cmake_cache", 2 * 1024 * 1024
    )
    try:
        text = data.decode("utf-8")
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_cmake_cache") from error
    expected = {
        "IDF_TARGET:STRING": IDF_TARGET,
        "INKLOOP_BOARD:STRING": BOARD_SKU.replace("-", "_"),
    }
    for key, value in expected.items():
        matches = re.findall(rf"^{re.escape(key)}=(.*)$", text, re.MULTILINE)
        if matches != [value]:
            raise ReleaseBuildError("generated_target_mismatch")
    return metadata


def _verify_project_description(
    output_directory: Path, firmware_version: str
) -> os.stat_result:
    data, metadata = _snapshot(
        output_directory / "project_description.json",
        "project_description",
        2 * 1024 * 1024,
    )
    try:
        description = json.loads(data.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseBuildError("invalid_project_description") from error
    if (
        not isinstance(description, dict)
        or description.get("project_name") != PROJECT_NAME
        or description.get("project_version") != firmware_version
        or description.get("target") != IDF_TARGET
        or description.get("app_bin") != f"{PROJECT_NAME}.bin"
    ):
        raise ReleaseBuildError("project_description_mismatch")
    return metadata


def _safe_flash_file(output_directory: Path, relative: str) -> Path:
    posix = PurePosixPath(relative)
    if posix.is_absolute() or ".." in posix.parts or posix.as_posix() != relative:
        raise ReleaseBuildError("invalid_flash_arguments")
    candidate = output_directory.joinpath(*posix.parts)
    try:
        candidate.relative_to(output_directory)
    except ValueError as error:
        raise ReleaseBuildError("invalid_flash_arguments") from error
    return candidate


def _materialize_json(value: object) -> object:
    if isinstance(value, JsonObjectPairs):
        keys = [key for key, _ in value]
        if len(keys) != len(set(keys)):
            raise ReleaseBuildError("invalid_flash_arguments")
        return {key: _materialize_json(item) for key, item in value}
    if isinstance(value, list):
        return [_materialize_json(item) for item in value]
    return value


def _verify_flash_arguments(
    output_directory: Path,
) -> tuple[bytes, bytes, tuple[os.stat_result, ...]]:
    flash_args, flash_metadata = _snapshot(
        output_directory / "flash_args", "flash_args", 4096
    )
    if flash_args != STANDARD_FLASH_ARGS:
        raise ReleaseBuildError("invalid_flash_arguments")
    flasher_data, flasher_metadata = _snapshot(
        output_directory / "flasher_args.json", "flasher_args", 32 * 1024
    )
    ordered_flasher = _ordered_json(flasher_data, FLASHER_KEYS, "flasher_args")
    flasher = {
        key: _materialize_json(value) for key, value in ordered_flasher.items()
    }
    if (
        flasher["write_flash_args"]
        != ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"]
        or flasher["flash_settings"]
        != {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"}
        or flasher["flash_files"] != STANDARD_FLASH_FILES
        or not isinstance(flasher["extra_esptool_args"], dict)
        or flasher["extra_esptool_args"].get("chip") != IDF_TARGET
    ):
        raise ReleaseBuildError("invalid_flash_arguments")
    roles = (
        ("bootloader", "0x0", "bootloader/bootloader.bin"),
        ("partition-table", "0x8000", "partition_table/partition-table.bin"),
        ("otadata", "0xe000", "ota_data_initial.bin"),
        ("app", "0x10000", "inkloop_idf.bin"),
    )
    artifact_metadata: list[os.stat_result] = [flash_metadata, flasher_metadata]
    for role, offset, filename in roles:
        if flasher[role] != {
            "offset": offset,
            "file": filename,
            "encrypted": "false",
        }:
            raise ReleaseBuildError("invalid_flash_arguments")
        _, metadata = _snapshot(
            _safe_flash_file(output_directory, filename),
            "flash_artifact",
            32 * 1024 * 1024,
        )
        artifact_metadata.append(metadata)
    return flash_args, flasher_data, tuple(artifact_metadata)


def _run_clean_idf_build(
    idf_directory: Path,
    project_directory: Path,
    output_directory: Path,
    merged_defaults: Path,
) -> None:
    shell_program = """set -euo pipefail
source "$1/export.sh" >/dev/null
cd "$2"
exec idf.py -B "$3" -DIDF_TARGET=esp32s3 \
  -DINKLOOP_BOARD=m5_papercolor_c151 \
  -DSDKCONFIG="$3/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$4" build
"""
    environment = os.environ.copy()
    environment["IDF_PATH"] = str(idf_directory)
    result = subprocess.run(
        (
            "/bin/bash",
            "-c",
            shell_program,
            "inkloop-ota-release-build",
            str(idf_directory),
            str(project_directory),
            str(output_directory),
            str(merged_defaults),
        ),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise ReleaseBuildError("idf_build_failed")


def _verify_idf_version(idf_directory: Path, expected: str) -> None:
    try:
        result = subprocess.run(
            ("git", "-C", str(idf_directory), "describe", "--tags", "--always"),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise ReleaseBuildError("invalid_idf_version") from error
    try:
        actual = result.stdout.decode("ascii").strip()
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_idf_version") from error
    if result.returncode != 0 or actual != expected:
        raise ReleaseBuildError("invalid_idf_version")


def _unchanged(path: Path, expected: os.stat_result, label: str) -> None:
    try:
        current = path.lstat()
    except OSError as error:
        raise ReleaseBuildError(f"{label}_changed") from error
    if not _same_file_snapshot(expected, current):
        raise ReleaseBuildError(f"{label}_changed")


def build_release(arguments: argparse.Namespace) -> dict[str, object]:
    project_directory = Path(__file__).resolve().parent.parent
    output_path = _absolute_without_traversal(arguments.output_dir, "output_dir")
    defaults_path = _absolute_without_traversal(
        arguments.public_defaults, "public_defaults"
    )
    idf_path = _absolute_without_traversal(arguments.idf_path, "idf_path")
    idf_directory = _directory_without_symlink(idf_path, "idf_path")
    export_metadata = (idf_directory / "export.sh").lstat()
    if stat.S_ISLNK(export_metadata.st_mode) or not stat.S_ISREG(export_metadata.st_mode):
        raise ReleaseBuildError("invalid_idf_path")
    expected_idf_data, expected_idf_metadata = _snapshot(
        project_directory / ".idf-version", "idf_version_file", 64
    )
    try:
        expected_idf = expected_idf_data.decode("ascii").rstrip("\n")
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_idf_version_file") from error
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", expected_idf):
        raise ReleaseBuildError("invalid_idf_version_file")
    _verify_idf_version(idf_directory, expected_idf)

    if arguments.board_sku != BOARD_SKU:
        raise ReleaseBuildError("invalid_board_sku")
    requested_version = SemVersion(arguments.firmware_version)
    version_data, version_metadata = _snapshot(
        project_directory / "version.txt", "version_file", 66
    )
    try:
        project_version = version_data.decode("ascii").rstrip("\n")
    except UnicodeError as error:
        raise ReleaseBuildError("invalid_version_file") from error
    if version_data not in (
        project_version.encode("ascii"),
        f"{project_version}\n".encode("ascii"),
    ):
        raise ReleaseBuildError("invalid_version_file")
    parsed_project_version = SemVersion(project_version)
    if requested_version.value != parsed_project_version.value:
        raise ReleaseBuildError("firmware_version_mismatch")

    defaults_real = defaults_path.resolve(strict=True)
    if _inside(defaults_real, project_directory):
        raise ReleaseBuildError("public_defaults_inside_project")
    public_defaults, defaults_metadata = _snapshot(
        defaults_path, "public_defaults", 1024
    )
    manifest_url, public_key_hex, deadline_ms = _parse_public_defaults(
        public_defaults, arguments.board_sku
    )
    base_defaults, base_metadata = _snapshot(
        project_directory / "sdkconfig.defaults", "base_defaults", 64 * 1024
    )
    board_defaults_path = (
        project_directory / "boards" / BOARD_SKU.replace("-", "_")
        / "sdkconfig.defaults"
    )
    board_defaults, board_metadata = _snapshot(
        board_defaults_path, "board_defaults", 64 * 1024
    )
    if any(key.encode("ascii") in base_defaults for key in OTA_DEFAULT_KEYS):
        raise ReleaseBuildError("development_defaults_enable_ota")

    try:
        output_path.lstat()
    except FileNotFoundError:
        pass
    except OSError as error:
        raise ReleaseBuildError("invalid_output_dir") from error
    else:
        raise ReleaseBuildError("output_dir_exists")
    output_parent = _directory_without_symlink(output_path.parent, "output_parent")
    output_directory = output_parent / output_path.name
    if _inside(output_directory, project_directory):
        raise ReleaseBuildError("output_inside_project")
    created_output = False
    try:
        output_directory.mkdir(mode=0o755)
        created_output = True
        with tempfile.TemporaryDirectory(
            prefix="inkloop-ota-release-defaults-"
        ) as temporary_name:
            merged_defaults = Path(temporary_name) / "sdkconfig.release.defaults"
            merged_defaults.write_bytes(
                base_defaults.rstrip(b"\n") + b"\n"
                + board_defaults.rstrip(b"\n") + b"\n"
                + public_defaults.rstrip(b"\n") + b"\n"
            )
            _run_clean_idf_build(
                idf_directory,
                project_directory,
                output_directory,
                merged_defaults,
            )

        _unchanged(defaults_path, defaults_metadata, "public_defaults")
        _unchanged(project_directory / "sdkconfig.defaults", base_metadata, "base_defaults")
        _unchanged(board_defaults_path, board_metadata, "board_defaults")
        _unchanged(project_directory / "version.txt", version_metadata, "version_file")
        _unchanged(
            project_directory / ".idf-version",
            expected_idf_metadata,
            "idf_version_file",
        )
        _unchanged(idf_directory / "export.sh", export_metadata, "idf_export")

        sdkconfig_data, _ = _verify_generated_sdkconfig(
            output_directory, manifest_url, public_key_hex, deadline_ms
        )
        _verify_cache(output_directory)
        _verify_project_description(output_directory, requested_version.value)
        flash_args, flasher_args, _ = _verify_flash_arguments(output_directory)
        app_data, _ = _snapshot(
            output_directory / f"{PROJECT_NAME}.bin", "app_binary", MAXIMUM_IMAGE_BYTES
        )
        if (
            app_data[0] != 0xE9
            or manifest_url.encode("ascii") not in app_data
            or public_key_hex.encode("ascii") not in app_data
        ):
            raise ReleaseBuildError("app_binary_contract_mismatch")
        public_key_bytes = bytes.fromhex(public_key_hex)
        receipt = {
            "schema_version": 1,
            "board_sku": arguments.board_sku,
            "firmware_version": requested_version.value,
            "target": IDF_TARGET,
            "app_size": len(app_data),
            "app_sha256": hashlib.sha256(app_data).hexdigest(),
            "ota_manifest_url": manifest_url,
            "ota_public_key_sha256": hashlib.sha256(public_key_bytes).hexdigest(),
            "ota_total_deadline_ms": deadline_ms,
            "sdkconfig_sha256": hashlib.sha256(sdkconfig_data).hexdigest(),
            "flash_args_sha256": hashlib.sha256(flash_args).hexdigest(),
            "flasher_args_sha256": hashlib.sha256(flasher_args).hexdigest(),
        }
        receipt_bytes = (
            json.dumps(receipt, separators=(",", ":"), ensure_ascii=True) + "\n"
        ).encode("ascii")
        _atomic_write(output_directory / RECEIPT_NAME, receipt_bytes)
        directory_descriptor = os.open(output_directory, os.O_RDONLY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
        return receipt
    except BaseException:
        if created_output:
            shutil.rmtree(output_directory)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create one clean, isolated, OTA-enabled C151 release build and "
            "credential-free receipt without deploying or flashing it."
        )
    )
    parser.add_argument("--idf-path", required=True, type=Path)
    parser.add_argument("--public-defaults", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--board-sku", required=True)
    parser.add_argument("--firmware-version", required=True)
    arguments = parser.parse_args()
    try:
        receipt = build_release(arguments)
    except (ReleaseError, OSError) as error:
        code = str(error) if isinstance(error, ReleaseError) else "filesystem_error"
        print(f"error: {code}", file=sys.stderr)
        return 2
    print(json.dumps(receipt, separators=(",", ":"), ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
