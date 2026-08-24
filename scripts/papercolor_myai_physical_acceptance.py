#!/usr/bin/env python3
"""Post-flash MyAI/voice/AIGC serial-chain proxy for a PaperColor.

The harness is deliberately non-destructive: it only sends status,
album-status, voice-tap and aigc-test.  It never prints transcripts, tokens,
cookies, pairing codes or raw serial lines.  `pyserial` is imported only after
an explicit port is supplied.  This is not evidence for the physical top
button, button latency/p99, audible quality, or the running firmware identity.
"""

from __future__ import annotations

import argparse
import math
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Callable


EVENT_RE = re.compile(r"^INKLOOP_([A-Z][A-Z0-9_]{0,63})(?::(.*))?$")
BOOT_LOG_RE = re.compile(
    r"^(?:ESP-ROM:|rst:0x|I \([0-9]+\) (?:boot|app_init):.*"
    r"(?:Project version|ESP-IDF|chip revision)|"
    r"I \([0-9]+\) cpu_start:.*App cpu up)"
)
STATUS_RE = re.compile(
    r"^runtime=([01]),wifi=([01]),storage=([01]),display_busy=([01]),"
    r"myai_authorized=([01]),myai_activation=([0-6]),voice_state=([0-6])$"
)
AIGC_STATE_RE = re.compile(
    r"^phase=([0-4]),admission_pending=([01]),exclusive=([01]),diagnostic=([01])$"
)
NETWORK_STATE_RE = re.compile(
    r"^operation=([0-9]|1[01]),age_ms=([0-9]{1,10}),queue_depth=([0-9]{1,3})$"
)
SERIAL_STATE_RE = re.compile(
    r"^drops=([0-9]{1,10}),write_failures=([0-9]{1,10}),"
    r"button_mailbox_overflows=([0-9]{1,10})$"
)
ALBUM_RE = re.compile(r"^READY:([0-9]{1,10}):([0-9]{1,10})$")
AUDIO_DMA_RE = re.compile(
    r"^available=([01]),callbacks=([0-9]{1,10}),underruns=([0-9]{1,10}),"
    r"expected_drain_overflows=([0-9]{1,10})$"
)
AUDIO_FEED_RE = re.compile(
    r"^available=([01]),streams=([0-9]{1,10}),submits=([0-9]{1,10}),"
    r"late_submits=([0-9]{1,10}),estimated_underruns=([0-9]{1,10})$"
)
AUDIO_TIMING_RE = re.compile(
    r"^available=([01]),max_gap_us=([0-9]{1,10}),min_lead_us=([0-9]{1,10}),"
    r"max_lead_us=([0-9]{1,10}),current_queue_frames=([0-9]{1,10})$"
)
AUDIO_QUEUE_RE = re.compile(
    r"^available=([01]),peak_frames=([0-9]{1,10}),clamps=([0-9]{1,10}),"
    r"capture_timeouts=([0-9]{1,10}),playback_timeouts=([0-9]{1,10})$"
)
SAFE_RESET_REASONS = frozenset({0, 1, 2, 3, 8, 11, 12})
MAX_NETWORK_OPERATION_AGE_MS = 120_000
SAFE_COMMANDS = frozenset({"status", "album-status", "voice-tap", "aigc-test"})
MAX_LINE_BYTES = 4096
MAX_EVENTS = 4096


@dataclass(frozen=True)
class Event:
    sequence: int
    name: str
    detail: str


@dataclass(frozen=True)
class StatusSnapshot:
    runtime: int
    wifi: int
    storage: int
    display_busy: int
    myai_authorized: int
    myai_activation: int
    voice_state: int


@dataclass(frozen=True)
class AudioSnapshot:
    callbacks: int
    dma_underruns: int
    expected_drain_overflows: int
    streams: int
    submits: int
    late_submits: int
    estimated_underruns: int
    max_gap_us: int
    min_lead_us: int
    max_lead_us: int
    current_queue_frames: int
    peak_frames: int
    clamps: int
    capture_timeouts: int
    playback_timeouts: int


@dataclass(frozen=True)
class DiagnosticSnapshot:
    reset_reason: int
    button_mailbox_overflows: int
    audio: AudioSnapshot


@dataclass(frozen=True)
class AudioProgress:
    callbacks: int
    streams: int
    submits: int
    max_gap_us: int
    max_lead_us: int


@dataclass(frozen=True)
class AlbumSnapshot:
    total_items: int
    current_one_based: int


class AcceptanceFailure(RuntimeError):
    pass


