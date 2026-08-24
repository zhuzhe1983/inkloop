#!/usr/bin/env python3

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "firmware/inkloop-idf/tools/c151_deferred_tf_app0_gate.py"
SPEC = importlib.util.spec_from_file_location("c151_deferred_tf_app0_gate", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Fixture:
    def __init__(self, root: Path):
        self.root = root
        self.now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
        self.candidate = root / "candidate-beta31.bin"
        candidate = b"fixture\x00" + b"0.4.0-beta.31" + b"C" * 0x1100
        self.candidate.write_bytes(candidate)
        self.candidate.chmod(0o600)

        partition = bytes((index * 17 + 3) & 0xFF for index in range(0xC00))
        nvs = b"N" * 0x1000
        app0 = b"A" * 0x3000
        app1 = b"B" * 0x1000
        littlefs = b"L" * 0x1000
        current_ota = bytearray(b"\xff" * 0x2000)
        current_ota[:32] = gate.core.make_ota_entry(1, 2)
        current_ota[0x1000 : 0x1000 + 32] = gate.core.make_ota_entry(2, 2)
        flash = bytearray(b"\xff" * 0x10000)
        flash[0x1000 : 0x1000 + len(partition)] = partition
        flash[0x2000:0x3000] = nvs
        flash[0x3000:0x5000] = current_ota
        flash[0x5000:0x8000] = app0
        flash[0x8000:0x9000] = app1
        flash[0x9000:0xA000] = littlefs
        self.flash = bytes(flash)
        self.policy = gate.GatePolicy(
            policy_id=gate.POLICY_ID,
            commit="1" * 40,
            version="0.4.0-beta.31",
            candidate_sha256=digest(candidate),
            candidate_bytes=len(candidate),
            expected_mac="28:84:85:43:da:0c",
            expected_port="/dev/cu.usbmodem21442201",
            flash_bytes=len(flash),
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
            app0_bytes=0x3000,
            app0_before_sha256=digest(app0),
            app1_offset=0x8000,
            app1_bytes=0x1000,
            app1_rollback_sha256=digest(app1),
            littlefs_offset=0x9000,
            littlefs_bytes=0x1000,
            otadata_before_sha256=digest(bytes(current_ota)),
            current_app0_sequence=1,
            current_app1_sequence=2,
            next_app0_sequence=3,
            next_app0_state=0,
            ota_valid_state=2,
            minimum_tf_image_bytes=1,
            minimum_candidate_beta=31,
            maximum_candidate_beta=31,
        )
        self.capture = root / "capture"
        self.capture.mkdir()
        self.capture.chmod(0o700)
        self._write_capture()
        self.acceptance = root / "acceptance.json"
        self._write_acceptance(False)
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
        self.witness = root / "continuous-tf-absence.witness"
        self.witness.write_bytes(b"two-person witnessed sealed-card evidence")
        self.witness.chmod(0o600)
        self.staging = root / "staging-receipt.json"
        self._write_staging()
        self.operator_auth = root / "operator-authorization.json"
        self._write_operator()

    def zulu(self, delta: dt.timedelta = dt.timedelta()) -> str:
        return (self.now + delta).isoformat().replace("+00:00", "Z")

    def _write_capture(self) -> None:
        logs = {
            "read-mac.log": (
                "Chip type: ESP32-S3 (revision v0.2)\n"
                f"MAC: {self.policy.expected_mac}\n"
            ),
            "chip-id.log": (
                "Chip is ESP32-S3 (revision v0.2)\n"
                f"MAC: {self.policy.expected_mac}\n"
            ),
            "flash-id.log": (
                "Chip is ESP32-S3 (revision v0.2)\n"
                f"MAC: {self.policy.expected_mac}\n"
                "Manufacturer: 20\nDevice: 4018\nDetected flash size: 16MB\n"
            ),
            "security-info.log": (
                "Chip is ESP32-S3 (revision v0.2)\n"
                f"MAC: {self.policy.expected_mac}\n"
                "Secure Boot: Disabled\nFlash Encryption: Disabled\n"
            ),
            "full-flash-before.log": "Read complete\n",
        }
        for name, text in logs.items():
            path = self.capture / name
            path.write_text(text, encoding="utf-8")
            path.chmod(0o600)
        full_flash_path = self.capture / "full-flash-before.bin"
        full_flash_path.write_bytes(self.flash)
        full_flash_path.chmod(0o600)
        files = {}
        for name in (*logs, "full-flash-before.bin"):
            path = self.capture / name
            files[name] = {"bytes": path.stat().st_size, "sha256": gate.core.sha256_file(path)}
        manifest_path = self.capture / "capture-manifest.json"
        manifest_path.write_text(
            json.dumps(
                {
                    "complete": True,
                    "policy_id": gate.POLICY_ID,
                    "port": self.policy.expected_port,
                    "captured_at_utc": self.zulu(dt.timedelta(minutes=-5)),
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
        manifest_path.chmod(0o600)

    def _write_acceptance(self, removable_accessed: object) -> None:
        self.acceptance.write_text(
            json.dumps(
                {
                    "status": "pass",
                    "commit": self.policy.commit,
                    "reviewed_at_utc": self.zulu(dt.timedelta(minutes=-20)),
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
                            "removable_media_accessed": removable_accessed,
                        },
                    },
                }
            ),
            encoding="utf-8",
        )

    def _write_staging(self) -> None:
        app = gate._binding(self.candidate, "candidate")
        acceptance = gate._binding(self.acceptance, "acceptance")
        self.staging.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "policy_id": gate.POLICY_ID,
                    "release_train": "beta31",
                    "authorized_for_app0_stage_only": True,
                    "authorized_for_selector": False,
                    "authorized_for_reset": False,
                    "authorized_for_boot": False,
                    "authorized_for_tf_access": False,
                    "reviewed_at_utc": self.zulu(dt.timedelta(minutes=-10)),
                    "reviewer": {"identity": "Independent Reviewer B", "independent": True},
                    "source": {"commit": self.policy.commit, "version": self.policy.version},
                    "application": app,
                    "acceptance": {**acceptance, "status": "PASS"},
                }
            ),
            encoding="utf-8",
        )
        self.staging.chmod(0o600)

    def _operator_value(self) -> dict[str, object]:
        capture_manifest = self.capture / "capture-manifest.json"
        capture_manifest_value = json.loads(capture_manifest.read_text(encoding="utf-8"))
        full_flash = self.capture / "full-flash-before.bin"
        capture_directory = self.capture.resolve()
        capture_directory_metadata = capture_directory.stat()
        created = self.zulu(dt.timedelta(minutes=-1))
        value: dict[str, object] = {
            "schema": 1,
            "policy_id": gate.POLICY_ID,
            "status": gate.OPERATOR_STATUS,
            "authorization_id": "a" * 64,
            "decision": gate.OPERATOR_DECISION,
            "created_at_utc": created,
            "expires_at_utc": self.zulu(dt.timedelta(hours=1)),
            "operator": {"identity": "Physical Operator A", "acknowledged_at_utc": created},
            "reviewer": {
                "identity": "Independent Reviewer B",
                "acknowledged_at_utc": created,
                "independent": True,
            },
            "source": {"commit": self.policy.commit, "version": self.policy.version},
            "application": gate._binding(self.candidate, "candidate"),
            "acceptance": gate._binding(self.acceptance, "acceptance"),
            "staging_receipt": gate._binding(self.staging, "staging"),
            "device": {
                "mac": self.policy.expected_mac,
                "port": self.policy.expected_port,
                "flash_bytes": self.policy.flash_bytes,
            },
            "baseline": gate._binding(self.baseline, "baseline"),
            "capture": {
                "capture_directory": {
                    "path": str(capture_directory),
                    "device": capture_directory_metadata.st_dev,
                    "inode": capture_directory_metadata.st_ino,
                },
                "manifest_path": str(capture_manifest.resolve()),
                "manifest_bytes": capture_manifest.stat().st_size,
                "manifest_sha256": gate.core.sha256_file(capture_manifest),
                "captured_at_utc": capture_manifest_value["captured_at_utc"],
                "full_flash_path": str(full_flash.resolve()),
                "full_flash_bytes": full_flash.stat().st_size,
                "full_flash_sha256": gate.core.sha256_file(full_flash),
            },
            "tf_absence": {
                "removed_at_utc": self.zulu(dt.timedelta(minutes=-15)),
                "powered_off_before_removal": True,
                "physically_removed": True,
                "sequestered": True,
                "no_tf_device_or_host_access_since_removal": True,
                "continuous_witness": gate._binding(self.witness, "witness"),
                "card_chain_id": "sealed-card-chain-0001",
            },
            "authorized_for_app0_stage_only": True,
            "authorized_for_selector": False,
            "authorized_for_reset": False,
            "authorized_for_boot": False,
            "authorized_for_tf_access": False,
            "terminal_scope": gate.TERMINAL_SCOPE,
            "tf_custody_status": "deferred",
        }
        value["binding_sha256"] = gate.core.canonical_json_sha256(value)
        return value

    def _write_operator(self, mutation: object = None) -> None:
        value = self._operator_value()
        if mutation is not None:
            mutation(value)
            value["binding_sha256"] = gate.core.canonical_json_sha256(
                gate._operator_binding_payload(value)
            )
        self.operator_auth.write_text(json.dumps(value), encoding="utf-8")
        self.operator_auth.chmod(0o600)

    def authorize(self, output: Path) -> Path:
        return gate.authorize_app0_stage_deferred_tf(
            self.capture,
            self.candidate,
            self.acceptance,
            self.staging,
            self.baseline,
            self.operator_auth,
            output,
            self.policy,
            now=self.now,
        )


