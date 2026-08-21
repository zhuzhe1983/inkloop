#!/usr/bin/env python3
"""Fail-closed post-flash serial acceptance harness for M5 PaperColor C151.

The parser and report builder intentionally use only the Python standard
library.  pyserial is imported only when the CLI opens an explicitly supplied
port, so transcript tests do not need hardware or third-party packages.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA_VERSION = "inkloop.papercolor-c151-bench.v1"
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT_SECONDS = 60.0
MIN_TIMEOUT_SECONDS = 5.0
MAX_TIMEOUT_SECONDS = 300.0
MAX_LINE_BYTES = 4096
MAX_EVENT_RECORDS = 768
MAX_JSON_DEPTH = 16
MAX_JSON_CONTAINER_ITEMS = 128
MAX_JSON_TOTAL_ITEMS = 384
MAX_FIRMWARE_ID_LENGTH = 64
MAX_RX_DRAIN_BYTES = 65536
MAX_RX_DRAIN_SECONDS = 0.25
EXPECTED_C151_BOARD = "28"
RESET_REASON_KINDS = {
    0: "UNKNOWN",
    1: "POWERON",
    2: "EXT",
    3: "SW",
    4: "PANIC",
    5: "INT_WDT",
    6: "TASK_WDT",
    7: "WDT",
    8: "DEEPSLEEP",
    9: "BROWNOUT",
    10: "SDIO",
}
CONTROLLED_REBOOT_REASON = 3

SAFE_BASE_COMMANDS = frozenset({"status", "diag"})
SAFE_OPTIONAL_COMMANDS = {
    "audio": "sound-test",
    "rgb": "led-test",
    "display": "screen-test",
}
EXPECTED_TEST_ACKS = {
    "sound-test": "SOUND_OK",
    "led-test": "LED_OK",
    "screen-test": "SCREEN_OK",
}

HUMAN_CHECKS = {
    "screen": "Screen shows the expected C151 content without visible corruption",
    "speaker": "Speaker tone/prompt is audible and undistorted",
    "microphone": "Microphone captures intelligible push-to-talk audio",
    "led_left": "The physical left RGB LED represents the configured voice role",
    "led_right": "The physical right RGB LED represents the configured image role",
    "led_colors": "Listening/thinking/speaking and image-state colors are correct",
    "led_animation": "Required RGB animation/blink patterns are visibly correct",
    "buttons": "Top/previous/next buttons produce the intended actions",
    "full_refresh": "A full 400x600 six-color refresh completes without artifacts",
    "deep_sleep_current": "Deep-sleep current was measured with external equipment",
    "wake": "Every documented button/RTC wake source wakes and reconnects correctly",
}

EVENT_RE = re.compile(r"^INKLOOP_([A-Z][A-Z0-9_]{0,63})(?::(.*))?$")
HARDWARE_ID_RE = re.compile(r"^M5PC-[0-9A-F]{12}$")
KNOWN_BOOT_NOISE_RE = re.compile(
    r"^(?:ESP-ROM:|Build:|rst:|Saved PC:|SPIWP:|mode:|load:|entry |waiting for download)",
    re.IGNORECASE,
)
PAIRING_KEY_RE = re.compile(
    r"^(?:(?:pairing|activation|onboarding)[_-]?code|device[_-]?code)$", re.IGNORECASE
)
SENSITIVE_LABEL_FRAGMENT_RE = re.compile(
    r"(?:authorization|bearer|token|secret|password|passwd|apikey|credential|cookie|"
    r"sessionid|portalaccess|portalnonce)",
    re.IGNORECASE,
)
SENSITIVE_LOG_LABEL = (
    r"[A-Za-z0-9_.-]*(?:authorization|bearer|token|secret|password|passwd|"
    r"api[_-]?key|credential|cookie|session[._-]?id|portal[_-]?(?:access|nonce))"
    r"[A-Za-z0-9_.-]*"
)
BEARER_RE = re.compile(r"(?i)(\bbearer\s+)[A-Za-z0-9._~+/=-]{6,}")
AUTHORIZATION_RE = re.compile(
    r"(?i)\bauthorization\b(?:\s*[:=]\s*|\s+)[^\r\n]+"
)
JWT_RE = re.compile(r"\beyJ[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{4,}\b")
LONG_HEX_RE = re.compile(r"\b[0-9a-fA-F]{48,}\b")
SENSITIVE_ASSIGNMENT_RE = re.compile(
    rf"(?i)\b({SENSITIVE_LOG_LABEL})(\s*[:=]\s*)(?:bearer\s+)?([^\s,}}\]]+)"
)
SENSITIVE_WHITESPACE_RE = re.compile(
    rf"(?i)\b({SENSITIVE_LOG_LABEL})(\s+)(?![:=])([^\s,}}\]]+)"
)
MULTIWORD_LABEL = (
    r"(?:api[\s._/-]+key|access[\s._/-]+token|refresh[\s._/-]+token|"
    r"device[\s._/-]+token|gateway[\s._/-]+token|pairing[\s._/-]+token|"
    r"portal[\s._/-]+(?:access|nonce)|session[\s._/-]+id)"
)
MULTIWORD_ASSIGNMENT_RE = re.compile(
    rf"(?i)\b({MULTIWORD_LABEL})(\s*(?:[:=]\s*|\s+))([^\s,}}\]]+)"
)
BLOCKED_MYAI_STATE_RE = re.compile(
    r"(?:payment[_ -]?required|payment[_ -]?needed|insufficient[_ -]?(?:credit|balance)|"
    r"account[_ -]?inactive|device[_ -]?inactive|(?<![a-z])inactive(?![a-z])|"
    r"not[_ -]?active|activation[_ -]?required)",
    re.IGNORECASE,
)
ACTIVE_MYAI_STATE_RE = re.compile(
    r"^(?:active|activated|authorized|claimed|bound|ready)$", re.IGNORECASE
)


class BenchInputError(ValueError):
    """Raised for unsafe or ambiguous CLI input."""


class JsonInputRejected(ValueError):
    """Raised when a diagnostic exceeds the strict JSON safety envelope."""


@dataclass(frozen=True)
class Check:
    name: str
    status: str
    evidence: str
    required: bool = True

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "status": self.status,
            "required": self.required,
            "evidence": self.evidence,
        }


@dataclass
class BenchAnalyzer:
    # `sent_commands` means writes known to have completed successfully.  Test
    # transcripts pass them at construction, which places them before line 1.
    sent_commands: tuple[str, ...] = ()
    observations: Mapping[str, str] = field(default_factory=dict)
    planned_commands: tuple[str, ...] = ()
    events: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    protocol_failures: list[str] = field(default_factory=list)
    secret_leak_count: int = 0
    structured_lines: int = 0
    unstructured_lines: int = 0
    timed_out: bool = False
    boot_versions: list[str] = field(default_factory=list)
    firmware_versions: list[str] = field(default_factory=list)
    hardware_ids: list[str] = field(default_factory=list)
    standalone_hardware_ids: list[str] = field(default_factory=list)
    wifi_states: list[str] = field(default_factory=list)
    pairing_codes: list[tuple[str, str]] = field(default_factory=list)
    pairing_expected: bool = False
    paired_seen: bool = False
    reset_reasons: list[dict[str, Any]] = field(default_factory=list)
    hardware_ready_seen: bool = False
    board_seen: bool = False
    pm1_ready_seen: bool = False
    papercolor_warning_seen: bool = False
    firmware_errors: list[str] = field(default_factory=list)
    firmware_warnings: list[str] = field(default_factory=list)
    myai_blocked_states: set[str] = field(default_factory=set)
    myai_active_seen: bool = False
    input_sequence: int = 0
    rx_epoch: int = 1
    rx_epoch_started_ms: int = 0
    command_events: list[tuple[str, int, int, int]] = field(default_factory=list)
    command_echo_events: list[tuple[str, int, int]] = field(default_factory=list)
    diagnostic_responses: list[tuple[str, int, int, str, str]] = field(default_factory=list)
    boot_events: list[tuple[int, str]] = field(default_factory=list)
    reset_events: list[tuple[int, int]] = field(default_factory=list)
    board_events: list[int] = field(default_factory=list)
    pm1_ready_events: list[int] = field(default_factory=list)
    hardware_ready_events: list[int] = field(default_factory=list)
    reboot_request_events: list[int] = field(default_factory=list)
    test_ack_events: list[tuple[str, int, int]] = field(default_factory=list)
    state_event_counts: dict[str, int] = field(
        default_factory=lambda: {
            "button": 0,
            "voice": 0,
            "image": 0,
            "sleep": 0,
            "wake": 0,
        }
    )

    def __post_init__(self) -> None:
        self.sent_commands = tuple(self.sent_commands)
        self.planned_commands = tuple(self.planned_commands or self.sent_commands)
        self.command_events.extend(
            (command, 0, self.rx_epoch, 0) for command in self.sent_commands
        )

    def begin_rx_epoch(self, elapsed_ms: int = 0) -> None:
        self.rx_epoch += 1
        self.rx_epoch_started_ms = max(0, int(elapsed_ms))

    def record_command_sent(self, command: str, elapsed_ms: int = 0) -> None:
        allowed = SAFE_BASE_COMMANDS | frozenset(SAFE_OPTIONAL_COMMANDS.values())
        if command not in allowed:
            raise BenchInputError("unsafe serial command rejected")
        if self.rx_epoch <= 0:
            raise BenchInputError("serial RX epoch must be established before command write")
        self.sent_commands = self.sent_commands + (command,)
        self.command_events.append(
            (command, self.input_sequence, self.rx_epoch, max(0, int(elapsed_ms)))
        )

    def command_completed(self, command: str) -> bool:
        matching = [event for event in self.command_events if event[0] == command]
        if not matching:
            return False
        _, command_sequence, command_epoch, _ = matching[-1]
        echoes = [
            echo_sequence
            for echoed_command, echo_sequence, echo_epoch in self.command_echo_events
            if echoed_command == command
            and echo_sequence > command_sequence
            and echo_epoch == command_epoch
        ]
        if not echoes:
            return False
        echo_sequence = min(echoes)
        if command in SAFE_BASE_COMMANDS:
            return any(
                response_sequence > echo_sequence and response_epoch == command_epoch
                for _, response_sequence, response_epoch, _, _ in self.diagnostic_responses
            )
        expected = EXPECTED_TEST_ACKS.get(command)
        return bool(expected) and any(
            value == expected
            and sequence > echo_sequence
            and ack_epoch == command_epoch
            for value, sequence, ack_epoch in self.test_ack_events
        )

    def _protocol_failure(self, code: str) -> None:
        if code not in self.protocol_failures:
            self.protocol_failures.append(code)

    def consume_line(self, raw: str | bytes, elapsed_ms: int = 0) -> None:
        try:
            if isinstance(raw, str):
                encoded = raw.encode("utf-8")
            else:
                encoded = raw
        except (UnicodeEncodeError, MemoryError):
            self._protocol_failure("invalid_utf8_serial_line")
            return
        if len(encoded) > MAX_LINE_BYTES:
            self._protocol_failure("oversized_serial_line")
            return
        try:
            line = encoded.decode("utf-8", errors="strict").rstrip("\r")
        except (UnicodeDecodeError, MemoryError):
            self._protocol_failure("invalid_utf8_serial_line")
            return
        if not line:
            return
        self.input_sequence += 1
        if any(ord(char) < 32 and char != "\t" for char in line):
            self._protocol_failure("control_character_in_serial_line")
            return

        if contains_secret(line):
            self.secret_leak_count += 1

        event_match = EVENT_RE.fullmatch(line)
        if event_match:
            name = event_match.group(1)
            value = event_match.group(2) or ""
            payload: Any = value
            if name in {"STATUS", "DIAG", "DIAGNOSTIC"} or value.lstrip().startswith(("{", "[")):
                try:
                    payload = strict_json_loads(value)
                except JsonInputRejected as error:
                    self._protocol_failure(error.args[0] if error.args else "unsafe_json_diagnostic")
                    self._record_event(elapsed_ms, "protocol", name, "[MALFORMED JSON]")
                    return
                if not isinstance(payload, dict):
                    self._protocol_failure("json_diagnostic_not_object")
                    return
            self.structured_lines += 1
            self._consume_event(name, payload, elapsed_ms)
            return

        if line.lstrip().startswith(("{", "[")):
            try:
                payload = strict_json_loads(line)
            except JsonInputRejected as error:
                self._protocol_failure(error.args[0] if error.args else "unsafe_json_diagnostic")
                return
            if not isinstance(payload, dict):
                self._protocol_failure("json_diagnostic_not_object")
                return
            self.structured_lines += 1
            name = str(payload.get("event") or payload.get("name") or "DIAGNOSTIC").upper()
            if not re.fullmatch(r"[A-Z][A-Z0-9_]{0,63}", name):
                self._protocol_failure("invalid_json_event_name")
                return
            self._consume_event(name, payload, elapsed_ms)
            return

        if line.startswith("INKLOOP_"):
            self._protocol_failure("malformed_inkloop_event")
            return

        self.unstructured_lines += 1
        category = "boot_noise" if KNOWN_BOOT_NOISE_RE.match(line) else "unstructured"
        self._record_event(elapsed_ms, category, "SERIAL_TEXT", redact_text(line))

    def finish(self, *, timed_out: bool = False, partial: bytes = b"") -> None:
        self.timed_out = timed_out
        if partial:
            self._protocol_failure("truncated_serial_line")

    def _record_event(self, elapsed_ms: int, category: str, name: str, value: Any) -> None:
        if len(self.events) >= MAX_EVENT_RECORDS:
            self._protocol_failure("event_record_capacity_exceeded")
            return
        sanitized = sanitize_value(value, name)
        sanitized_name = sanitize_label(name, "[REDACTED_SENSITIVE_EVENT]")
        self.events.append(
            {
                "elapsedMs": max(0, int(elapsed_ms)),
                "sequence": self.input_sequence,
                "rxEpoch": self.rx_epoch,
                "category": category,
                "name": sanitized_name,
                "value": sanitized,
            }
        )
        if category in self.state_event_counts:
            self.state_event_counts[category] += 1

    def _consume_event(self, name: str, payload: Any, elapsed_ms: int) -> None:
        category = event_category(name, payload)
        self._record_event(elapsed_ms, category, name, payload)
        flat = flatten_mapping(payload) if isinstance(payload, dict) else {}
        text_value = payload if isinstance(payload, str) else json.dumps(
            payload, sort_keys=True, allow_nan=False
        )

        if is_sensitive_label(name) and not PAIRING_KEY_RE.fullmatch(name):
            if text_value and text_value not in {"UNAVAILABLE", "NONE", "[REDACTED]"}:
                self.secret_leak_count += 1

        if name == "BOOT":
            if is_bounded_nonempty_text(payload, MAX_FIRMWARE_ID_LENGTH):
                version = payload.strip()
                self.boot_versions.append(version)
                self.firmware_versions.append(version)
                self.boot_events.append((self.input_sequence, version))
            else:
                self._protocol_failure("invalid_boot_identity")
        if name == "RESET_REASON":
            reason_text = payload.strip() if isinstance(payload, str) else ""
            if not re.fullmatch(r"[0-9]+", reason_text):
                self._protocol_failure("invalid_reset_reason")
            else:
                reason = int(reason_text)
                kind = RESET_REASON_KINDS.get(reason)
                if kind is None:
                    self._protocol_failure("invalid_reset_reason")
                else:
                    observation = {
                        "raw": reason,
                        "kind": kind,
                        "known": reason != 0,
                        "sequence": self.input_sequence,
                    }
                    self.reset_reasons.append(observation)
                    self.reset_events.append((self.input_sequence, reason))
                    if reason == 0 and "reset_reason_unknown" not in self.warnings:
                        self.warnings.append("reset_reason_unknown")
        if name == "STATE":
            state = str(payload).strip().upper()
            if state == "WAITING_BIND":
                self.pairing_expected = True
            if state == "PAIRED":
                self.paired_seen = True
            if state in {"REBOOTING", "NVS_RECOVERED_REBOOTING"}:
                self.reboot_request_events.append(self.input_sequence)
        if name == "HARDWARE_READY":
            if isinstance(payload, str) and payload.strip().upper() == "READY":
                self.hardware_ready_seen = True
                self.hardware_ready_events.append(self.input_sequence)
            else:
                self._protocol_failure("invalid_hardware_ready")
        if name == "BOARD":
            board = payload.strip() if isinstance(payload, str) else ""
            if board == EXPECTED_C151_BOARD:
                self.board_seen = True
                self.board_events.append(self.input_sequence)
            else:
                self._protocol_failure("unexpected_c151_board")
        if name == "PM1":
            if isinstance(payload, str) and payload.strip().upper() == "READY":
                self.pm1_ready_seen = True
                self.pm1_ready_events.append(self.input_sequence)
            else:
                self._protocol_failure("invalid_pm1_state")
        if name == "WARN":
            warning = str(payload).strip().upper()
            self.firmware_warnings.append(warning[:96])
            if "PAPERCOLOR_NOT_DETECTED" in warning:
                self.papercolor_warning_seen = True
        if name in {"ERROR", "FATAL"}:
            self.firmware_errors.append(str(payload).strip().upper()[:96])

        if name == "HARDWARE_ID" and isinstance(payload, str):
            hardware_id = payload.strip().upper()
            self.hardware_ids.append(hardware_id)
            self.standalone_hardware_ids.append(hardware_id)
        if name == "WIFI_AP" and isinstance(payload, str) and payload.strip():
            self.wifi_states.append("AP")
        if name == "WIFI_CONNECTED" and isinstance(payload, str) and payload.strip():
            self.wifi_states.append("CONNECTED")
        if name == "TEST" and isinstance(payload, str):
            self.test_ack_events.append(
                (payload.strip().upper(), self.input_sequence, self.rx_epoch)
            )
        if name == "COMMAND" and isinstance(payload, str):
            command = payload.strip().lower()
            if command in SAFE_BASE_COMMANDS | frozenset(SAFE_OPTIONAL_COMMANDS.values()):
                self.command_echo_events.append((command, self.input_sequence, self.rx_epoch))

        if name in {"STATUS", "DIAG", "DIAGNOSTIC"} and isinstance(payload, dict):
            identity = valid_diagnostic_identity(payload)
            if identity is not None:
                firmware, hardware_id = identity
                self.diagnostic_responses.append(
                    (name, self.input_sequence, self.rx_epoch, firmware, hardware_id)
                )

        if isinstance(payload, dict):
            self.secret_leak_count += count_sensitive_json_labels(payload)

        self._extract_mapping_fields(flat, name)
        self._extract_pairing_from_event(name, payload)
        self._extract_myai_state(name, payload, flat)

    def _extract_mapping_fields(self, flat: Mapping[str, Any], source: str) -> None:
        normalized_fields = {
            re.sub(r"[^a-z0-9]", "", raw_key.split(".")[-1].lower()): value
            for raw_key, value in flat.items()
        }
        myai_pairing_payload = (
            str(normalized_fields.get("appid", "")).lower() == "inkloop"
            and any(key in normalized_fields for key in ("pairingtoken", "bindingurl", "pairingstatus"))
        )
        for raw_key, value in flat.items():
            key = raw_key.split(".")[-1]
            normalized = re.sub(r"[^a-z0-9]", "", key.lower())
            if normalized in {"firmware", "firmwareversion", "buildversion"}:
                if isinstance(value, str) and value.strip():
                    self.firmware_versions.append(value.strip())
            elif normalized == "hardwareid":
                if isinstance(value, str):
                    self.hardware_ids.append(value.strip().upper())
            elif normalized == "wifi":
                if value is True:
                    self.wifi_states.append("CONNECTED")
                elif isinstance(value, str):
                    state = value.strip().upper()
                    if state in {"AP", "ACCESS_POINT", "CONNECTED"}:
                        self.wifi_states.append("AP" if state != "CONNECTED" else "CONNECTED")
            elif normalized == "paired" and value is True:
                self.paired_seen = True
            elif normalized == "pm1" and value is True:
                self.pm1_ready_seen = True
            elif PAIRING_KEY_RE.fullmatch(key):
                self._register_pairing_value(source + "." + key, value)
            elif normalized == "deviceid" and (
                myai_pairing_payload
                or any(marker in source.upper() for marker in ("MYAI", "PAIR", "ACTIVATION"))
            ):
                self._register_pairing_value(source + "." + key, value)
    def _extract_pairing_from_event(self, name: str, payload: Any) -> None:
        normalized = name.replace("_", "")
        if "PAIR" in name and "CODE" in name and isinstance(payload, str):
            self._register_pairing_value(name, payload)
        elif normalized in {"ACTIVATIONCODE", "ONBOARDINGCODE", "DEVICECODE"}:
            self._register_pairing_value(name, payload)

    def _register_pairing_value(self, source: str, value: Any) -> None:
        if value in {None, "", "UNAVAILABLE", "NONE", "------"}:
            return
        code = str(value).strip()
        if not re.fullmatch(r"[0-9]{6}", code):
            self._protocol_failure("malformed_pairing_code")
            return
        self.pairing_codes.append((source, code))

    def _extract_myai_state(
        self, name: str, payload: Any, flat: Mapping[str, Any]
    ) -> None:
        candidates: list[str] = []
        combined = name + ":" + str(payload)
        blocked = BLOCKED_MYAI_STATE_RE.search(combined)
        if blocked:
            self.myai_blocked_states.add(blocked.group(0).lower().replace(" ", "_"))
        flat_app_id = next(
            (
                str(value).lower()
                for key, value in flat.items()
                if re.sub(r"[^a-z0-9]", "", key.split(".")[-1].lower()) == "appid"
            ),
            "",
        )
        if (
            "MYAI" in name
            or "ACTIVATION" in name
            or "PAYMENT" in name
            or flat_app_id == "inkloop"
        ):
            candidates.append(str(payload))
            candidates.extend(str(value) for value in flat.values())
        for key, value in flat.items():
            lowered = key.lower()
            if any(part in lowered for part in ("myai", "activation", "payment", "account")):
                candidates.append(str(value))
        for candidate in candidates:
            match = BLOCKED_MYAI_STATE_RE.search(candidate)
            if match:
                self.myai_blocked_states.add(match.group(0).lower().replace(" ", "_"))
            if ACTIVE_MYAI_STATE_RE.fullmatch(candidate.strip()):
                self.myai_active_seen = True

    def build_report(
        self,
        *,
        port: str = "fake-transcript",
        baud: int = DEFAULT_BAUD,
        timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    ) -> dict[str, Any]:
        checks = self._machine_checks()
        machine_failed = any(check.required and check.status == "FAIL" for check in checks)
        human = []
        human_failure = False
        observed_count = 0
        for key, description in HUMAN_CHECKS.items():
            supplied = self.observations.get(key, "not-run")
            status = {
                "pass": "HUMAN_REPORTED_OK",
                "fail": "HUMAN_REPORTED_FAIL",
                "skip": "NOT_RUN",
                "not-run": "NOT_RUN",
            }.get(supplied, "NOT_RUN")
            if status.startswith("HUMAN_REPORTED"):
                observed_count += 1
            if status == "HUMAN_REPORTED_FAIL":
                human_failure = True
            human.append(
                {
                    "name": key,
                    "status": status,
                    "description": description,
                    "source": "operator assertion; not machine verified",
                }
            )

        machine_verdict = "FAIL" if machine_failed else "PASS"
        if human_failure:
            human_verdict = "HUMAN_REPORTED_FAIL"
        elif observed_count:
            human_verdict = "HUMAN_REPORTED_NO_FAILURE"
        else:
            human_verdict = "NOT_OBSERVED"
        overall_verdict = (
            "FAIL"
            if machine_failed or human_failure
            else "MACHINE_PASS_PHYSICAL_UNVERIFIED"
        )
        if observed_count:
            physical = "HUMAN_OBSERVATIONS_RECORDED_NOT_VERIFIED"
        else:
            physical = "NOT_TESTED_NOT_VERIFIED"
        if machine_failed and human_failure:
            summary = (
                "Machine-verifiable serial checks failed, and an operator also reported a physical failure; "
                "physical behavior is not certified by this harness."
            )
        elif machine_failed:
            summary = "Machine-verifiable serial checks failed."
        elif human_failure:
            summary = (
                "Machine-verifiable serial checks passed, but an operator reported a physical failure; "
                "physical behavior is not certified by this harness."
            )
        else:
            summary = (
                "Machine-verifiable serial checks passed; physical behavior is not certified by this harness."
            )
        distinct_pairing_codes = {code for _, code in self.pairing_codes}
        return {
            "schema": SCHEMA_VERSION,
            "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "scope": {
                "device": "M5 PaperColor C151",
                "port": redact_text(port),
                "baud": baud,
                "observationTimeoutSeconds": timeout_seconds,
                "commandsPlanned": list(self.planned_commands),
                "commandsSent": list(self.sent_commands),
                "rxEpochsEstablished": self.rx_epoch,
                "commandWrites": [
                    {
                        "command": command,
                        "sequence": sequence,
                        "rxEpoch": epoch,
                        "elapsedMs": elapsed_ms,
                    }
                    for command, sequence, epoch, elapsed_ms in self.command_events
                ],
                "realHardwareClaim": False,
            },
            "verdict": {
                "machine": machine_verdict,
                "human": human_verdict,
                "overall": overall_verdict,
                "physical": physical,
                "summary": summary,
            },
            "machineChecks": [check.as_dict() for check in checks],
            "humanObservedChecks": human,
            "pairing": {
                "emissions": len(self.pairing_codes),
                "distinctCodes": len(distinct_pairing_codes),
                "singleSixDigitCodeSemantics": (
                    len(distinct_pairing_codes) == 1
                    and not any(item == "malformed_pairing_code" for item in self.protocol_failures)
                ) if self.pairing_codes else None,
                "code": mask_pairing_code(next(iter(distinct_pairing_codes)))
                if len(distinct_pairing_codes) == 1 else None,
            },
            "resetReasons": list(self.reset_reasons),
            "stateEventCounts": dict(self.state_event_counts),
            "events": list(self.events),
            "warnings": list(self.warnings),
            "inputStats": {
                "structuredLines": self.structured_lines,
                "unstructuredLines": self.unstructured_lines,
                "timedOutAtWindowEnd": self.timed_out,
            },
        }

    def _machine_checks(self) -> list[Check]:
        checks: list[Check] = []
        protocol_ok = not self.protocol_failures
        checks.append(Check(
            "serial_protocol_integrity",
            "PASS" if protocol_ok else "FAIL",
            "all lines bounded, complete, UTF-8, and structurally valid"
            if protocol_ok else "rejected input: " + ", ".join(self.protocol_failures),
        ))
        checks.append(Check(
            "credential_hygiene",
            "PASS" if self.secret_leak_count == 0 else "FAIL",
            "no credential/token patterns observed"
            if self.secret_leak_count == 0 else f"{self.secret_leak_count} redacted credential-bearing line(s) observed",
        ))
        serial_ready = self.structured_lines > 0
        checks.append(Check(
            "serial_readiness",
            "PASS" if serial_ready else "FAIL",
            f"{self.structured_lines} structured event(s) received"
            if serial_ready else "no structured serial event before timeout",
        ))
        accepted_reset_sequences: list[tuple[int, int, int, int]] = []
        for reset_sequence, reset_reason in self.reset_events:
            later_boots = [sequence for sequence, _ in self.boot_events if sequence > reset_sequence]
            if not later_boots:
                continue
            boot_sequence = min(later_boots)
            later_board = [sequence for sequence in self.board_events if sequence > boot_sequence]
            if not later_board:
                continue
            board_sequence = min(later_board)
            later_pm1 = [sequence for sequence in self.pm1_ready_events if sequence > board_sequence]
            if not later_pm1:
                continue
            pm1_sequence = min(later_pm1)
            later_ready = [
                sequence for sequence in self.hardware_ready_events if sequence > pm1_sequence
            ]
            if later_ready:
                accepted_reset_sequences.append(
                    (reset_sequence, boot_sequence, min(later_ready), reset_reason)
                )

        checks.append(Check(
            "post_flash_boot",
            "PASS" if accepted_reset_sequences else "FAIL",
            "exact non-empty reset -> boot -> C151 board -> PM1 -> hardware-ready chain observed"
            if accepted_reset_sequences else "exact reset -> boot -> C151 board -> PM1 -> hardware-ready chain missing",
        ))
        firmware_values = {value for value in self.firmware_versions if value}
        firmware_ok = bool(firmware_values) and len(firmware_values) == 1
        checks.append(Check(
            "firmware_identity",
            "PASS" if firmware_ok else "FAIL",
            "one consistent non-empty firmware build identity observed"
            if firmware_ok else "missing or inconsistent firmware build identity",
        ))
        distinct_hardware_ids = {value for value in self.hardware_ids if value}
        hardware_ok = (
            len(distinct_hardware_ids) == 1
            and HARDWARE_ID_RE.fullmatch(next(iter(distinct_hardware_ids))) is not None
        )
        checks.append(Check(
            "hardware_identity",
            "PASS" if hardware_ok else "FAIL",
            "one valid M5PC-XXXXXXXXXXXX hardware ID observed"
            if hardware_ok else "missing, malformed, or inconsistent hardware ID",
        ))
        board_ok = (
            self.hardware_ready_seen
            and self.board_seen
            and self.pm1_ready_seen
            and bool(accepted_reset_sequences)
            and not self.papercolor_warning_seen
        )
        checks.append(Check(
            "c151_hardware_initialization",
            "PASS" if board_ok else "FAIL",
            "expected C151 board, PM1 READY, and hardware READY occurred in exact startup order"
            if board_ok else "C151 board/PM1/hardware-ready evidence incomplete or mismatch reported",
        ))
        reset_ok = bool(accepted_reset_sequences)
        checks.append(Check(
            "reset_and_runtime_serial_sequence",
            "PASS" if reset_ok else "FAIL",
            "supported reset reason followed by exact non-empty startup chain"
            if reset_ok else "reset reason and application boot sequence incomplete",
        ))
        if self.reboot_request_events:
            reboot_ok = all(
                any(
                    reset_sequence > request_sequence
                    and boot_sequence > reset_sequence
                    and ready_sequence > boot_sequence
                    and reset_reason == CONTROLLED_REBOOT_REASON
                    for (
                        reset_sequence, boot_sequence, ready_sequence, reset_reason
                    ) in accepted_reset_sequences
                )
                for request_sequence in self.reboot_request_events
            )
            reboot_status = "PASS" if reboot_ok else "FAIL"
            reboot_evidence = (
                "every reboot request is followed by software reset 3 -> boot -> runtime-ready"
                if reboot_ok else "a reboot request lacks a later ordered software reset 3 -> boot -> runtime-ready sequence"
            )
        else:
            reboot_status = "NOT_OBSERVED"
            reboot_evidence = "no reboot request state was emitted"
        checks.append(Check(
            "reboot_completion_sequence",
            reboot_status,
            reboot_evidence,
            required=bool(self.reboot_request_events),
        ))
        wifi_values = set(self.wifi_states)
        wifi_ok = bool(wifi_values) and wifi_values.issubset({"AP", "CONNECTED"})
        checks.append(Check(
            "wifi_ap_or_connected_state",
            "PASS" if wifi_ok else "FAIL",
            "observed valid Wi-Fi state(s): " + ", ".join(sorted(wifi_values))
            if wifi_ok else "neither Wi-Fi AP nor connected state was observed",
        ))

        distinct_codes = {code for _, code in self.pairing_codes}
        pairing_malformed = "malformed_pairing_code" in self.protocol_failures
        if pairing_malformed or len(distinct_codes) > 1:
            pairing_status = "FAIL"
            pairing_evidence = "pairing emissions were malformed or contained more than one distinct code"
        elif len(distinct_codes) == 1:
            pairing_status = "PASS"
            pairing_evidence = "all MyAI/Inkloop emissions reuse one exact six-digit code"
        elif self.pairing_expected and not self.paired_seen:
            pairing_status = "FAIL"
            pairing_evidence = "binding state was emitted without one valid six-digit code"
        else:
            pairing_status = "NOT_OBSERVED"
            pairing_evidence = "no pairing code was required/emitted during this window"
        checks.append(Check(
            "single_pairing_code_semantics",
            pairing_status,
            pairing_evidence,
            required=pairing_status != "NOT_OBSERVED",
        ))

        if self.myai_blocked_states:
            myai_status = "FAIL"
            myai_evidence = "blocked MyAI activation/payment state observed: " + ", ".join(
                sorted(self.myai_blocked_states)
            )
        elif self.myai_active_seen:
            myai_status = "PASS"
            myai_evidence = "active/authorized MyAI state observed"
        else:
            myai_status = "NOT_OBSERVED"
            myai_evidence = "MyAI activation state was not emitted during this window"
        checks.append(Check(
            "myai_activation_payment_state",
            myai_status,
            myai_evidence,
            required=myai_status != "NOT_OBSERVED",
        ))

        checks.append(Check(
            "firmware_runtime_errors",
            "PASS" if not self.firmware_errors else "FAIL",
            "no INKLOOP_ERROR/FATAL event observed"
            if not self.firmware_errors else f"{len(self.firmware_errors)} firmware error/fatal event(s) observed",
        ))
        remaining_sent = list(self.sent_commands)
        missing_planned: list[str] = []
        for command in self.planned_commands:
            if command in remaining_sent:
                remaining_sent.remove(command)
            else:
                missing_planned.append(command)
        checks.append(Check(
            "planned_commands_executed",
            "PASS" if not missing_planned else "FAIL",
            "every planned allowlisted command was successfully written"
            if not missing_planned else "one or more planned commands were not successfully written",
        ))

        diagnostic_command_events = [
            (command, sequence, epoch)
            for command, sequence, epoch, _ in self.command_events
            if command in SAFE_BASE_COMMANDS
        ]
        boot_identity_values = {version for _, version in self.boot_events}
        standalone_hardware_values = set(self.standalone_hardware_ids)
        diagnostic_response_ok = bool(diagnostic_command_events) and all(
            any(
                echoed_command == command
                and echo_sequence > command_sequence
                and echo_epoch == command_epoch
                and any(
                    response_sequence > echo_sequence
                    and response_epoch == command_epoch
                    and response_firmware in boot_identity_values
                    and response_hardware_id in standalone_hardware_values
                    for (
                        _, response_sequence, response_epoch,
                        response_firmware, response_hardware_id,
                    ) in self.diagnostic_responses
                )
                for echoed_command, echo_sequence, echo_epoch in self.command_echo_events
            )
            for command, command_sequence, command_epoch in diagnostic_command_events
        )
        checks.append(Check(
            "diagnostic_command_response",
            "PASS" if diagnostic_response_ok else "FAIL",
            "each sent status/diag has a later matching device echo and valid diagnostic object"
            if diagnostic_response_ok else "status/diag was not sent or lacks a later matching echo and valid response",
        ))

        for command, command_sequence, command_epoch, _ in self.command_events:
            expected = EXPECTED_TEST_ACKS.get(command)
            if not expected:
                continue
            acknowledged = any(
                value == expected
                and sequence > command_sequence
                and ack_epoch == command_epoch
                for value, sequence, ack_epoch in self.test_ack_events
            )
            checks.append(Check(
                f"{command}_acknowledgement",
                "PASS" if acknowledged else "FAIL",
                f"INKLOOP_TEST:{expected} observed"
                if acknowledged else f"INKLOOP_TEST:{expected} not observed after command; physical outcome still requires a human",
            ))
        return checks


def flatten_mapping(value: Mapping[str, Any], prefix: str = "") -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, item in value.items():
        path = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(item, Mapping):
            output.update(flatten_mapping(item, path))
        elif not isinstance(item, (list, tuple)):
            output[path] = item
    return output


def is_bounded_nonempty_text(value: Any, maximum: int) -> bool:
    return (
        isinstance(value, str)
        and 0 < len(value.strip()) <= maximum
        and all(ord(character) >= 32 and ord(character) != 127 for character in value)
    )


def valid_diagnostic_identity(payload: Mapping[str, Any]) -> tuple[str, str] | None:
    direct = {normalized_label(str(key)): value for key, value in payload.items()}
    firmware = direct.get("firmware")
    hardware_id = direct.get("hardwareid")
    if not is_bounded_nonempty_text(firmware, MAX_FIRMWARE_ID_LENGTH):
        return None
    if not isinstance(hardware_id, str):
        return None
    normalized_hardware_id = hardware_id.strip().upper()
    if HARDWARE_ID_RE.fullmatch(normalized_hardware_id) is None:
        return None
    return firmware.strip(), normalized_hardware_id


def normalized_label(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.lower())


def is_sensitive_label(value: str) -> bool:
    return SENSITIVE_LABEL_FRAGMENT_RE.search(normalized_label(value)) is not None


def sanitize_label(value: str, replacement: str = "[REDACTED_SENSITIVE_KEY]") -> str:
    if is_sensitive_label(value) and not PAIRING_KEY_RE.fullmatch(value):
        return replacement
    return redact_text(value)


def count_sensitive_json_labels(value: Any) -> int:
    count = 0
    stack = [value]
    while stack:
        current = stack.pop()
        if isinstance(current, Mapping):
            for key, item in current.items():
                key_text = str(key)
                if is_sensitive_label(key_text) and not PAIRING_KEY_RE.fullmatch(key_text):
                    count += 1
                stack.append(item)
        elif isinstance(current, list):
            stack.extend(current)
    return count


def contains_secret(text: str) -> bool:
    if (
        AUTHORIZATION_RE.search(text)
        or BEARER_RE.search(text)
        or JWT_RE.search(text)
        or LONG_HEX_RE.search(text)
        or SENSITIVE_WHITESPACE_RE.search(text)
        or MULTIWORD_ASSIGNMENT_RE.search(text)
    ):
        return True
    return SENSITIVE_ASSIGNMENT_RE.search(text) is not None


def mask_pairing_code(value: str) -> str:
    return value[:2] + "****" if re.fullmatch(r"[0-9]{6}", value) else "[INVALID]"


def redact_text(text: str) -> str:
    redacted = AUTHORIZATION_RE.sub("Authorization: [REDACTED]", text)
    redacted = BEARER_RE.sub(r"\1[REDACTED]", redacted)
    redacted = JWT_RE.sub("[REDACTED]", redacted)
    redacted = LONG_HEX_RE.sub("[REDACTED]", redacted)
    redacted = SENSITIVE_ASSIGNMENT_RE.sub(r"\1\2[REDACTED]", redacted)
    redacted = SENSITIVE_WHITESPACE_RE.sub(r"\1\2[REDACTED]", redacted)
    redacted = MULTIWORD_ASSIGNMENT_RE.sub(r"\1\2[REDACTED]", redacted)
    redacted = re.sub(r"\b[0-9]{6}\b", lambda match: mask_pairing_code(match.group(0)), redacted)
    return redacted[:MAX_LINE_BYTES]


def sanitize_value(value: Any, key: str = "") -> Any:
    if is_sensitive_label(key) and not PAIRING_KEY_RE.fullmatch(key):
        return "[REDACTED]" if value not in (None, "") else value
    if PAIRING_KEY_RE.fullmatch(key) and isinstance(value, (str, int)):
        return mask_pairing_code(str(value))
    if isinstance(value, Mapping):
        output: dict[str, Any] = {}
        sensitive_index = 0
        for item_key, item in value.items():
            original_key = str(item_key)
            safe_key = sanitize_label(original_key)
            if safe_key == "[REDACTED_SENSITIVE_KEY]":
                sensitive_index += 1
                safe_key = f"[REDACTED_SENSITIVE_KEY_{sensitive_index}]"
            output[safe_key] = sanitize_value(item, original_key)
        return output
    if isinstance(value, list):
        return [sanitize_value(item) for item in value[:64]]
    if isinstance(value, str):
        return redact_text(value)
    if isinstance(value, (bool, int, float)) or value is None:
        return value
    return redact_text(str(value))


def _reject_json_constant(_value: str) -> None:
    raise JsonInputRejected("non_finite_json_number")


def _enforce_json_text_depth(text: str) -> None:
    depth = 0
    in_string = False
    escaped = False
    for character in text:
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character in "[{":
            depth += 1
            if depth > MAX_JSON_DEPTH:
                raise JsonInputRejected("json_depth_limit_exceeded")
        elif character in "]}":
            depth -= 1
            if depth < 0:
                raise JsonInputRejected("malformed_json_diagnostic")


def _validate_json_value(value: Any) -> None:
    stack: list[tuple[Any, int]] = [(value, 1)]
    total_items = 0
    while stack:
        current, depth = stack.pop()
        if depth > MAX_JSON_DEPTH:
            raise JsonInputRejected("json_depth_limit_exceeded")
        if isinstance(current, dict):
            if len(current) > MAX_JSON_CONTAINER_ITEMS:
                raise JsonInputRejected("json_container_cardinality_exceeded")
            total_items += len(current)
            stack.extend((item, depth + 1) for item in current.values())
        elif isinstance(current, list):
            if len(current) > MAX_JSON_CONTAINER_ITEMS:
                raise JsonInputRejected("json_container_cardinality_exceeded")
            total_items += len(current)
            stack.extend((item, depth + 1) for item in current)
        elif isinstance(current, float) and not math.isfinite(current):
            raise JsonInputRejected("non_finite_json_number")
        elif not isinstance(current, (str, bool, int, float)) and current is not None:
            raise JsonInputRejected("unsupported_json_value")
        if total_items > MAX_JSON_TOTAL_ITEMS:
            raise JsonInputRejected("json_total_cardinality_exceeded")


def strict_json_loads(text: str) -> Any:
    try:
        _enforce_json_text_depth(text)
        value = json.loads(text, parse_constant=_reject_json_constant)
        _validate_json_value(value)
        return value
    except JsonInputRejected:
        raise
    except (RecursionError, MemoryError) as error:
        raise JsonInputRejected("json_resource_limit_exceeded") from error
    except (json.JSONDecodeError, UnicodeError, ValueError, OverflowError, TypeError) as error:
        raise JsonInputRejected("malformed_json_diagnostic") from error


def serialize_console_event(event: Mapping[str, Any]) -> str:
    return json.dumps(
        sanitize_value(event),
        ensure_ascii=False,
        sort_keys=True,
        allow_nan=False,
    )


def consume_serial_chunk(
    chunk: bytes,
    pending: bytearray,
    analyzer: BenchAnalyzer,
    elapsed_ms: int,
    *,
    emit_console: bool,
) -> bool:
    for byte in chunk:
        if byte == 10:
            before_events = len(analyzer.events)
            analyzer.consume_line(bytes(pending), elapsed_ms)
            if emit_console and len(analyzer.events) > before_events:
                print("serial: " + serialize_console_event(analyzer.events[-1]), flush=True)
            pending.clear()
            continue
        pending.append(byte)
        if len(pending) > MAX_LINE_BYTES:
            analyzer._protocol_failure("oversized_serial_line")
            pending.clear()
            return False
    return True


def drain_serial_rx_backlog(
    device: Any,
    pending: bytearray,
    analyzer: BenchAnalyzer,
    started: float,
    *,
    emit_console: bool = True,
) -> int:
    """Consume a bounded RX backlog, then discard any racing remainder.

    Consumed events remain in the pre-command epoch and therefore cannot
    acknowledge the command written after this function returns.
    """
    drained = 0
    deadline = time.monotonic() + MAX_RX_DRAIN_SECONDS
    has_waiting = hasattr(device, "in_waiting")
    reset_input = getattr(device, "reset_input_buffer", None)
    if not has_waiting and not callable(reset_input):
        raise RuntimeError("serial adapter cannot establish a bounded RX epoch")
    while has_waiting and drained < MAX_RX_DRAIN_BYTES and time.monotonic() < deadline:
        try:
            waiting = int(device.in_waiting)
        except (AttributeError, TypeError, ValueError, OSError) as error:
            raise RuntimeError("serial adapter cannot inspect RX backlog") from error
        if waiting <= 0:
            break
        amount = min(waiting, 1024, MAX_RX_DRAIN_BYTES - drained)
        chunk = device.read(amount)
        if not chunk:
            break
        drained += len(chunk)
        elapsed_ms = int((time.monotonic() - started) * 1000)
        if not consume_serial_chunk(
            chunk, pending, analyzer, elapsed_ms, emit_console=emit_console
        ):
            break
    # A partial pre-epoch line must never be joined to post-command bytes.
    pending.clear()
    if callable(reset_input):
        reset_input()
    elif int(device.in_waiting) > 0:
        raise RuntimeError("serial RX backlog exceeds bounded portable drain")
    return drained


def event_category(name: str, payload: Any) -> str:
    upper = name.upper()
    value = str(payload).upper()
    if upper == "BUTTON" or upper.startswith("BUTTON_"):
        return "button"
    if any(token in upper for token in ("VOICE", "AUDIO", "MIC", "SPEAK", "LISTEN", "THINK")):
        return "voice"
    if any(token in upper for token in ("DISPLAY", "IMAGE", "AIGC", "FRAME", "ALBUM", "PAGE")):
        return "image"
    if "SLEEP" in upper or "DEEP_SLEEP" in value:
        return "sleep"
    if any(token in upper for token in ("WAKE", "RESET", "BOOT")) or "REBOOT" in value:
        return "wake"
    return "system"


def validate_port(value: str) -> str:
    candidate = value.strip()
    if not candidate or len(candidate) > 255:
        raise BenchInputError("--port must be a non-empty explicit serial device path/name")
    if any(ord(char) < 33 or ord(char) == 127 for char in candidate):
        raise BenchInputError("--port cannot contain whitespace or control characters")
    if candidate.lower() in {"auto", "any", "first", "default", "*"} or any(
        token in candidate for token in ("*", "?", "[")
    ):
        raise BenchInputError("automatic, wildcard, or ambiguous serial-port selection is forbidden")
    return candidate


def bounded_timeout(value: str) -> float:
    try:
        timeout = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("timeout must be a number") from error
    if not MIN_TIMEOUT_SECONDS <= timeout <= MAX_TIMEOUT_SECONDS:
        raise argparse.ArgumentTypeError(
            f"timeout must be between {MIN_TIMEOUT_SECONDS:g} and {MAX_TIMEOUT_SECONDS:g} seconds"
        )
    return timeout


def valid_baud(value: str) -> int:
    try:
        baud = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("baud must be an integer") from error
    if not 1200 <= baud <= 3_000_000:
        raise argparse.ArgumentTypeError("baud must be between 1200 and 3000000")
    return baud


def parse_observations(values: Iterable[str]) -> dict[str, str]:
    observations: dict[str, str] = {}
    for item in values:
        key, separator, status = item.partition("=")
        key = key.strip().lower()
        status = status.strip().lower()
        if not separator or key not in HUMAN_CHECKS or status not in {"pass", "fail", "skip"}:
            raise BenchInputError(
                "--observe must be CHECK=pass|fail|skip; checks: " + ", ".join(HUMAN_CHECKS)
            )
        observations[key] = status
    return observations


def build_commands(diagnostic: str, *, audio: bool, rgb: bool, display: bool) -> tuple[str, ...]:
    if diagnostic not in SAFE_BASE_COMMANDS:
        raise BenchInputError("diagnostic command must be status or diag")
    commands = [diagnostic]
    for enabled, key in ((audio, "audio"), (rgb, "rgb"), (display, "display")):
        if enabled:
            commands.append(SAFE_OPTIONAL_COMMANDS[key])
    allowed = SAFE_BASE_COMMANDS | frozenset(SAFE_OPTIONAL_COMMANDS.values())
    if any(command not in allowed for command in commands):
        raise BenchInputError("unsafe serial command rejected")
    return tuple(commands)


def analyze_transcript(
    lines: Sequence[str | bytes],
    *,
    sent_commands: Sequence[str] = ("status",),
    observations: Mapping[str, str] | None = None,
    timed_out: bool = False,
    partial: bytes = b"",
) -> dict[str, Any]:
    analyzer = BenchAnalyzer(
        sent_commands=tuple(sent_commands),
        observations=observations or {},
        planned_commands=tuple(sent_commands),
    )
    for index, line in enumerate(lines):
        analyzer.consume_line(line, index * 10)
    analyzer.finish(timed_out=timed_out, partial=partial)
    return analyzer.build_report()


def write_report(path: Path, report: Mapping[str, Any]) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False
    )
    temporary = Path(handle.name)
    try:
        with handle:
            json.dump(
                report,
                handle,
                indent=2,
                sort_keys=True,
                ensure_ascii=False,
                allow_nan=False,
            )
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def run_serial_bench(args: argparse.Namespace) -> tuple[int, dict[str, Any]]:
    port = validate_port(args.port)
    observations = parse_observations(args.observe)
    commands = build_commands(
        args.diagnostic,
        audio=args.test_audio,
        rgb=args.test_rgb,
        display=args.test_display,
    )
    analyzer = BenchAnalyzer(
        sent_commands=(),
        observations=observations,
        planned_commands=commands,
        rx_epoch=0,
    )
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as error:
        raise RuntimeError(
            "pyserial is required only for live hardware. Install it with: "
            f"{sys.executable} -m pip install pyserial"
        ) from error

    started = time.monotonic()
    deadline = started + args.timeout
    pending = bytearray()
    command_index = 0
    next_command_at = started + 0.5
    print(
        f"C151 bench: explicit port={json.dumps(redact_text(port))} baud={args.baud} "
        f"window={args.timeout:g}s; no automatic port selection",
        flush=True,
    )
    with serial.Serial(
        port=port,
        baudrate=args.baud,
        timeout=0.1,
        write_timeout=1.0,
        exclusive=True if os.name == "posix" else None,
    ) as device:
        analyzer.begin_rx_epoch(int((time.monotonic() - started) * 1000))
        drain_serial_rx_backlog(device, pending, analyzer, started)
        while time.monotonic() < deadline:
            now = time.monotonic()
            if command_index < len(commands) and now >= next_command_at:
                if command_index > 0 and not analyzer.command_completed(
                    commands[command_index - 1]
                ):
                    chunk = device.read(1)
                    if chunk:
                        elapsed_ms = int((time.monotonic() - started) * 1000)
                        if not consume_serial_chunk(
                            chunk, pending, analyzer, elapsed_ms, emit_console=True
                        ):
                            break
                    continue
                command = commands[command_index]
                drain_serial_rx_backlog(device, pending, analyzer, started)
                command_elapsed_ms = int((time.monotonic() - started) * 1000)
                analyzer.begin_rx_epoch(command_elapsed_ms)
                device.write((command + "\n").encode("ascii"))
                device.flush()
                analyzer.record_command_sent(command, command_elapsed_ms)
                print(f"sent safe command: {command}", flush=True)
                command_index += 1
                next_command_at = now + 0.5

            chunk = device.read(1)
            if not chunk:
                continue
            elapsed_ms = int((time.monotonic() - started) * 1000)
            if not consume_serial_chunk(
                chunk, pending, analyzer, elapsed_ms, emit_console=True
            ):
                break

    analyzer.finish(timed_out=True, partial=bytes(pending))
    report = analyzer.build_report(
        port=port,
        baud=args.baud,
        timeout_seconds=args.timeout,
    )
    return (0 if report["verdict"]["overall"] != "FAIL" else 1), report


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Safe post-flash M5 PaperColor C151 serial acceptance harness"
    )
    parser.add_argument("--port", required=True, help="explicit serial port, e.g. /dev/cu.usbmodem1234 or COM5")
    parser.add_argument("--baud", type=valid_baud, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=bounded_timeout, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--report", type=Path, help="optional JSON report path")
    parser.add_argument("--diagnostic", choices=sorted(SAFE_BASE_COMMANDS), default="status")
    parser.add_argument("--test-audio", action="store_true", help="explicitly send sound-test")
    parser.add_argument("--test-rgb", action="store_true", help="explicitly send led-test")
    parser.add_argument("--test-display", action="store_true", help="explicitly send screen-test/full refresh")
    parser.add_argument(
        "--observe",
        action="append",
        default=[],
        metavar="CHECK=pass|fail|skip",
        help="record an operator observation without claiming machine verification",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = argument_parser()
    args = parser.parse_args(argv)
    try:
        exit_code, report = run_serial_bench(args)
    except (
        BenchInputError,
        RuntimeError,
        OSError,
        ValueError,
        TypeError,
        RecursionError,
        MemoryError,
    ) as error:
        print(f"C151 bench error: {redact_text(str(error))}", file=sys.stderr)
        return 2
    if args.report:
        try:
            write_report(args.report, report)
            print(f"redacted JSON report: {redact_text(str(args.report))}")
        except (OSError, ValueError, TypeError, RecursionError, MemoryError) as error:
            print(f"C151 bench report error: {redact_text(str(error))}", file=sys.stderr)
            return 2
    print(
        f"machine verdict: {report['verdict']['machine']}; "
        f"human verdict: {report['verdict']['human']}; "
        f"overall verdict: {report['verdict']['overall']}; "
        f"physical verdict: {report['verdict']['physical']}",
        flush=True,
    )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