class SerialEvents:
    def __init__(self, serial_port, clock: Callable[[], float] | None = None) -> None:
        self.serial = serial_port
        self.clock = clock or time.monotonic
        self.events: list[Event] = []
        self.sequence = 0
        self.boot_signals = 0

    def drain(self, seconds: float = 0.25) -> None:
        deadline = self.clock() + seconds
        while self.clock() < deadline:
            self._read_once()

    def send(self, command: str) -> int:
        if command not in SAFE_COMMANDS:
            raise AcceptanceFailure("unsafe serial command rejected")
        self.drain()
        marker = self.sequence
        self.serial.write((command + "\n").encode("ascii"))
        self.serial.flush()
        return marker

    def wait(
        self,
        name: str,
        predicate: Callable[[str], bool] | None,
        after: int,
        timeout: float,
    ) -> Event:
        deadline = self.clock() + timeout
        while self.clock() < deadline:
            for event in self.events:
                if event.sequence <= after or event.name != name:
                    continue
                if predicate is None or predicate(event.detail):
                    return event
            self._read_once()
        raise AcceptanceFailure(f"timeout waiting for {name}")

    def reject_error(self, after: int) -> None:
        for event in self.events:
            if event.sequence <= after:
                continue
            if event.name in {"VOICE_ERROR", "MYAI_ERROR", "AIGC_ERROR"}:
                raise AcceptanceFailure(f"device reported {event.name}")

    def _read_once(self) -> None:
        raw = self.serial.readline(MAX_LINE_BYTES + 1)
        if not raw:
            return
        if len(raw) > MAX_LINE_BYTES:
            return
        try:
            line = raw.decode("utf-8", "strict").strip("\r\n ")
        except UnicodeDecodeError:
            return
        match = EVENT_RE.fullmatch(line)
        if not match:
            if BOOT_LOG_RE.search(line):
                self.boot_signals += 1
            return
        self.sequence += 1
        detail = match.group(2) or ""
        # Store only bounded event details in memory. The script prints only
        # fixed verdict text and never exposes the detail of sensitive events.
        self.events.append(Event(self.sequence, match.group(1), detail[:512]))
        if len(self.events) > MAX_EVENTS:
            del self.events[: len(self.events) - MAX_EVENTS]


def speak(phrase: str, voice: str, rate: int) -> None:
    executable = shutil.which("say")
    if not executable:
        raise AcceptanceFailure("macOS say command is unavailable")
    result = subprocess.run(
        [executable, "-a", "90", "-v", voice, "-r", str(rate), phrase],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=30,
    )
    if result.returncode != 0:
        raise AcceptanceFailure("speech stimulus failed")


def wait_listening(events: SerialEvents, after: int) -> int:
    event = events.wait("VOICE_STATE", lambda value: value == "2", after, 90)
    return event.sequence


def require_ready_status(
    detail: str, *, require_quiescent: bool = False
) -> StatusSnapshot:
    match = STATUS_RE.fullmatch(detail)
    if not match:
        raise AcceptanceFailure("native status is malformed")
    snapshot = StatusSnapshot(*(int(value) for value in match.groups()))
    if snapshot.runtime != 1 or snapshot.wifi != 1 or snapshot.storage != 1:
        raise AcceptanceFailure("runtime, Wi-Fi or storage is not ready")
    # ActivationState::Bound is the stable public value 2. A stale durable
    # token or transient Offline state must not be mistaken for authorization.
    if snapshot.myai_authorized != 1 or snapshot.myai_activation != 2:
        raise AcceptanceFailure("MyAI is not currently Bound and authorized")
    if require_quiescent and (
        snapshot.display_busy != 0 or snapshot.voice_state != 1
    ):
        raise AcceptanceFailure("display or voice runtime is not quiescent")
    return snapshot


