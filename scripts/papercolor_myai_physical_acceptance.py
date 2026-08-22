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


def run(args: argparse.Namespace) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise AcceptanceFailure("pyserial is required for physical acceptance") from exc

    with serial.Serial(args.port, args.baud, timeout=0.20, write_timeout=2) as port:
        events = SerialEvents(port)
        events.drain(0.5)

        marker = events.send("status")
        events.wait("COMMAND", lambda value: value == "status", marker, 5)
        events.wait("STATUS", None, marker, 8)
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
