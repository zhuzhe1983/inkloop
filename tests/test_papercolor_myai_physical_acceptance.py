import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "papercolor_myai_physical_acceptance.py"
SPEC = importlib.util.spec_from_file_location("papercolor_myai_acceptance", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class FakeSerial:
    def __init__(self, lines):
        self.lines = [line.encode() + b"\n" for line in lines]
        self.writes = []

    def readline(self, _maximum):
        return self.lines.pop(0) if self.lines else b""

    def write(self, value):
        self.writes.append(value)

    def flush(self):
        pass


class AcceptanceParserTest(unittest.TestCase):
    def test_structured_events_are_bounded_and_match_exactly(self):
        fake = FakeSerial([
            "noise",
            "INKLOOP_VOICE_STATE:2",
            "INKLOOP_AIGC_PHASE:DISPLAY_COMPLETE",
        ])
        events = MODULE.SerialEvents(fake)
        events.drain(0.01)
        self.assertEqual([(e.name, e.detail) for e in events.events], [
            ("VOICE_STATE", "2"),
            ("AIGC_PHASE", "DISPLAY_COMPLETE"),
        ])

    def test_only_safe_commands_can_be_sent(self):
        fake = FakeSerial([])
        events = MODULE.SerialEvents(fake)
        events.send("aigc-test")
        self.assertEqual(fake.writes, [b"aigc-test\n"])
        with self.assertRaises(MODULE.AcceptanceFailure):
            events.send("format-storage")

    def test_error_events_fail_closed_without_echoing_detail(self):
        fake = FakeSerial(["INKLOOP_AIGC_ERROR:possibly-sensitive-detail"])
        events = MODULE.SerialEvents(fake)
        events.drain(0.01)
        with self.assertRaisesRegex(MODULE.AcceptanceFailure, "AIGC_ERROR"):
            events.reject_error(0)

    def test_native_status_requires_bound_authorized_online_storage(self):
        MODULE.require_ready_status(
            "runtime=1,wifi=1,storage=1,display_busy=0,"
            "myai_authorized=1,myai_activation=2,voice_state=1"
        )
        for changed in (
            "runtime=0,wifi=1,storage=1,display_busy=0,myai_authorized=1,myai_activation=2,voice_state=1",
            "runtime=1,wifi=0,storage=1,display_busy=0,myai_authorized=1,myai_activation=2,voice_state=1",
            "runtime=1,wifi=1,storage=1,display_busy=0,myai_authorized=0,myai_activation=5,voice_state=1",
            "runtime=1,wifi=1,storage=1,display_busy=0,myai_authorized=1,myai_activation=99,voice_state=1",
        ):
            with self.subTest(changed=changed), self.assertRaises(MODULE.AcceptanceFailure):
                MODULE.require_ready_status(changed)

    def test_safe_diagnostic_snapshot_rejects_crash_stalls_and_serial_loss(self):
        healthy = [
            "INKLOOP_RESET_REASON:3",
            "INKLOOP_AIGC_STATE:phase=0,admission_pending=0,exclusive=0,diagnostic=0",
            "INKLOOP_NETWORK_STATE:operation=8,age_ms=120000,queue_depth=0",
            "INKLOOP_SERIAL_STATE:drops=0,write_failures=0",
        ]
        events = MODULE.SerialEvents(FakeSerial(healthy))
        events.drain(0.01)
        MODULE.require_safe_diagnostics(events, 0)

        mutations = (
            (0, "INKLOOP_RESET_REASON:4"),
            (1, "INKLOOP_AIGC_STATE:phase=1,admission_pending=0,exclusive=0,diagnostic=0"),
            (2, "INKLOOP_NETWORK_STATE:operation=8,age_ms=120001,queue_depth=0"),
            (2, "INKLOOP_NETWORK_STATE:operation=0,age_ms=1,queue_depth=0"),
            (2, "INKLOOP_NETWORK_STATE:operation=0,age_ms=0,queue_depth=1"),
            (3, "INKLOOP_SERIAL_STATE:drops=1,write_failures=0"),
        )
        for index, replacement in mutations:
            with self.subTest(replacement=replacement):
                lines = healthy.copy()
                lines[index] = replacement
                failed = MODULE.SerialEvents(FakeSerial(lines))
                failed.drain(0.01)
                with self.assertRaises(MODULE.AcceptanceFailure):
                    MODULE.require_safe_diagnostics(failed, 0)


if __name__ == "__main__":
    unittest.main()