def require_safe_diagnostics(
    events: SerialEvents, marker: int
) -> DiagnosticSnapshot:
    reset = events.wait("RESET_REASON", None, marker, 8)
    if re.fullmatch(r"[0-9]{1,2}", reset.detail) is None:
        raise AcceptanceFailure("reset reason is malformed")
    if int(reset.detail) not in SAFE_RESET_REASONS:
        raise AcceptanceFailure("unsafe reset reason observed")

    aigc = events.wait("AIGC_STATE", None, marker, 8)
    aigc_match = AIGC_STATE_RE.fullmatch(aigc.detail)
    if not aigc_match:
        raise AcceptanceFailure("AIGC state is malformed")
    phase, admission, exclusive, _diagnostic = (
        int(value) for value in aigc_match.groups()
    )
    if phase != 0 or admission != 0 or exclusive != 0:
        raise AcceptanceFailure("AIGC runtime is not idle before test")

    network = events.wait("NETWORK_STATE", None, marker, 8)
    network_match = NETWORK_STATE_RE.fullmatch(network.detail)
    if not network_match:
        raise AcceptanceFailure("Network state is malformed")
    operation, age_ms, queue_depth = (
        int(value) for value in network_match.groups()
    )
    if (operation == 0 and age_ms != 0) or (
            operation != 0 and age_ms > MAX_NETWORK_OPERATION_AGE_MS):
        raise AcceptanceFailure("Network operation is stale")
    if queue_depth != 0:
        raise AcceptanceFailure("Network queue is not drained")

    serial_state = events.wait("SERIAL_STATE", None, marker, 8)
    serial_match = SERIAL_STATE_RE.fullmatch(serial_state.detail)
    if not serial_match:
        raise AcceptanceFailure("serial state is malformed")
    drops, write_failures, button_mailbox_overflows = (
        int(value) for value in serial_match.groups()
    )
    if drops != 0 or write_failures != 0 or button_mailbox_overflows != 0:
        raise AcceptanceFailure("serial diagnostics are not trustworthy")

    dma = events.wait("AUDIO_DMA", None, marker, 8)
    feed = events.wait("AUDIO_FEED", None, marker, 8)
    timing = events.wait("AUDIO_TIMING", None, marker, 8)
    queue = events.wait("AUDIO_QUEUE", None, marker, 8)
    dma_match = AUDIO_DMA_RE.fullmatch(dma.detail)
    feed_match = AUDIO_FEED_RE.fullmatch(feed.detail)
    timing_match = AUDIO_TIMING_RE.fullmatch(timing.detail)
    queue_match = AUDIO_QUEUE_RE.fullmatch(queue.detail)
    if not all((dma_match, feed_match, timing_match, queue_match)):
        raise AcceptanceFailure("audio diagnostics are malformed")
    assert dma_match and feed_match and timing_match and queue_match
    dma_values = tuple(int(value) for value in dma_match.groups())
    feed_values = tuple(int(value) for value in feed_match.groups())
    timing_values = tuple(int(value) for value in timing_match.groups())
    queue_values = tuple(int(value) for value in queue_match.groups())
    if {dma_values[0], feed_values[0], timing_values[0], queue_values[0]} != {1}:
        raise AcceptanceFailure("PaperColor audio diagnostics are unavailable")
    if timing_values[2] > timing_values[3]:
        raise AcceptanceFailure("audio queue lead diagnostics are inconsistent")
    audio = AudioSnapshot(
        callbacks=dma_values[1],
        dma_underruns=dma_values[2],
        expected_drain_overflows=dma_values[3],
        streams=feed_values[1],
        submits=feed_values[2],
        late_submits=feed_values[3],
        estimated_underruns=feed_values[4],
        max_gap_us=timing_values[1],
        min_lead_us=timing_values[2],
        max_lead_us=timing_values[3],
        current_queue_frames=timing_values[4],
        peak_frames=queue_values[1],
        clamps=queue_values[2],
        capture_timeouts=queue_values[3],
        playback_timeouts=queue_values[4],
    )
    return DiagnosticSnapshot(
        reset_reason=int(reset.detail),
        button_mailbox_overflows=button_mailbox_overflows,
        audio=audio,
    )


def require_audio_progress(
    before: AudioSnapshot, after: AudioSnapshot
) -> AudioProgress:
    monotonic_fields = (
        "callbacks",
        "dma_underruns",
        "expected_drain_overflows",
        "streams",
        "submits",
        "late_submits",
        "estimated_underruns",
        "peak_frames",
        "clamps",
        "capture_timeouts",
        "playback_timeouts",
    )
    for field in monotonic_fields:
        if getattr(after, field) < getattr(before, field):
            raise AcceptanceFailure("audio diagnostics counter regressed")
    error_fields = (
        "dma_underruns",
        "late_submits",
        "estimated_underruns",
        "playback_timeouts",
    )
    if any(getattr(after, field) != getattr(before, field) for field in error_fields):
        raise AcceptanceFailure("audio pipeline reported an underrun or timeout")
    callback_delta = after.callbacks - before.callbacks
    stream_delta = after.streams - before.streams
    submit_delta = after.submits - before.submits
    if callback_delta <= 0 or stream_delta <= 0 or submit_delta <= 0:
        raise AcceptanceFailure("audio playback diagnostics made no progress")
    if after.min_lead_us > after.max_lead_us or after.max_lead_us == 0:
        raise AcceptanceFailure("audio queue lead was not established")
    return AudioProgress(
        callbacks=callback_delta,
        streams=stream_delta,
        submits=submit_delta,
        max_gap_us=after.max_gap_us,
        max_lead_us=after.max_lead_us,
    )