class DeferredTfApp0GateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = Path(tempfile.mkdtemp(prefix="inkloop-no-tf-gate-test-"))
        self.fixture = Fixture(self.temporary)

    def tearDown(self) -> None:
        shutil.rmtree(self.temporary, ignore_errors=True)

    def test_policy_is_separate_and_stage_a_plan_has_no_later_stage_argv(self) -> None:
        self.assertEqual(gate.DEFERRED_TF_POLICY.flash_bytes, 0x1000000)
        self.assertNotEqual(gate.POLICY_ID, gate.core.PRODUCTION_POLICY.policy_id)
        output = self.fixture.authorize(self.temporary / "authorized")
        authorization = json.loads((output / "app0-stage-authorization.json").read_text())
        plan = json.loads((output / "app0-stage-plan.json").read_text())
        self.assertEqual(authorization["status"], gate.APP_STAGE_STATUS)
        self.assertEqual(authorization["scope"], gate._scope())
        self.assertEqual(authorization["stage_b"]["status"], gate.STAGE_B_STATUS)
        self.assertEqual(plan["authorized_mutation"]["offset"], self.fixture.policy.app0_offset)
        self.assertEqual(plan["authorized_mutation"]["flash_sectors_affected"]["bytes"], 0x2000)
        self.assertEqual(plan["execute_app0_stage_argv"][2], "execute-app0-stage-deferred-tf")
        self.assertFalse(plan["stage_b"]["argv_emitted"])
        self.assertFalse(any("selector" in key or "boot_argv" in key or "reset_argv" in key for key in plan))
        self.assertEqual(sorted(path.name for path in output.iterdir()), [
            "app0-stage-authorization.json",
            "app0-stage-plan.json",
            "candidate-app0.bin",
        ])
        with self.assertRaises(gate.GateError):
            gate.core.authorize_selector(
                output / "app0-stage-authorization.json",
                self.temporary / "fake-readback.bin",
                self.temporary / "old-selector-output",
                self.fixture.policy,
            )

    def test_expected_after_is_exact_full_flash_and_all_other_ranges_are_preserved(self) -> None:
        output = self.fixture.authorize(self.temporary / "authorized")
        _, _, context = gate._load_app_stage_authorization(
            output / "app0-stage-authorization.json",
            self.fixture.policy,
            now=self.fixture.now,
        )
        expected = context["expected_after"]
        candidate_end = self.fixture.policy.app0_offset + self.fixture.policy.candidate_bytes
        programmed_end = self.fixture.policy.app0_offset + 0x2000
        self.assertEqual(expected[: self.fixture.policy.app0_offset], self.fixture.flash[: self.fixture.policy.app0_offset])
        self.assertEqual(expected[candidate_end:programmed_end], b"\xff" * (programmed_end - candidate_end))
        self.assertEqual(expected[programmed_end:], self.fixture.flash[programmed_end:])
        readback = self.temporary / "full-after.bin"
        readback.write_bytes(expected)
        receipt = gate.core._validate_full_after_image(
            readback,
            expected,
            context["candidate"],
            self.fixture.policy,
            "Stage A full readback",
        )
        self.assertEqual(receipt["bytes"], self.fixture.policy.flash_bytes)
        protected = [0, self.fixture.policy.nvs_offset, self.fixture.policy.otadata_offset,
                     programmed_end, self.fixture.policy.app1_offset,
                     self.fixture.policy.littlefs_offset, self.fixture.policy.flash_bytes - 1]
        for index, offset in enumerate(protected):
            changed = bytearray(expected)
            changed[offset] ^= 1
            wrong = self.temporary / f"wrong-{index}.bin"
            wrong.write_bytes(changed)
            with self.assertRaisesRegex(gate.GateError, "out-of-delta mutation"):
                gate.core._validate_full_after_image(
                    wrong, expected, context["candidate"], self.fixture.policy, "Stage A full readback"
                )

    def test_stale_fake_and_mismatched_authorizations_fail_closed(self) -> None:
        mutations = {
            "stale": lambda value: value.update({
                "created_at_utc": self.fixture.zulu(dt.timedelta(hours=-26)),
                "expires_at_utc": self.fixture.zulu(dt.timedelta(hours=-25)),
            }),
            "fake-decision": lambda value: value.update({"decision": "yes"}),
            "mismatched-mac": lambda value: value["device"].update({"mac": "28:84:85:43:da:0d"}),
            "scope-escalation": lambda value: value.update({"authorized_for_boot": True}),
        }
        for label, mutation in mutations.items():
            with self.subTest(label=label):
                self.fixture._write_operator(mutation)
                output = self.temporary / f"rejected-{label}"
                with self.assertRaises(gate.GateError):
                    self.fixture.authorize(output)
                self.assertFalse(output.exists())

    def test_acceptance_must_explicitly_prove_no_removable_media_access(self) -> None:
        for value in (True, None):
            with self.subTest(value=value):
                self.fixture._write_acceptance(value)
                self.fixture._write_staging()
                self.fixture._write_operator()
                with self.assertRaisesRegex(gate.GateError, "removable_media_accessed"):
                    self.fixture.authorize(self.temporary / f"rejected-{value}")

    def test_one_time_marker_blocks_replay_even_with_a_different_output(self) -> None:
        authorized = self.fixture.authorize(self.temporary / "authorized")
        auth = authorized / "app0-stage-authorization.json"
        first_output = self.temporary / "first-execution"
        with mock.patch.object(gate.core, "_execute_reviewed_write", return_value=first_output) as execute:
            result = gate.execute_app0_stage_deferred_tf(
                auth,
                self.fixture.policy.expected_port,
                first_output,
                self.fixture.policy,
                now=self.fixture.now,
            )
            self.assertEqual(result, first_output)
            marker = self.fixture.capture / gate.ATTEMPT_MARKER_NAME
            self.assertTrue(marker.is_file())
            with self.assertRaisesRegex(gate.GateError, "already attempted or consumed"):
                gate.execute_app0_stage_deferred_tf(
                    auth,
                    self.fixture.policy.expected_port,
                    self.temporary / "replayed-execution",
                    self.fixture.policy,
                    now=self.fixture.now,
                )
            execute.assert_called_once()

    def test_capture_parent_swap_marks_original_inode_and_blocks_second_attempt(self) -> None:
        authorized = self.fixture.authorize(self.temporary / "authorized")
        auth = authorized / "app0-stage-authorization.json"
        held_original = self.temporary / "capture-held-original"
        decoy_path = self.fixture.capture
        real_preflight = gate._preflight_new_output

        def swap_parent_after_validation(path_value: str | Path) -> Path:
            output = real_preflight(path_value)
            decoy_path.rename(held_original)
            decoy_path.mkdir()
            decoy_path.chmod(0o700)
            return output

        first_output = self.temporary / "swapped-parent-execution"
        with (
            mock.patch.object(
                gate,
                "_preflight_new_output",
                side_effect=swap_parent_after_validation,
            ),
            mock.patch.object(
                gate.core, "_execute_reviewed_write", return_value=first_output
            ) as execute,
        ):
            result = gate.execute_app0_stage_deferred_tf(
                auth,
                self.fixture.policy.expected_port,
                first_output,
                self.fixture.policy,
                now=self.fixture.now,
            )
        self.assertEqual(result, first_output)
        self.assertTrue((held_original / gate.ATTEMPT_MARKER_NAME).is_file())
        self.assertFalse((decoy_path / gate.ATTEMPT_MARKER_NAME).exists())

        decoy_path.rmdir()
        held_original.rename(decoy_path)
        with (
            mock.patch.object(gate.core, "_execute_reviewed_write") as replayed,
            self.assertRaisesRegex(gate.GateError, "already attempted or consumed"),
        ):
            gate.execute_app0_stage_deferred_tf(
                auth,
                self.fixture.policy.expected_port,
                self.temporary / "parent-swap-replay",
                self.fixture.policy,
                now=self.fixture.now,
            )
        execute.assert_called_once()
        replayed.assert_not_called()

    def test_candidate_mutation_at_copy_boundary_cannot_publish_stage(self) -> None:
        real_ensure_output = gate.core._ensure_new_output

        def mutate_after_validation(path_value: str | Path) -> tuple[Path, Path]:
            output_and_temporary = real_ensure_output(path_value)
            mutated = bytearray(self.fixture.candidate.read_bytes())
            mutated[-1] ^= 1
            self.fixture.candidate.write_bytes(mutated)
            self.fixture.candidate.chmod(0o600)
            return output_and_temporary

        output = self.temporary / "candidate-copy-race-rejected"
        with (
            mock.patch.object(
                gate.core,
                "_ensure_new_output",
                side_effect=mutate_after_validation,
            ),
            self.assertRaisesRegex(gate.GateError, "candidate snapshot SHA-256"),
        ):
            self.fixture.authorize(output)
        self.assertFalse(output.exists())

    def test_same_length_staging_semantic_hash_swap_cannot_authorize_or_execute(self) -> None:
        safe_bytes = self.fixture.staging.read_bytes()
        invalid_bytes = safe_bytes.replace(
            b'"authorized_for_selector": false',
            b'"authorized_for_selector": true ',
            1,
        )
        self.assertEqual(len(invalid_bytes), len(safe_bytes))
        self.assertFalse(json.loads(safe_bytes)["authorized_for_selector"])
        self.assertTrue(json.loads(invalid_bytes)["authorized_for_selector"])
        real_sha256_bytes = gate.core.sha256_bytes

        def swapping_digest():
            swapped = False

            def digest_with_path_swap(data: bytes) -> str:
                nonlocal swapped
                if not swapped and data == safe_bytes:
                    replacement = self.temporary / "staging-invalid-same-length.json"
                    replacement.write_bytes(invalid_bytes)
                    replacement.chmod(0o600)
                    replacement.replace(self.fixture.staging)
                    swapped = True
                return real_sha256_bytes(data)

            return digest_with_path_swap

        rejected_output = self.temporary / "semantic-hash-swap-rejected"
        with (
            mock.patch.object(
                gate.core, "sha256_bytes", side_effect=swapping_digest()
            ),
            self.assertRaisesRegex(gate.GateError, "changed while it was snapshotted"),
        ):
            self.fixture.authorize(rejected_output)
        self.assertFalse(rejected_output.exists())

        self.fixture.staging.write_bytes(safe_bytes)
        self.fixture.staging.chmod(0o600)
        self.fixture._write_operator()
        authorized = self.fixture.authorize(self.temporary / "authorized-after-safe-restore")
        auth = authorized / "app0-stage-authorization.json"
        execution_output = self.temporary / "semantic-hash-execution-rejected"
        with (
            mock.patch.object(
                gate.core, "sha256_bytes", side_effect=swapping_digest()
            ),
            mock.patch.object(gate.core, "_execute_reviewed_write") as executed,
            self.assertRaisesRegex(gate.GateError, "changed while it was snapshotted"),
        ):
            gate.execute_app0_stage_deferred_tf(
                auth,
                self.fixture.policy.expected_port,
                execution_output,
                self.fixture.policy,
                now=self.fixture.now,
            )
        executed.assert_not_called()
        self.assertFalse(execution_output.exists())
        self.assertFalse((self.fixture.capture / gate.ATTEMPT_MARKER_NAME).exists())

    def test_stage_b_is_always_blocked_and_emits_nothing(self) -> None:
        output = self.temporary / "stage-b"
        with self.assertRaisesRegex(gate.GateError, "NOT AUTHORIZED / NOT IMPLEMENTED"):
            gate.authorize_stage_b(
                app_execution_receipt="fake",
                fresh_preboot_full_flash="fake",
                operator_authorization="fake",
                boot_health_evidence_plan="fake",
                output_value=output,
            )
        self.assertFalse(output.exists())
        choices = gate._parser()._subparsers._group_actions[0].choices
        self.assertNotIn("authorize-selector", choices)
        self.assertNotIn("execute-selector", choices)
        self.assertNotIn("boot", choices)

    def test_repository_paths_are_rejected_for_every_external_authority(self) -> None:
        repository_file = ROOT / "tests/test_c151_deferred_tf_app0_gate.py"
        with self.assertRaisesRegex(gate.GateError, "outside the repository"):
            gate._require_external(repository_file, "test authority")
        self.assertEqual(gate.REPO_ROOT, ROOT)

    def test_authority_files_must_be_private_single_link_owner_files(self) -> None:
        self.fixture.operator_auth.chmod(0o644)
        with self.assertRaisesRegex(gate.GateError, "private mode"):
            self.fixture.authorize(self.temporary / "public-auth-rejected")
        self.fixture.operator_auth.chmod(0o600)

        hardlink = self.temporary / "staging-hardlink.json"
        hardlink.hardlink_to(self.fixture.staging)
        with self.assertRaisesRegex(gate.GateError, "exactly one filesystem link"):
            self.fixture.authorize(self.temporary / "hardlink-rejected")

    def test_exact_timestamp_format_emitted_by_core_capture_is_accepted(self) -> None:
        manifest_path = self.fixture.capture / "capture-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["captured_at_utc"] = gate.core.utc_now()
        self.assertTrue(manifest["captured_at_utc"].endswith("+00:00"))
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        self.fixture._write_operator()
        output = self.fixture.authorize(self.temporary / "core-timestamp-authorized")
        self.assertTrue((output / "app0-stage-authorization.json").is_file())
        with self.assertRaisesRegex(gate.GateError, "explicit UTC timestamp"):
            gate._utc_timestamp("2026-08-24T12:00:00+08:00", "non-UTC")

    def test_operator_and_reviewer_future_acknowledgements_are_rejected(self) -> None:
        for party in ("operator", "reviewer"):
            with self.subTest(party=party):
                def future_acknowledgement(value: dict[str, object]) -> None:
                    acknowledgement = value[party]
                    assert isinstance(acknowledgement, dict)
                    acknowledgement["acknowledged_at_utc"] = self.fixture.zulu(
                        dt.timedelta(minutes=30)
                    )

                self.fixture._write_operator(future_acknowledgement)
                output = self.temporary / f"future-{party}-rejected"
                with self.assertRaisesRegex(gate.GateError, f"{party} acknowledgement is in the future"):
                    self.fixture.authorize(output)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
