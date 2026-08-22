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


if __name__ == "__main__":
    unittest.main()
