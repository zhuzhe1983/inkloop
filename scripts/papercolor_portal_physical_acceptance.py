#!/usr/bin/env python3
"""Authenticated, credential-safe local Portal acceptance for PaperColor.

The password is read with getpass and is never accepted on argv or printed.
The full flow uploads a generated diagnostic PNG, previews it, displays it,
changes its render strategy, restores the previous frame when one existed,
and deletes the temporary asset. It also exercises local chat, volume preview,
and the save/restore LED-brightness path. No MyAI credential is used.
"""

from __future__ import annotations

import argparse
import binascii
import getpass
import http.cookiejar
import ipaddress
import json
import secrets
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from dataclasses import dataclass
from typing import Any, Callable


MAX_JSON_BYTES = 65536
MAX_PREVIEW_BYTES = 12 * 1024 * 1024
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class AcceptanceFailure(RuntimeError):
    pass


class PortalHttpError(AcceptanceFailure):
    def __init__(self, method: str, path: str, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(f"{method} {path}: HTTP {status} {code}")


def local_base_url(value: str) -> str:
    parsed = urllib.parse.urlsplit(value)
    if (
        parsed.scheme != "http"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
    ):
        raise argparse.ArgumentTypeError("base URL must be a plain local http origin")
    host = parsed.hostname.rstrip(".").lower()
    allowed = host == "localhost" or host.endswith(".local")
    if not allowed:
        try:
            address = ipaddress.ip_address(host)
            allowed = address.is_private or address.is_loopback or address.is_link_local
        except ValueError:
            allowed = False
    if not allowed:
        raise argparse.ArgumentTypeError("base URL must target localhost, .local, or a private IP")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def _chunk(kind: bytes, body: bytes) -> bytes:
    return (
        struct.pack(">I", len(body))
        + kind
        + body
        + struct.pack(">I", binascii.crc32(kind + body) & 0xFFFFFFFF)
    )


def diagnostic_png(width: int, height: int) -> bytes:
    if width < 16 or height < 16 or width > 1200 or height > 1200:
        raise AcceptanceFailure("display dimensions are outside the diagnostic PNG bound")
    rows = bytearray()
    border = max(3, min(width, height) // 40)
    for y in range(height):
        rows.append(0)
        for x in range(width):
            if x < border or y < border or x >= width - border or y >= height - border:
                color = (0, 0, 0)
            elif y < height // 5:
                color = (220, 25, 35)
            elif x >= width * 4 // 5:
                color = (245, 205, 25)
            elif y >= height * 4 // 5:
                color = (35, 100, 200)
            elif x < width // 5:
                color = (25, 145, 70)
            else:
                color = (255, 255, 255) if ((x // 24) + (y // 24)) % 2 else (15, 15, 15)
            rows.extend(color)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return PNG_SIGNATURE + _chunk(b"IHDR", ihdr) + _chunk(
        b"IDAT", zlib.compress(bytes(rows), 9)
    ) + _chunk(b"IEND", b"")


def _error_code(body: bytes) -> str:
    if len(body) > MAX_JSON_BYTES:
        return "response_too_large"
    try:
        value = json.loads(body.decode("utf-8")) if body else {}
    except (UnicodeDecodeError, json.JSONDecodeError):
        return "invalid_response"
    if isinstance(value, dict) and isinstance(value.get("error"), str):
        code = value["error"]
        if 1 <= len(code) <= 96 and all(
            character.islower() or character.isdigit() or character in "_-"
            for character in code
        ):
            return code
    return "request_failed"


class PortalClient:
    def __init__(self, base_url: str, timeout: float) -> None:
        self.base_url = local_base_url(base_url)
        self.origin = self.base_url
        self.timeout = timeout
        self.csrf = ""
        cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(cookies)
        )

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        content_type: str | None = None,
    ) -> tuple[int, str, bytes]:
        if not path.startswith("/") or path.startswith("//"):
            raise AcceptanceFailure("invalid Portal path")
        headers = {"Accept": "application/json", "Connection": "close"}
        if method == "POST":
            headers["Origin"] = self.origin
            if self.csrf:
                headers["X-Inkloop-CSRF"] = self.csrf
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(
            self.base_url + path,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                payload = response.read(MAX_PREVIEW_BYTES + 1)
                return response.status, response.headers.get_content_type(), payload
        except urllib.error.HTTPError as error:
            payload = error.read(MAX_JSON_BYTES + 1)
            raise PortalHttpError(method, path, error.code, _error_code(payload)) from None

    def json(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        content_type: str | None = None,
    ) -> dict[str, Any]:
        status, _, payload = self.request(method, path, body, content_type)
        if status < 200 or status >= 300 or len(payload) > MAX_JSON_BYTES:
            raise AcceptanceFailure(f"{method} {path}: invalid response bounds")
        try:
            value = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise AcceptanceFailure(f"{method} {path}: invalid JSON") from None
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise AcceptanceFailure(f"{method} {path}: invalid success response")
        return value

    def form(self, path: str, fields: dict[str, str]) -> dict[str, Any]:
        body = urllib.parse.urlencode(fields).encode("ascii")
        return self.json(
            "POST", path, body, "application/x-www-form-urlencoded"
        )

    def login(self, password: str) -> None:
        if not password or len(password.encode("utf-8")) > 128:
            raise AcceptanceFailure("invalid local password length")
        result = self.form("/api/session", {"nonce": password})
        csrf = result.get("csrfToken")
        if not isinstance(csrf, str) or len(csrf) < 24 or len(csrf) > 128:
            raise AcceptanceFailure("Portal session omitted a bounded CSRF token")
        self.csrf = csrf

    def state(self) -> dict[str, Any]:
        value = self.json("GET", "/api/state").get("state")
        if not isinstance(value, dict):
            raise AcceptanceFailure("Portal state is missing")
        return value

    def album(self) -> list[dict[str, Any]]:
        output: list[dict[str, Any]] = []
        cursor = ""
        seen = set()
        for _ in range(8):
            query = urllib.parse.urlencode({"limit": "16", "cursor": cursor})
            value = self.json("GET", "/api/album?" + query)
            items = value.get("items")
            page = value.get("page")
            if not isinstance(items, list) or not isinstance(page, dict):
                raise AcceptanceFailure("Portal album page is invalid")
            for item in items:
                if not isinstance(item, dict) or not isinstance(item.get("id"), str):
                    raise AcceptanceFailure("Portal album item is invalid")
                output.append(item)
            next_cursor = page.get("nextCursor")
            if not isinstance(next_cursor, str):
                raise AcceptanceFailure("Portal album cursor is invalid")
            if not next_cursor:
                return output
            if next_cursor in seen:
                raise AcceptanceFailure("Portal album cursor loop detected")
            seen.add(next_cursor)
            cursor = next_cursor
        raise AcceptanceFailure("Portal album exceeds bounded pagination")


def _queued(value: dict[str, Any], command: str) -> None:
    if (
        value.get("state") != "queued"
        or value.get("command") != command
        or not isinstance(value.get("requestId"), int)
        or value["requestId"] < 1
    ):
        raise AcceptanceFailure(f"Portal did not queue {command}")


def _wait(
    predicate: Callable[[], Any], timeout: float, interval: float, description: str
) -> Any:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except PortalHttpError as error:
            if error.status not in (409, 503):
                raise
            last_error = error
        except (TimeoutError, urllib.error.URLError) as error:
            last_error = error
        time.sleep(interval)
    suffix = f" ({type(last_error).__name__})" if last_error else ""
    raise AcceptanceFailure(f"timeout waiting for {description}{suffix}")


def _state_contract(state: dict[str, Any]) -> None:
    required = {
        "firmwareVersion": str,
        "deviceName": str,
        "displayWidth": int,
        "displayHeight": int,
        "wifiOnline": bool,
        "storageReady": bool,
        "displayBusy": bool,
        "storageFreeBytes": int,
        "storageTotalBytes": int,
        "settings": dict,
        "capabilities": dict,
        "displayTiming": dict,
        "myAi": dict,
    }
    if any(not isinstance(state.get(key), kind) for key, kind in required.items()):
        raise AcceptanceFailure("Portal state contract is incomplete")
    if not state["storageReady"] or state["storageFreeBytes"] > state["storageTotalBytes"]:
        raise AcceptanceFailure("Portal storage state is invalid")


def _find_title(client: PortalClient, title: str) -> dict[str, Any] | None:
    return next((item for item in client.album() if item.get("title") == title), None)


def _display_completed(client: PortalClient, before: Any) -> bool:
    state = client.state()
    timing = state.get("displayTiming")
    return (
        isinstance(before, int)
        and state.get("displayBusy") is False
        and isinstance(timing, dict)
        and timing.get("completedRefreshes", 0) > before
    )


@dataclass(frozen=True)
class AcceptanceConfig:
    poll_timeout: float = 240.0
    poll_interval: float = 2.0
    read_only: bool = False


def run_acceptance(client: PortalClient, password: str, config: AcceptanceConfig) -> None:
    health = client.json("GET", "/health")
    if health.get("service") != "inkloop-portal":
        raise AcceptanceFailure("Portal health service identity is invalid")
    print("PORTAL HEALTH PASS")

    client.login(password)
    print("PORTAL SESSION PASS")
    state = client.state()
    _state_contract(state)
    print("PORTAL STATE PASS")

    original_album = client.album()
    original_current = next((item for item in original_album if item.get("current") is True), None)
    chat = client.json("GET", "/api/chat?limit=24&after=0")
    if chat.get("audioIncluded") is not False or not isinstance(chat.get("messages"), list):
        raise AcceptanceFailure("local chat contract is invalid")
    if any(
        "blank_audio" in str(item.get("text", "")).lower()
        for item in chat["messages"]
        if isinstance(item, dict)
    ):
        raise AcceptanceFailure("local chat exposed blank_audio")
    print("LOCAL CHAT PASS")
    if config.read_only:
        print("PORTAL READ-ONLY ACCEPTANCE PASS")
        return

    settings = state["settings"]
    capabilities = state["capabilities"]
    original_led = settings.get("ledMaximumBrightness")
    led_changed = False
    cleanup_errors: list[str] = []
    title = "Inkloop Portal E2E " + secrets.token_hex(4)
    uploaded_id = ""

    try:
        if capabilities.get("duplexAudio") is True:
            volume = settings.get("volume")
            if not isinstance(volume, int) or volume < 0 or volume > 100:
                raise AcceptanceFailure("saved volume is invalid")
            _queued(client.form("/api/audio/preview", {"volume": str(volume)}), "PREVIEW_VOLUME")
            print("AUDIO PREVIEW QUEUED")

        if isinstance(original_led, int) and capabilities.get("rgbPixels", 0) > 0:
            temporary = original_led - 1 if original_led > 0 else 1
            _queued(
                client.form("/api/settings", {"led_brightness": str(temporary)}),
                "UPDATE_SETTINGS",
            )
            _wait(
                lambda: client.state()["settings"].get("ledMaximumBrightness") == temporary,
                config.poll_timeout,
                config.poll_interval,
                "temporary LED brightness",
            )
            led_changed = True
            print("LED BRIGHTNESS TEST QUEUED")

        width, height = state["displayWidth"], state["displayHeight"]
        image = diagnostic_png(width, height)
        path = "/api/album/upload?" + urllib.parse.urlencode({"title": title})
        _queued(client.json("POST", path, image, "image/png"), "COMMIT_ALBUM_UPLOAD")
        uploaded = _wait(
            lambda: _find_title(client, title),
            config.poll_timeout,
            config.poll_interval,
            "uploaded album item",
        )
        uploaded_id = uploaded["id"]
        print("ALBUM UPLOAD PASS")

        status, content_type, preview = client.request(
            "GET", "/api/album/preview?" + urllib.parse.urlencode({"asset_id": uploaded_id})
        )
        if (
            status != 200
            or content_type != "image/png"
            or len(preview) > MAX_PREVIEW_BYTES
            or not preview.startswith(PNG_SIGNATURE)
        ):
            raise AcceptanceFailure("album preview contract failed")
        print("ALBUM PREVIEW PASS")

        before = client.state()["displayTiming"].get("completedRefreshes")
        _queued(client.form("/api/album/display", {"asset_id": uploaded_id}), "DISPLAY_ALBUM_ITEM")
        _wait(
            lambda: (_find_title(client, title) or {}).get("current") is True,
            config.poll_timeout,
            config.poll_interval,
            "diagnostic image display",
        )
        _wait(
            lambda: _display_completed(client, before),
            config.poll_timeout,
            config.poll_interval,
            "diagnostic image panel refresh",
        )
        print("ALBUM DISPLAY PASS")

        strategies = capabilities.get("renderStrategies")
        if not isinstance(strategies, list) or not strategies:
            raise AcceptanceFailure("render strategy catalog is unavailable")
        current_strategy = (_find_title(client, title) or {}).get("renderStrategy")
        strategy_ids = [entry.get("id") for entry in strategies if isinstance(entry, dict)]
        target_strategy = next(
            (value for value in strategy_ids if value and value != current_strategy),
            current_strategy,
        )
        if not isinstance(target_strategy, str) or not target_strategy:
            raise AcceptanceFailure("render strategy selection failed")
        before = client.state()["displayTiming"].get("completedRefreshes")
        _queued(
            client.form(
                "/api/album/render",
                {"asset_id": uploaded_id, "render_strategy": target_strategy},
            ),
            "SET_ALBUM_RENDER_STRATEGY",
        )
        _wait(
            lambda: (_find_title(client, title) or {}).get("renderStrategy") == target_strategy,
            config.poll_timeout,
            config.poll_interval,
            "album render strategy",
        )
        if target_strategy != current_strategy:
            _wait(
                lambda: _display_completed(client, before),
                config.poll_timeout,
                config.poll_interval,
                "strategy panel refresh",
            )
        print("ALBUM RENDER POLICY PASS")

        if original_current and original_current.get("id") != uploaded_id:
            original_id = original_current["id"]
            before = client.state()["displayTiming"].get("completedRefreshes")
            _queued(
                client.form("/api/album/display", {"asset_id": original_id}),
                "DISPLAY_ALBUM_ITEM",
            )
            _wait(
                lambda: next(
                    (item for item in client.album() if item.get("id") == original_id), {}
                ).get("current") is True,
                config.poll_timeout,
                config.poll_interval,
                "previous panel frame restoration",
            )
            _wait(
                lambda: _display_completed(client, before),
                config.poll_timeout,
                config.poll_interval,
                "previous panel frame refresh",
            )
            print("PREVIOUS PANEL FRAME RESTORED")
    finally:
        if uploaded_id:
            try:
                _queued(
                    client.form("/api/album/delete", {"asset_id": uploaded_id}),
                    "DELETE_ALBUM_ITEM",
                )
                _wait(
                    lambda: _find_title(client, title) is None,
                    config.poll_timeout,
                    config.poll_interval,
                    "temporary album deletion",
                )
                print("ALBUM DELETE PASS")
            except Exception as error:
                cleanup_errors.append("temporary album deletion")
                print(f"CLEANUP WARNING: temporary album item remains ({type(error).__name__})")
        if led_changed:
            try:
                _queued(
                    client.form("/api/settings", {"led_brightness": str(original_led)}),
                    "UPDATE_SETTINGS",
                )
                _wait(
                    lambda: client.state()["settings"].get("ledMaximumBrightness") == original_led,
                    config.poll_timeout,
                    config.poll_interval,
                    "LED brightness restoration",
                )
                print("LED BRIGHTNESS RESTORED")
            except Exception as error:
                cleanup_errors.append("LED brightness restoration")
                print(f"CLEANUP WARNING: LED brightness restore failed ({type(error).__name__})")

    if cleanup_errors:
        raise AcceptanceFailure("cleanup failed: " + ", ".join(cleanup_errors))
    print("PORTAL PHYSICAL PIPELINE PASS")
    print(
        "HUMAN CHECK REQUIRED: audio preview, LED sequence, colors, "
        "orientation, and final restored frame"
    )


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", type=local_base_url, default="http://inkloop.local")
    parser.add_argument("--timeout", type=positive_float, default=15.0)
    parser.add_argument("--poll-timeout", type=positive_float, default=240.0)
    parser.add_argument("--poll-interval", type=positive_float, default=2.0)
    parser.add_argument("--read-only", action="store_true")
    args = parser.parse_args()
    password = getpass.getpass("Inkloop 本地管理密码: ")
    try:
        run_acceptance(
            PortalClient(args.base_url, args.timeout),
            password,
            AcceptanceConfig(args.poll_timeout, args.poll_interval, args.read_only),
        )
        return 0
    except (AcceptanceFailure, OSError) as error:
        print(f"FAIL: {error}")
        return 1
    finally:
        password = "\0" * len(password)


if __name__ == "__main__":
    raise SystemExit(main())
