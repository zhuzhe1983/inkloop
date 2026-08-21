import contextlib
import importlib.util
import io
import json
import random
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "papercolor_c151_serial_bench.py"
SPEC = importlib.util.spec_from_file_location("papercolor_c151_serial_bench", MODULE_PATH)
assert SPEC and SPEC.loader
bench = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)


def happy_lines(code="123456"):
    status = {
        "firmware": "0.3.0-bench",
        "protocolFirmware": "0.3.0",
        "hardwareId": "M5PC-A1B2C3D4E5F6",
        "deviceId": "inkloop-device-permanent-id",
        "board": 28,
        "pm1": True,
        "wifi": False,
        "paired": False,
        "pairingCode": code,
        "displayBusy": False,
    }
    return [
        "ESP-ROM:esp32s3-20210327",
        "INKLOOP_RESET_REASON:1",
        "INKLOOP_BOOT:0.3.0-bench",
        "INKLOOP_BOARD:28",
        "INKLOOP_PM1:READY",
        "INKLOOP_HARDWARE_READY:READY",
        "INKLOOP_HARDWARE_ID:M5PC-A1B2C3D4E5F6",
        "INKLOOP_WIFI_AP:Inkloop-E5F6",
        f"INKLOOP_MYAI_PAIR_CODE:{code}",
        f"INKLOOP_PAIR_CODE:{code}",
        "INKLOOP_STATE:WAITING_BIND",
        "INKLOOP_COMMAND:status",
        "INKLOOP_STATUS:" + json.dumps(status),
        "INKLOOP_BUTTON:VOICE",
        "INKLOOP_VOICE_STATE:LISTENING",
        "INKLOOP_IMAGE_STATE:DOWNLOADING",
        "INKLOOP_SLEEP:ELIGIBLE",
        "INKLOOP_WAKE:TOP_BUTTON",
    ]


def check(report, name):
    return next(item for item in report["machineChecks"] if item["name"] == name)


