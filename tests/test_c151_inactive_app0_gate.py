#!/usr/bin/env python3

from __future__ import annotations

import ast
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "firmware/inkloop-idf/tools/c151_inactive_app0_gate.py"
SPEC = importlib.util.spec_from_file_location("c151_inactive_app0_gate", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FakeEsptool:
    def __init__(
        self,
        flash: bytes,
        policy: gate.GatePolicy,
        mac: str = "28:84:85:43:da:0c",
    ):
        self.flash = bytearray(flash)
        self.policy = policy
        self.mac = mac
        self.operations: list[str] = []

    def run(self, command: list[str], **kwargs: object) -> object:
        operation = next(
            token
            for token in command
            if token in gate.READ_ONLY_ESPTOOL_OPERATIONS or token == "write_flash"
        )
        self.operations.append(operation)
        stdout = kwargs["stdout"]
        assert hasattr(stdout, "write")
        if operation == "read_mac":
            stdout.write(
                (
                    "Chip type: ESP32-S3 (revision v0.2)\n"
                    f"MAC: {self.mac}\n"
                ).encode()
            )
        elif operation == "chip_id":
            stdout.write(
                (
                    "Chip is ESP32-S3 (revision v0.2)\n"
                    f"MAC: {self.mac}\n"
                ).encode()
            )
        elif operation == "flash_id":
            stdout.write(
                (
                    "Chip is ESP32-S3 (revision v0.2)\n"
                    f"MAC: {self.mac}\n"
                    "Manufacturer: 20\nDevice: 4018\n"
                    "Detected flash size: 16MB\n"
                ).encode()
            )
        elif operation == "get_security_info":
            stdout.write(
                (
                    "Chip is ESP32-S3 (revision v0.2)\n"
                    f"MAC: {self.mac}\n"
                    "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
                ).encode()
            )
        elif operation == "read_flash":
            Path(command[-1]).write_bytes(self.flash)
            stdout.write(b"Read complete\n")
        else:
            pass_fds = kwargs.get("pass_fds")
            assert isinstance(pass_fds, tuple) and len(pass_fds) == 1
            input_fd = pass_fds[0]
            self.assert_fd_only_write(command, input_fd)
            os.lseek(input_fd, 0, os.SEEK_SET)
            chunks = []
            while chunk := os.read(input_fd, 1024 * 1024):
                chunks.append(chunk)
            data = b"".join(chunks)
            offset = int(command[-2], 0)
            programmed_bytes = (len(data) + 0xFFF) & ~0xFFF
            self.flash[offset : offset + programmed_bytes] = b"\xff" * programmed_bytes
            self.flash[offset : offset + len(data)] = data
            stdout.write(b"Write complete\n")

        class Completed:
            returncode = 0

        return Completed()

    def assert_fd_only_write(self, command: list[str], input_fd: int) -> None:
        assert command[-1] == f"/dev/fd/{input_fd}"
        assert not any(
            token.endswith(".bin") or "candidate-app0.bin" in token
            for token in command
        )


class Fixture:
    def __init__(self, root: Path):
        self.root = root
        self.capture = root / "capture"
        self.capture.mkdir()
        self.candidate = root / "candidate.bin"
        candidate = b"fixture-header\x00" + b"0.4.0-beta.30" + b"\x7f" * 97
        self.candidate.write_bytes(candidate)
        partition = bytes((index * 17 + 3) & 0xFF for index in range(0xC00))
        app0 = b"A" * 0x2000
        app1 = b"B" * 0x1000
        littlefs = b"L" * 0x1000
        current_ota = bytearray(b"\xff" * 0x2000)
        current_ota[:32] = gate.make_ota_entry(1, 2)
        current_ota[0x1000 : 0x1000 + 32] = gate.make_ota_entry(2, 2)
        flash = bytearray(b"\xff" * 0x1000000)
        flash[0x1000 : 0x1000 + len(partition)] = partition
        flash[0x3000:0x5000] = current_ota
        flash[0x5000:0x7000] = app0
        flash[0x7000:0x8000] = app1
        flash[0x8000:0x9000] = littlefs
        self.flash = bytes(flash)
        self.prefix = self.flash[:0x5000]
        self.app1 = app1
        self.littlefs = littlefs
        self.policy = gate.GatePolicy(
            policy_id="fixture-policy",
            commit="1" * 40,
            version="0.4.0-beta.30",
            candidate_sha256=digest(candidate),
            candidate_bytes=len(candidate),
            expected_mac="28:84:85:43:da:0c",
            expected_port="/dev/cu.usbmodem21442201",
            flash_bytes=len(self.flash),
            flash_sha256_before=digest(self.flash),
            flash_manufacturer="20",
            flash_device="4018",
            partition_table_offset=0x1000,
            partition_table_file_bytes=len(partition),
            partition_table_sha256=digest(partition),
            nvs_offset=0x2000,
            nvs_bytes=0x1000,
            otadata_offset=0x3000,
            otadata_bytes=0x2000,
            app0_offset=0x5000,
            app0_bytes=0x2000,
            app0_before_sha256=digest(app0),
            app1_offset=0x7000,
            app1_bytes=0x1000,
            app1_rollback_sha256=digest(app1),
            littlefs_offset=0x8000,
            littlefs_bytes=0x1000,
            otadata_before_sha256=digest(bytes(current_ota)),
            current_app0_sequence=1,
            current_app1_sequence=2,
            next_app0_sequence=3,
            next_app0_state=0,
            ota_valid_state=2,
            minimum_tf_image_bytes=1,
            minimum_candidate_beta=30,
        )
        self._write_capture()
        self.acceptance = root / "acceptance.json"
        self.acceptance.write_text(
            json.dumps(
                {
                    "status": "pass",
                    "commit": self.policy.commit,
                    "reviewed_at_utc": gate.utc_now().replace("+00:00", "Z"),
                    "checks": {
                        "commit_identity": {
                            "status": "pass",
                            "expected": self.policy.commit,
                            "actual": self.policy.commit,
                        },
                        "worktree": {"status": "pass"},
                        "reproducible_binaries": {
                            "status": "pass",
                            "artifacts": [
                                {
                                    "name": "c151-a/inkloop_idf.bin",
                                    "sha256": self.policy.candidate_sha256,
                                    "bytes": self.policy.candidate_bytes,
                                },
                                {
                                    "name": "c151-b/inkloop_idf.bin",
                                    "sha256": self.policy.candidate_sha256,
                                    "bytes": self.policy.candidate_bytes,
                                },
                            ],
                        },
                        "constraint_compliance": {
                            "status": "pass",
                            "tracked_files_modified": False,
                            "device_accessed": False,
                            "device_written_or_flashed": False,
                            "push_performed": False,
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        self.baseline = root / "baseline.json"
        self.baseline.write_text(
            json.dumps(
                {
                    "complete": True,
                    "matching_full_reads": True,
                    "flash_bytes": self.policy.flash_bytes,
                    "device_mac": self.policy.expected_mac,
                    "full_flash_sha256": self.policy.flash_sha256_before,
                    "slot_state": {
                        "app0": {
                            "version": "0.4.0-beta.25",
                            "sha256": self.policy.app0_before_sha256,
                        },
                        "app1": {
                            "version": "0.4.0-beta.27",
                            "sha256": self.policy.app1_rollback_sha256,
                        },
                    },
                }
            ),
            encoding="utf-8",
        )
        self.tf_custody_dir = root / "tf-custody"
        self.tf_custody_dir.mkdir()
        self.tf_image = self.tf_custody_dir / "tf-whole-card.img"
        self.tf_image.write_bytes(b"offline-card-image")
        tf_sha = gate.sha256_file(self.tf_image)
        (self.tf_custody_dir / "SHA256SUMS").write_text(
            f"{tf_sha}  tf-whole-card.img\n", encoding="ascii"
        )
        tf_identity = {
            "deviceIdentifier": "disk9",
            "deviceNode": "/dev/disk9",
            "totalSize": self.tf_image.stat().st_size,
            "deviceBlockSize": 512,
            "mediaName": "Fixture TF",
            "mediaUUID": "fixture-media-uuid",
            "diskUUID": None,
            "deviceTreePath": "IODeviceTree:/fixture",
            "ioRegistryEntryName": "Fixture Reader",
            "busProtocol": "USB",
            "mediaType": "Generic",
            "removableMedia": True,
            "ejectable": True,
            "internal": False,
            "virtualOrPhysical": "Physical",
            "members": [
                {
                    "deviceIdentifier": "disk9",
                    "parentWholeDisk": None,
                    "totalSize": self.tf_image.stat().st_size,
                    "content": "FDisk_partition_scheme",
                    "filesystemType": None,
                    "partitionUUID": None,
                    "volumeUUID": None,
                    "diskUUID": None,
                    "mediaUUID": "fixture-media-uuid",
                    "mounted": False,
                    "mountPoint": None,
                    "volumeRoles": None,
                }
            ],
        }
        tf_fingerprint = gate.canonical_json_sha256(tf_identity)
        tf_target_info = {
            "DeviceIdentifier": "disk9",
            "DeviceNode": "/dev/disk9",
            "Whole": True,
            "TotalSize": self.tf_image.stat().st_size,
            "DeviceBlockSize": 512,
            "MediaName": "Fixture TF",
            "MediaUUID": "fixture-media-uuid",
            "DiskUUID": None,
            "DeviceTreePath": "IODeviceTree:/fixture",
            "IORegistryEntryName": "Fixture Reader",
            "BusProtocol": "USB",
            "MediaType": "Generic",
            "RemovableMedia": True,
            "Ejectable": True,
            "Internal": False,
            "VirtualOrPhysical": "Physical",
        }
        tf_member_infos = [
            {
                "DeviceIdentifier": "disk9",
                "ParentWholeDisk": None,
                "TotalSize": self.tf_image.stat().st_size,
                "Content": "FDisk_partition_scheme",
                "FilesystemType": None,
                "PartitionUUID": None,
                "VolumeUUID": None,
                "DiskUUID": None,
                "MediaUUID": "fixture-media-uuid",
                "Mounted": False,
                "MountPoint": None,
                "APFSVolumeRole": None,
            }
        ]
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
            if "members" in name:
                value: object = tf_member_infos
            elif "list" in name:
                value = {"AllDisksAndPartitions": [{"DeviceIdentifier": "disk9"}]}
            else:
                value = tf_target_info
            (self.tf_custody_dir / name).write_text(
                json.dumps(value), encoding="utf-8"
            )
        self.tf_custody = self.tf_custody_dir / "custody.json"
        self.tf_custody.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "complete": True,
                    "platform": "macOS",
                    "sourceAccess": "two full read-only raw-device passes",
                    "sourceWritesPerformed": False,
                    "automaticUnmountOrEjectPerformed": False,
                    "implicitPrivilegeEscalationPerformed": False,
                    "disk": "/dev/disk9",
                    "rawDisk": "/dev/rdisk9",
                    "bytes": self.tf_image.stat().st_size,
                    "sha256": tf_sha,
                    "image": "tf-whole-card.img",
                    "fingerprint": tf_fingerprint,
                    "identityStableAcrossSnapshots": True,
                    "snapshotFingerprints": {
                        "before": tf_fingerprint,
                        "preRead": tf_fingerprint,
                        "betweenPasses": tf_fingerprint,
                        "after": tf_fingerprint,
                    },
                    "sourcePasses": [
                        {"bytes": self.tf_image.stat().st_size, "sha256": tf_sha},
                        {"bytes": self.tf_image.stat().st_size, "sha256": tf_sha},
                    ],
                    "identity": tf_identity,
                }
            ),
            encoding="utf-8",
        )

    def _write_capture(self) -> None:
        logs = {
            "read-mac.log": "Chip type: ESP32-S3 (revision v0.2)\nMAC: 28:84:85:43:da:0c\n",
            "chip-id.log": "Chip is ESP32-S3 (revision v0.2)\nMAC: 28:84:85:43:da:0c\n",
            "flash-id.log": (
                "Chip is ESP32-S3 (revision v0.2)\nMAC: 28:84:85:43:da:0c\n"
                "Manufacturer: 20\nDevice: 4018\nDetected flash size: 16MB\n"
            ),
            "security-info.log": (
                "Chip is ESP32-S3 (revision v0.2)\nMAC: 28:84:85:43:da:0c\n"
                "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
            ),
            "full-flash-before.log": (
                "Chip is ESP32-S3 (revision v0.2)\nMAC: 28:84:85:43:da:0c\nRead complete\n"
            ),
        }
        for name, value in logs.items():
            (self.capture / name).write_text(value, encoding="utf-8")
        (self.capture / "full-flash-before.bin").write_bytes(self.flash)
        files = {}
        for path in self.capture.iterdir():
            files[path.name] = {
                "bytes": path.stat().st_size,
                "sha256": gate.sha256_file(path),
            }
        (self.capture / "capture-manifest.json").write_text(
            json.dumps(
                {
                    "complete": True,
                    "policy_id": "fixture-policy",
                    "port": "/dev/cu.usbmodem21442201",
                    "operations": [
                        "read_mac",
                        "chip_id",
                        "flash_id",
                        "get_security_info",
                        "read_flash",
                    ],
                    "files": files,
                }
            ),
            encoding="utf-8",
        )

    def gate_app(self, output: Path) -> Path:
        return gate.gate_app(
            self.capture,
            self.candidate,
            self.acceptance,
            self.baseline,
            self.tf_custody,
            output,
            self.policy,
        )

    def app_after(self) -> bytes:
        expected = bytearray(self.flash)
        candidate = self.candidate.read_bytes()
        expected[self.policy.app0_offset : self.policy.app0_offset + 0x1000] = (
            candidate + b"\xff" * (0x1000 - len(candidate))
        )
        return bytes(expected)

    def selected_after(self) -> bytes:
        expected = bytearray(self.app_after())
        current_otadata = expected[
            self.policy.otadata_offset : self.policy.otadata_offset
            + self.policy.otadata_bytes
        ]
        _, selected_otadata = gate.make_selected_app0_otadata(
            bytes(current_otadata), self.policy
        )
        expected[
            self.policy.otadata_offset : self.policy.otadata_offset
            + self.policy.otadata_bytes
        ] = selected_otadata
        return bytes(expected)


class C151InactiveApp0GateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = Path(tempfile.mkdtemp(prefix="inkloop-c151-gate-test-"))
        self.fixture = Fixture(self.temporary)

    def tearDown(self) -> None:
        shutil.rmtree(self.temporary, ignore_errors=True)

    def _run_emitted_cli(
        self, argv: list[str], simulator: FakeEsptool | None = None
    ) -> None:
        self.assertEqual(Path(argv[0]).resolve(), Path(sys.executable).resolve())
        self.assertEqual(Path(argv[1]).resolve(), TOOL_PATH.resolve())
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(gate, "PRODUCTION_POLICY", self.fixture.policy)
            )
            stack.enter_context(contextlib.redirect_stdout(stdout))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            if simulator is not None:
                stack.enter_context(
                    mock.patch.object(gate.subprocess, "run", simulator.run)
                )
                stack.enter_context(
                    mock.patch.object(gate, "_validate_authorized_port")
                )
            status = gate.main(argv[2:])
        self.assertEqual(
            status,
            0,
            f"emitted CLI failed\nstdout:\n{stdout.getvalue()}\nstderr:\n{stderr.getvalue()}",
        )

    def test_production_policy_requires_fresh_beta30_or_newer_binding(self) -> None:
        policy = gate.PRODUCTION_POLICY
        self.assertEqual(policy.commit, "")
        self.assertEqual(policy.version, "")
        self.assertEqual(policy.candidate_sha256, "")
        self.assertEqual(policy.candidate_bytes, 0)
        self.assertEqual(policy.minimum_candidate_beta, 30)
        self.assertEqual(policy.minimum_tf_image_bytes, 64 * 1024 * 1024)
        with self.assertRaises(gate.GateError):
            gate._require_bound_policy(policy)
        with self.assertRaises(gate.GateError):
            gate.bind_release(
                policy,
                "179849653f26027fafaea45ef9f4d289493363f4",
                "0.4.0-beta.29",
                "b7128c77dbb52b809a749bbba27aa765be77684e52e33fe460acc492f399411f",
                2_824_864,
            )
        bound = gate.bind_release(
            policy,
            "2" * 40,
            "0.4.0-beta.30",
            "a" * 64,
            2_900_000,
        )
        self.assertEqual(bound.commit, "2" * 40)
        self.assertEqual(bound.version, "0.4.0-beta.30")
        self.assertEqual(bound.candidate_sha256, "a" * 64)
        self.assertEqual(bound.candidate_bytes, 2_900_000)
        self.assertEqual(policy.app0_offset, 0x10000)
        self.assertEqual(policy.app1_offset, 0x650000)
        self.assertEqual(policy.app1_rollback_sha256, "872e1e749bc065fe0ab1b687be7dc0997953a60bcb582253957cd80c2daa2070")

    def test_selector_is_exact_32_byte_seq3_new_entry(self) -> None:
        selector = gate.make_ota_entry(3, 0)
        self.assertEqual(len(selector), 32)
        self.assertEqual(selector.hex()[:8], "03000000")
        self.assertEqual(selector[24:28].hex(), "00000000")
        self.assertEqual(selector[28:32].hex(), "11504aed")
        self.assertEqual(gate.ota_crc(3), 0xED4A5011)

    def test_two_phase_gate_and_final_verification(self) -> None:
        app_gate = self.fixture.gate_app(self.temporary / "app-gate")
        app_auth = json.loads((app_gate / "app-stage-authorization.json").read_text())
        self.assertEqual(app_auth["slot_proof"]["selected"], "app1")
        self.assertEqual(app_auth["slot_proof"]["inactive_target"], "app0")
        self.assertFalse(app_auth["selector_authorized"])
        forbidden = {item["name"] for item in app_auth["forbidden_ranges"]}
        self.assertTrue(
            {
                "bootloader-and-partition-table",
                "nvs",
                "otadata-until-second-gate",
                "app0-outside-programmed-range",
                "app1-rollback",
                "littlefs",
                "internal-flash-tail-including-coredump",
                "tf-card",
            }.issubset(forbidden)
        )
        app_plan = json.loads((app_gate / "app-stage-plan.json").read_text())
        self.assertEqual(app_plan["status"], "controlled-execution-only")
        self.assertEqual(app_plan["authorized_mutation"]["offset"], 0x5000)
        self.assertEqual(
            app_plan["authorized_mutation"]["flash_sectors_affected"]["bytes"],
            0x1000,
        )
        self.assertNotIn("write_flash", json.dumps(app_plan))
        self.assertEqual(app_plan["execute_app_argv"][2], "execute-app")
        self.assertEqual(app_plan["next_gate_argv"][2], "authorize-selector")
        for option, expected in (
            ("--expected-commit", self.fixture.policy.commit),
            ("--expected-version", self.fixture.policy.version),
            ("--expected-candidate-sha256", self.fixture.policy.candidate_sha256),
            ("--expected-candidate-bytes", str(self.fixture.policy.candidate_bytes)),
        ):
            index = app_plan["next_gate_argv"].index(option)
            self.assertEqual(app_plan["next_gate_argv"][index + 1], expected)

        simulator = FakeEsptool(self.fixture.flash, self.fixture.policy)
        self._run_emitted_cli(app_plan["execute_app_argv"], simulator)
        next_gate_argv = app_plan["next_gate_argv"]
        app_readback = Path(
            next_gate_argv[next_gate_argv.index("--full-flash-readback") + 1]
        )
        self.assertEqual(app_readback.read_bytes(), self.fixture.app_after())
        self.assertEqual(
            simulator.operations,
            [
                "read_mac",
                "chip_id",
                "flash_id",
                "get_security_info",
                "read_flash",
                "read_mac",
                "write_flash",
                "read_flash",
            ],
        )
        self._run_emitted_cli(next_gate_argv)
        selector_gate = Path(
            next_gate_argv[next_gate_argv.index("--output-dir") + 1]
        )
        selector = (selector_gate / "selector-entry0-seq3-new.bin").read_bytes()
        self.assertEqual(selector, gate.make_ota_entry(3, 0))
        self.assertEqual(len(selector), 32)
        selector_plan = json.loads(
            (selector_gate / "selector-stage-plan.json").read_text()
        )
        self.assertEqual(selector_plan["status"], "controlled-execution-only")
        self.assertEqual(
            selector_plan["authorized_selector_mutation"]["offset"], 0x3000
        )
        self.assertNotIn("write_flash", json.dumps(selector_plan))
        self.assertEqual(
            selector_plan["execute_selector_argv"][2], "execute-selector"
        )
        self.assertEqual(
            selector_plan["execute_rollback_argv"][2], "execute-rollback"
        )
        self.assertEqual(
            selector_plan["final_verification_argv"][-2:],
            ["--output", str(selector_gate / "selector-verification.json")],
        )
        for command in (
            selector_plan["final_verification_argv"],
            selector_plan["rollback_verification_argv"],
        ):
            for option, expected in (
                ("--expected-commit", self.fixture.policy.commit),
                ("--expected-version", self.fixture.policy.version),
                (
                    "--expected-candidate-sha256",
                    self.fixture.policy.candidate_sha256,
                ),
                (
                    "--expected-candidate-bytes",
                    str(self.fixture.policy.candidate_bytes),
                ),
            ):
                self.assertEqual(command[command.index(option) + 1], expected)

        self._run_emitted_cli(selector_plan["execute_selector_argv"], simulator)
        final_argv = selector_plan["final_verification_argv"]
        final_readback = Path(
            final_argv[final_argv.index("--full-flash-readback") + 1]
        )
        self.assertEqual(final_readback.read_bytes(), self.fixture.selected_after())
        self._run_emitted_cli(final_argv)
        result = Path(final_argv[final_argv.index("--output") + 1])
        verified = json.loads(result.read_text())
        self.assertEqual(verified["status"], "pass")
        self.assertEqual(verified["selected"], "app0")
        self.assertEqual(
            verified["readbacks"]["full_flash"]["sha256"],
            digest(self.fixture.selected_after()),
        )
        self.assertEqual(
            [
                region["name"]
                for region in verified["readbacks"]["full_flash"][
                    "continuous_ranges"
                ]
            ],
            [
                "bootloader-and-partition-table",
                "nvs",
                "otadata",
                "app0-programmed-range",
                "app0-suffix",
                "app1-rollback",
                "littlefs",
                "internal-flash-tail-including-coredump",
            ],
        )

        self._run_emitted_cli(selector_plan["execute_rollback_argv"], simulator)
        rollback_argv = selector_plan["rollback_verification_argv"]
        rollback_readback = Path(
            rollback_argv[rollback_argv.index("--full-flash-readback") + 1]
        )
        self.assertEqual(rollback_readback.read_bytes(), self.fixture.app_after())
        self._run_emitted_cli(rollback_argv)
        rollback_result = Path(rollback_argv[rollback_argv.index("--output") + 1])
        rolled_back = json.loads(rollback_result.read_text())
        self.assertEqual(rolled_back["status"], "pass")
        self.assertEqual(rolled_back["selected"], "app1")

        selector_auth_path = selector_gate / "selector-authorization.json"
        tampered = json.loads(selector_auth_path.read_text())
        tampered["expected_after"]["full_flash_sha256"] = "0" * 64
        selector_auth_path.write_text(json.dumps(tampered), encoding="utf-8")
        with self.assertRaises(gate.GateError):
            gate.verify_selector(
                selector_auth_path,
                final_readback,
                self.temporary / "tampered-result.json",
                False,
                self.fixture.policy,
            )

    def test_selector_stays_unavailable_when_app_readback_is_wrong(self) -> None:
        app_gate = self.fixture.gate_app(self.temporary / "app-gate")
        wrong = self.temporary / "wrong-app.bin"
        wrong_after = bytearray(self.fixture.app_after())
        wrong_after[self.fixture.policy.app0_offset] ^= 0x01
        wrong.write_bytes(wrong_after)
        output = self.temporary / "selector-gate"
        with self.assertRaises(gate.GateError):
            gate.authorize_selector(
                app_gate / "app-stage-authorization.json",
                wrong,
                output,
                self.fixture.policy,
            )
        self.assertFalse(output.exists())

    def test_all_stages_reject_out_of_delta_mutations(self) -> None:
        app_gate = self.fixture.gate_app(self.temporary / "app-gate")
        app_after = self.fixture.app_after()
        protected_offsets = {
            "boot-prefix": 0x100,
            "nvs": self.fixture.policy.nvs_offset,
            "otadata-before-selector": self.fixture.policy.otadata_offset + 64,
            "app0-suffix": self.fixture.policy.app0_offset + 0x1000,
            "app1": self.fixture.policy.app1_offset,
            "littlefs": self.fixture.policy.littlefs_offset,
            "flash-tail-coredump": self.fixture.policy.flash_bytes - 1,
        }
        for label, offset in protected_offsets.items():
            with self.subTest(stage="app", region=label):
                changed = bytearray(app_after)
                changed[offset] ^= 0x01
                readback = self.temporary / f"app-mutated-{label}.bin"
                readback.write_bytes(changed)
                output = self.temporary / f"selector-rejected-{label}"
                with self.assertRaisesRegex(
                    gate.GateError, "out-of-delta mutation"
                ):
                    gate.authorize_selector(
                        app_gate / "app-stage-authorization.json",
                        readback,
                        output,
                        self.fixture.policy,
                    )
                self.assertFalse(output.exists())

        app_readback = self.temporary / "full-app-after.bin"
        app_readback.write_bytes(app_after)
        selector_gate = gate.authorize_selector(
            app_gate / "app-stage-authorization.json",
            app_readback,
            self.temporary / "selector-gate",
            self.fixture.policy,
        )

        final_changed = bytearray(self.fixture.selected_after())
        final_changed[self.fixture.policy.app0_offset + 0x1000] ^= 0x01
        final_readback = self.temporary / "final-mutated-app0-suffix.bin"
        final_readback.write_bytes(final_changed)
        with self.assertRaisesRegex(gate.GateError, "out-of-delta mutation"):
            gate.verify_selector(
                selector_gate / "selector-authorization.json",
                final_readback,
                self.temporary / "rejected-final.json",
                False,
                self.fixture.policy,
            )
        self.assertFalse((self.temporary / "rejected-final.json").exists())

        rollback_changed = bytearray(app_after)
        rollback_changed[self.fixture.policy.flash_bytes - 1] ^= 0x01
        rollback_readback = self.temporary / "rollback-mutated-tail.bin"
        rollback_readback.write_bytes(rollback_changed)
        with self.assertRaisesRegex(gate.GateError, "out-of-delta mutation"):
            gate.verify_selector(
                selector_gate / "selector-authorization.json",
                rollback_readback,
                self.temporary / "rejected-rollback.json",
                True,
                self.fixture.policy,
            )
        self.assertFalse((self.temporary / "rejected-rollback.json").exists())

    def test_controlled_writes_reject_replaced_extended_or_symlinked_inputs(self) -> None:
        app_gate = self.fixture.gate_app(self.temporary / "app-gate")
        app_auth_path = app_gate / "app-stage-authorization.json"
        app_auth = json.loads(app_auth_path.read_text())
        staged_candidate = Path(app_auth["candidate"]["path"])

        app_readback = self.temporary / "full-app-after.bin"
        app_readback.write_bytes(self.fixture.app_after())
        selector_gate = gate.authorize_selector(
            app_auth_path,
            app_readback,
            self.temporary / "selector-gate",
            self.fixture.policy,
        )
        selector_auth_path = selector_gate / "selector-authorization.json"

        cases = (
            ("app", staged_candidate, gate.execute_app),
            (
                "selector",
                selector_gate / "selector-entry0-seq3-new.bin",
                gate.execute_selector,
            ),
            (
                "rollback",
                selector_gate / "rollback-entry0-seq1-valid.bin",
                gate.execute_rollback,
            ),
        )
        for stage, input_path, execute in cases:
            original = input_path.read_bytes()
            symlink_target = self.temporary / f"{stage}-symlink-target.bin"
            symlink_target.write_bytes(original)
            for mutation in ("replace", "extend", "symlink"):
                with self.subTest(stage=stage, mutation=mutation):
                    input_path.unlink()
                    if mutation == "replace":
                        changed = bytearray(original)
                        changed[0] ^= 0x01
                        input_path.write_bytes(changed)
                    elif mutation == "extend":
                        input_path.write_bytes(original + b"\x00")
                    else:
                        input_path.symlink_to(symlink_target)
                    if not input_path.is_symlink():
                        input_path.chmod(0o600)
                    with (
                        mock.patch.object(gate, "_validate_authorized_port"),
                        mock.patch.object(gate.subprocess, "run") as spawned,
                        self.assertRaises(gate.GateError),
                    ):
                        execute(
                            app_auth_path
                            if stage == "app"
                            else selector_auth_path,
                            self.fixture.policy.expected_port,
                            self.temporary / f"rejected-{stage}-{mutation}",
                            self.fixture.policy,
                        )
                    spawned.assert_not_called()
                    if input_path.exists() or input_path.is_symlink():
                        input_path.unlink()
                    input_path.write_bytes(original)
                    input_path.chmod(0o600)

    def test_execute_app_rejects_substituted_device_or_prewrite_flash(self) -> None:
        app_gate = self.fixture.gate_app(self.temporary / "app-gate")
        authorization = app_gate / "app-stage-authorization.json"
        changed_flash = bytearray(self.fixture.flash)
        changed_flash[self.fixture.policy.app1_offset] ^= 0x01
        simulators = {
            "wrong-mac": FakeEsptool(
                self.fixture.flash,
                self.fixture.policy,
                mac="28:84:85:43:da:0d",
            ),
            "changed-prewrite-flash": FakeEsptool(
                bytes(changed_flash), self.fixture.policy
            ),
        }
        for label, simulator in simulators.items():
            with (
                self.subTest(label=label),
                mock.patch.object(gate, "_validate_authorized_port"),
                mock.patch.object(gate.subprocess, "run", simulator.run),
                self.assertRaises(gate.GateError),
            ):
                gate.execute_app(
                    authorization,
                    self.fixture.policy.expected_port,
                    self.temporary / f"rejected-{label}",
                    self.fixture.policy,
                )
            self.assertNotIn("write_flash", simulator.operations)

    def test_gate_rejects_device_or_flash_drift_without_output(self) -> None:
        full_flash = self.fixture.capture / "full-flash-before.bin"
        changed = bytearray(full_flash.read_bytes())
        changed[self.fixture.policy.app1_offset] ^= 0x01
        full_flash.write_bytes(changed)
        manifest_path = self.fixture.capture / "capture-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["full-flash-before.bin"] = {
            "bytes": len(changed),
            "sha256": gate.sha256_file(full_flash),
        }
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        output = self.temporary / "rejected"
        with self.assertRaises(gate.GateError):
            self.fixture.gate_app(output)
        self.assertFalse(output.exists())

    def test_gate_rejects_enabled_flash_encryption(self) -> None:
        security = self.fixture.capture / "security-info.log"
        security.write_text(
            security.read_text().replace("Flash Encryption: Disabled", "Flash Encryption: Enabled"),
            encoding="utf-8",
        )
        manifest_path = self.fixture.capture / "capture-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["security-info.log"] = {
            "bytes": security.stat().st_size,
            "sha256": gate.sha256_file(security),
        }
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(gate.GateError):
            self.fixture.gate_app(self.temporary / "rejected")

    def test_gate_rejects_missing_offline_tf_custody(self) -> None:
        custody = json.loads(self.fixture.tf_custody.read_text())
        custody["sourceWritesPerformed"] = True
        self.fixture.tf_custody.write_text(json.dumps(custody), encoding="utf-8")
        with self.assertRaises(gate.GateError):
            self.fixture.gate_app(self.temporary / "rejected")

    def test_gate_binds_canonical_tf_identity_and_diskutil_receipts(self) -> None:
        original = self.fixture.tf_custody.read_text()
        custody = json.loads(original)
        custody["identity"]["mediaName"] = "substituted-card"
        self.fixture.tf_custody.write_text(json.dumps(custody), encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "canonical identity fingerprint"):
            self.fixture.gate_app(self.temporary / "identity-rejected")

        forged = json.loads(original)
        forged["identity"]["mediaName"] = "self-consistent-forgery"
        forged_fingerprint = gate.canonical_json_sha256(forged["identity"])
        forged["fingerprint"] = forged_fingerprint
        forged["snapshotFingerprints"] = {
            "before": forged_fingerprint,
            "preRead": forged_fingerprint,
            "betweenPasses": forged_fingerprint,
            "after": forged_fingerprint,
        }
        self.fixture.tf_custody.write_text(json.dumps(forged), encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "normalized identity"):
            self.fixture.gate_app(self.temporary / "receipts-disagree")

        self.fixture.tf_custody.write_text(original, encoding="utf-8")
        missing_receipt = self.fixture.tf_custody_dir / "diskutil-info-after.json"
        missing_receipt.unlink()
        with self.assertRaisesRegex(gate.GateError, "diskutil-info-after"):
            self.fixture.gate_app(self.temporary / "receipt-rejected")

    def test_capture_runner_has_a_strict_read_only_allowlist(self) -> None:
        for operation in gate.READ_ONLY_ESPTOOL_OPERATIONS:
            command = gate._read_command(self.fixture.policy.expected_port, operation)
            self.assertIn(operation, command)
            self.assertFalse(any(token in gate.FORBIDDEN_ESPTOOL_OPERATIONS for token in command))
        for operation in gate.FORBIDDEN_ESPTOOL_OPERATIONS:
            with self.assertRaises(gate.GateError):
                gate._read_command(self.fixture.policy.expected_port, operation)

    def test_only_central_reviewed_runner_can_spawn_a_subprocess(self) -> None:
        tree = ast.parse(TOOL_PATH.read_text(encoding="utf-8"))
        spawning_functions = []
        for function in (
            node for node in ast.walk(tree) if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        ):
            for call in (node for node in ast.walk(function) if isinstance(node, ast.Call)):
                called = call.func
                if (
                    isinstance(called, ast.Attribute)
                    and isinstance(called.value, ast.Name)
                    and called.value.id == "subprocess"
                    and called.attr == "run"
                ):
                    spawning_functions.append(function.name)
                    self.assertFalse(
                        any(
                            keyword.arg == "shell"
                            and isinstance(keyword.value, ast.Constant)
                            and keyword.value.value is True
                            for keyword in call.keywords
                        )
                    )
        self.assertEqual(spawning_functions, ["_run_esptool"])


if __name__ == "__main__":
    unittest.main()