def parse_album_status(detail: str) -> AlbumSnapshot:
    match = ALBUM_RE.fullmatch(detail)
    if not match:
        raise AcceptanceFailure("album status is malformed")
    total, current = (int(value) for value in match.groups())
    if (total == 0 and current != 0) or (total > 0 and not 1 <= current <= total):
        raise AcceptanceFailure("album selection is inconsistent")
    return AlbumSnapshot(total, current)


def send_and_ack(events: SerialEvents, command: str, timeout: float = 5) -> Event:
    marker = events.send(command)
    return events.wait("COMMAND", lambda value: value == command, marker, timeout)


def query_album(events: SerialEvents, timeout: float = 8) -> AlbumSnapshot | None:
    command = send_and_ack(events, "album-status")
    album = events.wait("ALBUM", None, command.sequence, timeout)
    if album.detail == "UNAVAILABLE":
        return None
    return parse_album_status(album.detail)


def wait_for_aigc_album(
    events: SerialEvents,
    before: AlbumSnapshot,
    timeout: float,
    sleeper: Callable[[float], None],
) -> AlbumSnapshot:
    deadline = events.clock() + timeout
    while events.clock() < deadline:
        remaining = deadline - events.clock()
        if remaining <= 0:
            break
        snapshot = query_album(events, min(8.0, remaining))
        if snapshot is not None:
            if snapshot.total_items < before.total_items:
                raise AcceptanceFailure("album item count regressed after AIGC")
            if (
                snapshot.total_items == before.total_items + 1
                and snapshot.current_one_based == snapshot.total_items
            ):
                return snapshot
        sleeper(min(0.5, max(0.0, deadline - events.clock())))
    raise AcceptanceFailure("AIGC asset is not visible as the current album item")


def exercise_voice_capture(
    events: SerialEvents,
    phrase: str,
    voice: str,
    rate: int,
    capture_window: float,
    speaker: Callable[[str, str, int], None],
    sleeper: Callable[[float], None],
) -> tuple[int, int]:
    start = send_and_ack(events, "voice-tap")
    listening = wait_listening(events, start.sequence)
    capture_started = events.clock()
    speaker(phrase, voice, rate)
    remaining = capture_window - (events.clock() - capture_started)
    if remaining > 0:
        sleeper(remaining)
    # Native press-to-talk semantics require an explicit second action.  Do
    # not accept an ASR result produced before this stop/cancel acknowledgement.
    stop = send_and_ack(events, "voice-tap")
    events.reject_error(start.sequence)
    return start.sequence, stop.sequence


def require_no_observed_restart(
    events: SerialEvents,
    after: int,
    reset_reason: int,
    boot_signals: int,
) -> None:
    if events.boot_signals != boot_signals:
        raise AcceptanceFailure("boot/reset log signal observed during acceptance")
    for event in events.events:
        if event.sequence <= after or event.name != "RESET_REASON":
            continue
        if event.detail != str(reset_reason):
            raise AcceptanceFailure("reset reason changed during acceptance")
    if not getattr(events.serial, "is_open", True):
        raise AcceptanceFailure("serial port disconnected during acceptance")