class ParserTests(unittest.TestCase):
    def test_happy_boot_is_machine_pass_but_never_physical_pass(self):
        report = bench.analyze_transcript(happy_lines(), timed_out=True)
        self.assertEqual(report["verdict"]["machine"], "PASS")
        self.assertEqual(report["verdict"]["physical"], "NOT_TESTED_NOT_VERIFIED")
        self.assertFalse(report["scope"]["realHardwareClaim"])
        self.assertEqual(report["pairing"]["distinctCodes"], 1)
        self.assertEqual(report["pairing"]["code"], "12****")
        self.assertEqual(check(report, "single_pairing_code_semantics")["status"], "PASS")
        self.assertEqual(report["stateEventCounts"], {
            "button": 1,
            "voice": 1,
            "image": 1,
            "sleep": 1,
            "wake": 3,
        })

    def test_timeout_without_structured_serial_fails_closed(self):
        report = bench.analyze_transcript([], timed_out=True)
        self.assertEqual(report["verdict"]["machine"], "FAIL")
        self.assertEqual(check(report, "serial_readiness")["status"], "FAIL")
        self.assertTrue(report["inputStats"]["timedOutAtWindowEnd"])

    def test_malformed_json_truncated_and_oversized_lines_fail_closed(self):
        malformed = bench.analyze_transcript(["INKLOOP_STATUS:{\"firmware\":"], timed_out=True)
        self.assertEqual(check(malformed, "serial_protocol_integrity")["status"], "FAIL")
        truncated = bench.analyze_transcript(happy_lines(), timed_out=True, partial=b"INKLOOP_STA")
        self.assertEqual(check(truncated, "serial_protocol_integrity")["status"], "FAIL")
        oversized = bench.analyze_transcript([b"X" * (bench.MAX_LINE_BYTES + 1)])
        self.assertEqual(check(oversized, "serial_protocol_integrity")["status"], "FAIL")

    def test_duplicate_same_code_is_allowed_but_two_distinct_codes_fail(self):
        duplicate = bench.analyze_transcript(happy_lines("654321") + [
            "INKLOOP_PAIR_CODE:654321",
            "INKLOOP_MYAI_PAIR_CODE:654321",
        ])
        self.assertEqual(duplicate["pairing"]["distinctCodes"], 1)
        self.assertEqual(check(duplicate, "single_pairing_code_semantics")["status"], "PASS")

        two_codes = bench.analyze_transcript(happy_lines("654321") + [
            "INKLOOP_PAIR_CODE:654320",
        ])
        self.assertEqual(two_codes["pairing"]["distinctCodes"], 2)
        self.assertEqual(check(two_codes, "single_pairing_code_semantics")["status"], "FAIL")
        self.assertEqual(two_codes["verdict"]["machine"], "FAIL")

    def test_malformed_pairing_code_fails_closed(self):
        report = bench.analyze_transcript(happy_lines() + ["INKLOOP_PAIR_CODE:12345"])
        self.assertEqual(check(report, "single_pairing_code_semantics")["status"], "FAIL")
        self.assertEqual(check(report, "serial_protocol_integrity")["status"], "FAIL")

    def test_secret_leak_is_detected_and_redacted_from_report(self):
        secret = "this-must-never-appear"
        report = bench.analyze_transcript(happy_lines() + [
            f"Authorization: Bearer {secret}",
            f"INKLOOP_GATEWAY_TOKEN:{secret}",
            "INKLOOP_DIAGNOSTIC:" + json.dumps({"device_token": secret}),
        ])
        serialized = json.dumps(report, sort_keys=True)
        self.assertEqual(check(report, "credential_hygiene")["status"], "FAIL")
        self.assertNotIn(secret, serialized)
        self.assertIn("[REDACTED]", serialized)

    def test_dynamic_secret_labels_and_whitespace_values_never_reach_output(self):
        dynamic_key_secret = "SUPERSECRETVALUE"
        event_suffix_secret = "SUFFIXSECRET42"
        whitespace_secret = "whiteSpaceSecret42"
        report = bench.analyze_transcript(happy_lines() + [
            'INKLOOP_DIAGNOSTIC:{"device_token_' + dynamic_key_secret + '":"x"}',
            'INKLOOP_DIAGNOSTIC:{"items":[{"custom_secret_NESTED":"x"}]}',
            "INKLOOP_TOKEN_" + event_suffix_secret + ":x",
            "api_key " + whitespace_secret,
            "Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
            "Authorization Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
            "Authorization: Token SUPERSECRET42",
            "api key SUPERSECRET42",
        ])
        serialized = json.dumps(report, sort_keys=True, allow_nan=False)
        console = "\n".join(bench.serialize_console_event(event) for event in report["events"])
        for secret in (
            dynamic_key_secret,
            event_suffix_secret,
            whitespace_secret,
            "custom_secret_NESTED",
            "QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
            "SUPERSECRET42",
        ):
            self.assertNotIn(secret, serialized)
            self.assertNotIn(secret, console)
        self.assertEqual(check(report, "credential_hygiene")["status"], "FAIL")
        self.assertIn("[REDACTED_SENSITIVE_EVENT]", serialized)
        self.assertIn("[REDACTED_SENSITIVE_KEY_1]", serialized)

    def test_secret_redaction_fuzzes_authorization_schemes_and_split_labels(self):
        lines = happy_lines()
        markers = []
        for index, scheme in enumerate(("Basic", "Bearer", "Token", "Digest")):
            marker = f"AuthMarker{index}Z9"
            markers.append(marker)
            lines.extend((
                f"Authorization: {scheme} {marker}",
                f"AUTHORIZATION {scheme.upper()} {marker}",
                f"authorization={scheme} {marker}",
            ))
        labels = (
            "api key", "API.KEY", "api/key", "access token", "device token",
            "refresh-token", "gateway.token", "pairing/token", "session id",
        )
        for index, label in enumerate(labels):
            marker = f"SplitMarker{index}Q7"
            markers.append(marker)
            lines.append(f"{label} {marker}")
        report = bench.analyze_transcript(lines)
        report_text = json.dumps(report, sort_keys=True, allow_nan=False)
        console_text = "\n".join(
            bench.serialize_console_event(event) for event in report["events"]
        )
        for marker in markers:
            self.assertNotIn(marker, report_text)
            self.assertNotIn(marker, console_text)
        self.assertEqual(check(report, "credential_hygiene")["status"], "FAIL")

    def test_secret_expression_fuzz_keeps_512_unique_markers_out_of_all_outputs(self):
        generator = random.Random(15142)
        schemes = ("Basic", "Bearer", "Token", "Digest")
        label_pairs = (
            ("api", "key"), ("access", "token"), ("device", "token"),
            ("refresh", "token"), ("gateway", "token"), ("session", "id"),
        )
        separators = (" ", ".", "/", "-", "_")
        for batch in range(8):
            lines = happy_lines()
            markers = []
            for offset in range(64):
                index = batch * 64 + offset
                marker = f"CredentialMarker{index:04d}X{generator.randrange(1000, 9999)}"
                markers.append(marker)
                if index % 2 == 0:
                    scheme = schemes[index % len(schemes)]
                    delimiter = ": " if index % 4 else " "
                    lines.append(f"Authorization{delimiter}{scheme} {marker}")
                else:
                    first, second = label_pairs[index % len(label_pairs)]
                    separator = separators[index % len(separators)]
                    lines.append(f"{first}{separator}{second} {marker}")
            report = bench.analyze_transcript(lines)
            outputs = json.dumps(report, allow_nan=False) + "\n" + "\n".join(
                bench.serialize_console_event(event) for event in report["events"]
            )
            for marker in markers:
                self.assertNotIn(marker, outputs)

    def test_strict_json_depth_cardinality_and_nonfinite_numbers_fail_without_exception(self):
        nested = '{"x":' + "[" * 1000 + "0" + "]" * 1000 + "}"
        self.assertEqual(len(nested), 2007)
        nested_report = bench.analyze_transcript(happy_lines() + [
            "INKLOOP_DIAGNOSTIC:" + nested,
        ])
        self.assertEqual(check(nested_report, "serial_protocol_integrity")["status"], "FAIL")
        self.assertIn("json_depth_limit_exceeded", check(
            nested_report, "serial_protocol_integrity"
        )["evidence"])

        crowded = json.dumps({"items": list(range(bench.MAX_JSON_CONTAINER_ITEMS + 1))})
        crowded_report = bench.analyze_transcript(happy_lines() + [
            "INKLOOP_DIAGNOSTIC:" + crowded,
        ])
        self.assertEqual(check(crowded_report, "serial_protocol_integrity")["status"], "FAIL")

        for constant in ("NaN", "Infinity", "-Infinity", "1e9999"):
            with self.subTest(constant=constant):
                report = bench.analyze_transcript(happy_lines() + [
                    'INKLOOP_DIAGNOSTIC:{"metric":' + constant + "}",
                ])
                self.assertEqual(check(report, "serial_protocol_integrity")["status"], "FAIL")
                json.dumps(report, allow_nan=False)

        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "invalid.json"
            with self.assertRaises(ValueError):
                bench.write_report(target, {"metric": float("nan")})
            self.assertFalse(target.exists())

        for resource_error in (RecursionError(), MemoryError()):
            with self.subTest(resource_error=type(resource_error).__name__):
                analyzer = bench.BenchAnalyzer(sent_commands=("status",))
                with mock.patch.object(bench.json, "loads", side_effect=resource_error):
                    analyzer.consume_line('INKLOOP_STATUS:{"firmware":"x"}')
                analyzer.finish(timed_out=True)
                report = analyzer.build_report()
                integrity = check(report, "serial_protocol_integrity")
                self.assertEqual(integrity["status"], "FAIL")
                self.assertIn("json_resource_limit_exceeded", integrity["evidence"])

    def test_inactive_and_payment_states_are_distinct_failures(self):
        for state in ("inactive", "device_inactive", "payment_required"):
            with self.subTest(state=state):
                report = bench.analyze_transcript(happy_lines() + [
                    f"INKLOOP_MYAI_STATE:{state}",
                ])
                state_check = check(report, "myai_activation_payment_state")
                self.assertEqual(state_check["status"], "FAIL")
                self.assertIn(state, state_check["evidence"])
                self.assertEqual(report["verdict"]["machine"], "FAIL")

        raw_json = bench.analyze_transcript(happy_lines() + [json.dumps({
            "app_id": "inkloop",
            "status": "payment_required",
        })])
        self.assertEqual(check(raw_json, "myai_activation_payment_state")["status"], "FAIL")

    def test_optional_hardware_commands_are_allowlisted_and_require_ack(self):
        commands = bench.build_commands("diag", audio=True, rgb=True, display=True)
        self.assertEqual(commands, ("diag", "sound-test", "led-test", "screen-test"))
        report = bench.analyze_transcript(
            [line.replace("INKLOOP_COMMAND:status", "INKLOOP_COMMAND:diag") for line in happy_lines()] + [
                "INKLOOP_TEST:SOUND_OK",
                "INKLOOP_TEST:LED_OK",
                "INKLOOP_TEST:SCREEN_OK",
            ],
            sent_commands=commands,
        )
        self.assertEqual(report["verdict"]["machine"], "PASS")
        missing_ack = bench.analyze_transcript(happy_lines(), sent_commands=("status", "screen-test"))
        self.assertEqual(check(missing_ack, "screen-test_acknowledgement")["status"], "FAIL")
        with self.assertRaises(bench.BenchInputError):
            bench.build_commands("reboot", audio=False, rgb=False, display=False)

    def test_diagnostic_response_must_follow_successfully_sent_status_or_diag(self):
        missing_response = bench.analyze_transcript(
            [
                line.replace("INKLOOP_COMMAND:status", "INKLOOP_COMMAND:diag")
                for line in happy_lines()
                if not line.startswith("INKLOOP_STATUS:")
            ],
            sent_commands=("diag",),
        )
        self.assertEqual(check(missing_response, "diagnostic_command_response")["status"], "FAIL")
        self.assertEqual(missing_response["verdict"]["machine"], "FAIL")

        stale = bench.BenchAnalyzer(planned_commands=("diag",))
        for line in happy_lines():
            stale.consume_line(line)
        stale.record_command_sent("diag")
        stale_report = stale.build_report()
        self.assertEqual(check(stale_report, "planned_commands_executed")["status"], "PASS")
        self.assertEqual(check(stale_report, "diagnostic_command_response")["status"], "FAIL")

        never_written = bench.BenchAnalyzer(planned_commands=("status",))
        for line in happy_lines():
            never_written.consume_line(line)
        never_written_report = never_written.build_report()
        self.assertEqual(check(never_written_report, "planned_commands_executed")["status"], "FAIL")
        self.assertEqual(never_written_report["scope"]["commandsSent"], [])

    def test_diagnostic_response_requires_typed_bounded_matching_identity(self):
        invalid_payloads = (
            {"firmware": None, "hardwareId": None},
            {"firmware": "", "hardwareId": ""},
            {"firmware": False, "hardwareId": False},
            {"firmware": 3, "hardwareId": 7},
            {"firmware": [], "hardwareId": []},
            {"firmware": {}, "hardwareId": {}},
            {"firmware": "x" * (bench.MAX_FIRMWARE_ID_LENGTH + 1), "hardwareId": "M5PC-A1B2C3D4E5F6"},
            {"firmware": "0.3.0-bench", "hardwareId": "M5PC-000000000000"},
            {"firmware": "different-build", "hardwareId": "M5PC-A1B2C3D4E5F6"},
        )
        for payload in invalid_payloads:
            with self.subTest(payload=payload):
                lines = [
                    "INKLOOP_STATUS:" + json.dumps(payload)
                    if line.startswith("INKLOOP_STATUS:") else line
                    for line in happy_lines()
                ]
                report = bench.analyze_transcript(lines)
                self.assertEqual(check(report, "diagnostic_command_response")["status"], "FAIL")
                self.assertEqual(report["verdict"]["machine"], "FAIL")

    def test_live_rx_epoch_drains_entire_prequeued_stale_response(self):
        transcript = ("\n".join(happy_lines()) + "\n").encode("utf-8")

        class FakeSerial:
            def __init__(self, queued):
                self.buffer = bytearray(queued)
                self.writes = []
                self.reset_calls = 0

            @property
            def in_waiting(self):
                return len(self.buffer)

            def reset_input_buffer(self):
                self.reset_calls += 1
                self.buffer.clear()

            def read(self, size=1):
                size = min(size, len(self.buffer))
                data = bytes(self.buffer[:size])
                del self.buffer[:size]
                return data

            def write(self, data):
                self.writes.append(data)
                return len(data)

            def flush(self):
                return None

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        fake = FakeSerial(transcript)
        fake_serial_module = types.SimpleNamespace(Serial=lambda **_kwargs: fake)

        class Clock:
            def __init__(self):
                self.value = 0.0

            def monotonic(self):
                self.value += 0.05
                return self.value

        clock = Clock()
        args = types.SimpleNamespace(
            port="/dev/fake-c151",
            baud=115200,
            timeout=5.0,
            report=None,
            diagnostic="status",
            test_audio=False,
            test_rgb=False,
            test_display=False,
            observe=[],
        )
        with mock.patch.dict(sys.modules, {"serial": fake_serial_module}), mock.patch.object(
            bench.time, "monotonic", side_effect=clock.monotonic
        ), contextlib.redirect_stdout(io.StringIO()):
            exit_code, report = bench.run_serial_bench(args)
        self.assertEqual(fake.writes, [b"status\n"])
        self.assertGreaterEqual(fake.reset_calls, 2)
        self.assertEqual(exit_code, 1)
        self.assertEqual(check(report, "diagnostic_command_response")["status"], "FAIL")
        self.assertNotEqual(report["verdict"]["machine"], "PASS")
        write_epoch = report["scope"]["commandWrites"][0]["rxEpoch"]
        stale_evidence = [
            event for event in report["events"] if event["name"] in {"COMMAND", "STATUS"}
        ]
        self.assertTrue(stale_evidence)
        self.assertTrue(all(event["rxEpoch"] < write_epoch for event in stale_evidence))

    def test_reset_and_reboot_evidence_is_ordered_and_cannot_reuse_stale_boot(self):
        wrong_order = happy_lines()
        reset = wrong_order.pop(wrong_order.index("INKLOOP_RESET_REASON:1"))
        wrong_order.insert(wrong_order.index("INKLOOP_BOOT:0.3.0-bench") + 1, reset)
        wrong_report = bench.analyze_transcript(wrong_order)
        self.assertEqual(check(wrong_report, "reset_and_runtime_serial_sequence")["status"], "FAIL")

        incomplete = bench.analyze_transcript(happy_lines() + ["INKLOOP_STATE:REBOOTING"])
        self.assertEqual(check(incomplete, "reboot_completion_sequence")["status"], "FAIL")

        complete = bench.analyze_transcript(happy_lines() + [
            "INKLOOP_STATE:REBOOTING",
            "INKLOOP_RESET_REASON:3",
            "INKLOOP_BOOT:0.3.0-bench",
            "INKLOOP_BOARD:28",
            "INKLOOP_PM1:READY",
            "INKLOOP_HARDWARE_READY:READY",
        ])
        self.assertEqual(check(complete, "reboot_completion_sequence")["status"], "PASS")
        self.assertEqual(complete["verdict"]["machine"], "PASS")

        unknown_after_controlled_reboot = bench.analyze_transcript(happy_lines() + [
            "INKLOOP_STATE:REBOOTING",
            "INKLOOP_RESET_REASON:0",
            "INKLOOP_BOOT:0.3.0-bench",
            "INKLOOP_BOARD:28",
            "INKLOOP_PM1:READY",
            "INKLOOP_HARDWARE_READY:READY",
        ])
        self.assertEqual(
            check(unknown_after_controlled_reboot, "reboot_completion_sequence")["status"],
            "FAIL",
        )

    def test_unknown_reset_is_truthful_typed_initial_boot_evidence(self):
        report = bench.analyze_transcript([
            "INKLOOP_RESET_REASON:0" if line == "INKLOOP_RESET_REASON:1" else line
            for line in happy_lines()
        ])
        self.assertEqual(report["verdict"]["machine"], "PASS")
        self.assertEqual(report["resetReasons"], [{
            "raw": 0,
            "kind": "UNKNOWN",
            "known": False,
            "sequence": 2,
        }])
        self.assertIn("reset_reason_unknown", report["warnings"])

    def test_pinned_reset_enum_is_exact_and_invalid_values_fail_closed(self):
        expected = {
            0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC",
            5: "INT_WDT", 6: "TASK_WDT", 7: "WDT", 8: "DEEPSLEEP",
            9: "BROWNOUT", 10: "SDIO",
        }
        for raw, kind in expected.items():
            with self.subTest(raw=raw):
                report = bench.analyze_transcript([
                    f"INKLOOP_RESET_REASON:{raw}"
                    if line == "INKLOOP_RESET_REASON:1" else line
                    for line in happy_lines()
                ])
                self.assertEqual(report["resetReasons"][0]["kind"], kind)
                self.assertEqual(
                    check(report, "serial_protocol_integrity")["status"], "PASS"
                )
        for invalid in ("", "-1", "11", "15", "999999999999999999999", "true", "POWERON"):
            with self.subTest(invalid=invalid):
                line = "INKLOOP_RESET_REASON" + (f":{invalid}" if invalid else "")
                report = bench.analyze_transcript([
                    line if item == "INKLOOP_RESET_REASON:1" else item
                    for item in happy_lines()
                ])
                self.assertEqual(
                    check(report, "serial_protocol_integrity")["status"], "FAIL"
                )

    def test_startup_chain_requires_exact_order_and_nonempty_supported_values(self):
        replacements = (
            ("INKLOOP_RESET_REASON:1", "INKLOOP_RESET_REASON"),
            ("INKLOOP_RESET_REASON:1", "INKLOOP_RESET_REASON:11"),
            ("INKLOOP_BOOT:0.3.0-bench", "INKLOOP_BOOT"),
            ("INKLOOP_BOARD:28", "INKLOOP_BOARD"),
            ("INKLOOP_BOARD:28", "INKLOOP_BOARD:15"),
            ("INKLOOP_PM1:READY", "INKLOOP_PM1"),
            ("INKLOOP_HARDWARE_READY:READY", "INKLOOP_HARDWARE_READY"),
        )
        for original, replacement in replacements:
            with self.subTest(replacement=replacement):
                lines = [replacement if line == original else line for line in happy_lines()]
                report = bench.analyze_transcript(lines)
                self.assertEqual(check(report, "reset_and_runtime_serial_sequence")["status"], "FAIL")
                self.assertEqual(report["verdict"]["machine"], "FAIL")

        reversed_board_pm1 = happy_lines()
        board_index = reversed_board_pm1.index("INKLOOP_BOARD:28")
        pm1_index = reversed_board_pm1.index("INKLOOP_PM1:READY")
        reversed_board_pm1[board_index], reversed_board_pm1[pm1_index] = (
            reversed_board_pm1[pm1_index], reversed_board_pm1[board_index]
        )
        report = bench.analyze_transcript(reversed_board_pm1)
        self.assertEqual(check(report, "reset_and_runtime_serial_sequence")["status"], "FAIL")

    def test_observations_remain_operator_assertions_and_report_write_is_redacted(self):
        analyzer = bench.BenchAnalyzer(
            sent_commands=("status",),
            observations={"screen": "pass", "speaker": "fail"},
        )
        for line in happy_lines():
            analyzer.consume_line(line)
        report = analyzer.build_report(port="/dev/cu.usbmodem-test")
        self.assertEqual(report["verdict"]["physical"], "HUMAN_OBSERVATIONS_RECORDED_NOT_VERIFIED")
        self.assertEqual(report["verdict"]["machine"], "PASS")
        self.assertEqual(report["verdict"]["human"], "HUMAN_REPORTED_FAIL")
        self.assertEqual(report["verdict"]["overall"], "FAIL")
        self.assertIn("operator reported a physical failure", report["verdict"]["summary"])
        statuses = {item["name"]: item["status"] for item in report["humanObservedChecks"]}
        self.assertEqual(statuses["screen"], "HUMAN_REPORTED_OK")
        self.assertEqual(statuses["speaker"], "HUMAN_REPORTED_FAIL")
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "report.json"
            bench.write_report(target, report)
            loaded = json.loads(target.read_text(encoding="utf-8"))
            self.assertEqual(loaded["schema"], bench.SCHEMA_VERSION)

    def test_port_and_timeout_require_explicit_bounded_values(self):
        self.assertEqual(bench.validate_port("/dev/cu.usbmodem123"), "/dev/cu.usbmodem123")
        for value in ("auto", "*", "/dev/cu.usb*", ""):
            with self.subTest(value=value), self.assertRaises(bench.BenchInputError):
                bench.validate_port(value)
        self.assertEqual(bench.bounded_timeout("60"), 60.0)
        for value in ("0", "301", "inf"):
            with self.subTest(value=value), self.assertRaises(Exception):
                bench.bounded_timeout(value)

    def test_runbook_and_cli_help_match_the_safety_contract(self):
        runbook = (ROOT / "docs" / "papercolor-c151-serial-bench.md").read_text(
            encoding="utf-8"
        )
        help_text = bench.argument_parser().format_help()
        for command in sorted(bench.SAFE_BASE_COMMANDS | frozenset(
            bench.SAFE_OPTIONAL_COMMANDS.values()
        )):
            self.assertIn(f"`{command}`", runbook)
        for flag in ("--port", "--timeout", "--report", "--test-audio", "--test-rgb", "--test-display"):
            self.assertIn(flag, help_text)
        self.assertIn("RESET_REASON:0..10", runbook)
        self.assertIn("BOARD:28", runbook)
        self.assertIn("fresh receive epoch", runbook)
        self.assertIn("allow_nan=false", runbook)

        path_secret = "whiteSpaceSecret42"
        self.assertNotIn(
            path_secret,
            bench.redact_text("/tmp/api_key " + path_secret + "/report.json"),
        )

    def test_random_byte_fuzz_has_no_uncaught_input_exception(self):
        generator = random.Random(151)
        for _ in range(5000):
            length = generator.randrange(0, bench.MAX_LINE_BYTES + 104)
            payload = bytes(generator.randrange(0, 256) for _ in range(length))
            analyzer = bench.BenchAnalyzer(sent_commands=("status",))
            analyzer.consume_line(payload)
            analyzer.finish(timed_out=True)
            report = analyzer.build_report()
            json.dumps(report, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
