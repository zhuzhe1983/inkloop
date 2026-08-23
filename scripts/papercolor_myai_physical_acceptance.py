#!/usr/bin/env python3
"""Post-flash MyAI/voice/AIGC acceptance for an attached PaperColor.

The harness is deliberately non-destructive: it only sends status,
album-status, voice-tap and aigc-test.  It never prints transcripts, tokens,
cookies, pairing codes or raw serial lines.  `pyserial` is imported only after
an explicit port is supplied.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Callable


EVENT_RE = re.compile(r"^INKLOOP_([A-Z][A-Z0-9_]{0,63})(?::(.*))?$")
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
    r"^drops=([0-9]{1,10}),write_failures=([0-9]{1,10})$"
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


class AcceptanceFailure(RuntimeError):
    pass


class SerialEvents:
    def __init__(self, serial_port) -> None:
        self.serial = serial_port
        self.events: list[Event] = []
        self.sequence = 0

    def drain(self, seconds: float = 0.25) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
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
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
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


def require_ready_status(detail: str) -> None:
    match = STATUS_RE.fullmatch(detail)
    if not match:
        raise AcceptanceFailure("native status is malformed")
    runtime, wifi, storage, _display, authorized, activation, _voice = (
        int(value) for value in match.groups()
    )
    if runtime != 1 or wifi != 1 or storage != 1:
        raise AcceptanceFailure("runtime, Wi-Fi or storage is not ready")
    # ActivationState::Bound is the stable public value 2. A stale durable
    # token or transient Offline state must not be mistaken for authorization.
    if authorized != 1 or activation != 2:
        raise AcceptanceFailure("MyAI is not currently Bound and authorized")


def require_safe_diagnostics(events: SerialEvents, marker: int) -> None:
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
    drops, write_failures = (int(value) for value in serial_match.groups())
    if drops != 0 or write_failures != 0:
        raise AcceptanceFailure("serial diagnostics are not trustworthy")


def run(args: argparse.Namespace) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise AcceptanceFailure("pyserial is required for physical acceptance") from exc

    with serial.Serial(args.port, args.baud, timeout=0.20, write_timeout=2) as port:
        events = SerialEvents(port)
        events.drain(0.5)

        marker = events.send("status")
        command = events.wait(
            "COMMAND", lambda value: value == "status", marker, 5)
        status = events.wait("STATUS", None, command.sequence, 8)
        require_ready_status(status.detail)
        require_safe_diagnostics(events, command.sequence)
        print("STATUS PASS")

        marker = events.send("album-status")
        events.wait("COMMAND", lambda value: value == "album-status", marker, 5)
        album = events.wait("ALBUM", lambda value: value != "UNAVAILABLE", marker, 8)
        if album.detail.count(":") < 2:
            raise AcceptanceFailure("album status is malformed")
        print("ALBUM PASS")

        marker = events.send("voice-tap")
        listening = wait_listening(events, marker)
        speak(args.local_phrase, args.voice, args.rate)
        local = events.wait(
            "VOICE_ASR_FINAL", lambda value: value.startswith("LOCAL:"),
            listening, 45)
        events.wait(
            "VOICE_TOOL", lambda value: value.startswith("storage.free:"),
            local.sequence, 15)
        events.reject_error(marker)
        print("VOICE LOCAL TOOL PASS")

        ready = events.wait("VOICE_STATE", lambda value: value == "1", marker, 20)
        marker = events.send("voice-tap")
        listening = wait_listening(events, max(marker, ready.sequence))
        speak(args.remote_phrase, args.voice, args.rate)
        remote = events.wait(
            "VOICE_ASR_FINAL", lambda value: value.startswith("REMOTE:"),
            listening, 45)
        thinking = events.wait(
            "VOICE_STATE", lambda value: value == "3", remote.sequence, 20)
        speaking = events.wait(
            "VOICE_STATE", lambda value: value == "4", thinking.sequence, 90)
        events.wait("VOICE_STATE", lambda value: value == "1", speaking.sequence, 120)
        events.reject_error(marker)
        print("VOICE REMOTE TTS PASS")

        marker = events.send("status")
        command = events.wait(
            "COMMAND", lambda value: value == "status", marker, 5)
        status = events.wait("STATUS", None, command.sequence, 8)
        require_ready_status(status.detail)
        require_safe_diagnostics(events, command.sequence)

        marker = events.send("aigc-test")
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

    print("DIGITAL PHYSICAL PIPELINE PASS")
    print("HUMAN CHECK REQUIRED: confirm final lighthouse image and RGB roles on panel")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
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
    parser.add_argument("--aigc-timeout", type=positive_int, default=600)
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