def run(
    args: argparse.Namespace,
    *,
    speaker: Callable[[str, str, int], None] = speak,
    sleeper: Callable[[float], None] = time.sleep,
    clock: Callable[[], float] = time.monotonic,
) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise AcceptanceFailure("pyserial is required for physical acceptance") from exc

    with serial.Serial(args.port, args.baud, timeout=0.20, write_timeout=2) as port:
        events = SerialEvents(port, clock=clock)
        events.drain(0.5)

        print(
            "SCOPE: serial chain proxy only; physical top-button and "
            "latency/p99 require separate evidence"
        )
        print(
            "SCOPE: running firmware version/boot identity requires the "
            "separate exact serial-bench receipt"
        )

        command = send_and_ack(events, "status")
        status = events.wait("STATUS", None, command.sequence, 8)
        require_ready_status(status.detail, require_quiescent=True)
        initial_diagnostics = require_safe_diagnostics(events, command.sequence)
        restart_marker = events.sequence
        initial_boot_signals = events.boot_signals
        print("STATUS PASS")

        initial_album = query_album(events)
        if initial_album is None:
            raise AcceptanceFailure("album is unavailable")
        print("ALBUM PASS")

        voice_marker, stopped = exercise_voice_capture(
            events, args.local_phrase, args.voice, args.rate,
            args.capture_window, speaker, sleeper)
        local = events.wait(
            "VOICE_ASR_FINAL", lambda value: value.startswith("LOCAL:"),
            stopped, 45)
        events.wait(
            "VOICE_TOOL", lambda value: value.startswith("storage.free:"),
            local.sequence, 15)
        events.reject_error(voice_marker)
        print("VOICE LOCAL TOOL PASS")

        ready = events.wait(
            "VOICE_STATE", lambda value: value == "1", local.sequence, 20)
        voice_marker, stopped = exercise_voice_capture(
            events, args.remote_phrase, args.voice, args.rate,
            args.capture_window, speaker, sleeper)
        remote = events.wait(
            "VOICE_ASR_FINAL", lambda value: value.startswith("REMOTE:"),
            max(stopped, ready.sequence), 45)
        thinking = events.wait(
            "VOICE_STATE", lambda value: value == "3", remote.sequence, 20)
        speaking = events.wait(
            "VOICE_STATE", lambda value: value == "4", thinking.sequence, 90)
        events.wait("VOICE_STATE", lambda value: value == "1", speaking.sequence, 120)
        events.reject_error(voice_marker)
        print("VOICE REMOTE TTS PASS")

        command = send_and_ack(events, "status")
        status = events.wait("STATUS", None, command.sequence, 8)
        require_ready_status(status.detail, require_quiescent=True)
        require_safe_diagnostics(events, command.sequence)

        command = send_and_ack(events, "aigc-test")
        marker = command.sequence
        queued = events.wait(
            "AIGC_DIAGNOSTIC", lambda value: value == "QUEUED", marker, 10)
        required = [
            "STARTING",
            "SUBMITTED",
            "GENERATION_COMPLETE",
            "CACHED",
            "DISPLAY_START",
            "DISPLAY_COMPLETE",
        ]
        after = queued.sequence
        for phase in required:
            event = events.wait(
                "AIGC_PHASE", lambda value, phase=phase: value == phase,
                after, args.aigc_timeout)
            after = event.sequence
            events.reject_error(marker)
            print(f"AIGC {phase} PASS")

        print("AIGC ASSET CACHED EVENT PASS")
        wait_for_aigc_album(
            events, initial_album, args.album_timeout, sleeper)
        print("AIGC ALBUM CURRENT-ASSET PASS")

        command = send_and_ack(events, "status")
        status = events.wait("STATUS", None, command.sequence, 8)
        require_ready_status(status.detail, require_quiescent=True)
        final_diagnostics = require_safe_diagnostics(events, command.sequence)
        if final_diagnostics.reset_reason != initial_diagnostics.reset_reason:
            raise AcceptanceFailure("reset reason changed during acceptance")
        require_no_observed_restart(
            events, restart_marker, initial_diagnostics.reset_reason,
            initial_boot_signals)
        events.reject_error(restart_marker)
        audio_progress = require_audio_progress(
            initial_diagnostics.audio, final_diagnostics.audio)
        print(
            "AUDIO PIPELINE PASS: "
            f"callbacks_delta={audio_progress.callbacks},"
            f"streams_delta={audio_progress.streams},"
            f"submits_delta={audio_progress.submits},"
            f"max_gap_us={audio_progress.max_gap_us},"
            f"max_lead_us={audio_progress.max_lead_us}"
        )
        print("FINAL STATUS/DIAGNOSTICS/SERIAL CONTINUITY PASS")

    print("SERIAL CHAIN PROXY PASS")
    print(
        "INDEPENDENT EVIDENCE REQUIRED: running version/boot identity, "
        "physical top-button behavior and button latency/p99"
    )
    print("HUMAN CHECK REQUIRED: confirm final lighthouse image and RGB roles on panel")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=positive_int, default=115200)
    parser.add_argument("--voice", default="Tingting")
    parser.add_argument("--rate", type=positive_int, default=105)
    parser.add_argument("--local-phrase", default="剩余空间")
    parser.add_argument("--remote-phrase", default="请用一句话介绍你自己")
    parser.add_argument(
        "--capture-window", type=positive_float, default=4.0,
        help="seconds from LISTENING acknowledgement to the explicit stop tap",
    )
    parser.add_argument("--aigc-timeout", type=positive_int, default=600)
    parser.add_argument("--album-timeout", type=positive_int, default=30)
    args = parser.parse_args()
    if not args.port.startswith("/dev/cu.") and not args.port.startswith("/dev/tty."):
        parser.error("an explicit serial device path is required")
    try:
        run(args)
        return 0
    except (AcceptanceFailure, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
