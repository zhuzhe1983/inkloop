import argparse
import contextlib
import importlib.util
import io
import sys
import types
import unittest
from unittest import mock
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


class FakeClock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        self.value += 0.01
        return self.value

    def sleep(self, seconds):
        self.value += max(0.0, seconds)


class VoiceProtocol:
    def __init__(self):
        self.sequence = 0
        self.sent = []
        self.value = 0.0

    def clock(self):
        return self.value

    def send(self, command):
        self.sequence += 1
        self.sent.append((command, self.value))
        return self.sequence

    def wait(self, name, predicate, _after, _timeout):
        self.sequence += 1
        detail = "voice-tap" if name == "COMMAND" else "2"
        if predicate is not None and not predicate(detail):
            raise AssertionError("test protocol predicate mismatch")
        return MODULE.Event(self.sequence, name, detail)

    def reject_error(self, _after):
        pass


class ScriptedDeviceSerial:
    def __init__(self):
        self.pending = []
        self.writes = []
        self.counts = {}
        self.is_open = True

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.is_open = False

    def readline(self, _maximum):
        return self.pending.pop(0) if self.pending else b""

    def write(self, value):
        command = value.decode("ascii").strip()
        self.writes.append(command)
        occurrence = self.counts.get(command, 0) + 1
        self.counts[command] = occurrence
        lines = self._response(command, occurrence)
        self.pending.extend(line.encode() + b"\n" for line in lines)

    def flush(self):
        pass

    @staticmethod
    def _status_response(occurrence):
        progressed = occurrence > 1
        return [
            "INKLOOP_COMMAND:status",
            "INKLOOP_STATUS:runtime=1,wifi=1,storage=1,display_busy=0,"
            "myai_authorized=1,myai_activation=2,voice_state=1",
            "INKLOOP_RESET_REASON:3",
            "INKLOOP_AIGC_STATE:phase=0,admission_pending=0,exclusive=0,diagnostic=0",
            "INKLOOP_NETWORK_STATE:operation=0,age_ms=0,queue_depth=0",
            "INKLOOP_SERIAL_STATE:drops=0,write_failures=0,"
            "button_mailbox_overflows=0",
            "INKLOOP_AUDIO_DMA:available=1,callbacks="
            + ("120" if progressed else "100")
            + ",underruns=0,expected_drain_overflows=0",
            "INKLOOP_AUDIO_FEED:available=1,streams="
            + ("5" if progressed else "4")
            + ",submits="
            + ("30" if progressed else "20")
            + ",late_submits=0,estimated_underruns=0",
            "INKLOOP_AUDIO_TIMING:available=1,max_gap_us="
            + ("12000" if progressed else "10000")
            + ",min_lead_us=10,max_lead_us="
            + ("120" if progressed else "100")
            + ",current_queue_frames=0",
            "INKLOOP_AUDIO_QUEUE:available=1,peak_frames=100,clamps=0,"
            "capture_timeouts=0,playback_timeouts=0",
        ]

    def _response(self, command, occurrence):
        if command == "status":
            return self._status_response(occurrence)
        if command == "album-status":
            # The first post-AIGC read intentionally observes the stale Portal
            # cache; the next one proves the newly committed current asset.
            detail = "READY:3:3" if occurrence == 3 else "READY:2:1"
            return ["INKLOOP_COMMAND:album-status", "INKLOOP_ALBUM:" + detail]
        if command == "voice-tap":
            responses = {
                1: ["INKLOOP_COMMAND:voice-tap", "INKLOOP_VOICE_STATE:2"],
                2: [
                    "INKLOOP_COMMAND:voice-tap",
                    "INKLOOP_VOICE_ASR_FINAL:LOCAL:4",
                    "INKLOOP_VOICE_TOOL:storage.free:OK",
                    "INKLOOP_VOICE_STATE:1",
                ],
                3: ["INKLOOP_COMMAND:voice-tap", "INKLOOP_VOICE_STATE:2"],
                4: [
                    "INKLOOP_COMMAND:voice-tap",
                    "INKLOOP_VOICE_ASR_FINAL:REMOTE:8",
                    "INKLOOP_VOICE_STATE:3",
                    "INKLOOP_VOICE_STATE:4",
                    "INKLOOP_VOICE_STATE:1",
                ],
            }
            return responses[occurrence]
        if command == "aigc-test":
            return [
                "INKLOOP_COMMAND:aigc-test",
                "INKLOOP_AIGC_DIAGNOSTIC:QUEUED",
                "INKLOOP_AIGC_PHASE:STARTING",
                "INKLOOP_AIGC_PHASE:SUBMITTED",
                "INKLOOP_AIGC_PHASE:GENERATION_COMPLETE",
                "INKLOOP_AIGC_PHASE:CACHED",
                "INKLOOP_AIGC_PHASE:DISPLAY_START",
                "INKLOOP_AIGC_PHASE:DISPLAY_COMPLETE",
            ]
        raise AssertionError("unexpected test command")


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

    def test_final_status_requires_quiescent_display_and_voice(self):
        with self.assertRaises(MODULE.AcceptanceFailure):
            MODULE.require_ready_status(
                "runtime=1,wifi=1,storage=1,display_busy=1,"
                "myai_authorized=1,myai_activation=2,voice_state=1",
                require_quiescent=True,
            )
        with self.assertRaises(MODULE.AcceptanceFailure):
            MODULE.require_ready_status(
                "runtime=1,wifi=1,storage=1,display_busy=0,"
                "myai_authorized=1,myai_activation=2,voice_state=4",
                require_quiescent=True,
            )

    def test_album_status_requires_coherent_current_selection(self):
        self.assertEqual(
            MODULE.parse_album_status("READY:3:2"), MODULE.AlbumSnapshot(3, 2)
        )
        self.assertEqual(
            MODULE.parse_album_status("READY:0:0"), MODULE.AlbumSnapshot(0, 0)
        )
        for value in ("READY:0:1", "READY:3:0", "READY:3:4", "3:2"):
            with self.subTest(value=value), self.assertRaises(
                MODULE.AcceptanceFailure
            ):
                MODULE.parse_album_status(value)

    def test_safe_diagnostic_snapshot_rejects_crash_stalls_and_serial_loss(self):
        healthy = [
            "INKLOOP_RESET_REASON:3",
            "INKLOOP_AIGC_STATE:phase=0,admission_pending=0,exclusive=0,diagnostic=0",
            "INKLOOP_NETWORK_STATE:operation=8,age_ms=120000,queue_depth=0",
            "INKLOOP_SERIAL_STATE:drops=0,write_failures=0,"
            "button_mailbox_overflows=0",
            "INKLOOP_AUDIO_DMA:available=1,callbacks=10,underruns=0,"
            "expected_drain_overflows=0",
            "INKLOOP_AUDIO_FEED:available=1,streams=1,submits=10,"
            "late_submits=0,estimated_underruns=0",
            "INKLOOP_AUDIO_TIMING:available=1,max_gap_us=100,min_lead_us=10,"
            "max_lead_us=20,current_queue_frames=0",
            "INKLOOP_AUDIO_QUEUE:available=1,peak_frames=10,clamps=0,"
            "capture_timeouts=0,playback_timeouts=0",
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
            (
                3,
                "INKLOOP_SERIAL_STATE:drops=1,write_failures=0,"
                "button_mailbox_overflows=0",
            ),
            (
                3,
                "INKLOOP_SERIAL_STATE:drops=0,write_failures=0,"
                "button_mailbox_overflows=1",
            ),
            (
                4,
                "INKLOOP_AUDIO_DMA:available=0,callbacks=0,underruns=0,"
                "expected_drain_overflows=0",
            ),
            (
                6,
                "INKLOOP_AUDIO_TIMING:available=1,max_gap_us=100,"
                "min_lead_us=21,max_lead_us=20,current_queue_frames=0",
            ),
        )
        for index, replacement in mutations:
            with self.subTest(replacement=replacement):
                lines = healthy.copy()
                lines[index] = replacement
                failed = MODULE.SerialEvents(FakeSerial(lines))
                failed.drain(0.01)
                with self.assertRaises(MODULE.AcceptanceFailure):
                    MODULE.require_safe_diagnostics(failed, 0)

    def test_audio_progress_requires_playback_without_new_errors(self):
        before = MODULE.AudioSnapshot(
            callbacks=10,
            dma_underruns=0,
            expected_drain_overflows=0,
            streams=1,
            submits=10,
            late_submits=0,
            estimated_underruns=0,
            max_gap_us=100,
            min_lead_us=10,
            max_lead_us=20,
            current_queue_frames=0,
            peak_frames=10,
            clamps=0,
            capture_timeouts=0,
            playback_timeouts=0,
        )
        after = MODULE.AudioSnapshot(
            **{
                **before.__dict__,
                "callbacks": 20,
                "streams": 2,
                "submits": 15,
                "max_gap_us": 125,
                "max_lead_us": 30,
            }
        )
        progress = MODULE.require_audio_progress(before, after)
        self.assertEqual((progress.callbacks, progress.streams, progress.submits), (10, 1, 5))

        no_progress = MODULE.AudioSnapshot(**before.__dict__)
        with self.assertRaisesRegex(MODULE.AcceptanceFailure, "no progress"):
            MODULE.require_audio_progress(before, no_progress)
        underrun = MODULE.AudioSnapshot(
            **{**after.__dict__, "estimated_underruns": 1}
        )
        with self.assertRaisesRegex(MODULE.AcceptanceFailure, "underrun"):
            MODULE.require_audio_progress(before, underrun)

    def test_voice_capture_uses_explicit_start_and_stop_taps(self):
        events = VoiceProtocol()
        spoken = []

        def speaker(phrase, voice, rate):
            spoken.append((phrase, voice, rate))
            events.value += 1.25

        MODULE.exercise_voice_capture(
            events,
            "test phrase",
            "voice",
            100,
            4.0,
            speaker,
            lambda seconds: setattr(events, "value", events.value + seconds),
        )
        self.assertEqual([item[0] for item in events.sent], ["voice-tap", "voice-tap"])
        self.assertAlmostEqual(events.sent[1][1] - events.sent[0][1], 4.0)
        self.assertEqual(spoken, [("test phrase", "voice", 100)])

    def test_boot_signal_or_reset_change_fails_closed(self):
        fake = FakeSerial(["ESP-ROM:esp32s3-20210327"])
        events = MODULE.SerialEvents(fake)
        events._read_once()
        with self.assertRaisesRegex(MODULE.AcceptanceFailure, "boot/reset"):
            MODULE.require_no_observed_restart(events, 0, 3, 0)

        events = MODULE.SerialEvents(FakeSerial([]))
        events.events.append(MODULE.Event(1, "RESET_REASON", "8"))
        with self.assertRaisesRegex(MODULE.AcceptanceFailure, "reset reason"):
            MODULE.require_no_observed_restart(events, 0, 3, 0)

    def test_full_proxy_rechecks_album_and_final_diagnostics(self):
        device = ScriptedDeviceSerial()
        clock = FakeClock()
        serial_module = types.SimpleNamespace(Serial=lambda *_args, **_kwargs: device)
        args = argparse.Namespace(
            port="/dev/cu.fake",
            baud=115200,
            voice="voice",
            rate=100,
            local_phrase="local",
            remote_phrase="remote",
            capture_window=0.25,
            aigc_timeout=10,
            album_timeout=10,
        )
        output = io.StringIO()
        with mock.patch.dict(
            sys.modules, {"serial": serial_module}
        ), contextlib.redirect_stdout(output):
            MODULE.run(
                args,
                speaker=lambda *_args: None,
                sleeper=clock.sleep,
                clock=clock.now,
            )
        self.assertEqual(device.counts["voice-tap"], 4)
        self.assertEqual(device.counts["album-status"], 3)
        self.assertEqual(device.counts["status"], 3)
        self.assertIn("AIGC ALBUM CURRENT-ASSET PASS", output.getvalue())
        self.assertIn("AUDIO PIPELINE PASS", output.getvalue())
        self.assertIn("FINAL STATUS/DIAGNOSTICS/SERIAL CONTINUITY PASS", output.getvalue())
        self.assertIn("SERIAL CHAIN PROXY PASS", output.getvalue())
        self.assertIn("physical top-button", output.getvalue())
        self.assertIn("running version/boot identity", output.getvalue())


if __name__ == "__main__":
    unittest.main()
